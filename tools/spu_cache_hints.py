#!/usr/bin/env python3
"""Take what an SPU program cache holds into the port's own tree, once.

An "spu-safe-v1-tane.dat" cache lists every piece of SPU code that was seen to run in earlier sessions of the
game: record = BE u16 checksum, BE u16 n, BE u32 local-store address, n BE instruction words. The code itself is
the game's (the job modules sit in EMAIN.ELF, the SPURS kernel in libsre); what the cache adds is WHERE execution
entered it -- function starts reached only through indirect branches, which no static scan finds.

Writes into --out (default spu_jobs/):
  entry_hints.json     {"<job module name>": ["0x....", ...]}  for  lift_jobmods.py --entry-hints
  low_entries.json     {"0x....": "<hex of the code>"}  pieces below LS 0x4000 (SPURS kernel, policy module)
  cache_coverage.txt   what every record is covered by, and what is not covered
After this has run, the cache files are not needed again: everything taken from them is in those files.

usage: python tools/spu_cache_hints.py <cache.dat> [<cache.dat> ...] [--elf game/EMAIN.ELF] [--extra game/firmware/libsre.prx]
"""
import argparse, collections, hashlib, json, os, struct, sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.join(ROOT, "ps3recomp", "tools"))
from lift_jobmods import find_engine_modules, BASE      # noqa: E402


def read_cache(path):
    b = open(path, "rb").read()
    o, out = 0, []
    while o + 8 <= len(b):
        _crc, n, addr = struct.unpack_from(">HHI", b, o)
        if n == 0 or addr >= 0x40000 or addr & 3 or o + 8 + 4 * n > len(b):
            sys.exit(f"{path}: not an spu-safe-v1 cache (record at 0x{o:X})")
        out.append((addr, b[o + 8:o + 8 + 4 * n]))
        o += 8 + 4 * n
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("caches", nargs="+")
    from game_paths import GAME
    ap.add_argument("--elf", default=str(GAME / 'EMAIN.ELF'))
    ap.add_argument("--extra", action="append", default=[],
                    help="other files of the game or firmware to look for the low pieces in (libsre.prx)")
    ap.add_argument("--out", default=os.path.join(ROOT, "spu_jobs"))
    args = ap.parse_args()

    recs = {}
    for path in args.caches:
        for addr, data in read_cache(path):
            recs[(addr, data)] = True
    recs = sorted(recs)
    elf = open(args.elf, "rb").read()
    mods = []
    for ea, raw in find_engine_modules(elf, True):
        entry_off = struct.unpack_from(">I", raw, 0x10)[0]
        mods.append((f"jobmod_{hashlib.sha1(raw).hexdigest()[:12]}_e{entry_off:04X}", ea, raw))

    hints = collections.defaultdict(set)
    per_mod = collections.Counter()
    n_job = n_low = n_low_found = 0
    low, unmatched = {}, []
    extra = [(os.path.basename(p), open(p, "rb").read()) for p in args.extra]
    low_where = collections.Counter()
    for addr, data in recs:
        if addr >= BASE + 0x30:
            hit = False
            off = addr - BASE
            for name, _ea, raw in mods:
                if raw[off:off + len(data)] == data:
                    hints[name].add(addr)
                    per_mod[name] += 1
                    hit = True
            if hit:
                n_job += 1
            else:
                unmatched.append((addr, data))
        else:
            n_low += 1
            low[f"0x{addr:05X}"] = data.hex()
            where = None
            if len(data) >= 16:
                i = elf.find(data)
                if i >= 0:
                    where = "EMAIN.ELF"
                    low_where[("EMAIN.ELF", i - addr)] += 1
                else:
                    for nm, blob in extra:
                        i = blob.find(data)
                        if i >= 0:
                            where = nm
                            low_where[(nm, i - addr)] += 1
                            break
            n_low_found += where is not None

    os.makedirs(args.out, exist_ok=True)
    with open(os.path.join(args.out, "entry_hints.json"), "w") as f:
        json.dump({k: [f"0x{a:X}" for a in sorted(v)] for k, v in sorted(hints.items())}, f, indent=1)
    with open(os.path.join(args.out, "low_entries.json"), "w") as f:
        json.dump(low, f, indent=0)

    L = []
    L.append(f"cache files: {len(args.caches)}; distinct records: {len(recs)} "
             f"({sum(len(d) // 4 for _, d in recs)} instruction words)")
    L.append(f"job modules in {os.path.basename(args.elf)}: {len(mods)}")
    L.append(f"records at LS >= 0x{BASE + 0x30:X}: {n_job + len(unmatched)}; found byte for byte in a job module: "
             f"{n_job}; in none: {len(unmatched)}")
    L.append(f"records below LS 0x{BASE:X}: {n_low}; of the ones with 4+ instructions found in the game or firmware "
             f"files: {n_low_found}")
    L.append("")
    L.append("where the low pieces are (file, file offset of LS 0: pieces):")
    for (nm, base), c in sorted(low_where.items(), key=lambda kv: -kv[1])[:12]:
        L.append(f"  {nm} 0x{base:X}: {c}")
    L.append("")
    L.append("job modules (name, EA, image bytes, entries from the cache):")
    for name, ea, raw in mods:
        L.append(f"  {name}  0x{ea:08X}  0x{len(raw):05X}  {per_mod[name]}")
    L.append("")
    L.append(f"modules no cache record names (never seen to run): "
             f"{sum(1 for name, _, _ in mods if not per_mod[name])}")
    L.append("")
    L.append("records in no job module (LS address, instructions, first words):")
    for addr, data in unmatched[:400]:
        L.append(f"  0x{addr:05X} {len(data) // 4:4d}  {data[:16].hex()}")
    if len(unmatched) > 400:
        L.append(f"  ... {len(unmatched) - 400} more")
    with open(os.path.join(args.out, "cache_coverage.txt"), "w") as f:
        f.write("\n".join(L) + "\n")
    print("\n".join(L[:12]))
    print(f"wrote {args.out}\\entry_hints.json ({sum(len(v) for v in hints.values())} entries for {len(hints)} modules), "
          f"low_entries.json ({len(low)}), cache_coverage.txt")


if __name__ == "__main__":
    main()
