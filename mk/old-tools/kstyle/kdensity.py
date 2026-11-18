#!/usr/bin/env python3
"""kdensity.py FILE...

Reports the functions that are not yet written as semantic paragraphs.

A paragraph is a run of statements introduced by a purpose comment and set
off by blank lines, so a long run of consecutive statements with neither is
a function that still has to be read and split up.  The report gives the
longest such run per function, which is the size of the work.
"""
import re
import sys

HEAD = re.compile(r'^[A-Za-z_]\w*\($')


def functions(lines):
    i = 0
    n = len(lines)
    while i < n:
        if HEAD.match(lines[i]):
            j = i
            while j < n and lines[j] != '{':
                j += 1
            if j >= n:
                i += 1
                continue
            depth = 1
            k = j + 1
            while k < n and depth > 0:
                depth += lines[k].count('{') - lines[k].count('}')
                k += 1
            yield lines[i][:-1], i + 1, lines[j + 1:k - 1]
            i = k
            continue
        i += 1


def longest_run(body):
    """Returns the longest run of statements that no comment introduces.

    A run of statements under a purpose comment is a paragraph and is fine;
    what this looks for is a stretch that a reader meets with no explanation,
    and a paragraph so long that one comment cannot describe it.
    """
    # The leading declaration group is not a paragraph of statements.
    start = 0
    while start < len(body) and body[start].strip() != '':
        start += 1
    longest = 0
    run = 0
    commented = False
    for line in body[start:]:
        text = line.strip()
        if text == '':
            longest = max(longest, run if not commented else max(0, run - 12))
            run = 0
            continue
        if re.match(r'^[A-Za-z_]\w*:$', text):
            continue
        if text.startswith(('/*', '*')):
            longest = max(longest, run if not commented else max(0, run - 12))
            run = 0
            commented = True
            continue
        if run == 0 and not commented:
            pass
        run += 1
    longest = max(longest, run if not commented else max(0, run - 12))
    return longest


def main():
    threshold = 6
    rows = []
    for path in sys.argv[1:]:
        lines = open(path).read().split('\n')
        dense = []
        total = 0
        for name, line, body in functions(lines):
            total += 1
            run = longest_run(body)
            if run >= threshold:
                dense.append((run, name, line))
        if dense:
            rows.append((path, total, dense))
    rows.sort(key=lambda r: -len(r[2]))
    grand = 0
    for path, total, dense in rows:
        grand += len(dense)
        print('%-34s %3d/%3d functions, longest run %d' %
              (path, len(dense), total, max(d[0] for d in dense)))
    print('total functions needing paragraphs: %d' % grand)


if __name__ == '__main__':
    main()
