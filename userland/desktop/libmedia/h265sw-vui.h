/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The video usability information of H.265: vui_parameters() (E.2.1,
 * E.3.1) and the hypothetical reference decoder parameters
 * (hrd_parameters() E.2.2, E.3.2, and sub_layer_hrd_parameters() E.2.3,
 * E.3.3), which an SPS's VUI and a VPS carry.
 *
 * Every field is kept as signalled, with the inferences of E.3: the
 * Vulkan Video path hands them to a driver that may rebuild the
 * parameter sets, and the software decoder uses the sample aspect ratio,
 * the range and colour description and field_seq_flag.
 */

#ifndef LIBMEDIA_H265SW_VUI_H
#define LIBMEDIA_H265SW_VUI_H

#include "h265sw.h"
#include "vcodec-bitreader.h"

/* aspect_ratio_idc of an explicit sample aspect ratio (Table E.1: EXTENDED_SAR). */
#define H265SW_EXTENDED_SAR 255U

/*
 * One sub_layer_hrd_parameters( subLayerId ) (E.2.3, E.3.3): for each of
 * the CpbCnt CPB specifications j (cpb_cnt_minus1 + 1 of the sub-layer)
 * bit_rate_value_minus1[ j ] and cpb_size_value_minus1[ j ] (0..2^32 - 2),
 * cpb_size_du_value_minus1[ j ] and bit_rate_du_value_minus1[ j ] (0
 * when sub_pic_hrd_params_present_flag is 0), and cbr_flag[ j ] as bit j
 * of cbr_flags.  Entries j at or past CpbCnt are 0.
 */
struct h265sw_sub_layer_hrd {
	uint32_t bit_rate_value_minus1[H265SW_MAX_CPB];
	uint32_t cpb_size_value_minus1[H265SW_MAX_CPB];
	uint32_t cpb_size_du_value_minus1[H265SW_MAX_CPB];
	uint32_t bit_rate_du_value_minus1[H265SW_MAX_CPB];
	uint32_t cbr_flags;
};

/*
 * One hrd_parameters( commonInfPresentFlag, maxNumSubLayersMinus1 )
 * (E.2.2) with the inferences of E.3.2.
 *
 * The common part, each named after its syntax element:
 * nal_hrd_present, vcl_hrd_present, sub_pic_hrd_params_present (0 when
 * absent), tick_divisor_minus2, du_cpb_removal_delay_increment_length_minus1,
 * sub_pic_cpb_params_in_pic_timing_sei (0 when absent),
 * dpb_output_delay_du_length_minus1, bit_rate_scale, cpb_size_scale,
 * cpb_size_du_scale, and the three length fields
 * initial_cpb_removal_delay_length_minus1,
 * au_cpb_removal_delay_length_minus1 and dpb_output_delay_length_minus1
 * (23 when absent).
 *
 * For each sub-layer i of 0..maxNumSubLayersMinus1 (entries above it are
 * 0): fixed_pic_rate_general[ i ], fixed_pic_rate_within_cvs[ i ] (1 when
 * the general flag is 1), elemental_duration_in_tc_minus1[ i ]
 * (0..2047, 0 when absent), low_delay_hrd[ i ] (0 when absent),
 * cpb_cnt_minus1[ i ] (0..31, 0 when absent), and nal[ i ] and vcl[ i ]
 * when the matching present flag is 1 (zero otherwise).
 */
struct h265sw_hrd {
	int nal_hrd_present;
	int vcl_hrd_present;
	int sub_pic_hrd_params_present;
	unsigned tick_divisor_minus2;
	unsigned du_cpb_removal_delay_increment_length_minus1;
	int sub_pic_cpb_params_in_pic_timing_sei;
	unsigned dpb_output_delay_du_length_minus1;
	unsigned bit_rate_scale;
	unsigned cpb_size_scale;
	unsigned cpb_size_du_scale;
	unsigned initial_cpb_removal_delay_length_minus1;
	unsigned au_cpb_removal_delay_length_minus1;
	unsigned dpb_output_delay_length_minus1;
	uint8_t fixed_pic_rate_general[H265SW_MAX_SUB_LAYERS];
	uint8_t fixed_pic_rate_within_cvs[H265SW_MAX_SUB_LAYERS];
	uint8_t low_delay_hrd[H265SW_MAX_SUB_LAYERS];
	uint16_t elemental_duration_in_tc_minus1[H265SW_MAX_SUB_LAYERS];
	uint8_t cpb_cnt_minus1[H265SW_MAX_SUB_LAYERS];
	struct h265sw_sub_layer_hrd nal[H265SW_MAX_SUB_LAYERS];
	struct h265sw_sub_layer_hrd vcl[H265SW_MAX_SUB_LAYERS];
};

