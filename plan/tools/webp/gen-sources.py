#!/usr/bin/env python3
# The source images of libwebp-compat's fixtures: deterministic RGBA PAM files of many sizes and kinds, written
# into OUTDIR as <kind>-<width>x<height>.pam.  Run with python3 -I.
#   python3 -I plan/tools/webp/gen-sources.py OUTDIR
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
import math
import os
import random
import sys

SIZES = [(1, 1), (1, 2), (2, 1), (3, 3), (7, 5), (15, 17), (16, 16), (17, 15), (31, 33), (64, 48), (100, 75),
         (256, 256), (333, 211)]
LARGE = [(1024, 683)]


def pam(path, width, height, pixels):
    header = b"P7\nWIDTH %d\nHEIGHT %d\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n" % (width, height)
    with open(path, "wb") as out:
        out.write(header + bytes(pixels))


def clamp(v):
    return 0 if v < 0 else 255 if v > 255 else int(v)


def make(kind, width, height):
    rnd = random.Random("%s-%d-%d" % (kind, width, height))
    pixels = bytearray()
    colors = [(rnd.randrange(256), rnd.randrange(256), rnd.randrange(256)) for _ in range(16)]
    for y in range(height):
        for x in range(width):
            fx = x / max(1, width - 1)
            fy = y / max(1, height - 1)
            a = 255
            if kind == "gradient":
                r, g, b = fx * 255, fy * 255, (1 - fx) * 255
            elif kind == "noise":
                r, g, b = rnd.randrange(256), rnd.randrange(256), rnd.randrange(256)
            elif kind == "photo":
                r = 128 + 100 * math.sin(fx * 6.3 + fy * 2) + rnd.gauss(0, 12)
                g = 128 + 90 * math.cos(fy * 5.1 - fx) + rnd.gauss(0, 12)
                b = 128 + 80 * math.sin((fx + fy) * 4.2) + rnd.gauss(0, 12)
            elif kind == "text":
                ink = ((x // 3 + y // 5) % 4 == 0) or ((x * 7 + y * 3) % 23 < 4)
                r, g, b = (20, 20, 30) if ink else (250, 250, 245)
            elif kind == "palette16":
                r, g, b = colors[((x // 4) * 5 + (y // 3) * 3) % 16]
            elif kind == "palette2":
                r, g, b = colors[((x // 2) + (y // 2)) % 2]
            elif kind == "alpha-gradient":
                r, g, b = 200 * fx + 30, 180, 255 * fy
                a = fx * 255
            elif kind == "alpha-cutout":
                r, g, b = 255 * fy, 60, 200
                a = 255 if (x - width / 2) ** 2 + (y - height / 2) ** 2 < (min(width, height) / 2.2) ** 2 else 0
            elif kind == "alpha-noise":
                r, g, b = rnd.randrange(256), rnd.randrange(256), rnd.randrange(256)
                a = rnd.randrange(256)
            elif kind == "alpha-zero":
                r, g, b = rnd.randrange(256), rnd.randrange(256), rnd.randrange(256)
                a = 0
            else:
                raise SystemExit("unknown kind " + kind)
            pixels += bytes((clamp(r), clamp(g), clamp(b), clamp(a)))
    return pixels


def main():
    outdir = sys.argv[1]
    os.makedirs(outdir, exist_ok=True)
    kinds = ["gradient", "noise", "photo", "text", "palette16", "palette2", "alpha-gradient", "alpha-cutout",
             "alpha-noise", "alpha-zero"]
    count = 0
    for kind in kinds:
        sizes = SIZES + (LARGE if kind in ("photo", "text", "alpha-cutout") else [])
        for width, height in sizes:
            pam(os.path.join(outdir, "%s-%dx%d.pam" % (kind, width, height)), width, height,
                make(kind, width, height))
            count += 1
    print("gen-sources: %d images" % count)


main()
