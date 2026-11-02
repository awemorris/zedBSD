#!/usr/bin/env python3
# Reads the kernel lines of nvrtx.hevc.trace=1 ("nvrtx: vk: video: trace N WHAT OFFSET: HEX") from a log and prints,
# for each traced picture, the H.265 picture structure's fields by name (the layout of NVIDIA open-gpu-doc
# classes/video/nvdec_drv.h nvdec_hevc_pic_s as nvdec-records.c writes it), the tile words, the push buffer's methods
# and the status words.
#   usage: hevc-trace-decode.py LOG
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
import re
import struct
import sys

LINE = re.compile(r'trace (\d+) (\w+) ([0-9a-f]{4}): ([0-9a-f]+)')
INFO = re.compile(r'trace (\d+): (.*)')

# (name, byte, low bit, bits); bits 0 for a whole little-endian value of the given bytes in low.
FIELDS = [
    ('stream_len', 48, 4, 0), ('gptimer', 60, 4, 0),
    ('tileformat', 64, 0, 2), ('gob_height', 64, 2, 3), ('sw_start_code_e', 65, 1, 0), ('disp_output_mode', 66, 1, 0),
    ('framestride0', 68, 4, 0), ('framestride1', 72, 4, 0), ('colMvBuffersize', 76, 4, 0),
    ('HevcSaoBufferOffset', 80, 4, 0), ('HevcBsdCtrlOffset', 84, 4, 0),
    ('width', 88, 2, 0), ('height', 90, 2, 0),
    ('chroma_format_idc', 92, 0, 4), ('bit_depth_luma', 92, 4, 4), ('bit_depth_chroma', 92, 8, 4),
    ('log2_min_cb', 92, 12, 4), ('log2_max_cb', 92, 16, 4), ('log2_min_tb', 92, 20, 4), ('log2_max_tb', 92, 24, 4),
    ('depth_inter', 96, 0, 3), ('depth_intra', 96, 3, 3), ('scaling', 96, 6, 1), ('amp', 96, 7, 1), ('sao', 96, 8, 1),
    ('pcm', 96, 9, 1), ('pcm_luma', 96, 10, 4), ('pcm_chroma', 96, 14, 4), ('min_pcm', 96, 18, 4), ('max_pcm', 96, 22, 4),
    ('pcm_filter_off', 96, 26, 1), ('tmvp', 96, 27, 1), ('strong_intra', 96, 28, 1),
    ('dependent', 100, 0, 1), ('output_flag', 100, 1, 1), ('extra_bits', 100, 2, 3), ('sign_hiding', 100, 5, 1),
    ('cabac_init', 100, 6, 1), ('l0_default', 100, 7, 4), ('l1_default', 100, 11, 4), ('init_qp', 100, 15, 7),
    ('constrained_intra', 100, 22, 1), ('transform_skip', 100, 23, 1), ('cu_qp_delta', 100, 24, 1), ('cu_qp_depth', 100, 25, 2),
    ('cb_qp', 104, 1, -1), ('cr_qp', 105, 1, -1), ('beta', 106, 1, -1), ('tc', 107, 1, -1),
    ('slice_qp', 108, 0, 1), ('weighted', 108, 1, 1), ('bipred', 108, 2, 1), ('bypass', 108, 3, 1), ('tiles', 108, 4, 1),
    ('entropy_sync', 108, 5, 1), ('tile_columns', 108, 6, 5), ('tile_rows', 108, 11, 5), ('across_tiles', 108, 16, 1),
    ('across_slices', 108, 17, 1), ('dbk_control', 108, 18, 1), ('dbk_override', 108, 19, 1), ('dbk_off', 108, 20, 1),
    ('lists_mod', 108, 21, 1), ('merge_level', 108, 22, 3), ('header_ext', 108, 25, 1),
    ('num_ref_frames', 112, 1, 0), ('longtermflag', 114, 2, 0),
    ('IDR', 180, 1, 0), ('RAP', 181, 1, 0), ('curr_pic_idx', 182, 1, 0), ('pattern_id', 183, 1, 0),
    ('sw_hdr_skip_length', 184, 2, 0),
    ('dxva_separate', 212, 0, 1), ('dxva_poc_lsb', 212, 1, 4), ('dxva_st_sets', 212, 5, 7), ('dxva_lt_sps', 212, 12, 6),
    ('dxva_bit_parsing_disable', 212, 18, 1), ('dxva_delta_pocs', 212, 19, 8), ('dxva_lt_present', 212, 27, 1),
    ('num_bits_st_in_slice', 216, 4, 0), ('HevcFltAboveOffset', 240, 4, 0), ('HevcSaoAboveOffset', 244, 4, 0),
]


