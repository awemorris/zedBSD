/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The parameter sets of H.265: the video parameter set (7.3.2.1,
 * 7.4.3.1), the sequence parameter set (7.3.2.2, 7.4.3.2) with its
 * profile, tier and level (7.3.3, 7.4.4), the picture parameter set
 * (7.3.2.3, 7.4.3.3), and the store that keeps them by id.
 *
 * Each set is kept with every field the Vulkan Video path hands to a
 * driver (the StdVideoH265 structures name them) and with the derived
 * values the software decoder uses.  The base layer's decoding does not
 * depend on the VPS (the software decoder need not read it); the Vulkan
 * Video path reads it because a session's parameters carry it.
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
#include "vcodec-bitreader.h"

/*
 * The general profile, tier and level of profile_tier_level( 1, ... )
 * (7.3.3): profile_space general_profile_space, tier general_tier_flag,
 * profile_idc general_profile_idc, compatibility the 32
 * general_profile_compatibility_flag bits (bit j is flag[ j ]),
 * progressive, interlaced, non_packed and frame_only the
 * general_progressive_source_flag, general_interlaced_source_flag,
 * general_non_packed_constraint_flag and general_frame_only_constraint_flag,
 * level_idc general_level_idc.  The 43 constraint and reserved bits
 * after the four source flags (whichever branch of 7.3.3 applies, 43
 * bits) and the one bit after them (general_inbld_flag or
 * general_reserved_zero_bit) are read and skipped: 44 bits, so the
 * general part is 88 bits before general_level_idc's 8.  The sub-layer
 * fields are read and skipped as well.  Informative
 * for a decoder: what it decodes is decided by the SPS's fields.
 */
struct h265sw_ptl {
	unsigned profile_space;
	unsigned tier;
	unsigned profile_idc;
	uint32_t compatibility;
	int progressive;
	int interlaced;
	int non_packed;
	int frame_only;
	unsigned level_idc;
};

/*
 * One video parameter set as read (7.3.2.1), each field named after its
 * syntax element and checked against 7.4.3.1: id
 * (vps_video_parameter_set_id 0..15), base_layer_internal,
 * base_layer_available, max_layers_minus1 (0..63: 7.4.3.1 reserves 63 but
 * has decoders accept it), max_sub_layers_minus1
 * (0..6), temporal_id_nesting, then vps_reserved_0xffff_16bits (u(16),
 * read and ignored: 7.4.3.1 has decoders ignore its value), ptl,
 * sub_layer_ordering_info_present,
 * max_dec_pic_buffering_minus1[ i ] (0..H265SW_MAX_DPB - 1),
 * max_num_reorder_pics[ i ] (at most max_dec_pic_buffering_minus1[ i ])
 * and max_latency_increase_plus1[ i ] (0..2^32 - 2) for every sub-layer
 * i of 0..max_sub_layers_minus1 (the values of the highest sub-layer
 * copied down when sub_layer_ordering_info_present is 0; entries above
 * max_sub_layers_minus1 are 0), max_layer_id (0..63, like
 * max_layers_minus1),
 * num_layer_sets_minus1 (0..1023; the layer_id_included_flag bits are
 * read and skipped), timing_present with num_units_in_tick and
 * time_scale (both non-zero), poc_proportional_to_timing with
 * num_ticks_poc_diff_one_minus1 (0..2^32 - 2), num_hrd_parameters
 * (0..num_layer_sets_minus1 + 1) and extension (vps_extension_flag; the
 * extension data is skipped up to the trailing bits).
 *
 * Of the HRD parameters only the first is kept: hrd (for
 * hrd_layer_set_idx[ 0 ], whose common part is always present), valid
 * when num_hrd_parameters is above 0 (zero otherwise).  The others are
 * read (each with the common part of the one before it) and dropped:
 * Vulkan's StdVideoH265VideoParameterSet carries one, and no decoding
 * process reads them.
 */
