#!/usr/bin/env python3
# Compares vkvideo-probe's raw H.265 frames (--raw=FILE: each frame's display window as raw NV12, output order) with a
# reference decode of the same stream (ffmpeg -i STREAM -pix_fmt nv12 -f rawvideo REF), frame by frame: whether the
# luma and chroma planes match, the bytes that differ, the PSNR of the luma, the first differing 64x64 block (column,
# row), the share of the luma that is zero, and the mean of both pictures' luma.
#   usage: h265-raw-compare.py GOT REF WIDTH HEIGHT [FRAMES]
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
import math
import sys


def main():
    got_path, ref_path = sys.argv[1], sys.argv[2]
    width, height = int(sys.argv[3]), int(sys.argv[4])
    limit = int(sys.argv[5]) if len(sys.argv) > 5 else 1 << 30
    luma = width * height
    frame = luma + width * (height // 2)
    with open(got_path, 'rb') as f:
        got = f.read()
    with open(ref_path, 'rb') as f:
        ref = f.read()
    count = min(len(got) // frame, len(ref) // frame, limit)
    print("frames: got %d, reference %d, compared %d (%dx%d)" % (len(got) // frame, len(ref) // frame, count, width, height))
    for index in range(count):
        a = got[index * frame:(index + 1) * frame]
        b = ref[index * frame:(index + 1) * frame]
        ya, yb = a[:luma], b[:luma]
        ca, cb = a[luma:], b[luma:]
        ydiff = sum(1 for x, y in zip(ya, yb) if x != y)
        cdiff = sum(1 for x, y in zip(ca, cb) if x != y)
        sse = sum((x - y) * (x - y) for x, y in zip(ya, yb))
        psnr = 99.0 if sse == 0 else 10.0 * math.log10(255.0 * 255.0 * luma / sse)
        first = '-'
        if ydiff:
            at = next(i for i in range(luma) if ya[i] != yb[i])
            first = '(%d,%d) px(%d,%d)' % ((at % width) // 64, (at // width) // 64, at % width, at // width)
        zeros = ya.count(0) * 100.0 / luma
        mean_a = sum(ya) / luma
        mean_b = sum(yb) / luma
        verdict = 'MATCH' if ydiff == 0 and cdiff == 0 else 'DIFFER'
        print("%3d %s luma %d diff, chroma %d diff, psnr %.2f, first block %s, zero %.1f%%, mean %.1f/%.1f" %
              (index, verdict, ydiff, cdiff, psnr, first, zeros, mean_a, mean_b))
        if ydiff:
            # Which 64x64 blocks differ, as a map of rows (. same, x differ).
            columns = (width + 63) // 64
            rows = (height + 63) // 64
            bad = [[False] * columns for _ in range(rows)]
            for i in range(luma):
                if ya[i] != yb[i]:
                    bad[(i // width) // 64][(i % width) // 64] = True
            if index < 3:
                for r in range(rows):
                    print("      " + ''.join('x' if v else '.' for v in bad[r]))


main()
