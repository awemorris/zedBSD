/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The parameter sets of the software H.264 decoder: the sequence
 * parameter set (7.3.2.1.1, 7.4.2.1.1), the part of its VUI the decoder
 * uses (E.1.1, E.2.1), the picture parameter set (7.3.2.2, 7.4.2.2), the
 * scaling lists of both and their fall-back rules (7.3.2.1.1.1, Table
 * 7-2), and the store that keeps the last set of each id.
 *
 * This reader is separate from h264.c, which reads the same syntax into
 * the StdVideo structures of Vulkan Video: the software decoder needs
 * every field in its own units, and must build and be tested without the
 * Vulkan headers.
 */

#ifndef LIBMEDIA_H264SW_PS_H
#define LIBMEDIA_H264SW_PS_H

#include "h264sw.h"
#include "vcodec-bitreader.h"

/*
 * The profile_idc values the decoder knows (A.2, 7.4.2.1.1).
 *
 * Baseline (with Constrained Baseline), Main and High are taken.  Extended
 * is taken only with constraint_set0_flag or constraint_set1_flag 1 (the
 * stream then also conforms to Baseline or Main, A.2.1, A.2.2).  The High
 * family (High 10, High 4:2:2, High 4:4:4 Predictive) is taken when the
 * SPS's content stays within the decoder's range (4:2:0, 8 bits, no
 * lossless): such a stream decodes as High.  Every other profile_idc is
 * refused.
 */
#define H264SW_PROFILE_BASELINE 66U
#define H264SW_PROFILE_MAIN 77U
#define H264SW_PROFILE_EXTENDED 88U
#define H264SW_PROFILE_HIGH 100U
#define H264SW_PROFILE_HIGH10 110U
#define H264SW_PROFILE_HIGH422 122U
#define H264SW_PROFILE_HIGH444 244U

/* The most offset_for_ref_frame values of one SPS (num_ref_frames_in_pic_order_cnt_cycle at most 255). */
#define H264SW_MAX_POC_CYCLE 255U

/*
 * What a scaling list of a parameter set says (7.3.2.1.1.1, Table 7-2):
 * not sent (the fall-back rule applies), sent explicitly, or sent as
 * useDefaultScalingMatrixFlag (the default list of Table 7-3 or 7-4).
 */
#define H264SW_LIST_ABSENT 0U
#define H264SW_LIST_EXPLICIT 1U
#define H264SW_LIST_DEFAULT 2U

/*
 * The scaling lists of a picture after both fall-back rules are applied,
 * in transmission (zig-zag) order as ScalingList4x4 and ScalingList8x8
 * are in the specification.
 *
 * list4x4[0..5] are Intra Y, Intra Cb, Intra Cr, Inter Y, Inter Cb,
 * Inter Cr (Table 7-2, i 0..5); list8x8[0] is Intra Y and [1] Inter Y
 * (i 6 and 7).  With no matrix in either set every entry is 16 (Flat_4x4_16
 * and Flat_8x8_16).
 */
struct h264sw_scaling {
	uint8_t list4x4[6][16];
	uint8_t list8x8[2][64];
};

/*
 * One sequence parameter set as read, with the values derived from it
 * (7.4.2.1.1).  Every field is in the unit the specification gives it;
 * the derived fields say how they are derived.
 *
 * An SPS is kept only when every value is in its range and the stream is
 * one this decoder takes: chroma_format_idc 1, bit depths 8,
 * frame_mbs_only_flag 1, qpprime_y_zero_transform_bypass_flag 0,
 * separate_colour_plane_flag absent or 0, and profile_idc taken as the
 * H264SW_PROFILE_* comment says.  The VUI is the exception: a VUI that
 * does not read, or holds values out of their ranges, is dropped
 * (vui_present 0 and its fields at their absent values) and the SPS kept,
 * because a picture can be decoded without it and real streams often get
 * it slightly wrong.
 */
struct h264sw_sps {
	/* profile_idc, the constraint_set0..5 flags as bits 0..5 (bit n is constraint_setn_flag), level_idc, seq_parameter_set_id. */
	unsigned profile_idc;
	unsigned constraint_flags;
	unsigned level_idc;
	unsigned id;

	/* The scaling lists: seq_scaling_matrix_present_flag, and for each list i 0..7 its H264SW_LIST_* state and values (in scan order). */
	int scaling_matrix_present;
	unsigned scaling_state[8];
	uint8_t scaling4x4[6][16];
	uint8_t scaling8x8[2][64];

	/* log2_max_frame_num_minus4 + 4, and MaxFrameNum = 2^that (7-10). */
	unsigned log2_max_frame_num;
	uint32_t max_frame_num;

	/* pic_order_cnt_type 0..2, and log2_max_pic_order_cnt_lsb_minus4 + 4 (type 0). */
	unsigned poc_type;
	unsigned log2_max_poc_lsb;