struct h265sw_vps {
	unsigned id;
	int base_layer_internal;
	int base_layer_available;
	unsigned max_layers_minus1;
	unsigned max_sub_layers_minus1;
	int temporal_id_nesting;
	struct h265sw_ptl ptl;
	int sub_layer_ordering_info_present;
	unsigned max_dec_pic_buffering_minus1[H265SW_MAX_SUB_LAYERS];
	unsigned max_num_reorder_pics[H265SW_MAX_SUB_LAYERS];
	uint32_t max_latency_increase_plus1[H265SW_MAX_SUB_LAYERS];
	unsigned max_layer_id;
	unsigned num_layer_sets_minus1;
	int timing_present;
	uint32_t num_units_in_tick;
	uint32_t time_scale;
	int poc_proportional_to_timing;
	uint32_t num_ticks_poc_diff_one_minus1;
	unsigned num_hrd_parameters;
	struct h265sw_hrd hrd;
	int extension;
};

/*
 * One sequence parameter set as read, with the derived values a decoder
 * uses.  It keeps every format a valid SPS may name (the parse does not
 * refuse what one back end cannot decode, so a parse shared with another
 * back end keeps working): chroma_format_idc, separate_colour_plane,
 * bit_depth_luma and bit_depth_chroma (8..16), range_extension with the
 * nine flags of 7.3.2.2.2 as bits (bit n is the n-th flag in syntax
 * order, from transform_skip_rotation_enabled_flag as bit 0).
 * h265sw_ps_sps_supported says whether the software decoder takes it.
 *
 * Signalled (7.3.2.2.1), each checked against 7.4.3.2.1: vps_id
 * (sps_video_parameter_set_id 0..15), max_sub_layers_minus1 (0..6),
 * temporal_id_nesting (sps_temporal_id_nesting_flag), ptl, id
 * (sps_seq_parameter_set_id 0..15), width and height
 * (pic_width_in_luma_samples and pic_height_in_luma_samples, non-zero
 * multiples of MinCbSizeY, each at most H265SW_MAX_SIDE, product at most
 * H265SW_MAX_LUMA_PS), conformance_window (conformance_window_flag) with
 * conf_win_offset[0..3] the left, right, top and bottom offsets as
 * signalled (in chroma sample units; 0 when absent), log2_max_poc_lsb
 * (log2_max_pic_order_cnt_lsb_minus4 + 4, 4..16),
 * sub_layer_ordering_info_present.
 *
 * Derived from the window: crop_x = SubWidthC * conf_win_offset[0],
 * crop_y = SubHeightC * conf_win_offset[2], crop_width and crop_height
 * the size inside the window (at least 1; a window that leaves no sample
 * is EINVAL), all in luma samples, computed in uint64_t.
 *
 * max_dec_pic_buffering_minus1[i], max_num_reorder_pics[i] and
 * max_latency_increase_plus1[i] for every sub-layer i (the values of
 * the highest sub-layer copied down when
 * sps_sub_layer_ordering_info_present_flag is 0, 7.4.3.2.1; entries above
 * max_sub_layers_minus1 are 0); max_dec_pic_buffering_minus1
 * 0..H265SW_MAX_DPB - 1, max_num_reorder_pics at most it,
 * max_latency_increase_plus1 0..2^32 - 2.
 *
 * The block sizes, each syntax element bounded before any sum so no sum
 * can wrap: log2_min_luma_coding_block_size_minus3 and
 * log2_diff_max_min_luma_coding_block_size each at most 3 (a larger value
 * makes CtbLog2SizeY above 6: ENOTSUP), giving min_cb_log2 MinCbLog2SizeY
 * (3..6) and ctb_log2 CtbLog2SizeY (min_cb_log2 + diff, 4..6, else
 * ENOTSUP); log2_min_luma_transform_block_size_minus2 below
 * min_cb_log2 - 2 (min_tb_log2 MinTbLog2SizeY 2..min_cb_log2 - 1, else
 * EINVAL) and log2_diff_max_min_luma_transform_block_size at most
 * Min( ctb_log2, 5 ) - min_tb_log2 (max_tb_log2 MaxTbLog2SizeY, else
 * EINVAL); max_th_depth_inter and max_th_depth_intra (0..ctb_log2 -
 * min_tb_log2).
 *
 * scaling_list_enabled with scaling_list_data_present
 * (sps_scaling_list_data_present_flag) and scaling (the SPS's lists:
 * signalled, or the defaults when scaling_list_enabled is 1 and
 * scaling_list_data_present is 0; zero when scaling_list_enabled is 0);
 * amp, sao; pcm with pcm_bit_depth_luma and pcm_bit_depth_chroma (1..the
 * matching bit depth), pcm_min_log2 Log2MinIpcmCbSizeY
 * (log2_min_pcm_luma_coding_block_size_minus3 + 3, checked before the
 * sum to be in Min( min_cb_log2, 5 )..Min( ctb_log2, 5 )) and
 * pcm_max_log2 Log2MaxIpcmCbSizeY (pcm_min_log2 +
 * log2_diff_max_min_pcm_luma_coding_block_size, the diff checked first to
 * be at most Min( ctb_log2, 5 ) - pcm_min_log2), else EINVAL, and
 * pcm_loop_filter_disabled; num_st_rps (0..64) with st_rps (derived) and
 * st_syntax (as signalled) of each; long_term_present with num_lt_sps
 * (0..32), lt_poc_lsb (lt_ref_pic_poc_lsb_sps, log2_max_poc_lsb bits) and
 * lt_used (used_by_curr_pic_lt_sps_flag); temporal_mvp,
 * strong_intra_smoothing; vui_present with vui; extension_present
 * (sps_extension_present_flag) and range_extension_present
 * (sps_range_extension_flag).
 *
 * Derived: pic_width_ctbs and pic_height_ctbs (PicWidthInCtbsY,
 * PicHeightInCtbsY, 7-15 and 7-17), pic_size_ctbs (7-19), and
 * pic_width_min_cbs and pic_height_min_cbs (7-14, 7-16).
 *
 * About 22 KiB; a store holds sixteen.
 */
