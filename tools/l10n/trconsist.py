# trconsist.py <scratch dir>  - texts that name another menu item in single quotes: does the English name it by that item's own English?
import io, sys, os, re, glob
sp = sys.argv[1]
keys = {}
for l in io.open(os.path.join(sp, 'tr_keys.tsv'), encoding='utf-8'):
    p = l.rstrip('\n').split('\t')
    if len(p) == 2: keys[p[0]] = p[1]
en = {}
for f in sorted(glob.glob(os.path.join(sp, 'tr_out_*.tsv'))):
    for l in io.open(f, encoding='utf-8-sig'):
        p = l.rstrip('\n').rstrip('\r').split('\t')
        if len(p) == 2 and p[0] in keys: en[keys[p[0]]] = p[1]
fix = os.path.join(sp, 'tr_fix.tsv')
if os.path.exists(fix):
    for l in io.open(fix, encoding='utf-8'):
        p = l.rstrip('\n').split('\t')
        if len(p) == 2: en[p[0]] = p[1]
han = re.compile('[가-힣]')
quote = re.compile(r"'([^']{2,40})'")
labels = {}                                    # a label's Korean -> its English (texts that are short and stand alone)
for ko, e in en.items():
    labels[ko] = e
    # labels often carry a tail in brackets: "코스 한계 학습 (달릴수록 빨라짐)"
    m = re.match(r'^(.*?)\s*[(（]', ko)
    if m and m.group(1) and m.group(1) not in labels:
        me = re.match(r'^(.*?)\s*[(（]', e)
        if me: labels[m.group(1)] = me.group(1)
bad = 0; seen = set()
for ko, e in sorted(en.items()):
    for q in quote.findall(ko):
        if not han.search(q) or q not in labels: continue
        want = labels[q]
        if want.lower() not in e.lower():
            key = (q, want)
            bad += 1
            if key not in seen:
                seen.add(key)
                got = quote.findall(e)
                print("'%s' is '%s' as a label, but named %s in: %s" % (q, want, got if got else '(no quotes)', e[:110]))
print('%d mismatches, %d different references' % (bad, len(seen)))
