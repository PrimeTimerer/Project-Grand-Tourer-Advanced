# l10n_wrap.py <mode cpp|cs> <groups.tsv> <file> ...   - wraps every string literal with Hangul in a call of the translation function,
# in place: C++  "…" -> T_("…")  (a run of adjacent literals is one unit),  C#  "…" -> L.T("…").  Literals that are wrapped already,
# that stand in comments or (C#) after "const string" / "case" are left alone. Appends to groups.tsv one line per unit:
#   file <TAB> line <TAB> part <US> part ...      (the raw contents of the unit's literals, in order; US = \x1f)
import io, sys, re
mode, groups_path = sys.argv[1], sys.argv[2]
han = re.compile('[가-힣]')
CALL = 'T_(' if mode == 'cpp' else 'L.T('

def literals(text):
    """(start, end, content, line) of every ordinary string literal outside comments and char literals; C++ raw strings skipped."""
    out = []; i = 0; n = len(text); line = 1
    while i < n:
        c = text[i]
        if c == '\n': line += 1; i += 1; continue
        if text.startswith('//', i):
            j = text.find('\n', i); i = n if j < 0 else j; continue
        if text.startswith('/*', i):
            j = text.find('*/', i + 2); j = n if j < 0 else j + 2
            line += text.count('\n', i, j); i = j; continue
        if c == "'":
            # a digit separator (1'000) is not a char literal
            if i > 0 and text[i - 1].isalnum() and i + 1 < n and text[i + 1].isalnum() and mode == 'cpp' and text[i - 1].isdigit(): i += 1; continue
            j = i + 1
            while j < n and text[j] != "'" and text[j] != '\n':
                j += 2 if text[j] == '\\' else 1
            i = j + 1; continue
        if mode == 'cpp' and c == 'R' and text.startswith('R"', i) and (i == 0 or not (text[i - 1].isalnum() or text[i - 1] == '_')):
            k = text.find('(', i + 2); delim = text[i + 2:k]; end = text.find(')' + delim + '"', k)
            end = n if end < 0 else end + len(delim) + 2
            line += text.count('\n', i, end); i = end; continue
        if mode == 'cs' and c == '@' and text.startswith('@"', i):
            j = i + 2
            while j < n:
                if text[j] == '"':
                    if j + 1 < n and text[j + 1] == '"': j += 2; continue
                    break
                j += 1
            line += text.count('\n', i, j); i = j + 1; continue
        if c == '"':
            j = i + 1
            while j < n and text[j] != '"':
                j += 2 if text[j] == '\\' else 1
            out.append((i, j + 1, text[i + 1:j], line))
            i = j + 1; continue
        i += 1
    return out

total = 0
with io.open(groups_path, 'a', encoding='utf-8', newline='\n') as gf:
    for path in sys.argv[3:]:
        text = io.open(path, encoding='utf-8', newline='').read()
        lits = literals(text)
        units = []                                   # runs of adjacent literals
        k = 0
        while k < len(lits):
            run = [lits[k]]
            if mode == 'cpp':
                while k + 1 < len(lits) and text[lits[k][1]:lits[k + 1][0]].strip() == '':
                    k += 1; run.append(lits[k])
            units.append(run); k += 1
        edits = []
        for run in units:
            if not any(han.search(l[2]) for l in run): continue
            start, end = run[0][0], run[-1][1]
            ls = text.rfind('\n', 0, start) + 1
            before = text[ls:start]
            if before.rstrip().endswith(CALL): continue                     # wrapped already
            if before.lstrip().startswith('#'): continue                    # a preprocessor line
            if mode == 'cs' and (re.search(r'\bconst\s+string\b', before) or re.search(r'\bcase\s*$', before) or before.lstrip().startswith('[')):
                print('  left alone (%s:%d): %s' % (path.replace('\\', '/').split('/')[-1], run[0][3], before.strip()[-50:] + ' "' + run[0][2][:30] + '"'))
                continue
            edits.append((start, end))
            gf.write('%s\t%d\t%s\n' % (path.replace('\\', '/').split('/')[-1], run[0][3], '\x1f'.join(l[2] for l in run)))
        for start, end in reversed(edits):
            text = text[:start] + CALL + text[start:end] + ')' + text[end:]
        io.open(path, 'w', encoding='utf-8', newline='').write(text)
        print('%s: %d units wrapped' % (path.replace('\\', '/').split('/')[-1], len(edits))); total += len(edits)
print('total', total)
