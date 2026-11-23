#!/usr/bin/env python3
"""More PDI leaves of the car vertex job (module 9039b3cc346b) in the form tools/optimize_pdi_leaf.py gave 14548.

  optimize_pdi_leaves.py              apply (idempotent)
  optimize_pdi_leaves.py --check      read-only: every region is in place and is what this tool writes
  optimize_pdi_leaves.py --restore    put the lifted bodies back
  optimize_pdi_leaves.py --pin        print the SHA-256 of the lifted bodies (to review and paste into LEAVES)
  optimize_pdi_leaves.py --fixture D  write D/pdi_leaves_cases.inc for tools/tests/test_pdi_leaves.c

Each function below is a loop or a straight run with no channel access, no DMA, no guest call and no write to the
float control: registers r5 and up are held in locals for the length of the call and written back before every
way out (SPU_RET, SPU_DIRECT), r0-r4 stay in the context because the local-store diagnostics read them, and the
float operations are the forms the helpers take in the SPU float mode, chosen once per call instead of once per
operation. The lifted body stays behind it, byte for byte, and runs whenever the fast path is not taken:
  - GT6_NATIVE_PDI_LEAVES is not exactly "1" (the launcher's opt-in for native PDI leaves), or
  - GT6_PDI_LEAVES2=0 (or the port cleared g_gt6_pdi_leaves2, GT6_AB "vleaves"), or
  - the thread is not in the SPU float mode (spu__x8_rz()), or the build has no AVX2 + FMA.
A body whose lifted text is not the reviewed one is refused; nothing is written then.
"""
from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
MODULE = "jobmod_9039b3cc346b_e0030"
SOURCE = ROOT / "spu_jobs" / MODULE / "spu_recomp.c"
PREFIX = MODULE + "_spu_func_"

# address -> SHA-256 of the lifted body (LF line ends), as reviewed on 2026-11-18
# (00004124 and 00008EF8 pass the same test but gain nothing, 0.98x and 1.00x: short integer loops. Left lifted.)
LEAVES = {
    "0000C438": "7d38c04f00dd4504c9b2232e0aeae5a4f38e4465287e2d1d1bbc88b5b15a5eb1",
    "0000C578": "9c84a8a9705d6c6b32a083d3458874c9f9d272eb271597812320f60bf65ca6d1",
    "0000C8C8": "520571e8f5e830d6d328be22553f4e12f368e2c737c0e2e54472a1f6cd427e58",
    "00011E18": "a8df5c8cdce07d17faf4fa41260d2783c73f7eaf098318fa6f8859b30669d6f1",
    "00012910": "e4f7278e39d6d44e2f6265d15d8c228819ffe3d28c7aab9f807d7841c25c8405",
    "00013E88": "10df90a1022ae85b788bcc6b076e79529ad88cae98340ee68c9e8436e82f094b",
}

