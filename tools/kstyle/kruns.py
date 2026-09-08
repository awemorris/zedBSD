#!/usr/bin/env python3
"""kruns.py FILE [LIMIT]

Prints the runs of statements that no comment introduces, with the two lines
of context on each side, so that a purpose comment can be written for each
one without reading the whole file.
"""
import importlib.util
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location('kd', os.path.join(HERE, 'kdensity.py'))
kd = importlib.util.module_from_spec(spec)
spec.loader.exec_module(kd)

LABEL = re.compile(r'^[A-Za-z_]\w*:$')


def runs(body):
    """Yields (index, lines) for every uncommented run of the body."""
    start = 0
    while start < len(body) and body[start].strip() != '':
        start += 1
    out = []
    run = []
    begin = 0
    commented = False
    for offset, line in enumerate(body[start:], start):
        text = line.strip()
        if LABEL.match(text):
            continue
        if text == '' or text.startswith(('/*', '*')):
            if run and not commented:
                out.append((begin, run))
            run = []
            commented = text.startswith(('/*', '*'))
            continue
        if not run:
            begin = offset
        run.append(line)
    if run and not commented:
        out.append((begin, run))
    return out


def main():
    path = sys.argv[1]
    limit = int(sys.argv[2]) if len(sys.argv) > 2 else 6
    lines = open(path).read().split('\n')
    for name, line, body in kd.functions(lines):
        for begin, run in runs(body):
            if len(run) < limit:
                continue
            print('=== %s  (%d statements, file line %d)' %
                  (name, len(run), line + begin + 2))
            for text in body[max(0, begin - 2):begin + len(run) + 2]:
                print('  ' + text)
            print()


if __name__ == '__main__':
    main()
