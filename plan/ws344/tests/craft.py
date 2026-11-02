#!/usr/bin/env python3
# Container variants of a WebP file for libwebp-compat's fixtures, made from a simple-format file (RIFF + one 'VP8 '
# or 'VP8L' chunk) or an extended one with ALPH, so that every layout RFC 9649 section 2 allows (and a few it does
# not) is decoded by both libwebp and libwebp-compat.  Run with python3 -I.
#   python3 -I plan/ws344/tests/craft.py OP IN.webp OUT.webp
# OP: bare (the bitstream alone), barechunk (its chunk without RIFF), trailing (bytes after File Size), extend (a
# VP8X chunk added), extend-unknown (VP8X, an odd-sized unknown chunk before and one after the bitstream),
# extend-meta (VP8X with I, E and X set; ICCP before, EXIF and XMP after), two-alph (a second, bogus ALPH before the
# real one; libwebp takes the last), canvas-mismatch (VP8X canvas one pixel wider than the bitstream),
# vp8x-short (a VP8X of 9 bytes), riff-long (File Size 2 more than the file holds).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
import struct
import sys


def chunks(data):
    if data[:4] != b"RIFF" or data[8:12] != b"WEBP":
        raise SystemExit("craft: not a RIFF WebP file")
    size = struct.unpack("<I", data[4:8])[0]
    pos = 12
    end = 8 + size
    out = []
    while pos + 8 <= end:
        tag = data[pos:pos + 4]
        length = struct.unpack("<I", data[pos + 4:pos + 8])[0]
        out.append((tag, data[pos + 8:pos + 8 + length]))
        pos += 8 + length + (length & 1)
    return out


def chunk(tag, payload):
    return tag + struct.pack("<I", len(payload)) + payload + (b"\0" if len(payload) & 1 else b"")


def riff(body):
    return b"RIFF" + struct.pack("<I", 4 + len(body)) + b"WEBP" + body


def size_of(tag, payload):
    if tag == b"VP8 ":
        return (struct.unpack("<H", payload[6:8])[0] & 0x3fff, struct.unpack("<H", payload[8:10])[0] & 0x3fff)
    if tag == b"VP8L":
        bits = struct.unpack("<I", payload[1:5])[0]
        return ((bits & 0x3fff) + 1, ((bits >> 14) & 0x3fff) + 1)
    raise SystemExit("craft: no bitstream")


def vp8x(flags, width, height):
    payload = bytes((flags, 0, 0, 0)) + struct.pack("<I", width - 1)[:3] + struct.pack("<I", height - 1)[:3]
    return chunk(b"VP8X", payload)


def main():
    op, source, target = sys.argv[1], sys.argv[2], sys.argv[3]
    data = open(source, "rb").read()
    parts = chunks(data)
    stream = [p for p in parts if p[0] in (b"VP8 ", b"VP8L")][0]
    alph = [p for p in parts if p[0] == b"ALPH"]
    width, height = size_of(*stream)
    lossless_alpha = stream[0] == b"VP8L" and (stream[1][4] >> 4) & 1
    alpha_flag = 0x10 if (alph or lossless_alpha) else 0
    if op == "bare":
        out = stream[1]
    elif op == "barechunk":
        out = chunk(*stream)
    elif op == "trailing":
        out = data + b"\x13\x37trailing bytes after the RIFF data"
    elif op == "extend":
        out = riff(vp8x(alpha_flag, width, height) + b"".join(chunk(*p) for p in alph) + chunk(*stream))
    elif op == "extend-unknown":
        out = riff(vp8x(alpha_flag, width, height) + chunk(b"XYZW", b"odd") + b"".join(chunk(*p) for p in alph) +
                   chunk(*stream) + chunk(b"ABCD", b"after the bitstream"))
    elif op == "extend-meta":
        out = riff(vp8x(alpha_flag | 0x20 | 0x08 | 0x04, width, height) + chunk(b"ICCP", b"\0" * 21) +
                   b"".join(chunk(*p) for p in alph) + chunk(*stream) + chunk(b"EXIF", b"Exif\0\0MM") +
                   chunk(b"XMP ", b"<x:xmpmeta/>"))
    elif op == "two-alph":
        if not alph:
            raise SystemExit("craft: two-alph needs an ALPH chunk")
        bogus = bytes((0,)) + b"\x55" * (width * height)
        out = riff(vp8x(0x10, width, height) + chunk(b"ALPH", bogus) + chunk(*alph[-1]) + chunk(*stream))
    elif op == "canvas-mismatch":
        out = riff(vp8x(alpha_flag, width + 1, height) + b"".join(chunk(*p) for p in alph) + chunk(*stream))
    elif op == "vp8x-short":
        bad = b"VP8X" + struct.pack("<I", 9) + bytes((alpha_flag, 0, 0, 0)) + b"\0" * 5 + b"\0"
        out = riff(bad + chunk(*stream))
    elif op == "riff-long":
        out = b"RIFF" + struct.pack("<I", len(data) - 8 + 2) + data[8:]
    else:
        raise SystemExit("craft: unknown op " + op)
    open(target, "wb").write(out)


main()
