#!/usr/bin/env python3
# ws099-p001, C5: reads zdesktop's log (--log-frames, with the at_ms of KWL COMPOSE and of App Home's and Wiseview's
# open and close lines) and prints, for each open and close, the milliseconds from the request to the first frame
# composed after it, to the settled line, and the longest gap between frames while it moves.
#
#   c5-parse.py LOG FIRST_FRAME_MS GAP_MS
# Prints one "C5 ..." line per transition and "C5 RESULT pass=N fail=M first_max=A gap_max=B".
# The first open and close of each kind is a warm-up: its numbers are printed ("warm-up") and in first_max and
# gap_max, but only a transition that drew no frame at all fails it.  QEMU draws that first one at about 200 ms and
# the later ones within the limits (T1-006, T1-477), and the user does not count it (2026-10-03: "C5の200msは問題視
# しません。clearでOKです。", the speed is F-072's).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
import re
import sys

STARTS = {
    'wiseview-open': re.compile(r'KWL WISEVIEW opening key at_ms=(\d+)'),
    'wiseview-close': re.compile(r'KWL WISEVIEW close key at_ms=(\d+)'),
    'home-open': re.compile(r'KWL HOME open via=\S+ at_ms=(\d+)'),
    'home-close': re.compile(r'KWL HOME close via=\S+ at_ms=(\d+)'),
}
ENDS = {
    'wiseview-open': re.compile(r'KWL WISEVIEW open windows=(\d+) at_ms=(\d+)'),
    'wiseview-close': re.compile(r'KWL WISEVIEW closed at_ms=(\d+)'),
    'home-open': re.compile(r'KWL HOME opened .* at_ms=(\d+)'),
    'home-close': re.compile(r'KWL HOME closed at_ms=(\d+)'),
}
FRAME = re.compile(r'KWL COMPOSE frame=\d+ image=\d+ windows=(\d+) at_ms=(\d+)')


def main():
    path, first_limit, gap_limit = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
    lines = open(path, errors='replace').read().splitlines()
    frames = []
    events = []
    for line in lines:
        m = FRAME.search(line)
        if m:
            frames.append(int(m.group(2)))
            continue
        for kind, pattern in STARTS.items():
            m = pattern.search(line)
            if m:
                events.append(('start', kind, int(m.group(1)), None))
        for kind, pattern in ENDS.items():
            m = pattern.search(line)
            if m:
                events.append(('end', kind, int(m.groups()[-1]), m.group(1) if kind == 'wiseview-open' else None))
    passed = failed = 0
    first_max = gap_max = 0
    open_start = {}
    warmed = set()
    for what, kind, at, windows in events:
        if what == 'start':
            open_start[kind] = at
            continue
        start = open_start.pop(kind, None)
        if start is None:
            continue
        during = [f for f in frames if start <= f <= at]
        after = [f for f in frames if f >= start]
        first = after[0] - start if after else None
        gaps = [b - a for a, b in zip([start] + during, during + [at])]
        gap = max(gaps) if gaps else None
        warm_up = kind not in warmed
        warmed.add(kind)
        if warm_up:
            ok = first is not None
        else:
            ok = first is not None and first <= first_limit and gap is not None and gap <= gap_limit
        passed += ok
        failed += not ok
        first_max = max(first_max, first or 0)
        gap_max = max(gap_max, gap or 0)
        extra = f' windows={windows}' if windows else ''
        mark = ' warm-up' if warm_up else ''
        print(f'C5 {kind}{extra}{mark}: first_frame_ms={first} settle_ms={at - start} frames={len(during)} '
              f'max_gap_ms={gap} {"ok" if ok else "FAIL"}')
    print(f'C5 RESULT pass={passed} fail={failed} first_max={first_max} gap_max={gap_max}')
    return 0 if failed == 0 and passed > 0 else 1


if __name__ == '__main__':
    sys.exit(main())
