#!/usr/bin/env python3
"""ws100-p009: the volume curve, percent -> dB(p) = 50 * log10(p / 100) (25 % = -30.1, 50 % = -15.1, 75 % = -6.2 dB).

Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

    volume-curve.py table          audiod's factors (1/65536) for 0..100 %, as the C array in userland/base/audiod/mix.c
    volume-curve.py quarters       pci-hda's gains in quarter dB for 0..100 %, as the C array in src/drivers/pci/pci-hda.c
    volume-curve.py check STEPS STEP_DB
                                   the codec's steps for 25, 50, 75 and 100 % (pci-hda's integer mapping: 100 % is the
                                   top step, each percent STEPS + round(quarters / step) clamped to 0..STEPS) and their dB
                                   against the targets -30, -15, -7 and 0 dB +- 3 dB
    volume-curve.py verify FILE... the arrays in the files are the ones this script makes
    volume-curve.py wav WAV.txt    the peaks of hda-wav-check.py's windows (100, 75, 50, 25 % in that order) in dB against
                                   the targets (audiod's own volume, QEMU's codec without a mixer)
    volume-curve.py dmesg FILE     pci-hda's lines ("hda: volume nid … steps S step Q/4 dB …" and "hda: volume P% -> step
                                   N/S"): the steps it chose for 25, 50, 75 and 100 % are the curve's, and their dB in range

Each prints "volume-curve: PASS" or "volume-curve: FAIL" last.
"""
import math
import re
import sys

TARGETS = {25: -30.0, 50: -15.0, 75: -7.0, 100: 0.0}
TOLERANCE = 3.0


def decibels(percent):
	"""The curve's gain of a percent in dB (None for 0: silence)."""
	if percent <= 0:
		return None
	return 50.0 * math.log10(percent / 100.0)


def factors():
	"""audiod's factors in 1/65536, 0..100 %."""
	values = []
	for percent in range(101):
		gain = decibels(percent)
		values.append(0 if gain is None else int(round(65536.0 * 10.0 ** (gain / 20.0))))
	return values


def quarters():
	"""pci-hda's gains in quarter dB (0 or below), 0..100 %; 0 % is the lowest the table holds (muted by the caller)."""
	values = []
	for percent in range(101):
		gain = decibels(percent)
		values.append(-400 if gain is None else int(round(4.0 * gain)))
	return values


def c_array(values, per_line):
	"""The values as the lines of a C array's body."""
	lines = []
	for start in range(0, len(values), per_line):
		lines.append("\t" + ", ".join(str(v) + ("U" if v >= 0 and per_line == 8 else "") for v in values[start:start + per_line]) + ",")
	return "\n".join(lines)


def step_of(percent, steps, step_quarters):
	"""pci-hda's step for a percent (as hda_volume_write computes it, in integers)."""
	if percent >= 100:
		return steps
	down = (-quarters()[percent] + step_quarters // 2) // step_quarters
	if down > steps:
		return 0
	return steps - down


def check(steps, step_db):
	"""The codec's steps of the targets' percents and their dB."""
	step_quarters = int(round(step_db * 4.0))
	ok = True
	for percent, target in sorted(TARGETS.items()):
		step = step_of(percent, steps, step_quarters)
		gain = (step - steps) * step_quarters / 4.0
		good = abs(gain - target) <= TOLERANCE
		ok = ok and good
		print(f"{percent:3d} % -> step {step}/{steps}: {gain:+.2f} dB (target {target:+.0f}) {'ok' if good else 'FAIL'}")
	return ok


def verify(paths):
	"""The arrays in the files against this script's."""
	ok = True
	wanted = {"volume_factors": factors(), "hda_volume_quarters": quarters()}
	for path in paths:
		text = open(path, encoding="utf-8").read()
		for name, values in wanted.items():
			match = re.search(name + r"\[[^\]]*\]\s*=\s*\{(.*?)\};", text, re.S)
			if not match:
				continue
			got = [int(v.rstrip("Uu")) for v in re.findall(r"-?\d+U?", match.group(1))]
			good = got == values
			ok = ok and good
			print(f"{path}: {name}: {len(got)} values {'ok' if good else 'FAIL (differs from the script)'}")
	return ok


def wav(path):
	"""The four sounds' peaks (100, 75, 50, 25 %) in dB below the first."""
	line = [l for l in open(path, encoding="utf-8").read().splitlines() if l.startswith("windows:")]
	if not line:
		print("no windows: line")
		return False
	windows = list(enumerate(int(v) for v in re.findall(r"-?\d+", line[0])))
	# The sounds: runs of windows above a floor, the largest peak of each.
	peaks = []
	last = None
	for index, peak in windows:
		if peak <= 20:
			continue
		if last is None or index > last + 1:
			peaks.append(peak)
		else:
			peaks[-1] = max(peaks[-1], peak)
		last = index
	if len(peaks) != 4:
		print(f"sounds: {peaks} (4 expected)")
		return False
	ok = True
	for percent, peak in zip((100, 75, 50, 25), peaks):
		gain = 20.0 * math.log10(peak / peaks[0])
		good = abs(gain - TARGETS[percent]) <= TOLERANCE
		ok = ok and good
		print(f"{percent:3d} %: peak {peak}, {gain:+.2f} dB (target {TARGETS[percent]:+.0f}) {'ok' if good else 'FAIL'}")
	return ok


def dmesg(path):
	"""pci-hda's chosen steps (from its log) against the curve's."""
	text = open(path, encoding="utf-8", errors="replace").read()
	caps = re.findall(r"hda: volume nid \d+ steps (\d+) step (\d+)/4 dB", text)
	if not caps:
		print("no 'hda: volume nid' line")
		return False
	steps, step_quarters = int(caps[-1][0]), int(caps[-1][1])
	print(f"codec: {steps} steps of {step_quarters / 4.0:.2f} dB")
	chosen = {}
	for percent, step, total in re.findall(r"hda: volume (\d+)% -> step (\d+)/(\d+)", text):
		chosen[int(percent)] = int(step)
	ok = True
	for percent, target in sorted(TARGETS.items()):
		if percent not in chosen:
			print(f"{percent:3d} %: no line")
			ok = False
			continue
		want = step_of(percent, steps, step_quarters)
		gain = (chosen[percent] - steps) * step_quarters / 4.0
		good = chosen[percent] == want and abs(gain - target) <= TOLERANCE
		ok = ok and good
		print(f"{percent:3d} % -> step {chosen[percent]}/{steps} (curve {want}): {gain:+.2f} dB (target {target:+.0f}) {'ok' if good else 'FAIL'}")
	return ok


def main():
	"""Runs one command."""
	if len(sys.argv) < 2:
		print(__doc__)
		return 2
	command = sys.argv[1]
	if command == "table":
		print(c_array(factors(), 8))
		return 0
	if command == "quarters":
		print(c_array(quarters(), 10))
		return 0
	if command == "check" and len(sys.argv) == 4:
		ok = check(int(sys.argv[2]), float(sys.argv[3]))
	elif command == "verify" and len(sys.argv) >= 3:
		ok = verify(sys.argv[2:])
	elif command == "wav" and len(sys.argv) == 3:
		ok = wav(sys.argv[2])
	elif command == "dmesg" and len(sys.argv) == 3:
		ok = dmesg(sys.argv[2])
	else:
		print(__doc__)
		return 2
	print("volume-curve: PASS" if ok else "volume-curve: FAIL")
	return 0 if ok else 1


if __name__ == "__main__":
	sys.exit(main())
