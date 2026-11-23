# l10n_tables.py <scratch dir> <groups_cpp.tsv> <out .inc> <groups_cs.tsv> <out .cs>  - the translation tables from the translators'
# files (tr_keys.tsv: id -> Korean, tr_out_*.tsv: id -> English) and the units the wrap tool wrapped.
import io, sys, os, re, glob
sp, gcpp, out_inc, gcs, out_cs = sys.argv[1:6]
han = re.compile('[가-힣]')
keys = {}
for l in io.open(os.path.join(sp, 'tr_keys.tsv'), encoding='utf-8'):
    p = l.rstrip('\n').split('\t')
    if len(p) == 2: keys[p[0]] = p[1]
en = {}
for f in sorted(glob.glob(os.path.join(sp, 'tr_out_*.tsv'))):
    for l in io.open(f, encoding='utf-8-sig'):
        p = l.rstrip('\n').rstrip('\r').split('\t')
        if len(p) == 2 and p[0] in keys: en[keys[p[0]]] = p[1]
fix = os.path.join(sp, 'tr_fix.tsv')                     # my own corrections: Korean <TAB> English
if os.path.exists(fix):
    for l in io.open(fix, encoding='utf-8'):
        p = l.rstrip('\n').split('\t')
        if len(p) == 2: en[p[0]] = p[1]
print('%d Korean texts, %d with English' % (len(keys), len(en)))
def units(path):
    seen = {}; missing = []
    for l in io.open(path, encoding='utf-8'):
        p = l.rstrip('\n').split('\t')
        if len(p) != 3: continue
        parts = p[2].split('\x1f')
        ko = ''.join(parts)
        if ko in seen: continue
        out = []; ok = True
        for part in parts:
            if han.search(part):
                if part in en: out.append(en[part])
                else: ok = False; missing.append((p[0], p[1], part))
            else: out.append(part)
        if ok: seen[ko] = ''.join(out)
    return seen, missing
cpp, miss1 = units(gcpp)
cs, miss2 = units(gcs)
# optional: <groups_trainer.tsv> <out trainer .cs> - the external trainer's table (namespace Gt6Trainer) [GT6 0089]
gtr, out_tr = (sys.argv[6], sys.argv[7]) if len(sys.argv) > 7 else (None, None)
tr, miss3 = units(gtr) if gtr else ({}, [])
for f, line, part in (miss1 + miss2 + miss3)[:40]: print('  NO ENGLISH: %s:%s  %s' % (f, line, part[:80]))
with io.open(out_inc, 'w', encoding='utf-8', newline='\n') as fo:
    fo.write('// Made by the translation tool: Korean text of the menu (as in the source) -> English. Do not edit by hand.\n')
    for ko in sorted(cpp): fo.write('{"%s", "%s"},\n' % (ko, cpp[ko]))
with io.open(out_cs, 'w', encoding='utf-8', newline='\r\n') as fo:
    fo.write('// Made by the translation tool: Korean text of the launcher (as in the source) -> English. Do not edit by hand.\n')
    fo.write('using System.Collections.Generic;\n\nnamespace GT6Launcher\n{\n    public static partial class L\n    {\n')
    fo.write('        static readonly Dictionary<string, string> Table = new Dictionary<string, string>\n        {\n')
    for ko in sorted(cs): fo.write('            {"%s", "%s"},\n' % (ko, cs[ko]))
    fo.write('        };\n    }\n}\n')
if gtr:
    with io.open(out_tr, 'w', encoding='utf-8', newline='\r\n') as fo:
        fo.write('// Made by the translation tool: Korean text of the trainer (as in the source) -> English. Do not edit by hand.\n')
        fo.write('namespace Gt6Trainer;\n\npublic static partial class L\n{\n')
        fo.write('    static readonly Dictionary<string, string> Table = new()\n    {\n')
        for ko in sorted(tr): fo.write('        {"%s", "%s"},\n' % (ko, tr[ko]))
        fo.write('    };\n}\n')
print('menu: %d units (%d without English) | launcher: %d units (%d without English)' % (len(cpp), len(miss1), len(cs), len(miss2))
      + (' | trainer: %d units (%d without English)' % (len(tr), len(miss3)) if gtr else ''))
