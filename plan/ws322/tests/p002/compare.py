# Compares the Wave511 probe's text (a read of /dev/vf2-vpu-probe) with the reference hashes make-streams.sh wrote,
# and prints one line per stream and way (ws322-p002):
#   frames, how many decoded without an error, how many matched the reference in display order (the frames sorted
#   by order count within each IDR), whether every frame came out at once (dec == disp), whether the host's pick
#   was taken (dec == pick, ways B, C, F), and for F whether the cached view showed the new picture or the fill.
#   python3 -I compare.py PROBE.txt REFDIR
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
import os
import re
import sys

probe, refdir = sys.argv[1], sys.argv[2]
sizes = {'h264-1080': (1920, 1080), 'h265-1080': (1920, 1080)}


def fill_hash(width, height, byte):
    h = 0x811c9dc5
    for _ in range(width * height * 3 // 2):
        h = ((h ^ byte) * 0x01000193) & 0xffffffff
    return h


frames = {}
others = []
for line in open(probe, errors='replace'):
    line = line.strip()
    if not line.startswith('frame '):
        if line:
            others.append(line)
        continue
    parts = line.split()
    name, way = parts[1], parts[2]
    fields = dict(p.split('=', 1) for p in parts[3:] if '=' in p)
    frames.setdefault((name, way), []).append(fields)

for line in others:
    if re.match(r'(vpu |stream |open |sequence |register |pool |flags |run |close |probe )', line):
        print('#', line)

fills = {}
for (name, way), rows in sorted(frames.items()):
    ref_path = os.path.join(refdir, name + '.ref')
    ref = [l.strip() for l in open(ref_path)] if os.path.exists(ref_path) else []
    codec = 'h265' if name.startswith('h265') else 'h264'
    ok = [r for r in rows if r.get('err') == '0' and r.get('ok') == '1']
    epoch = 0
    keyed = []
    for i, r in enumerate(rows):
        nal = (int(r.get('type', '0'), 16) >> 4) & 0x3f
        idr = (nal == 5) if codec == 'h264' else nal in (19, 20)
        if idr and i > 0:
            epoch += 1
        keyed.append(((epoch, int(r.get('poc', '0'))), r))
    shown = [r for _, r in sorted(keyed, key=lambda k: k[0])]
    matched = sum(1 for i, r in enumerate(shown) if i < len(ref) and r.get('hash') == ref[i])
    in_ref = sum(1 for r in rows if r.get('hash') in ref)
    at_once = sum(1 for r in rows if r.get('dec') == r.get('disp'))
    picked = sum(1 for r in rows if r.get('dec') == r.get('pick'))
    us = [int(r.get('us', '0')) for r in ok]
    ticks = [int(r.get('ticks', '0')) for r in ok]
    extra = ''
    if way == 'F':
        w, h = sizes.get(name, (352, 288))
        if (w, h) not in fills:
            fills[(w, h)] = '%08x' % fill_hash(w, h, 0x3c)
        fresh = sum(1 for i, r in enumerate(shown) if i < len(ref) and r.get('cached') == ref[i])
        stale = sum(1 for r in rows if r.get('cached') == fills[(w, h)])
        extra = ' cached-new=%d cached-fill=%d' % (fresh, stale)
    print('%s %s frames=%d ref=%d decoded-ok=%d match-display-order=%d hash-in-ref=%d dec==disp=%d dec==pick=%d'
          ' us-max=%d ticks-max=%d%s' % (name, way, len(rows), len(ref), len(ok), matched, in_ref, at_once, picked,
                                         max(us) if us else 0, max(ticks) if ticks else 0, extra))