struct h265sw_sps {
	unsigned vps_id;
	unsigned max_sub_layers_minus1;
	int temporal_id_nesting;
	struct h265sw_ptl ptl;
	unsigned id;
	unsigned chroma_format_idc;
	int separate_colour_plane;
	unsigned width;
	unsigned height;
	int conformance_window;
	unsigned conf_win_offset[4];
	unsigned crop_x;
	unsigned crop_y;
	unsigned crop_width;
	unsigned crop_height;
	unsigned bit_depth_luma;
	unsigned bit_depth_chroma;
	unsigned log2_max_poc_lsb;
	int sub_layer_ordering_info_present;
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
	int scaling_list_data_present;
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
	struct h265sw_st_rps_syntax st_syntax[H265SW_MAX_ST_RPS - 1U];
	int long_term_present;
	unsigned num_lt_sps;
	uint32_t lt_poc_lsb[H265SW_MAX_LT_SPS];
	uint8_t lt_used[H265SW_MAX_LT_SPS];
	int temporal_mvp;
	int strong_intra_smoothing;
	int vui_present;
	struct h265sw_vui vui;
	int extension_present;
	int range_extension_present;
	unsigned range_extension;

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
 * output_flag_present, num_extra_slice_header_bits (0..7 as the u(3)
 * reads it; 7.4.3.3.1 allows 0..2 for this version, and larger values
 * are read as the syntax says), sign_data_hiding, cabac_init_present,
 * num_ref_idx_default[2] (num_ref_idx_lX_default_active_minus1 + 1,
 * 1..15), init_qp (26 + init_qp_minus26; init_qp_minus26 is in -( 26 +
 * QpBdOffsetY )..25, so init_qp is checked here against -48..51, the
 * widest range any bit depth allows, and by h265sw_ps_check_pair against
 * -QpBdOffsetY..51), constrained_intra,
 * transform_skip, cu_qp_delta with diff_cu_qp_delta_depth, cb_qp_offset
 * and cr_qp_offset (-12..12), slice_chroma_qp_offsets_present,
 * weighted_pred, weighted_bipred, transquant_bypass, tiles, entropy_sync
 * (entropy_coding_sync_enabled_flag), with tiles: tile_columns and
 * tile_rows (num_tile_columns_minus1 + 1 ..., at most
 * H265SW_MAX_TILE_COLS and H265SW_MAX_TILE_ROWS, else ENOTSUP: only
 * levels above 6.2 allow more), uniform_spacing, column_width[] and
 * row_height[] (with uniform_spacing 0: column_width_minus1[ i ] + 1 and
 * row_height_minus1[ i ] + 1 in CTBs for i below tile_columns - 1 and
 * tile_rows - 1, each checked at once to be at most H265SW_MAX_SIDE / 16
 * = 1055, EINVAL otherwise, so no later sum of them can wrap; every
 * other entry 0, the last column and row being the rest of the picture,
 * which needs the SPS), loop_filter_across_tiles (1 when absent);
 * loop_filter_across_slices, deblocking_control_present
 * (deblocking_filter_control_present_flag), deblocking_override_enabled,
 * deblocking_disabled, beta_offset_div2 and tc_offset_div2 (-6..6, 0 when
 * absent), scaling_list_present (pps_scaling_list_data_present_flag) with
 * scaling, lists_modification_present, log2_parallel_merge_level
 * (log2_parallel_merge_level_minus2 + 2), slice_header_extension_present,
 * extension_present (pps_extension_present_flag) and
 * range_extension_present (pps_range_extension_flag).
 *
 * The range extension (7.3.2.3.2) is read when present; any of its tools
 * on (log2_max_transform_skip_block_size_minus2 above 0,
 * cross_component_prediction_enabled_flag,
 * chroma_qp_offset_list_enabled_flag, a log2_sao_offset_scale above 0)
 * is ENOTSUP, as are the multilayer, 3D and screen content extension
 * flags (their syntax is not read).
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
	int deblocking_control_present;
	int deblocking_override_enabled;
	int deblocking_disabled;
	int beta_offset_div2;
	int tc_offset_div2;
	int scaling_list_present;
	struct h265sw_scaling_list scaling;
	int lists_modification_present;
	unsigned log2_parallel_merge_level;
	int slice_header_extension_present;
	int extension_present;
	int range_extension_present;
};

