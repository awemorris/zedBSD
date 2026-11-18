#!/usr/bin/env python3
"""Split a C source file into one chunk per function, and put it back.

    ksplit.py split FILE DIR     writes DIR/manifest.txt and DIR/NNN-name.c
    ksplit.py join DIR FILE      concatenates the chunks in manifest order
    ksplit.py check FILE DIR     splits, joins, and reports whether the round
                                 trip is byte-identical

A function chunk begins at the comment block that introduces the function and
ends at its closing brace in column 0.  Everything else -- the file header,
includes, macros, types, file-scope variables, the forward-declaration block,
and anything stranded between functions -- is kept in its own chunk, so the
join reproduces the file exactly.
"""
import os
import re
import sys


def function_starts(lines):
    """Reports (definition_line, name, end_line) for every function."""
    found = []
    i = 0
    while i < len(lines):
        match = re.match(r'^([A-Za-z_][A-Za-z0-9_]*)\($', lines[i])
        if match:
            j = i
            depth = 0
            while j < len(lines):
                depth += lines[j].count('(') - lines[j].count(')')
                if depth == 0 and lines[j].rstrip().endswith(')'):
                    break
                j += 1
            if j + 1 < len(lines) and lines[j + 1].rstrip() == '{':
                end = j + 1
                depth = 0
                while end < len(lines):
                    depth += lines[end].count('{') - lines[end].count('}')
                    if depth == 0 and lines[end].rstrip() == '}':
                        break
                    end += 1
                found.append((i, match.group(1), end))
                i = end
        i += 1
    return found


def chunk_start(lines, definition):
    """Walks back over the return type and the comment that introduces it."""
    start = definition
    while start > 0:
        previous = lines[start - 1]
        if (previous.strip() and not previous.startswith((' ', '\t', '/', '*',
                '}', '#')) and not previous.rstrip().endswith((';', ')'))):
            start -= 1
            continue
        break
    while start > 0:
        previous = lines[start - 1].lstrip()
        if previous.startswith(('/*', '*')) or lines[start - 1].strip() == '*/':
            start -= 1
            continue
        break
    return start


def split(path, directory):
    with open(path) as handle:
        lines = handle.read().split('\n')

    os.makedirs(directory, exist_ok=True)
    for stale in os.listdir(directory):
        os.remove(os.path.join(directory, stale))

    pieces = []
    cursor = 0
    for definition, name, end in function_starts(lines):
        start = chunk_start(lines, definition)
        if start > cursor:
            pieces.append(('between', lines[cursor:start]))
        pieces.append((name, lines[start:end + 1]))
        cursor = end + 1
    if cursor < len(lines):
        pieces.append(('between', lines[cursor:]))

    manifest = []
    for index, (name, body) in enumerate(pieces):
        safe = re.sub(r'[^A-Za-z0-9_]', '_', name)
        chunk = '%03d-%s.c' % (index, safe)
        with open(os.path.join(directory, chunk), 'w') as handle:
            handle.write('\n'.join(body))
        manifest.append(chunk)
    with open(os.path.join(directory, 'manifest.txt'), 'w') as handle:
        handle.write('\n'.join(manifest) + '\n')

    functions = sum(1 for name, _ in pieces if name != 'between')
    print('%s: %d chunks, %d functions' % (path, len(pieces), functions))


def join(directory, path):
    with open(os.path.join(directory, 'manifest.txt')) as handle:
        manifest = handle.read().split()
    bodies = []
    for chunk in manifest:
        with open(os.path.join(directory, chunk)) as handle:
            bodies.append(handle.read())
    with open(path, 'w') as handle:
        handle.write('\n'.join(bodies))


def check(path, directory):
    with open(path) as handle:
        original = handle.read()
    split(path, directory)
    rebuilt = os.path.join(directory, '.rebuilt')
    join(directory, rebuilt)
    with open(rebuilt) as handle:
        text = handle.read()
    os.remove(rebuilt)
    if text == original:
        print('round trip: identical')
        return 0
    print('round trip: DIFFERENT')
    return 1


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    action, first, second = sys.argv[1:]
    if action == 'split':
        split(first, second)
    elif action == 'join':
        join(first, second)
    elif action == 'check':
        sys.exit(check(first, second))
    else:
        sys.exit(__doc__)


main()
