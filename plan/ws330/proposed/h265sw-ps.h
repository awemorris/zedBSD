/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The parameter sets of the software H.265 decoder: the sequence
 * parameter set (7.3.2.2, 7.4.3.2) with its profile, tier and level
 * (7.3.3, 7.4.4), the picture parameter set (7.3.2.3, 7.4.3.3), and the
 * store that keeps them by id.
 *
 * Video parameter sets are not read: nothing of the base layer's
 * decoding depends on them (the decoder skips VPS NAL units).
 *
 * A PPS is read without its SPS (the SPS may come later); the checks
 * that need both are made by h265sw_ps_check_pair when a picture starts.
 */

#ifndef LIBMEDIA_H265SW_PS_H
#define LIBMEDIA_H265SW_PS_H

#include "h265sw.h"
#include "h265sw-rps.h"
#include "h265sw-scaling.h"
#include "h265sw-vui.h"
#include "vcodec-bits.h"

/*
 * The general profile, tier and level of profile_tier_level( 1, ... )
 * (7.3.3): profile_space general_profile_space, tier general_tier_flag,
 * profile_idc general_profile_idc, compatibility the 32
 * general_profile_compatibility_flag bits (bit j is flag[ j ]),
 * progressive and interlaced general_progressive_source_flag and
 * general_interlaced_source_flag, level_idc general_level_idc.  The
 * sub-layer fields are read and skipped.  Informative for this decoder:
 * what it decodes is decided by the SPS's fields (h265sw.h).
 */
struct h265sw_ptl {
	unsigned profile_space;
	unsigned tier;
	unsigned profile_idc;
	uint32_t compatibility;
	int progressive;
	int interlaced;
	unsigned level_idc;
};

/*
 * One sequence parameter set, with the derived values the decoder uses.
 *
 * Signalled (7.3.2.2.1), each checked against 7.4.3.2.1: id
 * (sps_seq_parameter_set_id 0..15), max_sub_layers_minus1 (0..6), ptl,
 * width and height (pic_width_in_luma_samples and
 * pic_height_in_luma_samples, non-zero multiples of MinCbSizeY, each at
 * most H265SW_MAX_SIDE, product at most H265SW_MAX_LUMA_PS), the
 * conformance window in luma samples (crop_x = SubWidthC *
 * conf_win_left_offset and so on; crop_width and crop_height the size
 * inside it, at least 1), log2_max_poc_lsb (4..16).
 *
 * max_dec_pic_buffering_minus1[i], max_num_reorder_pics[i] and
 * max_latency_increase_plus1[i] for every sub-layer i (the values of
 * the highest sub-layer copied down when
 * sps_sub_layer_ordering_info_present_flag is 0, 7.4.3.2.1);
 * max_dec_pic_buffering_minus1 0..H265SW_MAX_DPB - 1,
 * max_num_reorder_pics at most it.
 *
 * min_cb_log2 MinCbLog2SizeY (3..), ctb_log2 CtbLog2SizeY (4..6, else
 * ENOTSUP), min_tb_log2 MinTbLog2SizeY (2.., below min_cb_log2),
 * max_tb_log2 MaxTbLog2SizeY (at most Min( ctb_log2, 5 )),
 * max_th_depth_inter and max_th_depth_intra (0..ctb_log2 - min_tb_log2).
 *
 * scaling_list_enabled with scaling (the SPS's lists: signalled, or
 * the defaults when sps_scaling_list_data_present_flag is 0); amp, sao;
 * pcm with pcm_bit_depth_luma and pcm_bit_depth_chroma (1..8),
 * pcm_min_log2 and pcm_max_log2 (Log2MinIpcmCbSizeY 3..Min( ctb_log2,
 * 5 ), Log2MaxIpcmCbSizeY up to Min( ctb_log2, 5 )) and
 * pcm_loop_filter_disabled; num_st_rps (0..64) with st_rps;
 * long_term_present with num_lt_sps (0..32), lt_poc_lsb and lt_used;
 * temporal_mvp, strong_intra_smoothing; vui_present with vui.
 *
 * Derived: pic_width_ctbs and pic_height_ctbs (PicWidthInCtbsY,
 * PicHeightInCtbsY, 7-15 and 7-17), pic_size_ctbs (7-19), and
 * pic_width_min_cbs and pic_height_min_cbs (7-14, 7-16).
 */
