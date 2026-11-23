#!/usr/bin/env python3
"""Reapply one reviewed, opt-in PDI leaf transform; never relift other code.

The original loop is retained byte-for-byte after LF normalization. Only r5+
locals and the existing exact extended-SPU RZ helpers are used in the fast path.
The checked original ELF bytes and generated source body are both identities,
not merely function names. Run after lifting this module; --check is read-only.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import struct

ROOT = Path(__file__).resolve().parents[1]
MODULE = "jobmod_9039b3cc346b_e0030"
NAME = MODULE + "_spu_func_00014548"
SOURCE = ROOT / "spu_jobs" / MODULE / "spu_recomp.c"
ORIGINAL_SHA256 = "8311020019cc559e04d1e04ebd50e41de39a0ab29a0bfb517d9912dc9ea74faa"
MODULE_SHA256 = "369cbecba9b2c983591d941892969faccbe88bf9eb2c8683d40a25e449b97544"
OPCODE_SHA256 = "71ea68729ccb8ead33209eb897f0230590936f5b9fd2dabb9b63cf2571f02462"
BEGIN = "/* GT6_NATIVE_PDI_14548_BEGIN */\n"
END = "/* GT6_NATIVE_PDI_14548_END */\n"
FALLBACK = "    /* GT6_NATIVE_PDI_14548_ORIGINAL_FALLBACK */\n"

GATE = r'''#include <stdatomic.h>

/* 0=unread, -1=initializing, 1=disabled, 2=explicitly enabled.
 * Contending first calls use the original path; they never spin or wait.
 * Only the exact value "1" opts in. The setting is fixed for this process.
 */
static atomic_int gt6_pdi_14548_state = 0;
static inline int gt6_pdi_14548_enabled(void)
{
    int state = atomic_load_explicit(&gt6_pdi_14548_state, memory_order_acquire);
    if (state == 0) {
        int expected = 0;
        if (atomic_compare_exchange_strong_explicit(&gt6_pdi_14548_state,
                &expected, -1, memory_order_acq_rel, memory_order_acquire)) {
            const char* value = getenv("GT6_NATIVE_PDI_LEAVES");
            state = value && value[0] == '1' && value[1] == '\0' ? 2 : 1;
            atomic_store_explicit(&gt6_pdi_14548_state, state, memory_order_release);
        } else {
            state = expected;
        }
    }
    return state == 2;
}

'''


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def verify_original(body: str) -> None:
    if sha256(body.encode()) != ORIGINAL_SHA256:
        raise ValueError("14548 original body changed: review required; no source written")


def transform_body(body: str) -> str:
    verify_original(body)
    start = body.index("{") + 1
    inner = body[start:body.rfind("}")]
    forbidden = ("spu_rdch", "spu_wrch", "spu_rchcnt", "SPU_DIRECT", "SPU_INDIRECT", "SPU_CALL")
    if any(token in inner for token in forbidden) or inner.count("SPU_RET(ctx);") != 1:
        raise ValueError("14548 is no longer the reviewed pure loop")
    regs = sorted({int(x) for x in re.findall(r"ctx->gpr\[(\d+)\]", inner) if int(x) >= 5})
    writes = sorted({int(x) for x in re.findall(r"ctx->gpr\[(\d+)\]\s*=", inner) if int(x) >= 5})
    fast = re.sub(r"ctx->gpr\[(\d+)\]",
                  lambda m: f"gpr_{m[1]}" if int(m[1]) >= 5 else m[0], inner)
    fast = re.sub(r"\bloc_([0-9A-F]+)\b", r"pdi_rz_loc_\1", fast)

    def fp(match: re.Match) -> str:
        op, args = match.groups()
        return f"spu__xr_fm({args})" if op == "fm" else f"spu__xr_fma({args}, 0, 0)"

    fast, count = re.subn(r"spu_(fma|fm)\(([^()]*)\)", fp, fast)
    if count != 4 or re.search(r"spu_(?:fm|fma)\(", fast):
        raise ValueError("14548 floating-point operations changed")
    init = "".join(f"        u128 gpr_{r} = ctx->gpr[{r}];\n" for r in regs)
    flush = "".join(f"        ctx->gpr[{r}] = gpr_{r};\n" for r in writes)
    fast = fast.replace("        SPU_RET(ctx);", flush + "        SPU_RET(ctx);")
    return body[:start] + '''
#if defined(__AVX2__) && defined(__FMA__) && !defined(SPU_SCALAR_HELPERS)
    /* No channel, DMA, guest call, or FP-control write occurs in this leaf.
     * r0-r4 remain observable by LS diagnostics; residency callbacks and the
     * exact LS helpers run in the same order. Commit r5+ before SPU_RET.
     * Count zero deliberately retains decrement/wrap semantics.
     */
    if (gt6_pdi_14548_enabled() && spu__x8_rz()) {
''' + init + fast + '''    }
#endif
''' + FALLBACK + body[start:]


def region(body: str) -> str:
    return BEGIN + GATE + transform_body(body) + END


def locate(text: str) -> tuple[int, int, str, bool]:
    if BEGIN in text:
        if text.count(BEGIN) != 1 or text.count(END) != 1:
            raise ValueError("ambiguous 14548 generated region")
        start = text.index(BEGIN)
        end = text.index(END, start) + len(END)
        current = text[start:end]
        fallback_start = current.index(FALLBACK) + len(FALLBACK)
        body = "void " + NAME + "(spu_context* ctx) {" + current[fallback_start:-len(END)]
        verify_original(body)
        if current != region(body):
            raise ValueError("14548 optimized region changed: review required; no source written")
        return start, end, body, True
    start = text.index("void " + NAME + "(")
    end = text.index("\nvoid ", start + 1)
    body = text[start:end]
    verify_original(body)
    return start, end, body, False


def verify_artifact() -> dict:
    manifest_path = ROOT / "spu_jobs/jobmods_manifest.json"
    manifest = next(x for x in json.loads(manifest_path.read_text()) if x["name"] == MODULE)
    expected = {"base": 0x4000, "ea": 0x143F280, "size": 77264,
                "entry": 0x4030, "code_end": 89744}
    if any(manifest[k] != v for k, v in expected.items()):
        raise ValueError("module manifest changed")
    game = (ROOT / (ROOT / "game.path").read_text(encoding="utf-8-sig").strip()).resolve()
    if not game.is_relative_to(ROOT):
        raise ValueError("game.path is outside the workspace")
    elf = game / "EMAIN.ELF"
    with elf.open("rb") as stream:
        header = stream.read(64)
        if header[:6] != b"\x7fELF\x02\x02":
            raise ValueError("expected original big-endian ELF64")
        phoff = struct.unpack_from(">Q", header, 32)[0]
        entsz, num = struct.unpack_from(">HH", header, 54)
        if entsz < 56 or num > 1024:
            raise ValueError("invalid ELF program header table")
        for index in range(num):
            stream.seek(phoff + index * entsz)
            typ, _, offset, va, _, size, _, _ = struct.unpack_from(">IIQQQQQQ", stream.read(entsz))
            if typ != 1 or not (va <= manifest["ea"] and manifest["ea"] + manifest["size"] <= va + size):
                continue
            module_offset = offset + manifest["ea"] - va
            stream.seek(module_offset)
            raw = stream.read(manifest["size"])
            if sha256(raw) != MODULE_SHA256 or hashlib.sha1(raw).hexdigest()[:12] != "9039b3cc346b":
                raise ValueError("original embedded module bytes changed")
            leaf = raw[0x14548 - 0x4000:0x145B0 - 0x4000]
            if len(leaf) != 104 or sha256(leaf) != OPCODE_SHA256:
                raise ValueError("original 14548 opcode bytes changed")
            return dict(elf=str(elf), manifest=str(manifest_path), module=manifest,
                        module_file_offset=module_offset, module_sha256=MODULE_SHA256,
                        leaf_file_offset=module_offset + 0x10548, leaf_start=0x14548,
                        return_pc=0x145A8, padding_pc=0x145AC, next_entry=0x145B0,
                        leaf_sha256=OPCODE_SHA256, leaf_hex=leaf.hex())
    raise ValueError("module is not entirely within one ELF PT_LOAD segment")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    action = parser.add_mutually_exclusive_group()
    action.add_argument("--check", action="store_true")
    action.add_argument("--restore", action="store_true")
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()
    artifact = verify_artifact()
    raw = SOURCE.read_bytes()
    text = raw.decode("utf-8").replace("\r\n", "\n")
    start, end, body, applied = locate(text)
    output = text[:start] + (body if args.restore else region(body)) + text[end:]
    changed = output != text
    if args.check:
        if not applied:
            raise ValueError("14548 transform has not been applied")
    elif changed:
        newline = "\r\n" if b"\r\n" in raw else "\n"
        SOURCE.write_bytes(output.replace("\n", newline).encode())
    report = dict(artifact=artifact, original_body_sha256=ORIGINAL_SHA256,
                  optimized_body_sha256=sha256(transform_body(body).encode()),
                  generated_region_sha256=sha256(region(body).encode()),
                  source=str(SOURCE), changed=changed and not args.check,
                  mode="check" if args.check else "restore" if args.restore else "apply",
                  default="OFF; only GT6_NATIVE_PDI_LEAVES=1 and existing AVX2/FMA RZ mode use fast path")
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