PRELUDE_BEGIN = "/* GT6_PDI_LEAVES2_PRELUDE_BEGIN */\n"
PRELUDE_END = "/* GT6_PDI_LEAVES2_PRELUDE_END */\n"
PRELUDE = PRELUDE_BEGIN + r'''/* tools/optimize_pdi_leaves.py. g_gt6_pdi_leaves2: -1 = not read yet, 0 = the lifted bodies, 1 = the fast paths.
 * It is read at every call, so the port may change it between two calls (GT6_AB "vleaves"). */
#include <stdlib.h>
#include <string.h>
#if defined(_MSC_VER)
__declspec(selectany) int g_gt6_pdi_leaves2 = -1;
#else
__attribute__((weak)) int g_gt6_pdi_leaves2 = -1;
#endif
static inline int gt6_pdi_leaves2_enabled(void)
{
    int v = g_gt6_pdi_leaves2;
    if (__builtin_expect(v < 0, 0)) {
        const char* native = getenv("GT6_NATIVE_PDI_LEAVES");
        const char* off = getenv("GT6_PDI_LEAVES2");
        v = native && native[0] == '1' && native[1] == '\0' && !(off && off[0] == '0' && off[1] == '\0');
        g_gt6_pdi_leaves2 = v;
    }
    return v;
}
#if defined(__AVX2__) && defined(__FMA__) && !defined(SPU_SCALAR_HELPERS)
/* spu_csflt / spu_cuflt where the SPU float mode is known (the caller tested spu__x8_rz() and nothing in a leaf
 * changes it): the same expressions as in spu_helpers.h [GT6 0052], without asking the mode again. */
#if !defined(SPU_CXFLT_EXACT_ONLY)
#define GT6_PDI_CXFLT_RANGE(i8) (__builtin_constant_p(i8) && ((i8) & 0xFF) >= 29 && ((i8) & 0xFF) <= 250)
#else
#define GT6_PDI_CXFLT_RANGE(i8) 0
#endif
SPU__XE_INLINE u128 gt6_pdi_csflt_rz(u128 a, int i8)
{
    if (GT6_PDI_CXFLT_RANGE(i8))
        return spu__xe_st(_mm_castps_si128(_mm_mul_ps(_mm_cvtepi32_ps(spu__xe_ld(a)), spu__xe_pow2((i8 & 0xFF) - 155))));
    return spu_csflt(a, i8);
}
SPU__XE_INLINE u128 gt6_pdi_cuflt_rz(u128 a, int i8)
{
    if (GT6_PDI_CXFLT_RANGE(i8)) {
        const __m128i v = spu__xe_ld(a);
        const __m128 hi = _mm_cvtepi32_ps(_mm_srli_epi32(v, 16)), lo = _mm_cvtepi32_ps(_mm_and_si128(v, _mm_set1_epi32(0xFFFF)));
        return spu__xe_st(_mm_castps_si128(_mm_mul_ps(_mm_fmadd_ps(hi, _mm_set1_ps(65536.0f), lo), spu__xe_pow2((i8 & 0xFF) - 155))));
    }
    return spu_cuflt(a, i8);
}
#endif
''' + PRELUDE_END

FORBIDDEN = ("spu_rdch", "spu_wrch", "spu_rchcnt", "spu_drain_call", "spu_indirect_branch", "g_spu_trampoline_fn",
             "spu_stop", "host_depth", "SPU_DRAIN", "spu_img_restore", "int_enable", "SPU_INDIRECT", "SPU_CALL")
# helpers that ask the float mode for every call, and the forms they take in it
FLOAT = {
    "fm": "spu__xr_fm({})",
    "fma": "spu__xr_fma({}, 0, 0)",
    "fms": "spu__xr_fma({}, 0, 0x80000000u)",
    "fnms": "spu__xr_fma({}, 0x80000000u, 0)",
    "fa": "spu__xr_fa({}, 0)",
    "fs": "spu__xr_fa({}, 0x80000000u)",
    "csflt": "gt6_pdi_csflt_rz({})",
    "cuflt": "gt6_pdi_cuflt_rz({})",
}
FLOAT_RE = re.compile(r"\bspu_(" + "|".join(FLOAT) + r")\(([^()]*)\)")


def sha256(text: str) -> str:
    return hashlib.sha256(text.encode()).hexdigest()


def name(addr: str) -> str:
    return PREFIX + addr


def begin(addr: str) -> str:
    return f"/* GT6_PDI_LEAF_{addr}_BEGIN */\n"


def end(addr: str) -> str:
    return f"/* GT6_PDI_LEAF_{addr}_END */\n"


def fallback(addr: str) -> str:
    return f"    /* GT6_PDI_LEAF_{addr}_ORIGINAL_FALLBACK */\n"


def verify_original(addr: str, body: str) -> None:
    if not LEAVES[addr] or sha256(body) != LEAVES[addr]:
        raise ValueError(f"{addr}: the lifted body is not the reviewed one; no source written")


def registers(inner: str) -> tuple[list[int], list[int]]:
    regs = sorted({int(x) for x in re.findall(r"ctx->gpr\[(\d+)\]", inner) if int(x) >= 5})
    writes = sorted({int(x) for x in re.findall(r"ctx->gpr\[(\d+)\]\s*=(?!=)", inner) if int(x) >= 5})
    return regs, writes


def flush_text(writes: list[int]) -> str:
    return " ".join(f"ctx->gpr[{r}] = gpr_{r};" for r in writes)


