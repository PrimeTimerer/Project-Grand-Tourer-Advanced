#!/usr/bin/env python3
"""[GT6 0171] Put the call of the native local-store fill (gt6_native_fill.c) at the top of 0x4124 in every lifted
job module whose 0x4124 is the job CRT's fill loop.

  native_fill_hook.py            apply (idempotent)
  native_fill_hook.py --check    read-only: how many modules have it
  native_fill_hook.py --restore  take it out again

A module counts only when the lifted body of its 0x4124 (from the label to the closing brace, the module's own name
written as "@") has the SHA-256 below - the one reviewed for gt6_native_fill.c; nothing is written otherwise. The
lifted code itself is not in this file. spu_jobs/*/spu_recomp.c are generated (ps3recomp/tools/lift_jobmods.py);
after a new lift run this again. The lifted loop stays behind the call: gt6_native_fill_4124 returns 0 whenever it
is to run.
"""
from __future__ import annotations

import hashlib
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BODY_SHA256 = "f0410c25d474e62925c541d38dc261eccf0b4a4424ae96336b599d31c66c7085"
DECL = ("/* GT6_NATIVE_FILL ([GT6 0171], gt6_native_fill.c, tools/native_fill_hook.py) */\n"
        "int gt6_native_fill_4124(spu_context* ctx);\n")
CALL = "        if (gt6_native_fill_4124(ctx)) { SPU_DIRECT(ctx, 0x407C, @_spu_func_0000407C); }   /* GT6_NATIVE_FILL */\n"


def body_ok(text: str, start: int, mod: str) -> bool:
    end = text.find("\n}\n", start)
    return end >= 0 and hashlib.sha256(text[start:end + 3].replace(mod, "@").encode()).hexdigest() == BODY_SHA256


def main() -> int:
    arg = sys.argv[1] if len(sys.argv) > 1 else ""
    changed = hooked = other = 0
    for src in sorted((ROOT / "spu_jobs").glob("*/spu_recomp.c")):
        raw = src.read_text(newline="")
        crlf = "\r\n" in raw
        text = raw.replace("\r\n", "\n")
        mod = src.parent.name
        head = f"void {mod}_spu_func_00004124(spu_context* ctx) {{\n"
        call = CALL.replace("@", mod)
        if text.count(head) != 1:
            other += 1
            continue
        at = text.index(head) + len(head)
        is_hooked = text.startswith(call, at) and text[:text.index(head)].endswith(DECL)
        start = at + len(call) if is_hooked else at
        if not body_ok(text, start, mod):
            other += 1
            continue
        new = None
        if is_hooked:
            hooked += 1
            if arg == "--restore":
                new = text.replace(DECL + head + call, head)
        elif arg in ("", "--apply"):
            new = text.replace(head, DECL + head + call)
        if new is not None:
            src.write_text(new.replace("\n", "\r\n") if crlf else new, newline="")
            changed += 1
    if arg == "--check":
        print(f"hooked {hooked}; modules without the reviewed loop {other}")
    else:
        print(f"{'restored' if arg == '--restore' else 'applied'} {changed}; hooked before {hooked}; modules without the reviewed loop {other}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
