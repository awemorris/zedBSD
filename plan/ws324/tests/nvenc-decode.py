#!/usr/bin/python3 -I
# The video encoder's self-test, checked on the host: the bitstreams the nvrtx driver's nvenc-test logged are put
# back together from the kernel log (each line and each run checked by its FNV-1a hash), given the SPS and PPS the
# test's setup describes (written here: Main, level 3.0, 256x128, CAVLC, POC type 0, QP 26), decoded by ffmpeg with
# errors fatal, and compared with the pattern the test wrote (16x16 blocks of one luma value each, one chroma
# colour; the P picture the I picture moved right by one block).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# usage: plan/ws324/tests/nvenc-decode.py LOG OUTDIR
#   LOG is the driver's lines (dmesg | grep nvrtx); OUTDIR gets per choice c<N>.264, c<N>.yuv, c<N>-I.png, c<N>-P.png,
#   pattern-I.png, pattern-P.png, ffmpeg's messages and the slice headers ffmpeg read; the summary is printed.
# Exit status 0 when some choice decoded without an error and both pictures' luma PSNR are at least 35 dB.
import math
import os
import re
import subprocess
import sys

WIDTH, HEIGHT, TILE = 256, 128, 16
CB, CR = 100, 150
PSNR_MIN = 35.0


def fnv(data):
    h = 2166136261
    for b in data:
        h = ((h ^ b) * 16777619) & 0xffffffff
    return h


def read_log(path):
    runs, sums, bad = {}, {}, []
    dump = re.compile(r'nvenc-dump (\S+) ([0-9a-f]{5}) ([0-9a-f]*) ([0-9a-f]{8})')
    total = re.compile(r'nvenc-sum (\S+) (\d+) ([0-9a-f]{8})')
    with open(path, errors='replace') as f:
        for line in f:
            m = dump.search(line)
            if m:
                tag, off, data, h = m.group(1), int(m.group(2), 16), bytes.fromhex(m.group(3)), int(m.group(4), 16)
                if fnv(data) != h:
                    bad.append('%s line %05x hash' % (tag, off))
                runs.setdefault(tag, {})[off] = data
                continue
            m = total.search(line)
            if m:
                sums[m.group(1)] = (int(m.group(2)), int(m.group(3), 16))
    out = {}
    for tag, (length, h) in sums.items():
        parts = runs.get(tag, {})
        data = b''.join(parts[k] for k in sorted(parts))
        if len(data) != length or fnv(data) != h:
            bad.append('%s run: %d bytes of %d, hash %08x of %08x' % (tag, len(data), length, fnv(data), h))
        out[tag] = data
    return out, bad


class Bits:
    def __init__(self):
        self.bits = []

    def u(self, n, v):
        for i in range(n - 1, -1, -1):
            self.bits.append((v >> i) & 1)

    def ue(self, v):
        v += 1
        n = v.bit_length()
        self.u(n - 1, 0)
        self.u(n, v)

    def se(self, v):
        self.ue(2 * v - 1 if v > 0 else -2 * v)

    def rbsp(self):
        self.bits.append(1)
        while len(self.bits) % 8:
            self.bits.append(0)
        return bytes(int(''.join(map(str, self.bits[i:i + 8])), 2) for i in range(0, len(self.bits), 8))


def nal(header, rbsp):
    out = bytearray(b'\x00\x00\x00\x01' + bytes([header]))
    zeros = 0
    for b in rbsp:
        if zeros >= 2 and b <= 3:
            out.append(3)
            zeros = 0
        out.append(b)
        zeros = zeros + 1 if b == 0 else 0
    return bytes(out)


