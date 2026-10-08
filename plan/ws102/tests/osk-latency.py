#!/usr/bin/env python3
"""The on-screen keyboard's L3 measurements (ws102-p010) from zdesktop's log.

Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

    plan/ws102/tests/osk-latency.py LOG [--check]

keyboard.c logs, for each key a release of the panel sends:
    KWL OSK latency send_us=N    the release taken to the key sent
    KWL OSK latency frame_us=N   the release taken to the next buffer the
                                 application it went to commits
and, for each slide of a panel (opening, leaving=0; closing, leaving=1):
    KWL OSK slide end leaving=L frames=F first_ms=A max_gap_ms=G
This prints each measurement's count, median, 95th percentile (nearest
rank) and largest, and the targets of design.md section 3 (L3):
    (a) send p95 <= 5 ms, frame p95 <= 50 ms
    (b) every opening slide's frames no more than 20 ms apart
With --check the exit status is 1 when a target is missed (or nothing was
measured); without it, 0.
"""

import re
import sys

SEND_TARGET_US = 5000
FRAME_TARGET_US = 50000
GAP_TARGET_MS = 20


def percentile(values, fraction):
	"""Gives the nearest-rank percentile of a list (None when empty)."""
	if not values:
		return None
	ordered = sorted(values)
	rank = max(1, -(-int(fraction * 1000) * len(ordered) // 1000))
	return ordered[min(rank, len(ordered)) - 1]


def summary(name, values, unit):
	"""Prints one measurement's count, median, p95 and largest."""
	if not values:
		print(f"{name}: none")
		return
	print(f"{name}: count={len(values)} median={percentile(values, 0.5)}{unit} "
	      f"p95={percentile(values, 0.95)}{unit} max={max(values)}{unit}")


def main():
	if len(sys.argv) < 2:
		raise SystemExit(__doc__)
	check = "--check" in sys.argv[2:]
	send = []
	frame = []
	slides = []
	with open(sys.argv[1], encoding="utf-8", errors="replace") as log:
		for line in log:
			match = re.search(r"KWL OSK latency send_us=(\d+)", line)
			if match:
				send.append(int(match.group(1)))
				continue
			match = re.search(r"KWL OSK latency frame_us=(\d+)", line)
			if match:
				frame.append(int(match.group(1)))
				continue
			match = re.search(r"KWL OSK slide end leaving=(\d+) frames=(\d+) first_ms=(\d+) max_gap_ms=(\d+)", line)
			if match:
				slides.append(tuple(int(group) for group in match.groups()))

	# The measurements.
	summary("send_us", send, "")
	summary("frame_us", frame, "")
	opening = [slide for slide in slides if slide[0] == 0]
	closing = [slide for slide in slides if slide[0] == 1]
	for kind, group in (("open", opening), ("close", closing)):
		summary(f"slide-{kind} max_gap_ms", [slide[3] for slide in group], "")
		summary(f"slide-{kind} first_ms", [slide[2] for slide in group], "")
		summary(f"slide-{kind} frames", [slide[1] for slide in group], "")

	# The targets.
	met = True
	p95 = percentile(send, 0.95)
	ok = p95 is not None and p95 <= SEND_TARGET_US
	met = met and ok
	print(f"target (a) send p95 <= {SEND_TARGET_US} us: {'met' if ok else 'missed'} ({p95})")
	p95 = percentile(frame, 0.95)
	ok = p95 is not None and p95 <= FRAME_TARGET_US
	met = met and ok
	print(f"target (a) frame p95 <= {FRAME_TARGET_US} us: {'met' if ok else 'missed'} ({p95})")
	worst = max((slide[3] for slide in opening), default=None)
	ok = worst is not None and worst <= GAP_TARGET_MS
	met = met and ok
	print(f"target (b) opening slide gaps <= {GAP_TARGET_MS} ms: {'met' if ok else 'missed'} ({worst})")
	if check and not met:
		return 1
	return 0


if __name__ == "__main__":
	sys.exit(main())
