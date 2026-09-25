#!/usr/bin/env python3
"""Checks a guest's sense of time against the host's clock.

Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

WS040 p005.  The guest must be at its login prompt on a serial console
(CONFIG_PCAT_SERIAL_MIRROR=y).  Every duration the guest reports is compared
with the host's monotonic clock, because the guest's own clock is the thing
under test.

    clock-check.py --socket S [--sleep 5] [--spin 20000]

Checks:
  clk_tck   getconf CLK_TCK is the fixed times(2) unit, 100.
  sleep     `sleep N` takes N seconds of host time (minus a `sleep 0`).
  time      /bin/time reports about N seconds of real time for `sleep N`.
  times     CPU time from the sh `times` builtin (times(2)) for a busy loop
            in a child shell is no more than the host time the loop took, and not far below
            it (the guest was busy the whole while).
"""
from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
import time

SERIAL = "plan/tools/guest/serial.py"


def run(socket_path: str, command: str, timeout: float) -> tuple[str, float]:
	"""Runs one guest command, returning its output and the host time taken."""
	started = time.monotonic()
	result = subprocess.run(
		[sys.executable, SERIAL, "--socket", socket_path,
		 "--timeout", str(timeout), "run", command],
		capture_output=True, text=True, timeout=timeout + 30)
	elapsed = time.monotonic() - started
	if result.returncode != 0:
		raise SystemExit(f"guest command failed ({result.returncode}): {command}\n"
				 f"{result.stdout}{result.stderr}")
	return result.stdout, elapsed


def seconds(text: str) -> float:
	"""Parses one `XmY.ZZs` field of the sh times builtin."""
	match = re.fullmatch(r"(\d+)m(\d+)\.(\d+)s", text)
	if match is None:
		raise SystemExit(f"unexpected times field: {text!r}")
	return int(match.group(1)) * 60 + int(match.group(2)) + int(match.group(3)) / 100


def main() -> int:
	parser = argparse.ArgumentParser()
	parser.add_argument("--socket", required=True)
	parser.add_argument("--sleep", type=float, default=5.0)
	parser.add_argument("--spin", type=int, default=20000,
			    help="iterations of the shell busy loop")
	parser.add_argument("--timeout", type=float, default=240.0)
	args = parser.parse_args()
	report: dict[str, object] = {}
	failed = []

	out, _ = run(args.socket, "getconf CLK_TCK", args.timeout)
	report["clk_tck"] = out.strip()
	if out.strip() != "100":
		failed.append("clk_tck")

	# The overhead of one round trip over the serial line.
	_, base = run(args.socket, "sleep 0", args.timeout)
	_, slept = run(args.socket, f"sleep {args.sleep:g}", args.timeout)
	report["sleep_host_s"] = round(slept - base, 3)
	if abs((slept - base) - args.sleep) > 0.1 * args.sleep + 0.5:
		failed.append("sleep")

	out, _ = run(args.socket, f"time sleep {args.sleep:g} 2>&1", args.timeout)
	match = re.search(r"real\s+([0-9.]+)", out)
	report["time_real_s"] = float(match.group(1)) if match else out.strip()
	if match is None or abs(float(match.group(1)) - args.sleep) > 0.1 * args.sleep + 0.5:
		failed.append("time")

	# The guest's awk and bc have no loops; a child shell does the work.
	# The shell's child totals add up over the session, so they are read
	# before and after the loop and only the difference counts.
	command = ("times; "
		   f"sh -c 'i=0; while [ $i -lt {args.spin} ]; do i=$((i+1)); done'; "
		   "times")
	out, spun = run(args.socket, command, args.timeout)
	lines = [line.split() for line in out.strip().splitlines() if line.strip()]
	if len(lines) != 4 or any(len(line) != 2 for line in lines):
		raise SystemExit(f"unexpected times output:\n{out}")
	child_cpu = (seconds(lines[3][0]) + seconds(lines[3][1]) -
		     seconds(lines[1][0]) - seconds(lines[1][1]))
	busy = spun - base
	report["spin_host_s"] = round(busy, 3)
	report["spin_child_cpu_s"] = round(child_cpu, 3)
	# CPU time can neither exceed the time that passed nor be a small part of
	# it.  It is sampled a tick at a time, and the host's figure has the
	# serial round trip taken out, so each side gets some slack.
	if child_cpu > 1.05 * busy + 0.2 or child_cpu < 0.5 * busy:
		failed.append("times")

	report["failed"] = failed
	print(json.dumps(report, indent=1))
	return 1 if failed else 0


if __name__ == "__main__":
	sys.exit(main())
