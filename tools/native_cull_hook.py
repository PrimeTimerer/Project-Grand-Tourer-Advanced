#!/usr/bin/env python3
"""[GT6 0168] Put the call of the native cull (gt6_native_cull.c) at the top of the car vertex job's 0x13D20.

  native_cull_hook.py            apply (idempotent)
  native_cull_hook.py --check    read-only: the call is in place, once
  native_cull_hook.py --restore  take it out again

spu_jobs/jobmod_9039b3cc346b_e0030/spu_recomp.c is generated (ps3recomp/tools/lift_jobmods.py); after a new lift run
this again. The lifted body of 0x13D20 stays as it is behind the call: gt6_native_cull_13d20 returns 0 whenever the
lifted code is to run, 1 after doing the whole function (then SPU_RET, as the lifted chain ends).
"""
from __future__ import annotations

import sys
from pathlib import Path

SOURCE = Path(__file__).resolve().parents[1] / "spu_jobs" / "jobmod_9039b3cc346b_e0030" / "spu_recomp.c"
HEAD = "void jobmod_9039b3cc346b_e0030_spu_func_00013D20(spu_context* ctx) {\n"
DECL = ("/* GT6_NATIVE_CULL ([GT6 0168], gt6_native_cull.c, tools/native_cull_hook.py) */\n"
        "int gt6_native_cull_13d20(spu_context* ctx);\n")
CALL = "        if (gt6_native_cull_13d20(ctx)) { SPU_RET(ctx); }   /* GT6_NATIVE_CULL */\n"


def main() -> int:
    text = SOURCE.read_text(newline="")
    nl = "\r\n" if "\r\n" in text else "\n"
    head, decl, call = (s.replace("\n", nl) for s in (HEAD, DECL, CALL))
    if text.count(head) != 1:
        print("0x13D20 not found once in", SOURCE)
        return 1
    applied = (decl + head + call) in text
    arg = sys.argv[1] if len(sys.argv) > 1 else ""
    if arg == "--check":
        print("in place" if applied and text.count(call) == 1 else "NOT in place")
        return 0 if applied and text.count(call) == 1 else 1
    if arg == "--restore":
        if applied:
            SOURCE.write_text(text.replace(decl + head + call, head), newline="")
        print("restored")
        return 0
    if not applied:
        if call in text or decl in text:
            print("a partial hook is there; --restore first")
            return 1
        SOURCE.write_text(text.replace(head, decl + head + call), newline="")
    print("applied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