def parameter_sets():
    s = Bits()
    s.u(8, 77)        # profile_idc: Main
    s.u(8, 0)         # constraint flags
    s.u(8, 30)        # level_idc
    s.ue(0)           # seq_parameter_set_id
    s.ue(0)           # log2_max_frame_num_minus4
    s.ue(0)           # pic_order_cnt_type
    s.ue(0)           # log2_max_pic_order_cnt_lsb_minus4
    s.ue(1)           # max_num_ref_frames
    s.u(1, 0)         # gaps_in_frame_num_value_allowed_flag
    s.ue(WIDTH // 16 - 1)
    s.ue(HEIGHT // 16 - 1)
    s.u(1, 1)         # frame_mbs_only_flag
    s.u(1, 1)         # direct_8x8_inference_flag
    s.u(1, 0)         # frame_cropping_flag
    s.u(1, 0)         # vui_parameters_present_flag
    p = Bits()
    p.ue(0)           # pic_parameter_set_id
    p.ue(0)           # seq_parameter_set_id
    p.u(1, 0)         # entropy_coding_mode_flag: CAVLC
    p.u(1, 0)         # bottom_field_pic_order_in_frame_present_flag
    p.ue(0)           # num_slice_groups_minus1
    p.ue(0)           # num_ref_idx_l0_default_active_minus1
    p.ue(0)           # num_ref_idx_l1_default_active_minus1
    p.u(1, 0)         # weighted_pred_flag
    p.u(2, 0)         # weighted_bipred_idc
    p.se(0)           # pic_init_qp_minus26
    p.se(0)           # pic_init_qs_minus26
    p.se(0)           # chroma_qp_index_offset
    p.u(1, 0)         # deblocking_filter_control_present_flag
    p.u(1, 0)         # constrained_intra_pred_flag
    p.u(1, 0)         # redundant_pic_cnt_present_flag
    return nal(0x67, s.rbsp()) + nal(0x68, p.rbsp())


def pattern(shift):
    bx = WIDTH // TILE
    y = bytearray(WIDTH * HEIGHT)
    for row in range(HEIGHT):
        for col in range(WIDTH):
            src = (col // TILE + bx - shift % bx) % bx
            y[row * WIDTH + col] = 32 + (src * 37 + (row // TILE) * 71) % 192
    return bytes(y)


def psnr(a, b):
    se = sum((x - y) * (x - y) for x, y in zip(a, b))
    if se == 0:
        return float('inf')
    return 10 * math.log10(255 * 255 * len(a) / se)


def tiles_of(plane, width, count):
    """The first count 16x16 tiles of a linear plane, each 256 bytes row by row."""
    out = bytearray()
    for t in range(count):
        for row in range(TILE):
            start = row * width + t * TILE
            out += plane[start:start + TILE]
    return bytes(out)


def save_png(path, luma, cb, cr):
    from PIL import Image
    img = Image.new('RGB', (WIDTH, HEIGHT))
    px = img.load()
    for row in range(HEIGHT):
        for col in range(WIDTH):
            yy = luma[row * WIDTH + col]
            u = cb[(row // 2) * (WIDTH // 2) + col // 2] - 128
            v = cr[(row // 2) * (WIDTH // 2) + col // 2] - 128
            r = yy + 1.402 * v
            g = yy - 0.344136 * u - 0.714136 * v
            b = yy + 1.772 * u
            px[col, row] = tuple(max(0, min(255, int(round(c)))) for c in (r, g, b))
    img.save(path)


def main():
    if len(sys.argv) != 3:
        sys.exit('usage: nvenc-decode.py LOG OUTDIR')
    log, outdir = sys.argv[1], sys.argv[2]
    os.makedirs(outdir, exist_ok=True)
    runs, bad = read_log(log)
    for b in bad:
        print('log damaged: ' + b)
    chroma_size = (WIDTH // 2) * (HEIGHT // 2)
    flat_cb, flat_cr = bytes([CB]) * chroma_size, bytes([CR]) * chroma_size
    ref = {'I': pattern(0), 'P': pattern(1)}
    for kind in 'IP':
        save_png(os.path.join(outdir, 'pattern-%s.png' % kind), ref[kind], flat_cb, flat_cr)
    choices = sorted({int(t[1:].split('-')[0]) for t in runs if t.endswith('-bits')})
    passed = False
    for c in choices:
        bi, bp = runs.get('c%d-I-bits' % c, b''), runs.get('c%d-P-bits' % c, b'')
        print('choice %d: I %d bytes (head %s), P %d bytes (head %s)' % (c, len(bi), bi[:8].hex(), len(bp), bp[:8].hex()))
        if not bi:
            continue
        stream = bytearray(parameter_sets())
        for kind, bits in (('I', bi), ('P', bp)):
            if not bits:
                continue
            if not (bits.startswith(b'\x00\x00\x01') or bits.startswith(b'\x00\x00\x00\x01')):
                print('  %s: no start code at the start; one put before it' % kind)
                stream += b'\x00\x00\x00\x01'
            stream += bits
        es = os.path.join(outdir, 'c%d.264' % c)
        with open(es, 'wb') as f:
            f.write(stream)
        trace = subprocess.run(['ffmpeg', '-hide_banner', '-v', 'trace', '-i', es, '-c:v', 'copy', '-bsf:v', 'trace_headers',
                                '-f', 'null', '-'], capture_output=True, text=True, timeout=120)
        with open(os.path.join(outdir, 'c%d-headers.txt' % c), 'w') as f:
            f.write(trace.stderr)
        yuv = os.path.join(outdir, 'c%d.yuv' % c)
        dec = subprocess.run(['ffmpeg', '-hide_banner', '-v', 'error', '-y', '-err_detect', '+crccheck+bitstream+buffer+explode',
                              '-xerror', '-f', 'h264', '-i', es, '-f', 'rawvideo', '-pix_fmt', 'yuv420p', yuv],
                             capture_output=True, text=True, timeout=120)
        with open(os.path.join(outdir, 'c%d-ffmpeg.txt' % c), 'w') as f:
            f.write(dec.stderr)
        msgs = [m for m in dec.stderr.splitlines() if m.strip()]
        print('  ffmpeg: exit %d, %d messages%s' % (dec.returncode, len(msgs), (': ' + ' | '.join(msgs[:4])) if msgs else ''))
        try:
            data = open(yuv, 'rb').read()
        except OSError:
            data = b''
        frame = WIDTH * HEIGHT * 3 // 2
        frames = len(data) // frame
        print('  decoded frames: %d' % frames)
        ok = dec.returncode == 0 and not msgs and frames == (2 if bp else 1)
        for n, kind in enumerate('IP'[:frames]):
            f = data[n * frame:(n + 1) * frame]
            luma, cb, cr = f[:WIDTH * HEIGHT], f[WIDTH * HEIGHT:WIDTH * HEIGHT + chroma_size], f[WIDTH * HEIGHT + chroma_size:]
            py, pu, pv = psnr(luma, ref[kind]), psnr(cb, flat_cb), psnr(cr, flat_cr)
            recon = runs.get('c%d-%s-luma' % (c, kind), b'')
            same = 'n/a'
            if recon:
                same = 'equal' if tiles_of(luma, WIDTH, len(recon) // 256) == recon else 'differ'
            print('  %s: PSNR Y %.2f dB, Cb %.2f, Cr %.2f; first luma tiles vs the encoder\'s reconstruction: %s' % (kind, py, pu, pv, same))
            save_png(os.path.join(outdir, 'c%d-%s.png' % (c, kind)), luma, cb, cr)
            if py < PSNR_MIN:
                ok = False
        print('  choice %d: %s' % (c, 'PASS' if ok else 'FAIL'))
        passed = passed or ok
    if not choices:
        print('no bitstream in the log')
    sys.exit(0 if passed and not bad else 1)


if __name__ == '__main__':
    main()
