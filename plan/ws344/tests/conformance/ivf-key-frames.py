#!/usr/bin/env python3
# Takes every key frame of a VP8 IVF file (libvpx's test vectors) and writes each as a still WebP file: RIFF, WEBP
# and one 'VP8 ' chunk holding the frame (RFC 9649 2.5).  Frames that are not key frames, or not shown, are skipped
# (a still WebP is one shown key frame).  Run with python3 -I.
#   python3 -I plan/ws344/tests/conformance/ivf-key-frames.py IN.ivf OUTDIR PREFIX
# Writes OUTDIR/PREFIX-fNNN.webp and prints the count.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
import os
import struct
import sys


def main():
    source, outdir, prefix = sys.argv[1], sys.argv[2], sys.argv[3]
    data = open(source, "rb").read()
    if data[:4] != b"DKIF" or data[8:12] != b"VP80":
        raise SystemExit("ivf-key-frames: %s is not a VP8 IVF file" % source)
    header = struct.unpack("<H", data[6:8])[0]
    pos = header
    index = 0
    written = 0
    while pos + 12 <= len(data):
        size = struct.unpack("<I", data[pos:pos + 4])[0]
        frame = data[pos + 12:pos + 12 + size]
        pos += 12 + size
        if len(frame) < 10:
            break
        tag = frame[0] | (frame[1] << 8) | (frame[2] << 16)
        key = (tag & 1) == 0
        shown = (tag >> 4) & 1
        if key and shown and frame[3:6] == b"\x9d\x01\x2a":
            chunk = b"VP8 " + struct.pack("<I", len(frame)) + frame + (b"\0" if len(frame) & 1 else b"")
            riff = b"RIFF" + struct.pack("<I", 4 + len(chunk)) + b"WEBP" + chunk
            open(os.path.join(outdir, "%s-f%03d.webp" % (prefix, index)), "wb").write(riff)
            written += 1
        index += 1
    print("ivf-key-frames: %s %d key frames of %d" % (os.path.basename(source), written, index))


main()
