#!/usr/bin/env python3
# ws291-p024: perf stat -I 1000 -x, output (nvk-profile.sh) as each event's median one-second count over the frames a
# second (FPS): one line an event, "EVENT per-frame VALUE UNIT".  The first and last two seconds are left out (start-up,
# the end of the spin).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
import statistics
import sys

path, fps = sys.argv[1], float(sys.argv[2])
seconds = {}
units = {}
with open(path) as f:
    for line in f:
        parts = line.strip().split(',')
        if len(parts) < 4 or line.startswith('#'):
            continue
        try:
            value = float(parts[1])
        except ValueError:
            continue
        seconds.setdefault(parts[3], []).append(value)
        units[parts[3]] = parts[2]
for event, values in seconds.items():
    kept = values[2:-2] if len(values) > 5 else values
    median = statistics.median(kept)
    unit = units[event]
    if event in ('user_time', 'system_time') or unit in ('ns',):
        print(f'{event} per-frame {median / fps / 1000:.1f} us ({len(kept)} s)' if fps else f'{event} {median}')
    elif unit == 'msec':
        print(f'{event} per-frame {median / fps * 1000:.1f} us ({len(kept)} s)' if fps else f'{event} {median}')
    else:
        print(f'{event} per-frame {median / fps:.1f} ({len(kept)} s)' if fps else f'{event} {median}')