/*
 * The parameter sets seen, by id.
 *
 * vps_present[id], sps_present[id] and pps_present[id] say whether a set
 * was stored.  vps_refused[id], sps_refused[id] and pps_refused[id] are 1
 * when the last set of that id was refused as ENOTSUP (a syntax this parse
 * does not read: the screen content, 3D or multilayer extensions, the PPS
 * range extension tools; a slice that names it is then outside the range
 * rather than broken); a stored set clears it.
 *
 * sps_generation[id] counts the stored SPSs of id whose content differed
 * from the one stored before: an SPS sent again with the same content
 * (7.4.2.4.2 allows it inside a coded video sequence) keeps the
 * generation.  generation counts every stored VPS, SPS or PPS whose
 * content differed from the one stored before under its id (a user of
 * the sets, such as a Vulkan session's parameters, compares it to notice
 * any change).  Both counters only increase, wrapping at 2^32 (a user
 * compares for equality only).  The comparison is a memcmp of the parsed
 * structures, each built in memory cleared with memset first and written
 * only by member stores (never a whole-structure assignment or copy
 * before the comparison), so the padding bytes stay 0 on the compilers
 * zedBSD uses; should padding ever differ, the cost is a needless
 * generation step (a rebuilt session parameters object), never a missed
 * change.
 *
 * About 600 KiB: callers allocate it (h265-stream.h embeds one).
 */
