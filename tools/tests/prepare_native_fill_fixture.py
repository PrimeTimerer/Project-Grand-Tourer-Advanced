#!/usr/bin/env python3
"""[GT6 0171] Input of tools/tests/test_native_fill.c: the lifted 0x4124 loop of one job module (the body
tools/native_fill_hook.py reviews), renamed orig_4124, its exit renamed orig_407C, without the native call.

  python tools/tests/prepare_native_fill_fixture.py <dir>   -> <dir>/native_fill_case.inc (never put it in git)
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MOD = "jobmod_bdc6f9d52851_e0030"


def main() -> int:
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    text = (ROOT / "spu_jobs" / MOD / "spu_recomp.c").read_text().replace("\r\n", "\n")
    a = text.index("loc_00004124:\n")
    b = text.index("\n}\n", a) + 3
    body = text[a:b].replace(MOD + "_spu_func_0000407C", "orig_407C")
    assert "jobmod_" not in body and "gt6_native" not in body
    (out / "native_fill_case.inc").write_text("void orig_4124(spu_context* ctx) {\n" + body)
    print("orig_4124: %d lines" % body.count("\n"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