	/* The type 1 fields: delta_pic_order_always_zero_flag, the offsets, the cycle and ExpectedDeltaPerPicOrderCntCycle (7-12). */
	int delta_pic_order_always_zero;
	int32_t offset_for_non_ref_pic;
	int32_t offset_for_top_to_bottom_field;
	unsigned poc_cycle_length;
	int32_t offset_for_ref_frame[H264SW_MAX_POC_CYCLE];
	int64_t expected_delta_per_poc_cycle;

	/* max_num_ref_frames (0..MaxDpbFrames) and gaps_in_frame_num_value_allowed_flag. */
	unsigned max_num_ref_frames;
	int gaps_allowed;

	/* PicWidthInMbs, FrameHeightInMbs (7-18; equal to PicHeightInMapUnits with frame_mbs_only_flag 1), PicSizeInMbs. */
	unsigned width_mbs;
	unsigned height_mbs;
	unsigned size_mbs;

	/* direct_8x8_inference_flag. */
	int direct_8x8_inference;

	/*
	 * The cropping window in luma samples (7-19..7-22 with CropUnitX and
	 * CropUnitY 2 for 4:2:0 frames): the columns left out on the left and
	 * the right, the rows left out on the top and the bottom.  All 0 when
	 * frame_cropping_flag is 0.  The window is checked to be non-empty.
	 */
	unsigned crop_left;
	unsigned crop_right;
	unsigned crop_top;
	unsigned crop_bottom;

	/* The VUI the decoder uses: vui_parameters_present_flag, the sample aspect ratio (Table E-1 for aspect_ratio_idc 1..16, sar_width and sar_height for 255; 0 and 0 when unknown). */
	int vui_present;
	unsigned sar_width;
	unsigned sar_height;

	/*
	 * video_signal_type_present_flag and video_full_range_flag (0 when
	 * absent); colour_description_present_flag and matrix_coefficients (2,
	 * unspecified, when absent).
	 */
	int signal_present;
	int full_range;
	int colour_present;
	unsigned matrix_coefficients;

	/* bitstream_restriction_flag, max_num_reorder_frames and max_dec_frame_buffering (meaningful only with the flag). */
	int restriction_present;
	unsigned max_num_reorder_frames;
	unsigned max_dec_frame_buffering;

	/*
	 * The size of the decoded picture buffer in frames: max_dec_frame_buffering
	 * when the VUI sends it, else Min( MaxDpbMbs / PicSizeInMbs, 16 ) with
	 * MaxDpbMbs of level_idc from Table A-1 (A.3.1 h), A.3.2 f)); never
	 * below max_num_ref_frames, never below 1, never above 16.  Level 1b
	 * is level_idc 9, or level_idc 11 with constraint_set3_flag 1 in the
	 * Baseline, Constrained Baseline and Main profiles (A.3.1, A.3.2).  A
	 * level_idc not in Table A-1 gives 16.
	 */
	unsigned dpb_frames;
};

/*
 * One picture parameter set as read (7.4.2.2).
 *
 * The scaling lists keep their H264SW_LIST_* state: the fall-back rule B
 * names the SPS's lists, and the SPS a PPS names may change before the
 * PPS is used, so h264sw_ps_scaling resolves them when a picture starts.
 */
struct h264sw_pps {
	/* pic_parameter_set_id and seq_parameter_set_id. */
	unsigned id;
	unsigned sps_id;

	/* entropy_coding_mode_flag (1: CABAC) and bottom_field_pic_order_in_frame_present_flag. */
	int cabac;
	int bottom_field_poc_present;

	/* num_ref_idx_l0_default_active_minus1 + 1 and the same for list 1 (1..32). */
	unsigned num_ref_idx_default[2];

	/* weighted_pred_flag and weighted_bipred_idc (0 default, 1 explicit, 2 implicit). */
	int weighted_pred;
	unsigned weighted_bipred_idc;

	/* 26 + pic_init_qp_minus26 (0..51), and chroma_qp_index_offset (-12..12). */
	int pic_init_qp;
	int chroma_qp_offset;

	/* deblocking_filter_control_present_flag, constrained_intra_pred_flag, redundant_pic_cnt_present_flag. */
	int deblocking_control_present;
	int constrained_intra_pred;
	int redundant_pic_cnt_present;

	/* transform_8x8_mode_flag (0 when absent). */
	int transform_8x8_mode;

	/* pic_scaling_matrix_present_flag and the lists' states and values (6 + 2 * transform_8x8_mode_flag lists sent at most). */
	int scaling_matrix_present;
	unsigned scaling_state[8];
	uint8_t scaling4x4[6][16];
	uint8_t scaling8x8[2][64];

	/* second_chroma_qp_index_offset (equal to chroma_qp_index_offset when absent). */
	int second_chroma_qp_offset;
};

