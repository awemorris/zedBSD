#!/usr/bin/python3 -I
# Writes a copy of an Annex B H.265 bitstream (standard input to standard output) whose parameter sets before the
# first VCL NAL unit are in the order VPS, SPS, PPS (each type keeping its own order, so the last copy of an id
# still wins), after any access unit delimiter and before the other NAL units there.  Used only for ffmpeg's input
# when ffmpeg cannot read a PPS that comes before its SPS (its trace_headers and decoder need the SPS first; the
# specification does not: 7.3.2.3 reads no SPS value), so the values the first picture sees are the same.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

import sys


def units(data):
    """The NAL units (bytes after the start code, trailing zero bytes dropped)."""
    starts = []
    position = 0
    while True:
        found = data.find(b'\x00\x00\x01', position)
        if found < 0:
            break
        starts.append(found + 3)
        position = found + 3
    out = []
    for i, start in enumerate(starts):
        end = starts[i + 1] - 3 if i + 1 < len(starts) else len(data)
        unit = data[start:end]
        while unit.endswith(b'\x00') and i + 1 < len(starts):
            unit = unit[:-1]
        out.append(unit)
    return out


def main():
    data = sys.stdin.buffer.read()
    nal_units = units(data)
    first_vcl = len(nal_units)
    for i, unit in enumerate(nal_units):
        if unit and ((unit[0] >> 1) & 0x3F) < 32:
            first_vcl = i
            break
    head = nal_units[:first_vcl]
    kinds = [(u[0] >> 1) & 0x3F if u else -1 for u in head]
    ordered = [u for u, k in zip(head, kinds) if k == 35]
    for wanted in (32, 33, 34):
        ordered += [u for u, k in zip(head, kinds) if k == wanted]
    ordered += [u for u, k in zip(head, kinds) if k not in (32, 33, 34, 35)]
    out = sys.stdout.buffer
    for unit in ordered + nal_units[first_vcl:]:
        out.write(b'\x00\x00\x00\x01' + unit)
    return 0


if __name__ == '__main__':
    sys.exit(main())
