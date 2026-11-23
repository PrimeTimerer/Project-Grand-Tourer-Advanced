# tr_prep.py <ko_all.tsv> <outdir>  - the unique Korean literals, numbered, split into four work files for translation:
#   A = gt6_menu_ui.cpp first half, B = second half, C = gt6_menu_engine.cpp, D = launcher. A text that occurs in several places
#   goes to the first place only.  Lines: id <TAB> file:line <TAB> what stands before it in the code <TAB> the Korean text (raw, as in the source)
import io, sys, os
src, outdir = sys.argv[1], sys.argv[2]
rows = [l.rstrip('\n').split('\t') for l in io.open(src, encoding='utf-8') if l.strip()]
rows = [r for r in rows if len(r) == 5]
seen = {}; uniq = []
for f, line, depth, before, text in rows:
    if text in seen: continue
    seen[text] = len(uniq); uniq.append((f, int(line), before, text))
ui = [u for u in uniq if u[0] == 'gt6_menu_ui.cpp']
half = len(ui) // 2
groups = {'A': ui[:half], 'B': ui[half:], 'C': [u for u in uniq if u[0] in ('gt6_menu_engine.cpp', 'gt6_menu.h')], 'D': [u for u in uniq if u[0].startswith('launcher/')]}
n = 0
for g, items in groups.items():
    with io.open(os.path.join(outdir, 'tr_in_%s.tsv' % g), 'w', encoding='utf-8', newline='\n') as fo:
        for f, line, before, text in items:
            fo.write('%d\t%s:%d\t%s\t%s\n' % (seen[text], f, line, before, text))
    print(g, len(items), 'texts', sum(len(u[3]) for u in items), 'chars'); n += len(items)
with io.open(os.path.join(outdir, 'tr_keys.tsv'), 'w', encoding='utf-8', newline='\n') as fo:
    for i, (f, line, before, text) in enumerate(uniq): fo.write('%d\t%s\n' % (i, text))
print('total', n, 'of', len(uniq))
