"""RPCS3 game patches -> switches of the native port.

  python -X utf8 tools\\rpcs3_patches.py <patch.yml> [--list] [--emit]

Reads an RPCS3 patch file (patch.yml, read only) and, for BCES01893 01.22, sorts each patch into
  insn   only whole instructions / data words replaced       -> can be a run-time switch (ppu_lifter --insn-patches)
  data   only bytes outside the code                         -> written into guest memory at start
  cave   needs new code (calloc / jumpl / alloc lines)       -> has to be written by hand as a hook (not done here)
--list prints that table. --emit writes, for the patches named in WANT below,
  out\\EMAIN.insn_patches.json   for `ppu_lifter.py --insn-patches`
  gt6_patches.inc                the guards, their environment names and the data writes, included by gt6_host.cpp
Patches whose author is Nenkai are not read (that author's work is left out on purpose).
The file needs no YAML library: it reads the few shapes the patch file uses, anchors (`name: &name` / `[ load, *name ]`)
included.
"""
import json, os, re, struct, sys

SERIAL, VERSION = "BCES01893", "01.22"
CODE_END = 0x10C2208
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
from game_paths import GAME
ELF = str(GAME / 'EMAIN.ELF')
# title in the patch file -> (guard symbol, environment variable). Only these are emitted.
WANT = {
    "Enable Bloom/Glare in All Cameras": ("g_gt6p_bloom_all_cameras", "GT6_PATCH_BLOOM_ALL_CAMERAS"),
    "Disable Depth of Field":            ("g_gt6p_no_dof",            "GT6_PATCH_NO_DOF"),
    "Disable Motion Blur":               ("g_gt6p_no_motion_blur",    "GT6_PATCH_NO_MOTION_BLUR"),
    "Unlock FPS":                        ("g_gt6p_unlock_fps",        "GT6_PATCH_UNLOCK_FPS"),
    "Disable Driver Rendering":          ("g_gt6p_no_driver",         "GT6_PATCH_NO_DRIVER"),
    # Not here: "Analog Dials in All Cameras" / "... in Roof Camera" replace branches with other branches, which
    # the lifter's switch cannot express (the replacement may not branch: ppu_lifter _patched_translate); they
    # have to be written by hand like GT6_NO_MLAA. The two also change the same addresses in different ways.
    "Use Sharper MLAA":                  ("g_gt6p_sharper_mlaa",      "GT6_PATCH_SHARPER_MLAA"),
    "Use Sharper MLAA - Strong":         ("g_gt6p_sharper_mlaa_strong", "GT6_PATCH_SHARPER_MLAA_STRONG"),
}
# Patches of which only the plain replacements are taken; their code caves are left out (noted in the list):
#   Unlock FPS   the two frame-rate cap immediates; not the cave that turns a 30 fps menu interval into 60.
PARTIAL = {"Unlock FPS"}

ITEM = re.compile(r'^\s*-\s*\[\s*([a-z0-9]+)\s*,\s*([^,\]]+?)\s*(?:,\s*(.+?))?\s*\]\s*(?:#\s*(.*))?$')


def read_patches(path):
    lines = open(path, encoding="utf-8", errors="replace").read().split("\n")
    anchors, i = {}, 0
    while i < len(lines):                      # anchors: `  name: &name` followed by list items
        m = re.match(r'^\s*([A-Za-z0-9_]+):\s*&([A-Za-z0-9_]+)\s*$', lines[i])
        if m:
            body, j = [], i + 1
            while j < len(lines) and (ITEM.match(lines[j]) or not lines[j].strip() or lines[j].lstrip().startswith("#")
                                      or re.match(r'^\s{4,}\S', lines[j])):
                if re.match(r'^  "(.+)":\s*$', lines[j]) or re.match(r'^\s*[A-Za-z0-9_]+:\s*&', lines[j]):
                    break
                body.append(lines[j]); j += 1
            anchors[m.group(2)] = body
            i = j
        else:
            i += 1
    out, i = [], 0
    while i < len(lines):
        m = re.match(r'^  "(.+)":\s*$', lines[i])
        if not m:
            i += 1; continue
        title, j, block = m.group(1), i + 1, []
        while j < len(lines) and not re.match(r'^  "(.+)":\s*$', lines[j]) and not re.match(r'^\S', lines[j]):
            block.append(lines[j]); j += 1
        text = "\n".join(block)
        if re.search(r'%s:\s*\[[^\]]*%s' % (SERIAL, re.escape(VERSION)), text):
            a = re.search(r'Author:\s*"?([^"\n]*)"?', text)
            author = a.group(1).strip() if a else "?"
            body = text.split("Patch:", 1)[1].split("\n") if "Patch:" in text else []
            out.append({"title": title, "author": author, "lines": body,
                        "configurable": "Configurable Values:" in text})
        i = j
    return out, anchors


def expand(lines, anchors, depth=0):
    items = []
    for ln in lines:
        m = ITEM.match(ln)
        if not m:
            continue
        kind, a, b, note = m.group(1), m.group(2), m.group(3), m.group(4) or ""
        if kind == "load":
            name = a.lstrip("*")
            if name in anchors and depth < 4:
                items += expand(anchors[name], anchors, depth + 1)
            else:
                items.append(("load?", name, None, note))
        else:
            items.append((kind, a, b, note))
    return items


class Elf:
    def __init__(self, path):
        self.d = open(path, "rb").read()
        phoff, = struct.unpack_from(">Q", self.d, 0x20)
        phentsize, phnum = struct.unpack_from(">HH", self.d, 0x36)
        self.seg = []
        for k in range(phnum):
            t, _f, off, va, _pa, fsz, _msz, _al = struct.unpack_from(">IIQQQQQQ", self.d, phoff + k * phentsize)
            if t == 1:
                self.seg.append((va, off, fsz))

    def word(self, va):
        for a, off, fsz in self.seg:
            if a <= va and va + 4 <= a + fsz:
                return struct.unpack_from(">I", self.d, off + (va - a))[0]
        return None