struct h265sw_ps_store {
	unsigned generation;
	int vps_present[H265SW_VPS_IDS];
	int vps_refused[H265SW_VPS_IDS];
	struct h265sw_vps vps[H265SW_VPS_IDS];
	int sps_present[H265SW_SPS_IDS];
	int sps_refused[H265SW_SPS_IDS];
	unsigned sps_generation[H265SW_SPS_IDS];
	struct h265sw_sps sps[H265SW_SPS_IDS];
	int pps_present[H265SW_PPS_IDS];
	int pps_refused[H265SW_PPS_IDS];
	struct h265sw_pps pps[H265SW_PPS_IDS];
};

/* Empties a store (every flag and counter 0). */
void h265sw_ps_init(struct h265sw_ps_store *store);

/*
 * Reads a profile_tier_level( 1, max_sub_layers_minus1 ) (7.3.3) into
 * *ptl.
 *
 * max_sub_layers_minus1 is 0..6 (above is EINVAL).  Reads the general
 * part, then the sub_layer_profile_present_flag and
 * sub_layer_level_present_flag pairs, the reserved_zero_2bits up to
 * eight entries when max_sub_layers_minus1 is above 0, and skips each
 * present sub-layer part (88 bits of profile, 8 of level).  general
 * profile_space other than 0 is read and kept (7.4.4: decoders ignore
 * the CVS then; the caller decides).  Returns 0; EINVAL for a read error.
 */
int h265sw_ps_read_ptl(struct h265sw_ptl *ptl, struct vcodec_bitreader *bits, unsigned max_sub_layers_minus1);

/*
 * Reads a video parameter set RBSP and keeps it in store (7.3.2.1).
 *
 * bits is at the first bit of the RBSP.  Reads every field in order
 * (struct h265sw_vps), the HRD parameters with h265sw_vui_read_hrd
 * (cprms_present_flag[ 0 ] inferred 1; for i above 0 the flag is read
 * and the previous structure gives the common part), then the extension
 * data (skipped while more_rbsp_data()) and the trailing bits.
 * hrd_layer_set_idx[ i ] is checked against 7.4.3.1 (in ( vps_base_layer_
 * internal_flag ? 0 : 1 )..num_layer_sets_minus1 and different from
 * every earlier one, kept as a bit array of 1024 bits).
 *
 * Checks every value and builds the set in a structure cleared to 0
 * before it stores it: a failed read leaves the store as it was.  That
 * structure and the two scratch struct h265sw_hrd for the dropped HRD
 * parameters (about 22 KiB in all) are allocated with malloc for the
 * call and freed before it returns, so no large structure is on the
 * stack.
 *
 * Returns 0; EINVAL for a syntax or range error; ENOMEM when the
 * allocation fails (the store is unchanged).  (No VPS syntax is refused
 * as ENOTSUP: the extension is skipped, not read.)
 */
int h265sw_ps_read_vps(struct h265sw_ps_store *store, struct vcodec_bitreader *bits);