/*
 * One vui_parameters() (E.2.1) with the inferences of E.3.1.
 *
 * As signalled, each named after its syntax element (0 when absent unless
 * said): aspect_ratio_present, aspect_ratio_idc, signalled_sar_width and
 * signalled_sar_height (sar_width and sar_height, read only for
 * EXTENDED_SAR); overscan_present, overscan_appropriate; signal_present
 * (video_signal_type_present_flag), video_format (5 when absent),
 * full_range (video_full_range_flag), colour_present
 * (colour_description_present_flag), colour_primaries,
 * transfer_characteristics and matrix_coeffs (2 when absent);
 * chroma_loc_present, chroma_sample_loc_type_top_field and
 * chroma_sample_loc_type_bottom_field (0..5); neutral_chroma_indication;
 * field_seq; frame_field_info_present; default_display_window with
 * def_disp_win_left_offset, def_disp_win_right_offset,
 * def_disp_win_top_offset and def_disp_win_bottom_offset (in chroma
 * sample units as signalled, each at most H265SW_MAX_SIDE, else EINVAL);
 * timing_present (vui_timing_info_present_flag) with num_units_in_tick
 * and time_scale (vui_num_units_in_tick and vui_time_scale, both non-zero
 * when present, E.3.1), poc_proportional_to_timing with
 * num_ticks_poc_diff_one_minus1 (0..2^32 - 2); hrd_present
 * (vui_hrd_parameters_present_flag) with hrd; bitstream_restriction with
 * tiles_fixed_structure, motion_vectors_over_pic_boundaries (1 when
 * absent), restricted_ref_pic_lists, min_spatial_segmentation_idc
 * (0..4095), max_bytes_per_pic_denom (0..16, 2 when absent),
 * max_bits_per_min_cu_denom (0..16, 1 when absent),
 * log2_max_mv_length_horizontal and log2_max_mv_length_vertical (0..15,
 * 15 when absent).
 *
 * Derived: sar_width and sar_height, the sample aspect ratio that
 * aspect_ratio_idc names (Table E.1, 1..16, or the signalled values for
 * EXTENDED_SAR), 0 and 0 when absent, unspecified (0), reserved
 * (17..254) or with a zero term.
 */
struct h265sw_vui {
	int aspect_ratio_present;
	unsigned aspect_ratio_idc;
	unsigned signalled_sar_width;
	unsigned signalled_sar_height;
	unsigned sar_width;
	unsigned sar_height;
	int overscan_present;
	int overscan_appropriate;
	int signal_present;
	unsigned video_format;
	int full_range;
	int colour_present;
	unsigned colour_primaries;
	unsigned transfer_characteristics;
	unsigned matrix_coeffs;
	int chroma_loc_present;
	unsigned chroma_sample_loc_type_top_field;
	unsigned chroma_sample_loc_type_bottom_field;
	int neutral_chroma_indication;
	int field_seq;
	int frame_field_info_present;
	int default_display_window;
	unsigned def_disp_win_left_offset;
	unsigned def_disp_win_right_offset;
	unsigned def_disp_win_top_offset;
	unsigned def_disp_win_bottom_offset;
	int timing_present;
	uint32_t num_units_in_tick;
	uint32_t time_scale;
	int poc_proportional_to_timing;
	uint32_t num_ticks_poc_diff_one_minus1;
	int hrd_present;
	struct h265sw_hrd hrd;
	int bitstream_restriction;
	int tiles_fixed_structure;
	int motion_vectors_over_pic_boundaries;
	int restricted_ref_pic_lists;
	unsigned min_spatial_segmentation_idc;
	unsigned max_bytes_per_pic_denom;
	unsigned max_bits_per_min_cu_denom;
	unsigned log2_max_mv_length_horizontal;
	unsigned log2_max_mv_length_vertical;
};

/*
 * Reads hrd_parameters( common_present, max_sub_layers_minus1 ) into hrd
 * (E.2.2, E.2.3).
 *
 * max_sub_layers_minus1 is 0..6 (above is EINVAL).  With common_present
 * 1 the common part is read; with 0 it is copied from previous (the
 * preceding hrd_parameters() of the same VPS, whose common part this one
 * shares by 7.4.3.1), which is then not NULL (NULL is EINVAL).  hrd is
 * written in full (cleared first, then the inferences of E.3.2, then the
 * fields read); hrd and previous may not be the same object.
 *
 * Returns 0; EINVAL for a read error or a value out of its range
 * (elemental_duration_in_tc_minus1 above 2047, cpb_cnt_minus1 above 31,
 * a bit rate or CPB size value above 2^32 - 2).  On error hrd is left
 * partly written.
 */
int h265sw_vui_read_hrd(struct h265sw_hrd *hrd, struct vcodec_bitreader *bits, int common_present, unsigned max_sub_layers_minus1, const struct h265sw_hrd *previous);

/*
 * Reads vui_parameters() into vui (E.2.1).
 *
 * max_sub_layers_minus1 is sps_max_sub_layers_minus1 (0..6), the
 * maxNumSubLayersMinus1 of hrd_parameters( 1, ... ).  Every field is
 * read in order, the HRD parameters with h265sw_vui_read_hrd( ..., 1,
 * ..., NULL ), so bits ends just after the VUI.  vui is written in full
 * (cleared first, then the inferences of E.3.1, then the fields read and
 * the derived sample aspect ratio).  The aspect ratios of Table E.1 are
 * data of this module.
 *
 * Returns 0; EINVAL for a read error or a value out of its range (a
 * ue(v) above its stated range, vui_num_units_in_tick or vui_time_scale
 * 0 when present, an error of the HRD parameters).  The caller decides
 * what a failed VUI means for the SPS (h265sw-ps.h).  On error vui is
 * left partly written.
 */
int h265sw_vui_read(struct h265sw_vui *vui, struct vcodec_bitreader *bits, unsigned max_sub_layers_minus1);

#endif