def classify(p, anchors, elf):
    """-> (kind, insn {addr: new word}, data [(addr, bytes)], why)"""
    if "nenkai" in p["author"].lower():
        return "skip", {}, [], "author"
    items = expand(p["lines"], anchors)
    if p["title"] in PARTIAL:                  # drop each cave: its calloc line and the lines at "address 0" after it
        kept, in_cave = [], False
        for it in items:
            if it[0] in ("calloc", "alloc"):
                in_cave = True; continue
            if in_cave and it[1].strip().lower() in ("0x0", "0"):
                continue
            in_cave = False
            kept.append(it)
        items = kept
    if any(k in ("calloc", "jumpl", "jump", "alloc", "code", "load?") for k, *_ in items):
        return "cave", {}, [], "needs new code: " + ", ".join(sorted({k for k, *_ in items if k in ("calloc", "jumpl", "jump", "alloc", "code", "load?")}))
    insn, data = {}, []
    for kind, a, b, _note in items:
        try:
            addr = int(a, 0)
        except ValueError:
            return "cave", {}, [], "address is not a number: " + a
        if kind == "bef32":
            try:
                raw = struct.pack(">f", float(b))
            except (TypeError, ValueError):
                return "config", {}, [], "value chosen in the patch manager: " + str(b)
        elif kind in ("be32", "be16", "byte", "be64"):
            try:
                v = int(b, 0)
            except (TypeError, ValueError):
                return "config", {}, [], "value chosen in the patch manager: " + str(b)
            raw = {"be32": struct.pack(">I", v & 0xFFFFFFFF), "be16": struct.pack(">H", v & 0xFFFF),
                   "byte": bytes([v & 0xFF]), "be64": struct.pack(">Q", v & 0xFFFFFFFFFFFFFFFF)}[kind]
        else:
            return "cave", {}, [], "line type " + kind
        if addr < CODE_END:
            for n, byte in enumerate(raw):              # into whole instruction words
                w = (addr + n) & ~3
                cur = insn.get(w, elf.word(w))
                if cur is None:
                    return "cave", {}, [], "0x%08X is not in the image" % w
                sh = (3 - ((addr + n) & 3)) * 8
                insn[w] = (cur & ~(0xFF << sh)) | (byte << sh)
        else:
            data.append((addr, raw))
    kind = "insn" if insn and not data else "data" if data and not insn else "insn+data" if insn else "empty"
    return kind, insn, data, ""


def main():
    args = sys.argv[1:]
    path = args[0]
    patches, anchors = read_patches(path)
    elf = Elf(ELF)
    rows = [(p, *classify(p, anchors, elf)) for p in patches]
    if "--list" in args or "--emit" not in args:
        for p, kind, insn, data, why in rows:
            mark = "*" if p["title"] in WANT else " "
            print(f"{mark} {kind:9} {len(insn):3} insn {sum(len(b) for _, b in data):5} data bytes  {p['title']}  -- {p['author']}"
                  + (f"   ({why})" if why else ""))
    if "--emit" not in args:
        return
    jp, inc, datas, guards = [], [], [], []
    for p, kind, insn, data, why in rows:
        if p["title"] not in WANT:
            continue
        guard, env = WANT[p["title"]]
        if kind not in ("insn", "data", "insn+data"):
            raise SystemExit(f"{p['title']}: cannot be emitted ({kind}: {why})")
        guards.append((guard, env, p["title"], p["author"]))
        for addr in sorted(insn):
            orig = elf.word(addr)
            if orig != insn[addr]:
                jp.append({"addr": "0x%08X" % addr, "orig": "0x%08X" % orig, "new": "0x%08X" % insn[addr],
                           "guard": guard, "note": p["title"]})
        for addr, raw in data:
            datas.append((guard, addr, raw))
    os.makedirs(os.path.join(ROOT, "out"), exist_ok=True)
    with open(os.path.join(ROOT, "out", "EMAIN.insn_patches.json"), "w", encoding="utf-8") as f:
        json.dump({"patches": jp}, f, indent=1)
    inc.append("/* Generated by tools\\rpcs3_patches.py from the launcher's RPCS3 patch file -- do not edit.\n"
               " * Each patch is a run-time switch: the lifted code tests the guard (ppu_lifter --insn-patches). */")
    inc.append('extern "C" {')
    for guard, *_ in guards:
        inc.append(f"int {guard} = 0;")
    inc.append("}")
    inc.append("static const struct { int* guard; const char* env; const char* title; const char* author; } k_gt6_patches[] = {")
    for guard, env, title, author in guards:
        inc.append(f'    {{ &{guard}, "{env}", "{title}", "{author}" }},')
    inc.append("};")
    inc.append("static const struct { int* guard; unsigned addr; unsigned len; const unsigned char* bytes; } k_gt6_patch_data[] = {")
    for guard, addr, raw in datas:
        lit = "".join("\\x%02X" % b for b in raw)
        inc.append(f'    {{ &{guard}, 0x{addr:08X}u, {len(raw)}u, (const unsigned char*)"{lit}" }},')
    inc.append("    { nullptr, 0u, 0u, nullptr }")
    inc.append("};")
    with open(os.path.join(ROOT, "gt6_patches.inc"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(inc) + "\n")
    print(f"wrote out\\EMAIN.insn_patches.json ({len(jp)} instruction patches) and gt6_patches.inc ({len(guards)} switches, {len(datas)} data writes)")


if __name__ == "__main__":
    main()
