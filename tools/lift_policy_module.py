# Lifts the body of GT6's SPU job manager (the WWS job manager policy module the PDI SPURS instance runs for all of
# its eight workloads) to C, and writes the unit that registers it with the runtime.
#
#   python tools\lift_policy_module.py [--extra 0x1234,0x5678] [--no-return-entries]
#
# The module: EMAIN.ELF EA 0x014EAA80, 0x2F80 bytes, loaded by the SPURS kernel with one GET to LS 0xA00.
#   0x0A00-0x0AF0  entry (four `ila` words, `br 0x2BC8`) and the interrupt glue at 0xA2C, which builds its
#                  register save/restore thunks at LS 0x3FEC0 at run time: left to the interpreter
#   0x0AF0-0x16A0  zero holes the title fills at run time (stage code, buffers), state and tables - the command
#                  table at 0x1650 names the handlers 0x22E8, 0x1BB0, 0x1C88, 0x21C0, 0x2260, 0x22AC
#   0x16A0-0x3940  the body: lifted here
# Until 2026-11-16 all of it ran in the SPU interpreter: 35.5 million steps a second while driving, about a third of
# every PDI SPU thread (SPU_INTERP_STATS=10).
#
# Output (a directory of its own, so the 56 job modules in spu_jobs\ are not recompiled - that takes 8 minutes):
#   spu_policy\gt6_pm\spu_recomp.c, .h   the lifted functions, prefix gt6_pm_
#   spu_policy\policy_register.c         registers them under image 300 and the body as a verified part of that load
#                                        (spu_overlay_register_verified_subregion, runtime spu_channels.c);
#                                        GT6_PM_LIFT=0 leaves the job manager to the interpreter
# Function entries offered to the lifter besides what it finds itself: the handler body the glue calls (0x2338), the
# command table's targets, every `ila` immediate that points into the body (hand-built return addresses and
# callbacks), and --extra (entries SPU_INTERP_STATS still shows as interpreted).
import argparse, os, struct, subprocess, sys
from game_paths import GAME

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KIT = os.path.join(ROOT, "ps3recomp", "tools")
sys.path.insert(0, KIT)
from wrap_spu_elf import wrap
from spu_disasm import disassemble_spu

ELF = str(GAME / 'EMAIN.ELF')
EA, SIZE, LOAD_LSA = 0x014EAA80, 0x2F80, 0x0A00
BODY_LO, BODY_HI = 0x16A0, 0x3940
ENTRY = 0x2BC8
TABLE = 0x1650
IMAGE = 300
OUT = os.path.join(ROOT, "spu_policy")
NAME = "gt6_pm"

ap = argparse.ArgumentParser()
ap.add_argument("--extra", default="", help="comma-separated extra function entries (LS addresses)")
ap.add_argument("--no-return-entries", action="store_true")
a = ap.parse_args()


def file_offset(elf, ea):
    """EA -> file offset through the PT_LOAD headers of the 64-bit big-endian ELF."""
    phoff, = struct.unpack_from(">Q", elf, 0x20)
    phentsize, phnum = struct.unpack_from(">HH", elf, 0x36)
    for i in range(phnum):
        p = phoff + i * phentsize
        p_type, = struct.unpack_from(">I", elf, p)
        p_offset, p_vaddr = struct.unpack_from(">QQ", elf, p + 8)
        p_filesz, = struct.unpack_from(">Q", elf, p + 0x20)
        if p_type == 1 and p_vaddr <= ea < p_vaddr + p_filesz:
            return p_offset + (ea - p_vaddr)
    sys.exit("EA 0x%08X is in no PT_LOAD segment" % ea)


