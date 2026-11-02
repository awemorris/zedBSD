#!/usr/bin/env python3
# Reads ffmpeg's trace_headers output of an H.265 stream on stdin and prints one line of its first SPS's and PPS's
# features: NAME FRAMES key=value ... (profile, depth, chroma, size, CTB, tools), for picking streams by tool.
#   usage: ffmpeg -v trace -i S -c copy -bsf:v trace_headers -f null - 2>&1 | h265-features.py NAME FRAMES
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
import re
import sys

KEYS = {
    'general_profile_idc': 'profile', 'chroma_format_idc': 'chroma', 'bit_depth_luma_minus8': 'depth8',
    'pic_width_in_luma_samples': 'w', 'pic_height_in_luma_samples': 'h',
    'log2_min_luma_coding_block_size_minus3': 'mincb3', 'log2_diff_max_min_luma_coding_block_size': 'diffcb',
    'log2_min_luma_transform_block_size_minus2': 'mintb2', 'log2_diff_max_min_luma_transform_block_size': 'difftb',
    'max_transform_hierarchy_depth_inter': 'dinter', 'max_transform_hierarchy_depth_intra': 'dintra',
    'scaling_list_enabled_flag': 'scaling', 'amp_enabled_flag': 'amp', 'sample_adaptive_offset_enabled_flag': 'sao',
    'pcm_enabled_flag': 'pcm', 'num_short_term_ref_pic_sets': 'strps', 'long_term_ref_pics_present_flag': 'lt',
    'sps_temporal_mvp_enabled_flag': 'tmvp', 'strong_intra_smoothing_enabled_flag': 'strong',
    'dependent_slice_segments_enabled_flag': 'depslice', 'sign_data_hiding_enabled_flag': 'sdh',
    'cabac_init_present_flag': 'cabacinit', 'transform_skip_enabled_flag': 'tskip', 'cu_qp_delta_enabled_flag': 'cuqpd',
    'weighted_pred_flag': 'wp', 'weighted_bipred_flag': 'wbp', 'transquant_bypass_enabled_flag': 'bypass',
    'tiles_enabled_flag': 'tiles', 'entropy_coding_sync_enabled_flag': 'wpp', 'lists_modification_present_flag': 'listmod',
    'pps_scaling_list_data_present_flag': 'ppssl', 'deblocking_filter_control_present_flag': 'dbkctl',
    'pps_slice_chroma_qp_offsets_present_flag': 'slicecqp', 'num_extra_slice_header_bits': 'extrabits',
    'output_flag_present_flag': 'outflag', 'constrained_intra_pred_flag': 'cip', 'init_qp_minus26': 'initqp',
}
LINE = re.compile(r'\]\s+\d+\s+(\S+)\s+\S+\s+=\s+(-?\d+)')


def main():
    found = {}
    slices = 0
    for line in sys.stdin:
        m = LINE.search(line)
        if not m:
            continue
        name, value = m.group(1), m.group(2)
        if name == 'first_slice_segment_in_pic_flag':
            slices += 1
        key = KEYS.get(name)
        if key and key not in found:
            found[key] = value
    print('%s frames=%s segments=%d %s' % (sys.argv[1], sys.argv[2], slices,
                                            ' '.join('%s=%s' % (k, found.get(k, '-')) for k in KEYS.values())))


main()
