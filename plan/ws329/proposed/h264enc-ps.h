/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The parameter sets the software H.264 encoder writes: the choices of
 * one sequence (struct h264enc_sequence), the choice of the level from
 * Table A-1, and the sequence parameter set (7.3.2.1.1 with the VUI of
 * E.1.1) and picture parameter set (7.3.2.2) RBSPs.
 *
 * The encoder reads every set it writes back with the decoder's reader
 * (h264sw_ps_read_sps, h264sw_ps_read_pps) and uses the structures the
 * reader gives: what the encoder believes and what a decoder reads cannot
 * differ.
 */

#ifndef LIBMEDIA_H264ENC_PS_H
#define LIBMEDIA_H264ENC_PS_H

#include "h264enc.h"
#include "h264sw.h"
#include "vcodec-bitwriter.h"

/*
 * The choices of one sequence and its one picture parameter set (id 0 for
 * both).
 *
 * profile_idc 66 (with constraint_flags bits 0 and 1 set: Constrained
 * Baseline, A.2.1.1), 77 or 100; level_idc of Table A-1 (not 9; level 1b
 * is never chosen).  width_mbs, height_mbs: the coded size; crop_right,
 * crop_bottom: the luma samples cut on the right and the bottom (even;
 * frame_cropping_flag is 1 when either is above 0; the left and top
 * offsets are 0).  max_num_ref_frames 1..16.  log2_max_frame_num 4..16;
 * pic_order_cnt_type is always 0 with log2_max_poc_lsb 4..16.
 * direct_8x8_inference_flag is always 1 (A.3.3 c) for every level this
 * encoder may choose with B slices).
 *
 * The VUI (vui_parameters_present_flag 1 always): aspect_ratio_info when
 * sar_width is above 0 (aspect_ratio_idc 255 with sar_width and
 * sar_height, or the Table E-1 index when the pair is one of its rows);
 * video_signal_type when full_range is 1 or colour_present is 1
 * (video_format 5, video_full_range_flag, colour_description_present_flag
 * colour_present with primaries, transfer, matrix); chroma_loc_info when
 * chroma_location is above 0 (both fields that value); timing_info with
 * num_units_in_tick and time_scale (time_scale = 2 * rate_num,
 * num_units_in_tick = rate_den: a frame is two ticks) and
 * fixed_frame_rate_flag 1; no HRD parameters; pic_struct_present_flag 0;
 * bitstream_restriction_flag 1 with motion_vectors_over_pic_boundaries_flag
 * 1, max_bytes_per_pic_denom 0, max_bits_per_mb_denom 0,
 * log2_max_mv_length_horizontal and _vertical 15, max_num_reorder_frames
 * and max_dec_frame_buffering as given.
 *
 * The PPS: entropy_coding_mode_flag cabac; num_ref_idx_default[0] and
 * [1] (1..32; the slices override them when they differ);
 * weighted_pred_flag 0, weighted_bipred_idc 0; pic_init_qp (26 + the
 * minus26 field); chroma_qp_index_offset 0;
 * deblocking_filter_control_present_flag 1; constrained_intra_pred_flag
 * 0; redundant_pic_cnt_present_flag 0; and, for profile 100 only, the
 * fields after it: transform_8x8_mode_flag transform_8x8,
 * pic_scaling_matrix_present_flag 0, second_chroma_qp_index_offset 0.
 * bottom_field_pic_order_in_frame_present_flag 0; num_slice_groups_minus1
 * 0.
 */
struct h264enc_sequence {
	unsigned profile_idc;
	unsigned constraint_flags;
	unsigned level_idc;
	unsigned width_mbs;
	unsigned height_mbs;
	unsigned crop_right;
	unsigned crop_bottom;
	unsigned max_num_ref_frames;
	unsigned log2_max_frame_num;
	unsigned log2_max_poc_lsb;

	unsigned sar_width;
	unsigned sar_height;
	int full_range;
	int colour_present;
	unsigned primaries;
	unsigned transfer;
	unsigned matrix;
	unsigned chroma_location;
	uint32_t num_units_in_tick;
	uint32_t time_scale;
	unsigned max_num_reorder_frames;
	unsigned max_dec_frame_buffering;

	int cabac;
	unsigned num_ref_idx_default[2];
	int pic_init_qp;
	int transform_8x8;
};

/*
 * Chooses the lowest level a sequence fits (Table A-1 through
 * h264sw_levels, A.3.1 and A.3.3).
 *
 * The levels are tried in increasing order, skipping 1b.  A level fits
 * when: PicSizeInMbs <= MaxFS; width_mbs and height_mbs each <= Sqrt(
 * 8 * MaxFS ) (A.3.1 f), g)); PicSizeInMbs * rate_num <= MaxMBPS *
 * rate_den (the macroblock rate of A.3.1 a), b) at a constant frame
 * rate); max_bitrate <= MaxBR * cpbBrVclFactor (1000 for profiles 66 and
 * 77, 1250 for 100; max_bitrate 0 is not checked: a constant quantizer);
 * dpb_frames * PicSizeInMbs <= MaxDpbMbs (A.3.1 h)).  Products are
 * computed in uint64_t.  Returns 0 and *level_idc, or ENOTSUP when even
 * level 6.2 does not fit.
 */
int h264enc_level_choose(unsigned profile_idc, unsigned width_mbs, unsigned height_mbs, uint32_t rate_num, uint32_t rate_den, uint32_t max_bitrate, unsigned dpb_frames, unsigned *level_idc);

/* Checks that a sequence fits a given level by the same rules: 0, or ENOTSUP (also for a level_idc not in the table, or 9). */
int h264enc_level_check(unsigned profile_idc, unsigned level_idc, unsigned width_mbs, unsigned height_mbs, uint32_t rate_num, uint32_t rate_den, uint32_t max_bitrate, unsigned dpb_frames);

/*
 * Writes a sequence parameter set RBSP (7.3.2.1.1), including
 * rbsp_trailing_bits, into writer (which the caller has reset).
 *
 * seq_parameter_set_id 0; for profile 100: chroma_format_idc 1,
 * bit_depth_luma_minus8 0, bit_depth_chroma_minus8 0,
 * qpprime_y_zero_transform_bypass_flag 0, seq_scaling_matrix_present_flag
 * 0; pic_order_cnt_type 0; gaps_in_frame_num_value_allowed_flag 0;
 * pic_width_in_mbs_minus1, pic_height_in_map_units_minus1,
 * frame_mbs_only_flag 1, direct_8x8_inference_flag 1, the cropping and
 * the VUI as struct h264enc_sequence says.  Returns 0, EINVAL for a
 * field out of its range (checked before anything is written), or the
 * writer's error.
 */
int h264enc_sps_write(struct vcodec_bitwriter *writer, const struct h264enc_sequence *sequence);

/* Writes the picture parameter set RBSP (7.3.2.2) with its trailing bits.  Same returns. */
int h264enc_pps_write(struct vcodec_bitwriter *writer, const struct h264enc_sequence *sequence);

#endif
