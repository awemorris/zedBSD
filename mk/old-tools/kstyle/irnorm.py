#!/usr/bin/env python3
"""Normalize -O0 LLVM IR for equivalence comparison."""
import hashlib, re, sys
lines = [l.rstrip('\n') for l in open(sys.argv[1])]
strings = {}
for l in lines:
    m = re.match(r'^(@\.str(?:\.\d+)?) = .*c"(.*)", align', l)
    if m:
        strings[m.group(1)] = 'str_' + hashlib.md5(m.group(2).encode()).hexdigest()[:10]
def fix(l):
    l = re.sub(r'@\.str(?:\.\d+)?\b', lambda m: '@' + strings.get(m.group(0), m.group(0)), l)
    l = re.sub(r' #\d+', '', l)
    l = re.sub(r'!dbg !\d+', '', l)
    l = re.sub(r'!(\d+)\b', '!M', l)
    return l
out, declares, strdefs, allocas, fbody = [], [], [], [], []
funcs = []
cur = None
in_func = False
for line in lines:
    if line.startswith(('; ModuleID', 'source_filename', 'target ', '!', '; Function Attrs', 'attributes #')):
        continue
    if line.startswith('declare '):
        declares.append(fix(line))
        continue
    if re.match(r'^@\.str', line):
        strdefs.append(fix(line))
        continue
    if re.match(r'^%[A-Za-z_.0-9]+ = type ', line):
        strdefs.append(fix(line))
        continue
    if line.startswith('define '):
        in_func = True
        cur = [fix(line)]
        continue
    if in_func and line == '}':
        body = [fix(b) for b in fbody]
        names = {}
        def canon(m):
            n = m.group(0)
            if n not in names:
                names[n] = '%%v%d' % len(names)
            return names[n]
        labels = {}
        def lcanon(m):
            n = m.group(1)
            if n not in labels:
                labels[n] = 'L%d' % len(labels)
            return labels[n] + ':'
        # First pass: order of appearance in non-alloca lines.
        renamed = []
        for b in body:
            if re.match(r'^\s+%[^ ]+ = alloca ', b):
                renamed.append(None)
                continue
            b = re.sub(r'^([A-Za-z0-9_.]+):', lambda m: '%' + m.group(1) + ':', b)
            b = re.sub(r'%[A-Za-z0-9_.]+', canon, b)
            renamed.append(b)
        alloca_lines = []
        for idx, b in enumerate(body):
            if renamed[idx] is None:
                alloca_lines.append(re.sub(r'%[A-Za-z0-9_.]+', canon, b))
        cur.extend(sorted(alloca_lines))
        cur.extend([r for r in renamed if r is not None])
        cur.append(line)
        funcs.append(cur)
        cur = None
        fbody = []
        in_func = False
        continue
    if in_func:
        fbody.append(line)
        continue
    out.append(fix(line))
funcs.sort(key=lambda b: b[0])
body = []
for b in funcs:
    body.extend(b)
text = '\n'.join(out + body + sorted(strdefs) + sorted(declares))
text = re.sub(r'[ \t]+;', ' ;', text)
print(text)
