#!/usr/bin/env python3
"""convert.py: the handwriting's templates from the Hershey fonts (ws165-p002, plan/ws165/phase001/phase.md section 4).

Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

    convert.py MAPPING OUTPUT SHAR...

SHAR is a part of the Usenet distribution of the Hershey fonts (comp.sources.unix volume 4, "hershey", parts 2 to 5),
already uncompressed.  Each part is a shell archive; it is read as text and never run: the files between
"cat << \\SHAR_EOF > 'hersh.xxN'" and "SHAR_EOF" are taken out.  Of the font data (hersh.oc1-4, hersh.or1-4), the
glyphs MAPPING names are written to OUTPUT, one a line, in MAPPING's order:

    U+3042 6000 x,y x,y ... / x,y ...

the code point, the Hershey number, then the strokes, each a run of points (Hershey's coordinates: x to the right and
y down, relative to the glyph's centre), separated by " / ".  A Hershey record is its number in columns 1-5, its
count of pairs in columns 6-8, then the pairs: the first is the left and right side, each other is a point or " R"
(the pen lifted), every coordinate a character less 'R'.  A record goes on over the next lines until it has its pairs.

The occidental (hersh.oc1-4) and the oriental (hersh.or1-4) files number their glyphs apart, and a few numbers are in
both (509, 511, 617, 619, 622, 703 and 715: the simplex I, K, q, s, v, 3 and ? in the occidental files, kanji in the
oriental ones).  The mapping's Roman glyphs are the occidental ones and its kana (6000 on) only the oriental files
have, so a number in both is taken from the occidental files.

The oriental glyphs are drawn bold: some strokes are drawn again a unit beside a longer one (ほ has three of them, at
the top right where a dakuten goes, and the recognizer took them for one).  A stroke all of whose length lies within
OVERSTRIKE units of a longer stroke of the same glyph is such a second line and is left out (backlog-p2 line 57,
ws177-p009); a dakuten's two strokes are about three units apart and stay.
"""

import math
import re
import sys

BEGIN = re.compile(r"^cat << \\SHAR_EOF > '(hersh\.o[cr][1-4])'$")

# How near a stroke must lie to a longer one, all along, to be its second line (Hershey's units; a glyph is about 22).
OVERSTRIKE = 1.5

# How far apart the points looked at along a stroke are, for the overstrike test.
OVERSTRIKE_STEP = 0.5


def shar_files(text):
    """Gives the font files of a shell archive's text: name -> content."""
    files = {}
    name = None
    lines = []
    for line in text.split('\n'):
        if name is None:
            match = BEGIN.match(line)
            if match:
                name = match.group(1)
                lines = []
            continue
        if line == 'SHAR_EOF':
            files[name] = '\n'.join(lines) + '\n'
            name = None
            continue
        lines.append(line)
    return files


def glyphs(text):
    """Reads Hershey records: number -> list of strokes (lists of (x, y))."""
    found = {}
    rows = text.split('\n')
    index = 0
    while index < len(rows):
        row = rows[index]
        index += 1
        if len(row.strip()) == 0:
            continue
        number = int(row[0:5])
        count = int(row[5:8])
        data = row[8:]
        while len(data) < 2 * count and index < len(rows):
            data += rows[index]
            index += 1
        if len(data) < 2 * count:
            raise ValueError('glyph %d is cut short' % number)
        strokes = []
        stroke = []
        for pair in range(1, count):
            a = data[2 * pair]
            b = data[2 * pair + 1]
            if a == ' ' and b == 'R':
                if stroke:
                    strokes.append(stroke)
                stroke = []
                continue
            stroke.append((ord(a) - ord('R'), ord(b) - ord('R')))
        if stroke:
            strokes.append(stroke)
        found[number] = strokes
    return found


def segment_distance(point, start, end):
    """Gives how far a point is from a segment."""
    dx = end[0] - start[0]
    dy = end[1] - start[1]
    squared = dx * dx + dy * dy
    along = 0.0
    if squared > 0:
        along = ((point[0] - start[0]) * dx + (point[1] - start[1]) * dy) / squared
        along = max(0.0, min(1.0, along))
    return math.hypot(point[0] - start[0] - along * dx, point[1] - start[1] - along * dy)


