"""Bind the byte-checked GT6 1.22 trainer sites to native C++ helpers.

The same guest address can be emitted in several tail-entry bodies. The frozen
manifest covers those copies too. All anchors are checked before any file write;
reapplication removes only our exact generated lines and must be byte-identical.
Every lifted file that holds lines of this tool is cleaned, also one none of the
listed sites is in any more (a shorter list leaves no stale bindings behind).
"""
from pathlib import Path
import argparse, hashlib, json, re, struct

ROOT = Path(__file__).resolve().parents[1]
INCLUDE = '#include "gt6_trainer.h"\n'
FUNC = re.compile(r'^void func_([0-9A-F]{8})(?:_guest)?\(ppu_context\* ctx\) \{\n.*?^\}', re.M | re.S)
LINE = re.compile(r'^        .* /\*GT6_TRAINER:[0-9A-F]{8}\*/\n', re.M)
# the sites whose binding returns early: the entries of the course graph search and of the walk along that graph
# (crash guards - ended where the console's stack would end)
BYPASS = {'003A3D58': 'return;', '003A3F20': 'return;'}


def bindings_path():
    p = ROOT / 'trainer/native_bindings.json'
    return p if p.exists() else ROOT / 'tools/native_bindings.json'


def unbind(text):
    """A lifted file without the lines this tool wrote into it."""
    text = text.replace(INCLUDE, '')
    return LINE.sub('', text)


def verify_image(sites):
    route = (ROOT / 'game.path').read_text(encoding='utf-8').strip()
    image = (ROOT / route / 'EMAIN.ELF').resolve()
    blob = image.read_bytes()
    if blob[:6] != b'\x7fELF\x02\x02':
        raise RuntimeError('Trainer requires the GT6 1.22 big-endian ELF64 image')
    phoff = struct.unpack_from('>Q', blob, 32)[0]
    size, count = struct.unpack_from('>HH', blob, 54)
    segs = [struct.unpack_from('>IIQQQQQQ', blob, phoff+i*size) for i in range(count)]
    for site in sites:
        addr = int(site['site'], 16)
        spans = [s for s in segs if s[0] == 1 and s[3] <= addr < s[3]+s[5]]
        if len(spans) != 1: raise RuntimeError(f'Unmapped trainer site {addr:08X}')
        s = spans[0]
        word = struct.unpack_from('>I', blob, s[2]+addr-s[3])[0]
        if word != int(site['word'], 16):
            raise RuntimeError(f'Trainer site {addr:08X} image mismatch: {word:08X}')
    return hashlib.sha256(blob).hexdigest()


def apply_trainer(directory, check=False, bindings=None):
    directory = Path(directory)
    sites = json.loads(Path(bindings or bindings_path()).read_text(encoding='utf-8'))
    image_sha = verify_image(sites)
    edits = {}; coverage = []
    names = {c['file'] for s in sites for c in s['copies']}
    names |= {p.name for p in directory.glob('*.cpp') if 'GT6_TRAINER' in p.read_text(encoding='utf-8')}
    for name in sorted(names):
        p = directory / name
        original = p.read_text(encoding='utf-8')
        clean = unbind(original)
        if 'GT6_TRAINER' in clean: raise RuntimeError(f'Unrecognized trainer marker in {name}')
        functions = {m[1]:m for m in FUNC.finditer(clean)}
        changes = []
        for s in sites:
            for copy in (c for c in s['copies'] if c['file'] == name):
                m = functions.get(copy['entry'])
                if m is None: raise RuntimeError(f'Missing trainer function {copy}')
                anchor = s['before'] + s['anchor'] + s['after']
                body = m[0]
                if body.count(anchor) != 1: raise RuntimeError(f'Ambiguous trainer site {s["site"]} in {copy}')
                pos = m.start() + body.index(anchor) + len(s['before'])
                site = s['site']; marker = f' /*GT6_TRAINER:{site}*/\n'
                if site in BYPASS:
                    target = BYPASS[site]
                    # Insert at line start to keep the original instruction byte-for-byte.
                    changes.append((pos-8, 0, f'        if (gt6_trainer_bypass(ctx, 0x{site}u)) {target}'+marker))
                else:
                    end = pos + len(s['anchor'])
                    changes.append((end+1, 0, f'        gt6_trainer_after(ctx, 0x{site}u);'+marker))
                coverage.append({'site':site, 'function':copy['entry'], 'file':name})
        for pos,n,new in sorted(changes, reverse=True): clean=clean[:pos]+new+clean[pos+n:]
        if changes: clean = INCLUDE + clean
        edits[p]=(original, clean)
    if check and any(a!=b for a,b in edits.values()):
        raise RuntimeError('Trainer bindings missing/stale; run trainer_post_lift.py')
    changed=[]
    if not check:
        for p,(old,new) in edits.items():
            if old!=new:
                # Generated C++ convention is CRLF; read_text normalized both sides.
                p.write_text(new, encoding='utf-8', newline='\r\n')
                changed.append(p.name)
    result={'sites':len(sites),'bindings':len(coverage),'changed':changed,'image_sha256':image_sha,'coverage':coverage}
    print(f'trainer_post_lift: {len(sites)} byte-checked sites, {len(coverage)} native bindings; changed {len(changed)} files')
    return result

if __name__ == '__main__':
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('directory', nargs='?', default=str(ROOT/'recompiled'))
    ap.add_argument('--check', action='store_true')
    ap.add_argument('--bindings', help='the site list (default: trainer/native_bindings.json, else tools/native_bindings.json)')
    ap.add_argument('--report')
    args=ap.parse_args()
    result=apply_trainer(args.directory,args.check,args.bindings)
    if args.report: Path(args.report).write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