struct h265sw_sps {
	unsigned id;
	unsigned max_sub_layers_minus1;
	struct h265sw_ptl ptl;
	unsigned width;
	unsigned height;
	unsigned crop_x;
	unsigned crop_y;
	unsigned crop_width;
	unsigned crop_height;
	unsigned log2_max_poc_lsb;
	unsigned max_dec_pic_buffering_minus1[H265SW_MAX_SUB_LAYERS];
	unsigned max_num_reorder_pics[H265SW_MAX_SUB_LAYERS];
	uint32_t max_latency_increase_plus1[H265SW_MAX_SUB_LAYERS];
	unsigned min_cb_log2;
	unsigned ctb_log2;
	unsigned min_tb_log2;
	unsigned max_tb_log2;
	unsigned max_th_depth_inter;
	unsigned max_th_depth_intra;
	int scaling_list_enabled;
	struct h265sw_scaling_list scaling;
	int amp;
	int sao;
	int pcm;
	unsigned pcm_bit_depth_luma;
	unsigned pcm_bit_depth_chroma;
	unsigned pcm_min_log2;
	unsigned pcm_max_log2;
	int pcm_loop_filter_disabled;
	unsigned num_st_rps;
	struct h265sw_st_rps st_rps[H265SW_MAX_ST_RPS - 1U];
	int long_term_present;
	unsigned num_lt_sps;
	uint32_t lt_poc_lsb[H265SW_MAX_LT_SPS];
	uint8_t lt_used[H265SW_MAX_LT_SPS];
	int temporal_mvp;
	int strong_intra_smoothing;
	int vui_present;
	struct h265sw_vui vui;

	unsigned pic_width_ctbs;
	unsigned pic_height_ctbs;
	unsigned pic_size_ctbs;
	unsigned pic_width_min_cbs;
	unsigned pic_height_min_cbs;
};

/*
 * One picture parameter set.
 *
 * Signalled (7.3.2.3.1), each checked against 7.4.3.3.1 where the check
 * needs no SPS: id (0..63), sps_id (0..15), dependent_slices,
 * output_flag_present, num_extra_slice_header_bits (0..2 by 7.4.3.3.1;
 * larger values are read as the syntax says), sign_data_hiding,
 * cabac_init_present, num_ref_idx_default[2]
 * (num_ref_idx_lX_default_active_minus1 + 1, 1..15), init_qp
 * (26 + init_qp_minus26, 0..51 for 8 bits), constrained_intra,
 * transform_skip, cu_qp_delta with diff_cu_qp_delta_depth, cb_qp_offset
 * and cr_qp_offset (-12..12), slice_chroma_qp_offsets_present,
 * weighted_pred, weighted_bipred, transquant_bypass, tiles,
 * entropy_sync (entropy_coding_sync_enabled_flag), with tiles:
 * tile_columns and tile_rows (num_tile_columns_minus1 + 1 ..., at most
 * H265SW_MAX_TILE_COLS and H265SW_MAX_TILE_ROWS, else ENOTSUP),
 * uniform_spacing, column_width[] and row_height[] (column_width_minus1
 * + 1 ..., in CTBs, the last one not signalled), loop_filter_across_tiles
 * (inferred 1 without tiles); loop_filter_across_slices,
 * deblocking_override_enabled, deblocking_disabled, beta_offset_div2 and
 * tc_offset_div2 (-6..6, 0 when absent), scaling_list_present with
 * scaling, lists_modification_present, log2_parallel_merge_level
 * (log2_parallel_merge_level_minus2 + 2), slice_header_extension_present.
 *
 * The range extension (7.3.2.3.2) is read when present; any of its tools
 * on (log2_max_transform_skip_block_size_minus2 above 0,
 * cross_component_prediction_enabled_flag, chroma_qp_offset_list_enabled_flag,
 * a log2_sao_offset_scale above 0) is ENOTSUP, as are the multilayer, 3D
 * and screen content extension flags.
 */
struct h265sw_pps {
	unsigned id;
	unsigned sps_id;
	int dependent_slices;
	int output_flag_present;
	unsigned num_extra_slice_header_bits;
	int sign_data_hiding;
	int cabac_init_present;
	unsigned num_ref_idx_default[2];
	int init_qp;
	int constrained_intra;
	int transform_skip;
	int cu_qp_delta;
	unsigned diff_cu_qp_delta_depth;
	int cb_qp_offset;
	int cr_qp_offset;
	int slice_chroma_qp_offsets_present;
	int weighted_pred;
	int weighted_bipred;
	int transquant_bypass;
	int tiles;
	int entropy_sync;
	unsigned tile_columns;
	unsigned tile_rows;
	int uniform_spacing;
	unsigned column_width[H265SW_MAX_TILE_COLS];
	unsigned row_height[H265SW_MAX_TILE_ROWS];
	int loop_filter_across_tiles;
	int loop_filter_across_slices;
	int deblocking_override_enabled;
	int deblocking_disabled;
	int beta_offset_div2;
	int tc_offset_div2;
	int scaling_list_present;
	struct h265sw_scaling_list scaling;
	int lists_modification_present;
	unsigned log2_parallel_merge_level;
	int slice_header_extension_present;
};

