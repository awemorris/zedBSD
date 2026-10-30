#!/usr/bin/env python3
"""ws291-p022: sums up the GT samples 5330-mview.sh takes during one mview run.

Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

    telemetry-sum.py zedbsd FILE   lines of `sysctl hw.gputelemetry` (gpu=0): cur_mhz, and with the diagnostic
                                   kernel (i915-stats-diag.patch) objects_bytes = the package energy status (low
                                   word) and its unit register (high word), objects_limit = GT0_PERF_LIMIT_REASONS,
                                   min_mhz/max_mhz = the package power limit registers
    telemetry-sum.py linux FILE    lines "ns act_mhz energy_uj" (intel-rapl:0)

Prints the most frequent actual frequencies, the package power over the busy samples and the limit reasons.
"""

import collections
import re
import sys


def zedbsd(path):
	rows = []
	for line in open(path):
		fields = dict(re.findall(r'(\w+)=(\S+)', line))
		if 'time_ns' in fields:
			rows.append(fields)
	freqs = collections.Counter(int(r.get('cur_mhz', 0)) for r in rows)
	out = 'cur_mhz ' + ' '.join(f'{f}x{n}' for f, n in freqs.most_common(4))
	busy = [r for r in rows if int(r.get('cur_mhz', 0)) > 0 and int(r.get('objects_bytes', 0)) != 0]
	if len(busy) >= 2:
		first, last = busy[0], busy[-1]
		unit = (int(first['objects_bytes']) >> 32 >> 8) & 0x1f
		joules = ((int(last['objects_bytes']) - int(first['objects_bytes'])) & 0xffffffff) / float(1 << unit)
		seconds = (int(last['time_ns']) - int(first['time_ns'])) / 1e9
		if seconds > 0:
			out += f' package {joules / seconds:.1f} W'
		limit = int(first.get('min_mhz', 0))
		out += f' pl1 {(limit & 0x7fff) / 8.0:.1f} W(raw 0x{limit:08x}) reasons 0x{int(first.get("objects_limit", 0)):08x}'
	return out


def linux(path):
	rows = [line.split() for line in open(path) if len(line.split()) == 3]
	freqs = collections.Counter(int(r[1]) for r in rows)
	out = 'act_mhz ' + ' '.join(f'{f}x{n}' for f, n in freqs.most_common(4))
	busy = [r for r in rows if int(r[1]) > 0]
	if len(busy) >= 2:
		joules = (int(busy[-1][2]) - int(busy[0][2])) / 1e6
		seconds = (int(busy[-1][0]) - int(busy[0][0])) / 1e9
		if seconds > 0:
			out += f' package {joules / seconds:.1f} W'
	return out


def main():
	if len(sys.argv) != 3 or sys.argv[1] not in ('zedbsd', 'linux'):
		print(__doc__.strip().splitlines()[3], file=sys.stderr)
		return 2
	print(zedbsd(sys.argv[2]) if sys.argv[1] == 'zedbsd' else linux(sys.argv[2]))
	return 0


if __name__ == '__main__':
	sys.exit(main())
