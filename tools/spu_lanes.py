#!/usr/bin/env python3
"""[GT6 0179] Lane liveness of generated SPU code (the fused loops of tools/native_*_hook.py), for dropping the exact
fix-ups of float lanes no one reads.

SPU scalar code keeps its values in word 0 of a register; the other three words carry whatever the instructions put
there - often values of biased exponent 255 that the host reads as Inf / NaN, so every float op on such a register
takes the helpers' exact per-lane fix-up (spu_helpers.h). This module finds, for every float op of a straight-line /
goto program over SPU registers held in locals r0 .. r127, which words of its result can reach anything observable:
a local-store store (all 16 bytes), a branch condition (word 0), or the registers at the program's exit (all of them,
all words). A float op whose result reaches nothing is dropped; one with some dead words gets the live-lane form
(the host's native op; the exact helper only when a LIVE word leaves the normal range); the rest keep the exact
helper. Words flow between lanes only through shufb / rotqby(i) / shlqbyi / gb / cwd / cbd / addresses, and those are
followed byte by byte where the control is a known constant: immediates (il / ila / ilhu / iohl), quadwords the code
loads from fixed local-store addresses of the module (the caller checks them at run time), and insertion controls
(cwd / cbd) of registers whose address is known modulo 16 (r1, the stack pointer, from the gate's alignment check).
An unknown control makes both of its sources fully live.
"""
from __future__ import annotations

import re

LABEL_RE = re.compile(r"^(\w+):;$")
GOTO_RE = re.compile(r"^goto (\w+);$")
IFGOTO_RE = re.compile(r"^if \((.*)\) goto (\w+);$")
ASSIGN_RE = re.compile(r"^r(\d+) = (spu_\w+)\((.*)\);$")
STORE_RE = re.compile(r"^spu_ls_write128\(ctx, (.*), r(\d+)\);$")
REG_RE = re.compile(r"\br(\d+)\b")
FLOAT_OPS = {"spu_fm", "spu_fa", "spu_fs", "spu_fma", "spu_fms", "spu_fnms"}
LANEWISE = {"spu_fm", "spu_fa", "spu_fs", "spu_fma", "spu_fms", "spu_fnms", "spu_fcgt", "spu_fcmgt", "spu_fceq",
            "spu_ceq", "spu_ceqi", "spu_cgt", "spu_cgti", "spu_clgt", "spu_clgti", "spu_ceqb", "spu_ceqbi",
            "spu_and", "spu_andi", "spu_or", "spu_ori", "spu_xor", "spu_xori", "spu_nor", "spu_nand", "spu_andc",
            "spu_orc", "spu_a", "spu_ai", "spu_sf", "spu_sfi", "spu_selb", "spu_cflts", "spu_cfltu", "spu_csflt",
            "spu_cuflt", "spu_shli", "spu_rotmi", "spu_rotmai", "spu_roti", "spu_iohl", "spu_xsbh", "spu_xshw",
            "spu_xswd", "spu_mpyu", "spu_mpyh", "spu_mpyhhu", "spu_mpy", "spu_ah", "spu_ahi"}
NO_SOURCE = {"spu_il", "spu_ila", "spu_ilh", "spu_ilhu", "spu_link", "spu_fsmbi"}
ALL = 0xF
NREG = 128


def split_args(s: str) -> list[str]:
    out, depth, cur = [], 0, ""
    for ch in s:
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur.strip())
            cur = ""
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


def num(s: str) -> int | None:
    try:
        return int(s, 0)
    except ValueError:
        return None


class Stmt:
    def __init__(self, text: str):
        self.text = text
        self.kind = "other"
        self.label = self.target = None
        self.dst = None
        self.op = None
        self.args: list[str] = []
        self.uses0: list[int] = []      # registers whose word 0 is read (conditions, addresses)
        m = LABEL_RE.match(text)
        if m:
            self.kind, self.label = "label", m.group(1)
            return
        m = GOTO_RE.match(text)
        if m:
            self.kind, self.target = "goto", m.group(1)
            return
        m = IFGOTO_RE.match(text)
        if m:
            self.kind, self.target = "ifgoto", m.group(2)
            self.uses0 = [int(r) for r in REG_RE.findall(m.group(1))]
            return
        m = ASSIGN_RE.match(text)
        if m:
            self.kind, self.dst, self.op = "assign", int(m.group(1)), m.group(2)
            self.args = split_args(m.group(3))
            return
        m = STORE_RE.match(text)
        if m:
            self.kind = "store"
            self.val = int(m.group(2))
            self.uses0 = [int(r) for r in REG_RE.findall(m.group(1))]
            return
        raise SystemExit("spu_lanes: a statement it does not know: " + text)


def be_words(b: bytes) -> tuple:
    return tuple(int.from_bytes(b[4 * k:4 * k + 4], "big") for k in range(4))