def stroke_distance(point, stroke):
    """Gives how far a point is from a stroke (a lone point's stroke: from that point)."""
    if len(stroke) == 1:
        return math.hypot(point[0] - stroke[0][0], point[1] - stroke[0][1])
    return min(segment_distance(point, stroke[index], stroke[index + 1]) for index in range(len(stroke) - 1))


def stroke_length(stroke):
    """Gives the length of a stroke."""
    return sum(math.hypot(stroke[index + 1][0] - stroke[index][0], stroke[index + 1][1] - stroke[index][1])
               for index in range(len(stroke) - 1))


def stroke_along(stroke):
    """Gives the points along a stroke, OVERSTRIKE_STEP apart or nearer, its own points among them."""
    points = [stroke[0]]
    for index in range(len(stroke) - 1):
        start = stroke[index]
        end = stroke[index + 1]
        steps = max(1, int(math.ceil(math.hypot(end[0] - start[0], end[1] - start[1]) / OVERSTRIKE_STEP)))
        for step in range(1, steps + 1):
            points.append((start[0] + (end[0] - start[0]) * step / steps, start[1] + (end[1] - start[1]) * step / steps))
    return points


def without_overstrikes(strokes):
    """Gives a glyph's strokes without the second lines drawn beside longer ones (the module's note)."""
    dropped = set()
    for index, stroke in enumerate(strokes):
        along = stroke_along(stroke)
        for other_index, other in enumerate(strokes):
            # A stroke is weighed only against a longer one still kept (equal lengths: the later one is longer).
            if other_index == index or other_index in dropped:
                continue
            if (stroke_length(other), other_index) <= (stroke_length(stroke), index):
                continue
            if all(stroke_distance(point, other) <= OVERSTRIKE for point in along):
                dropped.add(index)
                break
    return [stroke for index, stroke in enumerate(strokes) if index not in dropped]


def main(arguments):
    """Writes the templates; the exit status says whether every glyph named was found."""
    if len(arguments) < 4:
        sys.stderr.write('usage: convert.py MAPPING OUTPUT SHAR...\n')
        return 2
    mapping = []
    with open(arguments[1], encoding='ascii') as source:
        for line in source:
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            number, code = line.split()
            mapping.append((int(number), int(code, 16)))
    texts = {}
    for path in arguments[3:]:
        with open(path, encoding='latin-1') as source:
            texts.update(shar_files(source.read()))
    # The oriental files first, so that a number in both sets is the occidental glyph (the module's note).
    found = {}
    for name in sorted(texts, key=lambda name: (not name.startswith('hersh.or'), name)):
        found.update(glyphs(texts[name]))
    out = ['# The handwriting templates of Kei (ws165-p002), converted from the Hershey fonts by convert.py.',
           '# The Hershey Fonts were originally created by Dr. A. V. Hershey while working at the U. S. National',
           '# Bureau of Standards.  The format of the font data this was converted from was originally created by',
           '# James Hurt, Cognition, Inc., 900 Technology Park Drive, Billerica, MA 01821 (mit-eddie!ci-dandelion!hurt).',
           '# A line: the code point, the Hershey number, the strokes (x,y points; y down) separated by " / ".']
    missing = 0
    for number, code in mapping:
        strokes = found.get(number)
        if not strokes:
            sys.stderr.write('convert.py: no glyph %d\n' % number)
            missing += 1
            continue
        strokes = without_overstrikes(strokes)
        parts = [' '.join('%d,%d' % point for point in stroke) for stroke in strokes]
        out.append('U+%04X %d %s' % (code, number, ' / '.join(parts)))
    with open(arguments[2], 'w', encoding='ascii') as target:
        target.write('\n'.join(out) + '\n')
    sys.stderr.write('convert.py: %d templates, %d missing\n' % (len(mapping) - missing, missing))
    if missing:
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
