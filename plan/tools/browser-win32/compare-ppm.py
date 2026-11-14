#!/usr/bin/env python3
# zedBSD
# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
"""Compares two pictures (PPM) that two builds of the browser drew of one page.

  compare-ppm.py NAME A.ppm B.ppm

Prints one line: the name, the sizes, the number of pixels that differ and the largest difference of any
channel.  The words "same" (every pixel equal), "close" (no channel differs by more than 2, the limit
plan/ws074/tests/gpu-compare.py uses) or "DIFFER".
"""

import sys

from PIL import Image, ImageChops


def main():
    name, first, second = sys.argv[1:4]
    a = Image.open(first).convert("RGB")
    b = Image.open(second).convert("RGB")
    if a.size != b.size:
        print("%s: sizes %dx%d and %dx%d: DIFFER" % (name, a.size[0], a.size[1], b.size[0], b.size[1]))
        return 1
    difference = ImageChops.difference(a, b)
    largest = 0
    count = 0
    for pixel in difference.getdata():
        channel = max(pixel)
        if channel > 0:
            count += 1
        if channel > largest:
            largest = channel
    if count == 0:
        verdict = "same"
    elif largest <= 2:
        verdict = "close"
    else:
        verdict = "DIFFER"
    print("%s: %dx%d, %d pixels differ, largest channel difference %d: %s"
          % (name, a.size[0], a.size[1], count, largest, verdict))
    return 0 if verdict != "DIFFER" else 1


if __name__ == "__main__":
    sys.exit(main())
