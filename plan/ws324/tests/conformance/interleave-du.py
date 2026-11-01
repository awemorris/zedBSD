#!/usr/bin/python3 -I
# Writes a copy of an Annex B H.265 bitstream (standard input to standard output) with NAL units placed between the
# slice segments of each picture, as 7.4.2.4.4 allows and as no HEVC_v1 conformance bitstream has them: before
# segment k (k >= 1) of a picture, a prefix SEI NAL unit with one decoding unit information SEI message (D.2.21,
# payloadType 130, decoding_unit_idx k, dpb_output_du_delay_present_flag 0; the SPS must have
# sub_pic_cpb_params_in_pic_timing_sei_flag 1, so du_spt_cpb_removal_delay_increment is absent), and before segment 1
# also a copy of the last PPS NAL unit before the picture (the same content, so 7.4.3.3.1 holds).  Made for
# HRD_A_Fujitsu_3 (4 decoding units per access unit, sub_pic_cpb_params_in_pic_timing_sei_flag 1).
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


def escape(rbsp):
    """Emulation prevention (7.4.2): 0x03 after two zero bytes when the next byte is 0..3."""
    out = bytearray()
    zeros = 0
    for byte in rbsp:
        if zeros >= 2 and byte <= 3:
            out.append(3)
            zeros = 0
        out.append(byte)
        zeros = zeros + 1 if byte == 0 else 0
    return bytes(out)


def du_info_sei(index, temporal_id):
    """A prefix SEI NAL unit holding decoding_unit_info( index ) and rbsp_trailing_bits()."""
    bits = []
    code = index + 1
    length = code.bit_length() - 1
    bits += [0] * length + [int(b) for b in bin(code)[2:]]
    bits.append(0)
    if len(bits) % 8:
        bits.append(1)
        while len(bits) % 8:
            bits.append(0)
    payload = bytes(int(''.join(str(b) for b in bits[i:i + 8]), 2) for i in range(0, len(bits), 8))
    rbsp = bytes([130, len(payload)]) + payload + b'\x80'
    header = bytes([39 << 1, temporal_id + 1])
    return header + escape(rbsp)


def main():
    data = sys.stdin.buffer.read()
    out = sys.stdout.buffer
    last_pps = None
    segment = 0
    for unit in units(data):
        kind = (unit[0] >> 1) & 0x3F
        if kind == 34:
            last_pps = unit
        if kind < 32 and len(unit) > 2:
            segment = 0 if unit[2] >> 7 else segment + 1
            if segment >= 1:
                if segment == 1 and last_pps is not None:
                    out.write(b'\x00\x00\x01' + last_pps)
                out.write(b'\x00\x00\x01' + du_info_sei(segment, (unit[1] & 7) - 1))
        out.write(b'\x00\x00\x00\x01' + unit)
    return 0


if __name__ == '__main__':
    sys.exit(main())