def fast_body(addr: str, inner: str, budget: bool = False) -> tuple[str, str]:
    """The fast path's statements and the declarations of its locals."""
    if any(token in inner for token in FORBIDDEN):
        raise ValueError(f"{addr} is not a pure leaf any more")
    if "SPU_RET(ctx);" not in inner and "SPU_DIRECT(ctx," not in inner:
        raise ValueError(f"{addr}: no way out found")
    regs, writes = registers(inner)
    fast = re.sub(r"ctx->gpr\[(\d+)\]", lambda m: f"gpr_{m[1]}" if int(m[1]) >= 5 else m[0], inner)
    fast = re.sub(r"\bloc_([0-9A-F]+)\b", r"pdi_rz_loc_\1", fast)
    fast, count = FLOAT_RE.subn(lambda m: FLOAT[m[1]].format(m[2]), fast)
    if FLOAT_RE.search(fast) or re.search(r"\bspu_(?:" + "|".join(FLOAT) + r")\(", fast):
        raise ValueError(f"{addr}: a float operation was left in its mode-asking form")
    expected = len(re.findall(r"\bspu_(?:" + "|".join(FLOAT) + r")\(", inner))
    if count != expected:
        raise ValueError(f"{addr}: {expected} float operations, {count} rewritten")
    flush = flush_text(writes)
    ways_out = fast.count("SPU_RET(ctx);") + fast.count("SPU_DIRECT(ctx,")
    if flush:
        fast = fast.replace("SPU_RET(ctx);", flush + " SPU_RET(ctx);").replace("SPU_DIRECT(ctx,", flush + " SPU_DIRECT(ctx,")
    if fast.count("SPU_RET(ctx);") + fast.count("SPU_DIRECT(ctx,") != ways_out:
        raise ValueError(f"{addr}: ways out changed")
    if budget:
        fast = add_budget(fast, flush)
    init = "".join(f"        u128 gpr_{r} = ctx->gpr[{r}];\n" for r in regs)
    return init, fast


def add_budget(text: str, flush: str) -> str:
    """Test only: leave after a given number of loop passes, the registers written back."""
    out, n = re.subn(r"^(\s*)(if \([^\n]*\) goto (?:pdi_rz_)?loc_[0-9A-F]+;)$",
                     lambda m: f"{m[1]}if (--test_budget == 0) {{ {flush} SPU_RET(ctx); }}\n{m[1]}{m[2]}", text, flags=re.M)
    return out


def transform_body(addr: str, body: str, budget: bool = False) -> str:
    verify_original(addr, body)
    start = body.index("{") + 1
    inner = body[start:body.rfind("}")]
    init, fast = fast_body(addr, inner, budget)
    tail = body[start:]
    if budget:
        tail = add_budget(tail, "")
    return body[:start] + f'''
#if defined(__AVX2__) && defined(__FMA__) && !defined(SPU_SCALAR_HELPERS)
    /* tools/optimize_pdi_leaves.py: no channel, DMA, guest call or float-control write in this leaf. r5 and up
     * are locals until a way out, r0-r4 stay in the context (the LS diagnostics read them), the float operations
     * are the SPU-float-mode forms. The lifted body below is the same program. */
    if (gt6_pdi_leaves2_enabled() && spu__x8_rz()) {{
''' + init + fast + '''    }
#endif
''' + fallback(addr) + tail


def region(addr: str, body: str) -> str:
    return begin(addr) + transform_body(addr, body) + end(addr)


def locate(text: str, addr: str) -> tuple[int, int, str, bool]:
    b, e, f = begin(addr), end(addr), fallback(addr)
    if b in text:
        if text.count(b) != 1 or text.count(e) != 1:
            raise ValueError(f"{addr}: ambiguous generated region")
        start = text.index(b)
        stop = text.index(e, start) + len(e)
        current = text[start:stop]
        body = "void " + name(addr) + "(spu_context* ctx) {" + current[current.index(f) + len(f):-len(e)]
        verify_original(addr, body)
        if current != region(addr, body):
            raise ValueError(f"{addr}: the generated region was changed; no source written")
        return start, stop, body, True
    start = text.index("void " + name(addr) + "(spu_context* ctx) {")
    stop = text.index("\nvoid ", start + 1) + 1
    return start, stop, text[start:stop], False


def lifted_body(text: str, addr: str) -> str:
    return locate(text, addr)[2]


def strip_prelude(text: str) -> str:
    if PRELUDE_BEGIN not in text:
        return text
    if text.count(PRELUDE_BEGIN) != 1 or text.count(PRELUDE_END) != 1:
        raise ValueError("ambiguous prelude")
    a = text.index(PRELUDE_BEGIN)
    b = text.index(PRELUDE_END, a) + len(PRELUDE_END)
    if text[a:b] != PRELUDE:
        raise ValueError("the prelude was changed; no source written")
    return text[:a] + text[b:]


