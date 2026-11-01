# Prints the FNV-1a 32 of every frame of a raw NV12 file, one hex per line (ws322-p002).
#   python3 -I fnv-frames.py FILE WIDTHxHEIGHT
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
import sys

path, size = sys.argv[1], sys.argv[2]
width, height = (int(v) for v in size.split('x'))
frame = width * height * 3 // 2
data = open(path, 'rb').read()
for start in range(0, len(data) - frame + 1, frame):
    h = 0x811c9dc5
    for b in data[start:start + frame]:
        h = ((h ^ b) * 0x01000193) & 0xffffffff
    print('%08x' % h)
