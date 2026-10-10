#!/usr/bin/env python3
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
"""Compare native diagnostic RMS rows against independent normalized PCM."""
import argparse
from pathlib import Path
import re

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("reference", type=Path)
parser.add_argument("actual", type=Path)
args = parser.parse_args()
pattern = re.compile(r"RMS frame=(\d+) count=(\d+) left=([\d.]+) right=([\d.]+)")

def rows(path):
    result = []
    for line in path.read_text().splitlines():
        match = pattern.fullmatch(line)
        if match is None:
            raise SystemExit(f"malformed RMS row: {path}")
        result.append((int(match[1]), int(match[2]), float(match[3]), float(match[4])))
    return result

reference = rows(args.reference)
actual = rows(args.actual)
if len(reference) != len(actual):
    raise SystemExit("RMS block count mismatch")
maximum = 0.0
for expected, observed in zip(reference, actual):
    if expected[:2] != observed[:2]:
        raise SystemExit("RMS block position/count mismatch")
    for a, b in zip(expected[2:], observed[2:]):
        difference = abs(a - b)
        limit = 1e-6 if a < 1e-4 else a * 1e-3
        maximum = max(maximum, difference)
        if difference > limit:
            raise SystemExit(f"RMS exceeds tolerance at frame {expected[0]}: {difference}")
print(f"Native probe RMS PASS blocks={len(actual)} maximum_absolute_difference={maximum:.9g}")
