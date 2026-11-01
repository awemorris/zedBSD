#!/usr/bin/env python3
# zedBSD
# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
"""Compares two reports of plan/ws074/tests/run-wpt-reftests.py (the host's and Wine's) test by test.

  compare-reftests.py NAME HOST-report.json WINE-report.json

Prints one line: the counts of each status on both sides, how many tests have another status on the other side,
and how many have another number of different pixels in any reference (the pictures differ though the verdict
is the same).  Exits 1 when a test has another status.
"""

import json
import sys


def load(path):
    report = json.load(open(path, encoding="utf-8"))
    return {row["test"]: row for row in report["results"]}


def counts(rows):
    total = {}
    for row in rows.values():
        total[row["status"]] = total.get(row["status"], 0) + 1
    return dict(sorted(total.items()))


def main():
    name, host_path, wine_path = sys.argv[1:4]
    host = load(host_path)
    wine = load(wine_path)
    status = [test for test in host if wine.get(test, {}).get("status") != host[test]["status"]]
    pixels = []
    for test in host:
        if test in wine and test not in status:
            a = [reference.get("different_pixels") for reference in host[test].get("references", [])]
            b = [reference.get("different_pixels") for reference in wine[test].get("references", [])]
            if a != b:
                pixels.append(test)
    print("%s: %d tests; host %s; Wine %s; other status %d%s; other pixel counts %d%s"
          % (name, len(host), counts(host), counts(wine), len(status), (" " + str(status)) if status else "",
             len(pixels), (" " + str(pixels)) if pixels else ""))
    return 1 if status else 0


if __name__ == "__main__":
    sys.exit(main())
