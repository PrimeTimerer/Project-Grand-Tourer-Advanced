# tr_check.py <tr_in_X.tsv> <tr_out_X.tsv>  - checks a translation file against its input. Output lines: id <TAB> English (raw text,
# with the same escape sequences as the source would have). Prints every problem; exit code 1 if there is one.
import io, sys, re
inp = {}
for l in io.open(sys.argv[1], encoding='utf-8'):
    p = l.rstrip('\n').split('\t')
    if len(p) == 4: inp[p[0]] = p[3]
out = {}; bad = 0
for k, l in enumerate(io.open(sys.argv[2], encoding='utf-8')):
    l = l.rstrip('\n')
    if not l.strip(): continue
    p = l.split('\t')
    if len(p) != 2: print('line %d: not "id<TAB>text" (%d fields)' % (k + 1, len(p))); bad += 1; continue
    if p[0] in out: print('id %s twice' % p[0]); bad += 1
    out[p[0]] = p[1]
spec = re.compile(r'%[-+ 0#]*\d*(?:\.\d+)?(?:hh|h|ll|l|z|I64)?[diufsxXgGeEcp%]')
brace = re.compile(r'\{\d+(?::[^}]*)?\}')
han = re.compile('[가-힣]')
def lead(s): return len(s) - len(s.lstrip(' '))
def trail(s): return len(s) - len(s.rstrip(' '))
for i, ko in inp.items():
    if i not in out: print('id %s missing' % i); bad += 1; continue
    en = out[i]; why = []
    if spec.findall(ko) != spec.findall(en): why.append('printf specifiers differ: %s -> %s' % (spec.findall(ko), spec.findall(en)))
    if sorted(brace.findall(ko)) != sorted(brace.findall(en)): why.append('{n} placeholders differ')
    if ('##' in ko) != ('##' in en) or ('##' in ko and ko[ko.index('##'):] != en[en.index('##'):]): why.append('the part from ## on must stay as it is')
    if ko.count('\\n') != en.count('\\n'): why.append('number of \\n differs (%d -> %d)' % (ko.count('\\n'), en.count('\\n')))
    if lead(ko) != lead(en) or (trail(ko) > 0) != (trail(en) > 0): why.append('leading / trailing spaces differ')
    if han.search(en): why.append('Hangul left in the English')
    if not en.strip() and ko.strip(): why.append('empty')
    if en.count('"') - en.count('\\"') != 0: why.append('an unescaped double quote')
    if re.search(r'\\(?![n"\\tu%x0-9])', en): why.append('a stray backslash')
    if why: print('id %s: %s\n    ko: %s\n    en: %s' % (i, '; '.join(why), ko, en)); bad += 1
for i in out:
    if i not in inp: print('id %s is not in the input' % i); bad += 1
print('%d texts, %d problems' % (len(inp), bad))
sys.exit(1 if bad else 0)