elf = open(ELF, "rb").read()
off = file_offset(elf, EA)
module = elf[off:off + SIZE]
words = struct.unpack(">%dI" % (SIZE // 4), module)
w = lambda lsa: words[(lsa - LOAD_LSA) // 4]
print("module: EA 0x%08X = file offset 0x%X, first words %s" % (EA, off, " ".join("%08X" % x for x in words[:4])))
if words[:4] != (0x43A67C02, 0x42212C82, 0x436F7502, 0x43034382):
    sys.exit("not the module this script was written for (other game version?)")
print("words at the start of the body: " + " ".join("%05X:%08X" % (x, w(x)) for x in range(BODY_LO - 8, BODY_LO + 12, 4)))
body = module[BODY_LO - LOAD_LSA:BODY_HI - LOAD_LSA]

in_body = lambda x: BODY_LO <= x < BODY_HI and not x & 3
seeds = {0x2338}
seeds |= {w(TABLE + 4 * i) for i in range(8) if in_body(w(TABLE + 4 * i))}
for ins in disassemble_spu(body, BODY_LO):
    if ins.mnemonic == "ila":
        imm = ins.raw & 0 if False else (ins.raw >> 7) & 0x3FFFF      # ila: 18-bit immediate in bits 7..24
        if in_body(imm):
            seeds.add(imm)
seeds |= {int(x, 0) for x in a.extra.split(",") if x.strip()}
seeds = sorted(s for s in seeds if in_body(s) and s != ENTRY)
print("%d function entries offered: %s" % (len(seeds), " ".join("%X" % s for s in seeds)))

os.makedirs(os.path.join(OUT, NAME), exist_ok=True)
tmp_elf = os.path.join(OUT, NAME + ".elf")
with open(tmp_elf, "wb") as f:
    f.write(wrap(body, base=BODY_LO, entry=ENTRY))
cmd = [sys.executable, os.path.join(KIT, "spu_lifter.py"), "--auto-functions", tmp_elf, "--base", hex(BODY_LO),
       "--symbol-prefix", NAME + "_", "-o", os.path.join(OUT, NAME),
       "--source-name", "spu_recomp.c", "--header-name", "spu_recomp.h",
       "--report", os.path.join(OUT, NAME + "_report.json"),
       "--extra-funcs", ",".join(hex(s) for s in seeds)]
if not a.no_return_entries:
    cmd.append("--return-entries")
r = subprocess.run(cmd, capture_output=True, text=True)
sys.stdout.write(r.stdout[-3000:])
sys.stderr.write(r.stderr[-3000:])
if r.returncode:
    sys.exit("spu_lifter failed (%d)" % r.returncode)

L = []
L.append("/* policy_register.c -- GENERATED by tools/lift_policy_module.py.")
L.append(" * Gran Turismo 6's SPU job manager (WWS job manager policy module, EA 0x%08X): its body, LS 0x%X-0x%X," % (EA, BODY_LO, BODY_HI))
L.append(" * registered under image %d as a verified part of the load that puts the module at LS 0x%X. Contains the" % (IMAGE, LOAD_LSA))
L.append(" * title's code bytes, like the lifted C it registers. GT6_PM_LIFT=0 leaves the module to the interpreter. */")
L.append("#include <stdint.h>")
L.append("#include <stdio.h>")
L.append("#include <stdlib.h>")
L.append('#include "spu_overlay_residency.h"')
L.append("")
L.append("extern void spu_begin_image(int image_id);")
L.append("extern void %s_spu_recomp_register(void);" % NAME)
L.append("/* runtime spu_channels.c; declared here because spu_overlay_residency.h is included by every lifted SPU source */")
L.append("extern int spu_overlay_register_verified_subregion(uint32_t load_ea, uint32_t load_lsa, uint32_t sub_lsa,")
L.append("    uint32_t span, int image_id, const spu_overlay_code_segment* code, unsigned count);")
L.append("")
L.append("static const uint8_t code_body[%d] = {" % len(body))
for k in range(0, len(body), 16):
    L.append("    " + ",".join("0x%02X" % x for x in body[k:k + 16]) + ",")
L.append("};")
L.append("")
L.append("__attribute__((constructor)) static void gt6_policy_register_ctor(void)")
L.append("{")
L.append("    static const spu_overlay_code_segment seg = {0x%Xu, sizeof code_body, code_body};" % BODY_LO)
L.append('    const char* e = getenv("GT6_PM_LIFT");')
L.append("    if (e && *e == '0') {")
L.append('        fprintf(stderr, "[gt6-pm] GT6_PM_LIFT=0: the job manager policy module runs in the SPU interpreter\\n");')
L.append("        return;")
L.append("    }")
L.append("    spu_begin_image(%d);" % IMAGE)
L.append("    %s_spu_recomp_register();" % NAME)
L.append("    spu_begin_image(0);")
L.append("    if (!spu_overlay_register_verified_subregion(0x%08Xu, 0x%Xu, 0x%Xu, 0x%Xu, %d, &seg, 1))"
         % (EA, LOAD_LSA, BODY_LO, BODY_HI - BODY_LO, IMAGE))
L.append('        fprintf(stderr, "[gt6-pm] the job manager body was not registered: interpreted\\n");')
L.append("}")
L.append("")
reg = os.path.join(OUT, "policy_register.c")
with open(reg, "w") as f:
    f.write("\n".join(L))
print("wrote %s (%d bytes of code to verify)" % (reg, len(body)))
