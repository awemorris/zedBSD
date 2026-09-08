#!/usr/bin/env python3
"""Insert a blank line and a purpose comment before an anchor inside a function.

Spec lines on stdin:  func|anchor-regex|occurrence|comment text
Occurrence is 1-based.  A blank line is inserted only when the previous line is
not already blank, and the comment inherits the anchor's indentation.
"""
import re
import sys


def find_function(lines, name):
    pat = re.compile(r'^' + re.escape(name) + r'\($')
    for i, line in enumerate(lines):
        if pat.match(line):
            for j in range(i, len(lines)):
                if lines[j].rstrip().endswith(');'):
                    break
                if lines[j].rstrip() == '{':
                    return j
    return None


def main():
    path = sys.argv[1]
    with open(path) as handle:
        lines = handle.read().split('\n')

    edits = []
    for spec in sys.stdin:
        spec = spec.rstrip('\n')
        if not spec.strip() or spec.startswith('#'):
            continue
        name, anchor, occ, comment = spec.split(';;', 3)
        start = find_function(lines, name)
        if start is None:
            sys.exit('function not found: ' + name)
        pat = re.compile(anchor)
        depth = 0
        seen = 0
        hit = None
        for i in range(start, len(lines)):
            if pat.search(lines[i]):
                seen += 1
                if seen == int(occ):
                    hit = i
                    break
            depth += lines[i].count('{') - lines[i].count('}')
            if depth == 0 and i > start:
                break
        if hit is None:
            sys.exit('anchor not found in %s: %s' % (name, anchor))
        edits.append((hit, comment))

    for hit, comment in sorted(edits, reverse=True):
        indent = re.match(r'[ \t]*', lines[hit]).group(0)
        block = []
        if lines[hit - 1].strip() != '':
            block.append('')
        block.append(indent + '/* ' + comment + ' */')
        lines[hit:hit] = block

    with open(path, 'w') as handle:
        handle.write('\n'.join(lines))


main()
