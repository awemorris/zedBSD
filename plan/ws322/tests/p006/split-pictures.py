#!/usr/bin/env python3
# The reference of an Annex B H.265 stream whose pictures change size (ws322-p006): ffmpeg scales the pictures of a
# later size to the first one's when it decodes a stream whole, so each picture is decoded alone, behind every
# parameter set of the stream (in the stream's order), and its NV12 SHA-256 printed, a line a picture (NONE for one
# ffmpeg gives no frame of).  For a stream whose parameter sets come before their use in another order, give it
# plan/ws324/tests/conformance/reorder-ps.py's copy.
#   python3 -I plan/ws322/tests/p006/split-pictures.py < STREAM.h265 > STREAM.sha256
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
import sys, subprocess, hashlib
data = sys.stdin.buffer.read()
# NAL units by start codes
starts = []
i = 0
while True:
    j = data.find(b'\x00\x00\x01', i)
    if j < 0: break
    starts.append(j + 3); i = j + 3
nals = []
for k, s in enumerate(starts):
    e = starts[k+1] - 3 if k + 1 < len(starts) else len(data)
    while e > s and data[e-1] == 0: e -= 1
    nals.append(data[s:e])
params = [n for n in nals if (n[0] >> 1) & 63 in (32, 33, 34)]
pictures = []
for n in nals:
    t = (n[0] >> 1) & 63
    if t < 32:
        if n[2] & 0x80: pictures.append([])
        if pictures: pictures[-1].append(n)
for pic in pictures:
    stream = b''.join(b'\x00\x00\x00\x01' + n for n in params + pic)
    out = subprocess.run(['ffmpeg','-hide_banner','-loglevel','error','-nostdin','-f','hevc','-i','pipe:0','-f','framehash','-hash','sha256','-pix_fmt','nv12','-'], input=stream, capture_output=True, timeout=60).stdout.decode()
    rows = [l.split(',')[-1].strip() for l in out.splitlines() if l and not l.startswith('#')]
    print(rows[0] if rows else 'NONE')