def rewrite(text: str, restore: bool) -> tuple[str, int]:
    text = strip_prelude(text)
    applied = 0
    for addr in sorted(LEAVES):
        start, stop, body, was = locate(text, addr)
        verify_original(addr, body)
        applied += was
        text = text[:start] + (body if restore else region(addr, body)) + text[stop:]
    if not restore:
        first = min(text.index(begin(addr)) for addr in LEAVES)
        text = text[:first] + PRELUDE + text[first:]
    return text, applied


def fixture(text: str, out: Path) -> None:
    """Both forms of every leaf as functions with a pass budget, for the differential test."""
    out.mkdir(parents=True, exist_ok=True)
    parts = []
    table = []
    for addr in sorted(LEAVES):
        body = lifted_body(strip_prelude(text), addr)
        verify_original(addr, body)
        start = body.index("{") + 1
        original = body[:start] + add_budget(body[start:], "")
        native = transform_body(addr, body, budget=True)
        for label, src in (("original_", original), ("native_", native)):
            # the same signature as the lifted function: SPU_DIRECT is a musttail call. The pass budget is a
            # variable of the test (test_budget), not a parameter.
            parts.append(src.replace("void " + name(addr) + "(spu_context* ctx)",
                                     "__declspec(noinline) void " + label + addr + "(spu_context* ctx)", 1))
        regs, writes = registers(body[start:body.rfind("}")])
        counter = re.findall(r"if \(ctx->gpr\[(\d+)\]\._u32\[0\] != 0\) goto loc_", body)
        targets = sorted(set(re.findall(r"SPU_DIRECT\(ctx, 0x[0-9A-Fa-f]+, (\w+)\)", body)))
        table.append((addr, counter[0] if counter else "-1", targets, len(regs), len(writes)))
    stubs = sorted({t for row in table for t in row[2]})
    head = PRELUDE + "".join(f"void {t}(spu_context* ctx) {{ stub_call(ctx, 0x{t[-8:]}u); }}\n" for t in stubs)
    parts.insert(0, head)
    parts.append("static const struct { const char* name; void (*original)(spu_context*); "
                 "void (*native)(spu_context*); int counter; } leaf_cases[] = {\n" +
                 "".join(f'    {{ "{a}", original_{a}, native_{a}, {c} }},\n' for a, c, _, _, _ in table) + "};\n")
    (out / "pdi_leaves_cases.inc").write_text("\n".join(parts))
    for a, c, t, nr, nw in table:
        print(f"{a}: counter r{c}, {nr} locals, {nw} written back, direct targets {', '.join(x[-8:] for x in t) or 'none'}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    action = parser.add_mutually_exclusive_group()
    action.add_argument("--check", action="store_true")
    action.add_argument("--restore", action="store_true")
    action.add_argument("--pin", action="store_true")
    action.add_argument("--fixture", type=Path)
    args = parser.parse_args()
    raw = SOURCE.read_bytes()
    text = raw.decode("utf-8").replace("\r\n", "\n")
    if args.pin:
        plain = strip_prelude(text)
        for addr in sorted(LEAVES):
            if begin(addr) in plain:
                raise ValueError(f"{addr} is already transformed: --restore first")
            start = plain.index("void " + name(addr) + "(spu_context* ctx) {")
            body = plain[start:plain.index("\nvoid ", start + 1) + 1]
            print(f'    "{addr}": "{sha256(body)}",')
        return
    if args.fixture:
        fixture(text, args.fixture)
        return
    output, applied = rewrite(text, args.restore)
    changed = output != text
    if args.check:
        if applied != len(LEAVES) or changed:
            raise ValueError(f"{applied} of {len(LEAVES)} leaves are in place" + (", and the file is not what this tool writes" if changed else ""))
        print(f"check: {applied} leaves in place, the file is what this tool writes")
        return
    if changed:
        newline = "\r\n" if b"\r\n" in raw else "\n"
        SOURCE.write_bytes(output.replace("\n", newline).encode())
    print(("restored" if args.restore else "applied") + f": {len(LEAVES)} leaves, file {'written' if changed else 'unchanged'}")


if __name__ == "__main__":
    main()
