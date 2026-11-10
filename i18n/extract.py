"""Find (and optionally wrap) user-visible Cyrillic string literals in C++ / Java sources.

python3 i18n/extract.py list  FILES...   -> prints JSON list of unique string values
python3 i18n/extract.py wrap  FILES...   -> wraps C++ literals in TR()/TRW() and Java literals in L.t()
Adjacent literals ("a" "b" in C++, "a" + "b" in Java) are treated as one string.
"""
import json, re, sys

CYR = re.compile('[А-Яа-яЁёІіЇїЄєҐґ]')
LIT = r'(?:L|u8)?"(?:[^"\\\n]|\\.)*"'

def unescape(body):
    out, i = [], 0
    while i < len(body):
        c = body[i]
        if c == '\\' and i + 1 < len(body):
            n = body[i + 1]
            m = {'n': '\n', 't': '\t', 'r': '\r', '"': '"', '\\': '\\', "'": "'", '0': '\0'}
            if n in m: out.append(m[n]); i += 2; continue
            if n == 'u': out.append(chr(int(body[i + 2:i + 6], 16))); i += 6; continue
            if n == 'x':
                j = i + 2
                while j < len(body) and body[j] in '0123456789abcdefABCDEF': j += 1
                out.append(chr(int(body[i + 2:j], 16))); i = j; continue
        out.append(c); i += 1
    return ''.join(out)

def lit_value(lit):
    return unescape(lit[lit.index('"') + 1:-1])

def code_spans(src, java):
    """Yield (start, end) of regions that are code (not comments / char literals / preprocessor includes)."""
    i, n, start = 0, len(src), 0
    spans = []
    while i < n:
        if src.startswith('//', i):
            spans.append((start, i)); j = src.find('\n', i); i = n if j < 0 else j; start = i; continue
        if src.startswith('/*', i):
            spans.append((start, i)); j = src.find('*/', i + 2); i = n if j < 0 else j + 2; start = i; continue
        if src[i] == '"':
            j = i + 1
            while j < n and src[j] != '"':
                j += 2 if src[j] == '\\' else 1
            i = j + 1; continue
        if src[i] == "'":
            j = i + 1
            while j < n and src[j] != "'":
                j += 2 if src[j] == '\\' else 1
            i = j + 1; continue
        if not java and src[i] == '#' and (i == 0 or src[i - 1] == '\n'):
            j = src.find('\n', i); spans.append((start, i)); i = n if j < 0 else j; start = i; continue
        i += 1
    spans.append((start, n))
    return spans

def find_sequences(src, java):
    sep = r'\s*\+\s*' if java else r'\s*'
    seq = re.compile(LIT + '(?:' + sep + LIT + ')*')
    res = []
    for a, b in code_spans(src, java):
        for m in seq.finditer(src, a, b):
            text = m.group(0)
            lits = re.findall(LIT, text)
            val = ''.join(lit_value(l) for l in lits)
            if not CYR.search(val): continue
            # already wrapped?
            before = src[max(0, m.start() - 6):m.start()]
            if re.search(r'(TRW?|L\.t)\($', before): continue
            res.append((m.start(), m.end(), val, lits[0].startswith('L')))
    return res

def main():
    mode, files = sys.argv[1], sys.argv[2:]
    allv = []
    for f in files:
        if f.endswith('Strings.java') or f.endswith('L.java'): continue
        src = open(f, encoding='utf-8').read()
        java = f.endswith('.java')
        seqs = find_sequences(src, java)
        if mode == 'list':
            for s in seqs:
                if s[2] not in allv: allv.append(s[2])
        else:
            out, last = [], 0
            for a, b, v, wide in seqs:
                out.append(src[last:a])
                fn = 'L.t' if java else ('TRW' if wide else 'TR')
                out.append(fn + '(' + src[a:b] + ')')
                last = b
            out.append(src[last:])
            open(f, 'w', encoding='utf-8').write(''.join(out))
            print(f, len(seqs), 'wrapped', file=sys.stderr)
    if mode == 'list':
        json.dump(allv, sys.stdout, ensure_ascii=False, indent=0)

main()