/*
 * The parameter sets seen, by id.
 *
 * sps_present[id] and pps_present[id] say whether a set was stored.
 * sps_generation[id] counts the SPSs stored under id (a picture compares
 * the generation it started with to notice a replaced SPS).
 * sps_refused[id] and pps_refused[id] are 1 when the last set of that id
 * was refused as ENOTSUP (a slice that names it is then outside the
 * decoder's range rather than broken); a stored set clears it.
 */
struct h265sw_ps_store {
	int sps_present[H265SW_SPS_IDS];
	int sps_refused[H265SW_SPS_IDS];
	unsigned sps_generation[H265SW_SPS_IDS];
	struct h265sw_sps sps[H265SW_SPS_IDS];
	int pps_present[H265SW_PPS_IDS];
	int pps_refused[H265SW_PPS_IDS];
	struct h265sw_pps pps[H265SW_PPS_IDS];
};

/* Empties a store. */
void h265sw_ps_init(struct h265sw_ps_store *store);

/*
 * Reads a sequence parameter set RBSP and keeps it in store (7.3.2.2).
 *
 * bits is at the first bit of the RBSP.  Reads every field in order:
 * profile_tier_level( 1, sps_max_sub_layers_minus1 ) (7.3.3), the
 * sub-layer ordering, the block sizes, the scaling lists
 * (h265sw_scaling_read), PCM, the short-term sets (h265sw_rps_read with
 * max_pictures = max_dec_pic_buffering_minus1 of the highest sub-layer),
 * the long-term pictures, the VUI (h265sw_vui_read) and the extension
 * flags, then the range extension when present, then the trailing bits
 * (vcodec_bits_trailing; extension data after sps_extension_4bits is
 * skipped up to the trailing bits).
 *
 * The decoder's range: chroma_format_idc 1, bit_depth_luma_minus8 and
 * bit_depth_chroma_minus8 0, ctb_log2 4..6, every range extension flag
 * 0, no multilayer, 3D or screen content extension, and a VUI without
 * field_seq_flag; anything else valid is ENOTSUP (sps_refused set).
 *
 * A VUI that fails to read (E.2.1 streams from some old encoders have
 * misplaced fields) is dropped when ptl.profile_idc is 1, 2 or 3 (Main,
 * Main 10, Main Still Picture: their extension flags are 0 by A.3.2 to
 * A.3.4): vui_present becomes 0 and the extension flags and trailing
 * bits are not read.  For other profiles it is EINVAL.
 *
 * Checks every value against 7.4.3.2.1 and the limits of h265sw.h and
 * computes the derived fields before it stores the set: a failed read
 * leaves the store as it was (except sps_refused).
 *
 * Returns 0; EINVAL for a syntax or range error; ENOTSUP as above.
 */
int h265sw_ps_read_sps(struct h265sw_ps_store *store, struct vcodec_bits *bits);

/*
 * Reads a picture parameter set RBSP and keeps it in store (7.3.2.3).
 *
 * Reads every field and the trailing bits; the extensions as struct
 * h265sw_pps says.  pps_scaling_list_data_present_flag 1 reads the lists
 * with h265sw_scaling_read.  A failed read leaves the store as it was
 * (except pps_refused).
 *
 * Returns 0; EINVAL for a syntax or range error; ENOTSUP for a tool
 * outside the range (pps_refused set).
 */
int h265sw_ps_read_pps(struct h265sw_ps_store *store, struct vcodec_bits *bits);

/*
 * Checks a PPS against the SPS it names, for the values whose ranges
 * depend on the SPS (7.4.3.3.1): diff_cu_qp_delta_depth at most
 * ctb_log2 - min_cb_log2; log2_parallel_merge_level at most ctb_log2;
 * tile_columns at most pic_width_ctbs and tile_rows at most
 * pic_height_ctbs; for non-uniform spacing, the signalled widths and
 * heights sum to less than pic_width_ctbs and pic_height_ctbs (the last
 * column and row take the rest, at least 1).  Returns 0 or EINVAL.
 */
int h265sw_ps_check_pair(const struct h265sw_sps *sps, const struct h265sw_pps *pps);

/*
 * Chooses the scaling lists a picture uses (7.4.3.3.1): none when the
 * SPS's scaling_list_enabled is 0 (returns NULL: m is flat 16), the
 * PPS's when it has its own, else the SPS's.
 */
const struct h265sw_scaling_list *h265sw_ps_scaling(const struct h265sw_sps *sps, const struct h265sw_pps *pps);

#endif
