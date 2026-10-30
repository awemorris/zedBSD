#!/usr/bin/env python3
# Lists the guide's references to lines of its examples ("keiland-examples/NAME.c#L10-L20" and "#L10") with the text of
# the lines they name, so that a change to an example can be followed by its references: each reference prints its
# guide line, the range, and the first and last line it points at.  A range outside the file is a FAIL.  The text
# right after a reference (its "(`function`)", the code block below it) is for the reader of this list to compare.
#   python3 -I plan/ws327/tests/doc-lines.py [NAME...]     (from the worktree's root; NAME: 08-worker.c and the like)
# Last line: "ws327-doc-lines: PASS" (every range inside its file) or "FAIL".
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
import os
import re
import sys

GUIDE = 'docs/howto/keiland-programming.md'
EXAMPLES = 'docs/howto/keiland-examples'
REFERENCE = re.compile(r'keiland-examples/([\w.-]+\.c)#L(\d+)(?:-L(\d+))?')


def main():
    """Prints every reference of the named examples (all when none is named)."""
    names = set(sys.argv[1:])
    files = {}
    failed = False
    with open(GUIDE) as guide:
        lines = guide.read().split('\n')
    for number, line in enumerate(lines, 1):
        for match in REFERENCE.finditer(line):
            name = match.group(1)
            if names and name not in names:
                continue
            if name not in files:
                with open(os.path.join(EXAMPLES, name)) as example:
                    files[name] = example.read().split('\n')
            first = int(match.group(2))
            last = int(match.group(3) or match.group(2))
            source = files[name]
            if first < 1 or last < first or last > len(source):
                print(f'FAIL {GUIDE}:{number}: {name} L{first}-L{last} outside its {len(source)} lines')
                failed = True
                continue
            print(f'{GUIDE}:{number}: {name} L{first}-L{last}')
            print(f'    first: {source[first - 1].strip()}')
            print(f'    last:  {source[last - 1].strip()}')
    print('ws327-doc-lines: FAIL' if failed else 'ws327-doc-lines: PASS')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
