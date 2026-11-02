#!/usr/bin/env python3
# The per-stream table of an rtx-video324.sh run's H.265 passes: for each OUT/h1-TAG-NAME.full, the stream, its
# frames decoded and output, the frames matching ffmpeg's hashes, pictures not COMPLETE, the probe's exit status
# (from OUT/h1-TAG-NAME.log) and MATCH (every frame of the reference output and equal) or DIFFER; then the counts.
# With FEATURES (h265-conformance-prep.sh's features.txt) a DIFFER row also names the stream's tools.
#   usage: h265-table.py OUT TAG [FEATURES]
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
import glob
import os
import re
import sys


def main():
    out, tag = sys.argv[1], sys.argv[2]
    features = {}
    if len(sys.argv) > 3:
        with open(sys.argv[3]) as f:
            for line in f:
                parts = line.split()
                features[parts[0]] = ' '.join(p for p in parts[2:] if p.endswith('=1') or p.startswith(('w=', 'h=')))
    rows = []
    for full in sorted(glob.glob(os.path.join(out, 'h1-%s-*.full' % tag))):
        name = os.path.basename(full)[len('h1-%s-' % tag):-len('.full')]
        text = open(full, errors='replace').read()
        log = open(full[:-len('.full')] + '.log', errors='replace').read() if os.path.exists(full[:-len('.full')] + '.log') else ''
        rc = re.search(r'vkvideo-probe rc (\d+)', log)
        rc = rc.group(1) if rc else '-'
        m = re.search(r'(\d+) frames decoded, (\d+) output(?:, (\d+) match the reference)?', text)
        decoded, output, matched = (m.group(1), m.group(2), m.group(3) or '0') if m else ('-', '-', '0')
        missing = len(re.findall(r'reference frame \S+ not output', text))
        bad = len(re.findall(r'picture \d+ status', text))
        verdict = 'MATCH' if rc == '0' else 'DIFFER'
        rows.append((name, decoded, output, matched, missing, bad, rc, verdict))
    print('| stream | decoded | output | match | not output | not complete | rc | result |')
    print('| --- | --- | --- | --- | --- | --- | --- | --- |')
    for row in rows:
        print('| %s | %s | %s | %s | %d | %d | %s | %s |' % row)
    good = sum(1 for r in rows if r[7] == 'MATCH')
    print('\n%d streams: %d MATCH, %d DIFFER' % (len(rows), good, len(rows) - good))
    for row in rows:
        if row[7] != 'MATCH':
            print('  DIFFER %s rc %s: %s' % (row[0], row[6], features.get(row[0], '')))


main()
