# koextract.py <root> <out.tsv> <file> ...  - every string literal with Hangul in the given source files: file, line, kind of place, text.
# Comments are left out. Also says how each literal is used (inside a function body or at file / class scope).
import re, io, sys, os
root, out = sys.argv[1], sys.argv[2]
han = re.compile('[가-힣]')
rows = []
def scan(path, text):
    i = 0; n = len(text); line = 1; depth = 0
    res = []
    while i < n:
        c = text[i]
        if c == '\n': line += 1; i += 1; continue
        if text.startswith('//', i):
            j = text.find('\n', i); i = n if j < 0 else j; continue
        if text.startswith('/*', i):
            j = text.find('*/', i + 2); j = n if j < 0 else j + 2
            line += text.count('\n', i, j); i = j; continue
        if c == '{': depth += 1; i += 1; continue
        if c == '}': depth -= 1; i += 1; continue
        if c == "'":
            j = i + 1
            while j < n and text[j] != "'":
                j += 2 if text[j] == '\\' else 1
            i = j + 1; continue
        if c == '@' and i + 1 < n and text[i + 1] == '"':       # C# verbatim string
            j = i + 2
            while j < n:
                if text[j] == '"':
                    if j + 1 < n and text[j + 1] == '"': j += 2; continue
                    break
                j += 1
            s = text[i + 2:j]
            if han.search(s): res.append((line, depth, 'verbatim', s))
            line += text.count('\n', i, j); i = j + 1; continue
        if c == '"':
            j = i + 1
            while j < n and text[j] != '"':
                j += 2 if text[j] == '\\' else 1
            s = text[i + 1:j]
            if han.search(s):
                ls = text.rfind('\n', 0, i) + 1
                res.append((line, depth, text[ls:i].strip()[-60:], s))
            i = j + 1; continue
        i += 1
    return res
tot = 0
with io.open(out, 'w', encoding='utf-8', newline='\n') as fo:
    for f in sys.argv[3:]:
        text = io.open(os.path.join(root, f), encoding='utf-8', errors='replace').read()
        res = scan(f, text)
        tot += len(res)
        for line, depth, before, s in res:
            fo.write('%s\t%d\t%d\t%s\t%s\n' % (f, line, depth, before.replace('\t', ' '), s))
        d0 = sum(1 for r in res if r[1] <= (1 if f.endswith('.cs') else 0))
        print('%5d literals in %s (%d at file / class scope)' % (len(res), f, d0))
print('total', tot)