/*
 * The parameter sets a decoder has seen: the last valid set of each id.
 *
 * sps_present and pps_present say which entries hold a set.  A set
 * replaces the earlier one of its id only when it reads without error.
 * sps_generation[id] is incremented each time the SPS of that id is
 * replaced by one with different content, so the decoder can tell, when
 * an IDR picture starts, whether the sequence changed (7.4.1.2.1).
 * Owned by the decoder, embedded in it (about 50 KiB).
 */
struct h264sw_ps_store {
	int sps_present[H264SW_SPS_IDS];
	unsigned sps_generation[H264SW_SPS_IDS];
	struct h264sw_sps sps[H264SW_SPS_IDS];
	int pps_present[H264SW_PPS_IDS];
	struct h264sw_pps pps[H264SW_PPS_IDS];
};

/* Empties a store: no set present, every generation 0. */
void h264sw_ps_init(struct h264sw_ps_store *store);

/*
 * Reads a sequence parameter set RBSP and keeps it in store (7.3.2.1.1).
 *
 * bits is at the first bit of the RBSP (after the NAL unit header).
 * Reads every field including the scaling lists (7.3.2.1.1.1, with the
 * fall-back rule A of Table 7-2 applied to the SPS's own lists: list 0
 * falls back to Default_4x4_Intra, 1 and 2 to the list before, 3 to
 * Default_4x4_Inter, 4 and 5 to the list before, 6 and 7 to the 8x8
 * defaults), the offsets for the order count type 1, the cropping
 * window and the VUI (E.1.1; the HRD parameters, E.1.2, are read to skip
 * them).  The fields after the VUI and the trailing bits are checked.
 *
 * Checks every value against its range in 7.4.2.1.1 (the VUI's against
 * E.2.1, dropping the VUI as struct h264sw_sps says) and the
 * limits of h264sw.h (width and height in macroblocks at most
 * H264SW_MAX_SIDE_MBS, size at most H264SW_MAX_FRAME_MBS), computes the
 * derived fields, and only then stores the set: a failed read leaves the
 * store as it was.
 *
 * Returns 0; EINVAL for a syntax or range error (a delta_scale outside
 * -128..127 is one; nextScale itself is taken modulo 256 as the syntax
 * of 7.3.2.1.1.1 says, so it never leaves 0..255); ENOTSUP for a valid SPS outside the
 * decoder's range (profile, chroma format, bit depth, interlace,
 * lossless, a size above the limits).  An SPS for an unsupported
 * profile_idc is refused before its profile-specific fields are read.
 */
int h264sw_ps_read_sps(struct h264sw_ps_store *store, struct vcodec_bitreader *bits);

/*
 * Reads a picture parameter set RBSP and keeps it in store (7.3.2.2).
 *
 * Reads every field; the fields after redundant_pic_cnt_present_flag are
 * read only when more_rbsp_data() says they are there (else
 * transform_8x8_mode_flag is 0, no scaling matrix, and
 * second_chroma_qp_index_offset equals chroma_qp_index_offset).  The SPS
 * of seq_parameter_set_id need not be present yet: nothing of the PPS
 * depends on it with chroma_format_idc 1 (6 + 2 * transform_8x8_mode_flag
 * scaling lists).
 *
 * Returns 0; EINVAL for a syntax or range error (pic_init_qp_minus26
 * outside -26..25, num_ref_idx_lX_default_active_minus1 above 31,
 * weighted_bipred_idc 3, a chroma offset outside -12..12, missing
 * trailing bits); ENOTSUP for num_slice_groups_minus1 above 0 (slice
 * groups, the Baseline profile's FMO).  A failed read leaves the store as
 * it was.
 */
int h264sw_ps_read_pps(struct h264sw_ps_store *store, struct vcodec_bitreader *bits);

/*
 * Resolves the scaling lists a picture uses from its SPS and PPS (7.4.2.1.1, 7.4.2.2, Table 7-2).
 *
 * When neither set has a matrix: every list Flat_16.  When only the SPS
 * has one: the SPS's lists (fall-back rule A already applied).  When the
 * PPS has one: each list the PPS sent is used as sent (or the default
 * list for H264SW_LIST_DEFAULT); an absent list falls back by rule A
 * (when the SPS has no matrix) or rule B (when it has one): list 0 to the
 * SPS's list 0, 3 to the SPS's list 3, 6 and 7 to the SPS's 6 and 7, and
 * 1, 2, 4, 5 to the list before in the PPS's resolved lists.  The 8x8
 * lists of a PPS with transform_8x8_mode_flag 0 are not used.
 *
 * Cannot fail: both sets were checked when they were read.
 */
void h264sw_ps_scaling(const struct h264sw_sps *sps, const struct h264sw_pps *pps, struct h264sw_scaling *scaling);

#endif