/*
 * Reads a sequence parameter set RBSP and keeps it in store (7.3.2.2).
 *
 * bits is at the first bit of the RBSP.  Reads every field in order:
 * profile_tier_level( 1, sps_max_sub_layers_minus1 ) (h265sw_ps_read_ptl),
 * the sub-layer ordering, the block sizes, the scaling lists
 * (h265sw_scaling_read, or h265sw_scaling_default when enabled without
 * data), PCM, the short-term sets (h265sw_rps_read for i =
 * 0..num_st_rps - 1 with num_sets num_st_rps, the sets read so far and
 * max_pictures = max_dec_pic_buffering_minus1 of the highest sub-layer,
 * with st_syntax[ i ]), the long-term pictures, the VUI (h265sw_vui_read)
 * and the extension flags, then the range extension when present, then
 * the extension data (skipped while more_rbsp_data() when
 * sps_extension_4bits is not 0) and the trailing bits
 * (vcodec_bitreader_trailing).
 *
 * Stored whatever its format (chroma_format_idc 0..3, bit depths up to
 * 16, the range extension flags).  ENOTSUP (sps_refused set): the screen
 * content, 3D and multilayer extension flags (whose syntax this parse
 * does not read), and CtbLog2SizeY outside 4..6 (every profile of Annex
 * A, A.3.2 to A.3.8, limits it so).
 *
 * A VUI that fails to read (E.2.1 streams from some old encoders have
 * misplaced fields) is dropped when ptl.profile_idc is 1, 2 or 3 (Main,
 * Main 10, Main Still Picture: their extension flags are 0 by A.3.2 to
 * A.3.4): vui_present becomes 0, vui is cleared, and the extension flags
 * and trailing bits are not read.  For other profiles it is EINVAL.
 *
 * Checks every value against 7.4.3.2.1 and the limits of h265sw.h and
 * computes the derived fields in a structure cleared to 0 (allocated
 * with malloc for the call, about 22 KiB, and freed before it returns)
 * before it stores the set: a failed read leaves the store as it was
 * (except sps_refused).
 *
 * Returns 0; EINVAL for a syntax or range error; ENOTSUP as above;
 * ENOMEM when the allocation fails (the store is unchanged).
 */
int h265sw_ps_read_sps(struct h265sw_ps_store *store, struct vcodec_bitreader *bits);

/*
 * Reports whether the software decoder takes a sequence (h265sw.h):
 * returns 0 for chroma_format_idc 1, both bit depths 8, range_extension
 * 0 and no VUI with field_seq_flag; ENOTSUP otherwise.  The software
 * decoder calls it when it activates the SPS; the Vulkan Video path
 * checks the range of its driver instead (the profile and the format the
 * driver reports).
 */
int h265sw_ps_sps_supported(const struct h265sw_sps *sps);

/*
 * Reads a picture parameter set RBSP and keeps it in store (7.3.2.3).
 *
 * Reads every field and the trailing bits; the extensions as struct
 * h265sw_pps says (with pps_extension_4bits not 0, the extension data is
 * skipped while more_rbsp_data()).  pps_scaling_list_data_present_flag 1
 * reads the lists with h265sw_scaling_read.  Builds the set in a
 * structure cleared to 0 first (on the stack: about 2 KiB); a failed read
 * leaves the store as it was (except pps_refused).
 *
 * Returns 0; EINVAL for a syntax or range error; ENOTSUP for a tool
 * outside the range (pps_refused set).
 */
int h265sw_ps_read_pps(struct h265sw_ps_store *store, struct vcodec_bitreader *bits);

/*
 * Checks a PPS against the SPS it names, for the values whose ranges
 * depend on the SPS (7.4.3.3.1): scaling_list_present 0 when the SPS's
 * scaling_list_enabled is 0; init_qp at least -QpBdOffsetY (-6 *
 * ( bit_depth_luma - 8 )); diff_cu_qp_delta_depth at most ctb_log2 -
 * min_cb_log2; log2_parallel_merge_level at most ctb_log2; tile_columns
 * at most pic_width_ctbs and tile_rows at most pic_height_ctbs; for
 * non-uniform spacing, the signalled widths and heights sum (in
 * uint64_t) to less than pic_width_ctbs and pic_height_ctbs (the last
 * column and row take the rest, at least 1).  sps->id must equal
 * pps->sps_id (else EINVAL: a caller's mistake).  Returns 0 or EINVAL.
 */
int h265sw_ps_check_pair(const struct h265sw_sps *sps, const struct h265sw_pps *pps);

#endif
