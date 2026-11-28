#!/usr/bin/env python3
"""[GT6 0174] Input of tools/tests/test_native_a8dec1.c, from the lifted module a8dec1 (hooked or not):
  - the lifted functions the fused loop is made of, as they are, renamed orig_<pc> (their transfers and calls too;
    the loop's exit orig_00006840 is the test's own);
  - the fused function and its block list exactly as tools/native_a8dec1_hook.py generates them.

  python tools/tests/prepare_native_a8dec1_fixture.py <dir>   -> <dir>/native_a8dec1_case.inc and <dir>/mod_a8dec1.bin
  (the module's own bytes from the game's EMAIN.ELF, for the live-lane form's constants; never put either in git)
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import native_a8dec1_hook as hook  # noqa: E402


def main() -> int:
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    text = (ROOT / "spu_jobs" / hook.MOD / "spu_recomp.c").read_text().replace("\r\n", "\n")
    if hook.BEGIN in text:
        a, b = text.index(hook.BEGIN), text.index(hook.END) + len(hook.END)
        text = (text[:a] + text[b:]).replace(hook.HOOK.replace("@", hook.MOD), "", 1)
    funcs = hook.functions(text, hook.MOD)
    assert hook.body_hash(funcs, hook.MOD, "") == hook.BODY_SHA256, "not the reviewed lifted loop"
    pcs = sorted(hook.fused_set(funcs))
    pre = hook.MOD + "_spu_func_"
    parts = ["void orig_%08X(spu_context* ctx);\n" % pc for pc in pcs + [hook.EXIT]]
    for pc in pcs:
        parts.append("void orig_%08X(spu_context* ctx) {\n%s}\n" % (pc, funcs[pc][2].replace(pre, "orig_")))
    parts.append(hook.generate(funcs, hook.MOD))
    body = "".join(parts)
    assert "jobmod_" not in body
    (out / "native_a8dec1_case.inc").write_text(body)
    raw = hook.module_bytes(hook.MOD)
    assert raw, "the module's bytes (EMAIN.ELF of game.path) are needed for the live-lane form's constants"
    (out / "mod_a8dec1.bin").write_bytes(raw)
    print("%d lifted functions, %d lines" % (len(pcs), body.count("\n")))
    return 0


if __name__ == "__main__":
    sys.exit(main())