def field(pic, at, low, bits):
    if bits == 0:
        return int.from_bytes(pic[at:at + low], 'little')
    if bits == -1:
        return struct.unpack('b', pic[at:at + 1])[0]
    word = int.from_bytes(pic[at:at + 4], 'little')
    return (word >> low) & ((1 << bits) - 1)


def main():
    blobs = {}
    infos = {}
    order = []
    with open(sys.argv[1], errors='replace') as f:
        for line in f:
            m = LINE.search(line)
            if m:
                serial, what, at, data = int(m.group(1)), m.group(2), int(m.group(3), 16), bytes.fromhex(m.group(4))
                key = (len(order) if (serial, what) not in [k[1:] for k in order[-6:]] else None, serial, what)
                if what == 'pic' and at == 0:
                    order.append(('pic', serial, len(order)))
                current = len([o for o in order if o[0] == 'pic']) - 1
                blob = blobs.setdefault((current, what), bytearray())
                if len(blob) < at + len(data):
                    blob.extend(b'\0' * (at + len(data) - len(blob)))
                blob[at:at + len(data)] = data
                continue
            m = INFO.search(line)
            if m:
                infos[len([o for o in order if o[0] == 'pic'])] = m.group(2)
    pictures = len([o for o in order if o[0] == 'pic'])
    for index in range(pictures):
        print('=== picture %d (serial %d): %s' % (index, order[index][1], infos.get(index, '')))
        pic = bytes(blobs.get((index, 'pic'), b''))
        print('  ' + ' '.join('%s=%d' % (name, field(pic, at, low, bits)) for name, at, low, bits in FIELDS))
        print('  list0=%s' % list(pic[116:132]))
        print('  list1=%s' % list(pic[132:148]))
        print('  pocdiff=%s' % [struct.unpack('<h', pic[148 + 2 * i:150 + 2 * i])[0] for i in range(16)])
        nonzero = [i for i in range(len(pic)) if pic[i] and not (48 <= i < 120) and not (148 <= i < 220) and i not in range(240, 248)]
        if nonzero:
            print('  other nonzero bytes: %s' % ', '.join('%d=%02x' % (i, pic[i]) for i in nonzero))
        tiles = bytes(blobs.get((index, 'tiles'), b''))
        print('  tiles=%s' % [int.from_bytes(tiles[i:i + 2], 'little') for i in range(0, len(tiles), 2)])
        push = bytes(blobs.get((index, 'push'), b''))
        words = [int.from_bytes(push[i:i + 4], 'little') for i in range(0, len(push), 4)]
        text = []
        at = 0
        while at < len(words):
            header = words[at]
            count = (header >> 16) & 0x1fff
            method = (header & 0x1fff) << 2
            text.append('%03x:%s' % (method, ','.join('%x' % w for w in words[at + 1:at + 1 + count])))
            at += 1 + count
        print('  push ' + ' '.join(text))
        status = bytes(blobs.get((index, 'status'), b''))
        print('  status=%s' % ['%x' % int.from_bytes(status[i:i + 4], 'little') for i in range(0, len(status), 4)])
        stream = bytes(blobs.get((index, 'stream'), b''))
        print('  stream=%s' % stream[:32].hex())


main()
