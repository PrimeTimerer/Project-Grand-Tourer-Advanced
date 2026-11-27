#!/usr/bin/env python3
"""[GT6 0168] Inputs of tools/tests/test_native_cull.c: the lifted 0x13D20 chain (13D20, 13D50, 13E80, 13E88 with its
PDI-leaf fast path and that tool's prelude) renamed orig_*, without the native call, and the module's bytes.

  python tools/tests/prepare_native_cull_fixture.py <dir>
  -> <dir>/native_cull_cases.inc, <dir>/mod9039.bin (from the game's EMAIN.ELF of game.path, read only; never put it
     in git)
"""
import hashlib
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "spu_jobs" / "jobmod_9039b3cc346b_e0030" / "spu_recomp.c"
PFX = "jobmod_9039b3cc346b_e0030_spu_func_000"
FUNCS = ["13D20", "13D50", "13E80", "13E88"]


def main() -> int:
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    text = SOURCE.read_text().replace("\r\n", "\n")
    p0 = text.index("/* GT6_PDI_LEAVES2_PRELUDE_BEGIN */")
    p1 = text.index("/* GT6_PDI_LEAVES2_PRELUDE_END */")
    prelude = text[p0:p1]
    a = text.index("void " + PFX + "13D20(spu_context* ctx) {")
    b = text.index("/* GT6_PDI_LEAF_00013E88_END */")
    body = text[a:b]
    body = body.replace("        if (gt6_native_cull_13d20(ctx)) { SPU_RET(ctx); }   /* GT6_NATIVE_CULL */\n", "")
    assert "gt6_native_cull" not in body
    for f in FUNCS:
        body = body.replace(PFX + f, "orig_" + f)
    left = sorted(set(re.findall(PFX + r"[0-9A-F]+", body)))
    assert not left, left
    protos = "".join("void orig_%s(spu_context* ctx);\n" % f for f in FUNCS)
    (out / "native_cull_cases.inc").write_text(prelude + protos + body)
    sys.path.insert(0, str(ROOT / "ps3recomp" / "tools"))
    sys.path.insert(0, str(ROOT / "tools"))
    from lift_jobmods import find_engine_modules
    from game_paths import GAME
    elf = (GAME / "EMAIN.ELF").read_bytes()
    raw = next(r for _, r in find_engine_modules(elf, True) if hashlib.sha1(r).hexdigest()[:12] == "9039b3cc346b")
    (out / "mod9039.bin").write_bytes(raw)
    print("orig_%s: %d lines; module %d bytes" % ("/".join(FUNCS), body.count("\n"), len(raw)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