def words_be(w: tuple) -> bytes:
    return b"".join((x & 0xFFFFFFFF).to_bytes(4, "big") for x in w)


def sext(v: int, bits: int) -> int:
    v &= (1 << bits) - 1
    return v - (1 << bits) if v >> (bits - 1) else v


class Program:
    """statements, the CFG, the forward constant / alignment facts and the backward lane liveness"""

    def __init__(self, lines: list[str], module: bytes | None, module_base: int = 0x4000, aligned_regs=(1,)):
        self.st = [Stmt(t) for t in lines]
        self.module, self.module_base = module, module_base
        self.labels = {s.label: i for i, s in enumerate(self.st) if s.kind == "label"}
        self.succ: list[list[int]] = []
        for i, s in enumerate(self.st):
            if s.kind == "goto":
                self.succ.append([self.labels[s.target]] if s.target in self.labels else [-1])
            elif s.kind == "ifgoto":
                t = self.labels[s.target] if s.target in self.labels else -1
                self.succ.append([t, i + 1] if i + 1 < len(self.st) else [t])
            else:
                self.succ.append([i + 1] if i + 1 < len(self.st) else [-1])
        self.pred: list[list[int]] = [[] for _ in self.st]
        for i, ss in enumerate(self.succ):
            for j in ss:
                if j >= 0:
                    self.pred[j].append(i)
        self.lsconst: dict[int, bytes] = {}
        self.entry_facts = {r: ("mod", 0) for r in aligned_regs}
        self.consts()
        self.liveness()

    # ---- forward: constants (16 bytes, SPU order) and addresses known modulo 16 (word 0) ----------------------------
    def evaluate(self, s: Stmt, env: dict) -> object:
        op, a = s.op, s.args

        def const(i):
            v = env.get(int(a[i][1:])) if a[i].startswith("r") else None
            return v[1] if v and v[0] == "const" else None

        def base(i):
            v = env.get(int(a[i][1:])) if a[i].startswith("r") else None
            if not v:
                return None
            return v[1] if v[0] == "mod" else be_words(v[1])[0] & 15
        if op == "spu_il":
            return ("const", words_be((sext(num(a[0]), 16),) * 4))
        if op == "spu_ila":
            return ("const", words_be(((num(a[0]) & 0x3FFFF),) * 4))
        if op == "spu_ilhu":
            return ("const", words_be((((num(a[0]) & 0xFFFF) << 16),) * 4))
        if op == "spu_iohl":
            c = const(0)
            return ("const", words_be(tuple(w | (num(a[1]) & 0xFFFF) for w in be_words(c)))) if c else None
        if op in ("spu_ori", "spu_shlqbyi") and num(a[1]) == 0:
            return env.get(int(a[0][1:]))
        if op == "spu_ai":
            v = env.get(int(a[0][1:]))
            if v and v[0] == "mod":
                return ("mod", (v[1] + num(a[1])) & 15)
            if v and v[0] == "const":
                return ("const", words_be(tuple(w + sext(num(a[1]), 10) for w in be_words(v[1]))))
            return None
        if op in ("spu_cwd", "spu_cbd"):
            b = base(0)
            if b is None:
                return None
            t = (b + num(a[1])) & (0xC if op == "spu_cwd" else 0xF)
            m = bytearray(range(0x10, 0x20))
            if op == "spu_cwd":
                m[t:t + 4] = bytes((0, 1, 2, 3))
            else:
                m[t] = 3
            return ("const", bytes(m))
        if op == "spu_ls_read128" and len(a) == 2 and num(a[1]) is not None and self.module is not None:
            addr = num(a[1]) & ~15
            off = addr - self.module_base
            if 0 <= off and off + 16 <= len(self.module):
                self.lsconst[addr] = self.module[off:off + 16]
                return ("const", self.module[off:off + 16])
        return None

    def consts(self) -> None:
        n = len(self.st)
        self.env_in: list[dict | None] = [None] * n
        self.env_in[0] = dict(self.entry_facts)
        work = list(range(n))
        while work:
            i = work.pop(0)
            env = self.env_in[i]
            if env is None:
                continue
            s = self.st[i]
            out = dict(env)
            if s.kind == "assign":
                v = self.evaluate(s, env)
                if v is None:
                    out.pop(s.dst, None)
                else:
                    out[s.dst] = v
            for j in self.succ[i]:
                if j < 0:
                    continue
                old = self.env_in[j]
                if old is None:
                    new = dict(out)
                else:
                    new = {r: v for r, v in old.items() if out.get(r) == v}
                if new != old:
                    self.env_in[j] = new
                    work.append(j)

    def control(self, i: int, reg_arg: str):
        env = self.env_in[i] or {}
        v = env.get(int(reg_arg[1:])) if reg_arg.startswith("r") else None
        return v[1] if v and v[0] == "const" else None

    # ---- backward: which words of which registers are read later ------------------------------------------------------
    def transfer(self, i: int, out: list) -> list:
        s = self.st[i]
        live = list(out)
        if s.kind in ("ifgoto",):
            for r in s.uses0:
                live[r] |= 1
            return live
        if s.kind == "store":
            live[s.val] = ALL
            for r in s.uses0:
                live[r] |= 1
            return live
        if s.kind != "assign":
            return live
        L = out[s.dst]
        live[s.dst] = 0
        regs = [int(x[1:]) for x in s.args if re.fullmatch(r"r\d+", x)]
        op = s.op
        if L == 0 and op != "spu_ls_read128":
            return live
        if op in NO_SOURCE:
            pass
        elif op in LANEWISE:
            for r in regs:
                live[r] |= L
        elif op == "spu_ls_read128":
            if L:
                for r in (int(x) for x in REG_RE.findall(", ".join(s.args[1:]))):
                    live[r] |= 1
        elif op in ("spu_cwd", "spu_cbd"):
            live[regs[0]] |= 1
        elif op == "spu_gb" or op == "spu_gbh" or op == "spu_gbb":
            if L & 1:
                live[regs[0]] = ALL
        elif op in ("spu_rotqbyi", "spu_shlqbyi", "spu_rotqby"):
            n = num(s.args[1]) if op != "spu_rotqby" else None
            src = regs[0]
            if n is None:
                live[src] = ALL
                if op == "spu_rotqby":
                    live[regs[1]] |= 1
            else:
                for w in range(4):
                    if L >> w & 1:
                        for j in range(4 * w, 4 * w + 4):
                            k = (j + n) & 15 if op == "spu_rotqbyi" else j + n
                            if k < 16:
                                live[src] |= 1 << (k >> 2)
        elif op == "spu_shufb":
            ra, rb, rc = (int(x[1:]) for x in s.args)
            ctl = self.control(i, s.args[2])
            live[rc] |= L
            if ctl is None:
                live[ra] = ALL
                live[rb] = ALL
            else:
                for w in range(4):
                    if L >> w & 1:
                        for j in range(4 * w, 4 * w + 4):
                            c = ctl[j]
                            if c & 0x80:
                                continue
                            c &= 0x1F
                            if c < 16:
                                live[ra] |= 1 << (c >> 2)
                            else:
                                live[rb] |= 1 << ((c - 16) >> 2)
        else:
            raise SystemExit("spu_lanes: no lane rule for " + op)
        return live

    def liveness(self) -> None:
        n = len(self.st)
        exit_live = [ALL] * NREG
        self.live_out: list[list] = [[0] * NREG for _ in range(n)]
        self.live_in: list[list] = [[0] * NREG for _ in range(n)]
        changed = True
        while changed:
            changed = False
            for i in range(n - 1, -1, -1):
                out = [0] * NREG
                for j in self.succ[i]:
                    src = exit_live if j < 0 else self.live_in[j]
                    for r in range(NREG):
                        out[r] |= src[r]
                inn = self.transfer(i, out)
                if out != self.live_out[i] or inn != self.live_in[i]:
                    self.live_out[i], self.live_in[i] = out, inn
                    changed = True

    # ---- the rewrite ---------------------------------------------------------------------------------------------------
    def rewrite(self, fixed_prefix: str) -> tuple[list[str], dict]:
        """the program with float ops by liveness; statements whose labels start with fixed_prefix keep the exact
        helpers (the copy that runs the last pass). Returns the lines and counts."""
        out, counts = [], {"dead": 0, "partial": 0, "full": 0}
        cur_fixed = False
        for i, s in enumerate(self.st):
            if s.kind == "label":
                cur_fixed = s.label.startswith(fixed_prefix)
            if s.kind == "assign" and s.op in FLOAT_OPS and not cur_fixed:
                L = self.live_out[i][s.dst]
                if L == 0:
                    out.append("/* dead: %s */" % s.text)
                    counts["dead"] += 1
                    continue
                if L != ALL:
                    extra = {"spu_fm": "", "spu_fa": ", 0u", "spu_fs": ", 0x80000000u", "spu_fma": ", 0u, 0u",
                             "spu_fms": ", 0u, 0x80000000u", "spu_fnms": ", 0x80000000u, 0u"}[s.op]
                    fn = {"spu_fm": "a8lv_fm", "spu_fa": "a8lv_fa", "spu_fs": "a8lv_fa"}.get(s.op, "a8lv_fma")
                    out.append("r%d = %s(%s%s, %d);" % (s.dst, fn, ", ".join(s.args), extra, L))
                    counts["partial"] += 1
                    continue
                counts["full"] += 1
            out.append(s.text)
        return out, counts
