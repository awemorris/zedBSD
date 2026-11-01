/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A writer of H.265 syntax for the host unit tests of libmedia's H.265
 * parse: the tests build their inputs (RBSPs, NAL units, Annex B byte
 * streams) from the syntax tables of ITU-T H.265 (V11) (01/2026) clause
 * 7.3 and Annex E with it, so no input is copied from another decoder or
 * encoder and no code of the parse is used to make its own inputs.
 *
 * Each syntax structure has a value structure whose fields are its syntax
 * elements as signalled (named after them, in uint32_t or int32_t so a
 * test can write a value outside the element's range to check that the
 * parse refuses it), and a function that writes the structure in the
 * order and with the conditions of its syntax table.  The writer derives
 * nothing the parse derives: where a syntax table depends on a value
 * another structure gives (a bit length, a count of a referenced set, a
 * presence flag of the SPS or PPS), the caller passes it.  Where an
 * element is absent its field is not read; where a condition depends on
 * an absent element, the inferred value of the semantics is used and the
 * comment says so.
 *
 * Loops whose count comes from a field are bounded by the arrays: a count
 * above an array's size writes the count as given and then only as many
 * entries as the array holds (a test of a count out of range expects the
 * parse to stop at the count).
 *
 * Built on bitwriter.h (u(n), ue(v), se(v), rbsp_trailing_bits()).
 */

#ifndef H265SW_TEST_H265WRITE_H
#define H265SW_TEST_H265WRITE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "bitwriter.h"

/* The most sub-layers (vps_max_sub_layers_minus1 and sps_max_sub_layers_minus1 0..6). */
#define H265W_MAX_SUB_LAYERS 7U

/* The most CPB specifications of one sub_layer_hrd_parameters() (cpb_cnt_minus1 0..31). */
#define H265W_MAX_CPB 32U

/* The most entries of one explicit short-term set (num_negative_pics + num_positive_pics). */
#define H265W_MAX_RPS 16U

/* The most flag pairs of one predicted short-term set (NumDeltaPocs[ RefRpsIdx ] + 1). */
#define H265W_MAX_RPS_FLAGS 17U

/* The most short-term sets of an SPS (num_short_term_ref_pic_sets 0..64). */
#define H265W_MAX_ST_RPS 64U

/* The most long-term pictures of an SPS (num_long_term_ref_pics_sps 0..32). */
#define H265W_MAX_LT_SPS 32U

/* The most long-term entries of one slice header (num_long_term_sps + num_long_term_pics). */
#define H265W_MAX_LT 16U

/* The most entries of one reference picture list (num_ref_idx_lX_active_minus1 + 1). */
#define H265W_MAX_REFS 16U

/* The most column widths and row heights of a PPS (num_tile_columns_minus1 and num_tile_rows_minus1). */
#define H265W_MAX_TILES 32U

/* The most chroma QP offset list entries of a PPS range extension (chroma_qp_offset_list_len_minus1 0..5). */
#define H265W_MAX_QP_OFFSETS 6U

/* The most hrd_parameters() of a VPS the writer holds (vps_num_hrd_parameters). */
#define H265W_VPS_HRDS 4U

/* The most slice_segment_header_extension_data_byte a header holds. */
#define H265W_MAX_EXTENSION 256U

/* The nal_unit_type values the writer's defaults use (Table 7-1). */
#define H265W_NAL_TRAIL_R 1U
#define H265W_NAL_IDR_W_RADL 19U
#define H265W_NAL_IDR_N_LP 20U
#define H265W_NAL_VPS 32U
#define H265W_NAL_SPS 33U
#define H265W_NAL_PPS 34U

/* The slice_type values (Table 7-7). */
#define H265W_SLICE_B 0U
#define H265W_SLICE_P 1U
#define H265W_SLICE_I 2U

/*
 * One profile part of profile_tier_level() (7.3.3): the general part or
 * one sub-layer's.
 *
 * profile_space, tier and profile_idc are u(2), u(1) and u(5);
 * compatibility holds the 32 compatibility flags with flag[ j ] as bit j;
 * progressive, interlaced, non_packed and frame_only the four source and
 * constraint flags.  bits43 is the 43 bits that follow them, most
 * significant first: whichever branch of the syntax applies, those bits
 * are 43 (the constraint flags and the reserved bits), so a test sets
 * the pattern it means.  bit44 is the one bit after them (inbld_flag or
 * reserved_zero_bit).
 */
struct h265w_profile {
	uint32_t profile_space;
	uint32_t tier;
	uint32_t profile_idc;
	uint32_t compatibility;
	uint32_t progressive;
	uint32_t interlaced;
	uint32_t non_packed;
	uint32_t frame_only;
	uint64_t bits43;
	uint32_t bit44;
};

/*
 * One profile_tier_level( 1, maxNumSubLayersMinus1 ) (7.3.3).
 *
 * general and level_idc (general_level_idc, u(8)); for each sub-layer i
 * below maxNumSubLayersMinus1, profile_present[ i ] and level_present[ i ]
 * (sub_layer_profile_present_flag, sub_layer_level_present_flag),
 * sub_profile[ i ] and sub_level_idc[ i ] written when present;
 * reserved_2bits[ i ] for i of maxNumSubLayersMinus1..7 (written only when
 * maxNumSubLayersMinus1 is above 0).
 */
struct h265w_ptl {
	struct h265w_profile general;
	uint32_t level_idc;
	uint32_t profile_present[H265W_MAX_SUB_LAYERS];
	uint32_t level_present[H265W_MAX_SUB_LAYERS];
	uint32_t reserved_2bits[8];
	struct h265w_profile sub_profile[H265W_MAX_SUB_LAYERS];
	uint32_t sub_level_idc[H265W_MAX_SUB_LAYERS];
};

/*
 * One scaling_list_data() (7.3.4), as signalled.
 *
 * For sizeId 0..3 and matrixId 0..5 (only 0 and 3 for sizeId 3):
 * pred_mode[ sizeId ][ matrixId ] (scaling_list_pred_mode_flag); with it
 * 0, pred_matrix_id_delta (ue(v)); with it 1, dc_coef_minus8[ sizeId - 2
 * ][ matrixId ] (se(v), sizeId above 1) and delta_coef[ sizeId ][ matrixId
 * ][ i ] for the coefNum = Min( 64, 1 << ( 4 + ( sizeId << 1 ) ) )
 * entries (scaling_list_delta_coef, se(v)).
 */
struct h265w_scaling {
	uint32_t pred_mode[4][6];
	uint32_t pred_matrix_id_delta[4][6];
	int32_t dc_coef_minus8[2][6];
	int32_t delta_coef[4][6][64];
};

/*
 * One sub_layer_hrd_parameters( subLayerId ) (E.2.3): for each CPB
 * specification i, bit_rate_value_minus1, cpb_size_value_minus1,
 * cpb_size_du_value_minus1 and bit_rate_du_value_minus1 (the last two
 * only with sub_pic_hrd_params_present_flag), cbr_flag.
 */
struct h265w_sub_layer_hrd {
	uint32_t bit_rate_value_minus1[H265W_MAX_CPB];
	uint32_t cpb_size_value_minus1[H265W_MAX_CPB];
	uint32_t cpb_size_du_value_minus1[H265W_MAX_CPB];
	uint32_t bit_rate_du_value_minus1[H265W_MAX_CPB];
	uint32_t cbr_flag[H265W_MAX_CPB];
};

/*
 * One hrd_parameters( commonInfPresentFlag, maxNumSubLayersMinus1 )
 * (E.2.2), every field named after its syntax element.
 *
 * nal_present, vcl_present and sub_pic_present are also what the
 * sub-layer loop reads; for commonInfPresentFlag 0 the caller sets them to
 * the values of the hrd_parameters() whose common part this one shares
 * (they are then not written).  In the sub-layer loop
 * fixed_pic_rate_within_cvs[ i ] is written only when
 * fixed_pic_rate_general[ i ] is 0 and taken as 1 otherwise (E.3.2);
 * low_delay_hrd[ i ] is written only when that is 0 and taken as 0
 * otherwise; cpb_cnt_minus1[ i ] is written only when low_delay_hrd[ i ]
 * is 0 and taken as 0 otherwise.  CpbCnt is cpb_cnt_minus1 + 1.
 */
struct h265w_hrd {
	uint32_t nal_present;
	uint32_t vcl_present;
	uint32_t sub_pic_present;
	uint32_t tick_divisor_minus2;
	uint32_t du_cpb_removal_delay_increment_length_minus1;
	uint32_t sub_pic_cpb_params_in_pic_timing_sei;
	uint32_t dpb_output_delay_du_length_minus1;
	uint32_t bit_rate_scale;
	uint32_t cpb_size_scale;
	uint32_t cpb_size_du_scale;
	uint32_t initial_cpb_removal_delay_length_minus1;
	uint32_t au_cpb_removal_delay_length_minus1;
	uint32_t dpb_output_delay_length_minus1;
	uint32_t fixed_pic_rate_general[H265W_MAX_SUB_LAYERS];
	uint32_t fixed_pic_rate_within_cvs[H265W_MAX_SUB_LAYERS];
	uint32_t elemental_duration_in_tc_minus1[H265W_MAX_SUB_LAYERS];
	uint32_t low_delay_hrd[H265W_MAX_SUB_LAYERS];
	uint32_t cpb_cnt_minus1[H265W_MAX_SUB_LAYERS];
	struct h265w_sub_layer_hrd nal[H265W_MAX_SUB_LAYERS];
	struct h265w_sub_layer_hrd vcl[H265W_MAX_SUB_LAYERS];
};

/*
 * One vui_parameters() (E.2.1), every field named after its syntax
 * element (the _flag suffixes dropped).  The fields under a present flag
 * are written only when it is 1; hrd is written as hrd_parameters( 1,
 * sps_max_sub_layers_minus1 ) when timing_present and hrd_present are 1.
 * def_disp_win_offset[] are the left, right, top and bottom offsets.
 */
struct h265w_vui {
	uint32_t aspect_ratio_present;
	uint32_t aspect_ratio_idc;
	uint32_t sar_width;
	uint32_t sar_height;
	uint32_t overscan_present;
	uint32_t overscan_appropriate;
	uint32_t signal_present;
	uint32_t video_format;
	uint32_t full_range;
	uint32_t colour_present;
	uint32_t colour_primaries;
	uint32_t transfer_characteristics;
	uint32_t matrix_coeffs;
	uint32_t chroma_loc_present;
	uint32_t chroma_loc_top;
	uint32_t chroma_loc_bottom;
	uint32_t neutral_chroma;
	uint32_t field_seq;
	uint32_t frame_field_info_present;
	uint32_t default_display_window;
	uint32_t def_disp_win_offset[4];
	uint32_t timing_present;
	uint32_t num_units_in_tick;
	uint32_t time_scale;
	uint32_t poc_proportional;
	uint32_t num_ticks_poc_diff_one_minus1;
	uint32_t hrd_present;
	struct h265w_hrd hrd;
	uint32_t bitstream_restriction;
	uint32_t tiles_fixed_structure;
	uint32_t motion_vectors_over_pic_boundaries;
	uint32_t restricted_ref_pic_lists;
	uint32_t min_spatial_segmentation_idc;
	uint32_t max_bytes_per_pic_denom;
	uint32_t max_bits_per_min_cu_denom;
	uint32_t log2_max_mv_length_horizontal;
	uint32_t log2_max_mv_length_vertical;
};

/*
 * One st_ref_pic_set( stRpsIdx ) (7.3.7), as signalled.
 *
 * inter_rps_pred is inter_ref_pic_set_prediction_flag (written only when
 * stRpsIdx is not 0, taken as 0 otherwise).  With it 1: delta_idx_minus1
 * (written only when stRpsIdx is num_short_term_ref_pic_sets),
 * delta_rps_sign, abs_delta_rps_minus1, and for j = 0..ref_num_delta_pocs
 * used_by_curr_pic_flag[ j ] and, when that is 0, use_delta_flag[ j ].
 * ref_num_delta_pocs is NumDeltaPocs[ RefRpsIdx ], which the test gives
 * (the writer derives no set).  With it 0: num_negative_pics,
 * num_positive_pics, and each entry's delta_poc_sX_minus1 and
 * used_by_curr_pic_sX_flag.
 */
struct h265w_st_rps {
	uint32_t inter_rps_pred;
	uint32_t delta_idx_minus1;
	uint32_t delta_rps_sign;
	uint32_t abs_delta_rps_minus1;
	uint32_t ref_num_delta_pocs;
	uint32_t used_by_curr_pic_flag[H265W_MAX_RPS_FLAGS];
	uint32_t use_delta_flag[H265W_MAX_RPS_FLAGS];
	uint32_t num_negative_pics;
	uint32_t num_positive_pics;
	uint32_t delta_poc_s0_minus1[H265W_MAX_RPS];
	uint32_t used_by_curr_pic_s0_flag[H265W_MAX_RPS];
	uint32_t delta_poc_s1_minus1[H265W_MAX_RPS];
	uint32_t used_by_curr_pic_s1_flag[H265W_MAX_RPS];
};

/*
 * One video_parameter_set_rbsp() (7.3.2.1), every field named after its
 * syntax element.
 *
 * layer_id_included is written as every layer_id_included_flag[ i ][ j ]
 * (i = 1..num_layer_sets_minus1, j = 0..max_layer_id).  The HRD loop
 * writes hrd_layer_set_idx[ i ], cprms_present[ i ] (for i above 0) and
 * hrd[ i ] as hrd_parameters( cprms_present[ i ], max_sub_layers_minus1 )
 * with cprms_present[ 0 ] taken as 1.  With extension 1,
 * extension_data_bits bits of extension_data (the low bits, most
 * significant first; at most 32) are written as vps_extension_data_flag.
 * The RBSP ends with rbsp_trailing_bits().
 */
struct h265w_vps {
	uint32_t vps_id;
	uint32_t base_layer_internal;
	uint32_t base_layer_available;
	uint32_t max_layers_minus1;
	uint32_t max_sub_layers_minus1;
	uint32_t temporal_id_nesting;
	uint32_t reserved_0xffff_16bits;
	struct h265w_ptl ptl;
	uint32_t sub_layer_ordering_info_present;
	uint32_t max_dec_pic_buffering_minus1[H265W_MAX_SUB_LAYERS];
	uint32_t max_num_reorder_pics[H265W_MAX_SUB_LAYERS];
	uint32_t max_latency_increase_plus1[H265W_MAX_SUB_LAYERS];
	uint32_t max_layer_id;
	uint32_t num_layer_sets_minus1;
	uint32_t layer_id_included;
	uint32_t timing_present;
	uint32_t num_units_in_tick;
	uint32_t time_scale;
	uint32_t poc_proportional;
	uint32_t num_ticks_poc_diff_one_minus1;
	uint32_t num_hrd_parameters;
	uint32_t hrd_layer_set_idx[H265W_VPS_HRDS];
	uint32_t cprms_present[H265W_VPS_HRDS];
	struct h265w_hrd hrd[H265W_VPS_HRDS];
	uint32_t extension;
	uint32_t extension_data_bits;
	uint32_t extension_data;
};

/*
 * One seq_parameter_set_rbsp() (7.3.2.2.1), every field named after its
 * syntax element.
 *
 * The sub-layer ordering arrays are written for i from
 * ( sub_layer_ordering_info_present ? 0 : max_sub_layers_minus1 ) to
 * max_sub_layers_minus1.  scaling is written when scaling_list_enabled
 * and scaling_list_data_present are 1; st_rps[ i ] as st_ref_pic_set( i )
 * with num_short_term_ref_pic_sets num_st_rps; lt_poc_lsb[ i ] in
 * log2_max_poc_lsb_minus4 + 4 bits.  The extension flags
 * (range_extension_flag, multilayer_extension_flag,
 * extension_3d_flag, scc_extension_flag, extension_4bits) are written when
 * extension_present is 1; range_extension's bit n is the n-th flag of
 * sps_range_extension() (transform_skip_rotation_enabled_flag as bit 0),
 * written when range_extension_flag is 1.  The multilayer, 3D and screen
 * content extensions write nothing of their own syntax (the parse refuses
 * them on the flag).  With extension_4bits not 0, extension_data_bits bits
 * of extension_data are written as sps_extension_data_flag.  The RBSP
 * ends with rbsp_trailing_bits().
 */
struct h265w_sps {
	uint32_t vps_id;
	uint32_t max_sub_layers_minus1;
	uint32_t temporal_id_nesting;
	struct h265w_ptl ptl;
	uint32_t sps_id;
	uint32_t chroma_format_idc;
	uint32_t separate_colour_plane;
	uint32_t width;
	uint32_t height;
	uint32_t conformance_window;
	uint32_t conf_win_offset[4];
	uint32_t bit_depth_luma_minus8;
	uint32_t bit_depth_chroma_minus8;
	uint32_t log2_max_poc_lsb_minus4;
	uint32_t sub_layer_ordering_info_present;
	uint32_t max_dec_pic_buffering_minus1[H265W_MAX_SUB_LAYERS];
	uint32_t max_num_reorder_pics[H265W_MAX_SUB_LAYERS];
	uint32_t max_latency_increase_plus1[H265W_MAX_SUB_LAYERS];
	uint32_t log2_min_cb_minus3;
	uint32_t log2_diff_max_min_cb;
	uint32_t log2_min_tb_minus2;
	uint32_t log2_diff_max_min_tb;
	uint32_t max_th_depth_inter;
	uint32_t max_th_depth_intra;
	uint32_t scaling_list_enabled;
	uint32_t scaling_list_data_present;
	struct h265w_scaling scaling;
	uint32_t amp;
	uint32_t sao;
	uint32_t pcm;
	uint32_t pcm_bit_depth_luma_minus1;
	uint32_t pcm_bit_depth_chroma_minus1;
	uint32_t log2_min_pcm_minus3;
	uint32_t log2_diff_max_min_pcm;
	uint32_t pcm_loop_filter_disabled;
	uint32_t num_st_rps;
	struct h265w_st_rps st_rps[H265W_MAX_ST_RPS];
	uint32_t long_term_present;
	uint32_t num_lt_sps;
	uint32_t lt_poc_lsb[H265W_MAX_LT_SPS];
	uint32_t lt_used[H265W_MAX_LT_SPS];
	uint32_t temporal_mvp;
	uint32_t strong_intra_smoothing;
	uint32_t vui_present;
	struct h265w_vui vui;
	uint32_t extension_present;
	uint32_t range_extension_flag;
	uint32_t multilayer_extension_flag;
	uint32_t extension_3d_flag;
	uint32_t scc_extension_flag;
	uint32_t extension_4bits;
	uint32_t range_extension;
	uint32_t extension_data_bits;
	uint32_t extension_data;
};

/*
 * One pic_parameter_set_rbsp() (7.3.2.3.1) with its range extension
 * (7.3.2.3.2), every field named after its syntax element.
 *
 * The tile fields are written when tiles is 1, column_width_minus1 and
 * row_height_minus1 when uniform_spacing is 0; the deblocking fields
 * when deblocking_control_present is 1 (the offsets when
 * deblocking_disabled is 0); scaling when scaling_list_present is 1.  The
 * extension flags are written when extension_present is 1, the range
 * extension (log2_max_transform_skip_block_size_minus2 only when
 * transform_skip is 1, the chroma QP offset list when
 * chroma_qp_offset_list_enabled is 1) when range_extension_flag is 1; the
 * multilayer, 3D and screen content extensions write nothing of their own
 * syntax.  With extension_4bits not 0, extension_data_bits bits of
 * extension_data are written as pps_extension_data_flag.  The RBSP ends
 * with rbsp_trailing_bits().
 */
struct h265w_pps {
	uint32_t pps_id;
	uint32_t sps_id;
	uint32_t dependent_slices;
	uint32_t output_flag_present;
	uint32_t num_extra_slice_header_bits;
	uint32_t sign_data_hiding;
	uint32_t cabac_init_present;
	uint32_t num_ref_idx_l0_default_minus1;
	uint32_t num_ref_idx_l1_default_minus1;
	int32_t init_qp_minus26;
	uint32_t constrained_intra;
	uint32_t transform_skip;
	uint32_t cu_qp_delta;
	uint32_t diff_cu_qp_delta_depth;
	int32_t cb_qp_offset;
	int32_t cr_qp_offset;
	uint32_t slice_chroma_qp_offsets_present;
	uint32_t weighted_pred;
	uint32_t weighted_bipred;
	uint32_t transquant_bypass;
	uint32_t tiles;
	uint32_t entropy_sync;
	uint32_t num_tile_columns_minus1;
	uint32_t num_tile_rows_minus1;
	uint32_t uniform_spacing;
	uint32_t column_width_minus1[H265W_MAX_TILES];
	uint32_t row_height_minus1[H265W_MAX_TILES];
	uint32_t loop_filter_across_tiles;
	uint32_t loop_filter_across_slices;
	uint32_t deblocking_control_present;
	uint32_t deblocking_override_enabled;
	uint32_t deblocking_disabled;
	int32_t beta_offset_div2;
	int32_t tc_offset_div2;
	uint32_t scaling_list_present;
	struct h265w_scaling scaling;
	uint32_t lists_modification_present;
	uint32_t log2_parallel_merge_level_minus2;
	uint32_t slice_header_extension_present;
	uint32_t extension_present;
	uint32_t range_extension_flag;
	uint32_t multilayer_extension_flag;
	uint32_t extension_3d_flag;
	uint32_t scc_extension_flag;
	uint32_t extension_4bits;
	uint32_t log2_max_transform_skip_block_size_minus2;
	uint32_t cross_component_prediction;
	uint32_t chroma_qp_offset_list_enabled;
	uint32_t diff_cu_chroma_qp_offset_depth;
	uint32_t chroma_qp_offset_list_len_minus1;
	int32_t cb_qp_offset_list[H265W_MAX_QP_OFFSETS];
	int32_t cr_qp_offset_list[H265W_MAX_QP_OFFSETS];
	uint32_t log2_sao_offset_scale_luma;
	uint32_t log2_sao_offset_scale_chroma;
	uint32_t extension_data_bits;
	uint32_t extension_data;
};

/*
 * What a slice segment header's syntax reads from its NAL unit, SPS and
 * PPS: the test fills it (h265w_slice_context_from fills it from writer
 * structures) and may set a value that disagrees with them to build a
 * broken header.
 *
 * nal_type (nal_unit_type); from the PPS dependent_slices,
 * num_extra_slice_header_bits, output_flag_present, cabac_init_present,
 * num_ref_idx_default_minus1[2], lists_modification_present,
 * weighted_pred, weighted_bipred, slice_chroma_qp_offsets_present,
 * deblocking_override_enabled, pps_deblocking_disabled (the inferred
 * slice_deblocking_filter_disabled_flag), loop_filter_across_slices
 * (pps_loop_filter_across_slices_enabled_flag), tiles, entropy_sync,
 * slice_header_extension_present; from the SPS separate_colour_plane,
 * log2_max_poc_lsb (the bits of slice_pic_order_cnt_lsb and poc_lsb_lt),
 * num_st_rps, long_term_present, num_lt_sps, temporal_mvp, sao,
 * chroma_array_type (ChromaArrayType, 0 with separate_colour_plane_flag
 * 1, else chroma_format_idc), pic_size_ctbs (PicSizeInCtbsY, for the
 * bits of slice_segment_address); and num_pic_total_curr
 * (NumPicTotalCurr of the header being written, for the condition and
 * the bits of the list modification), which the test computes.
 */
struct h265w_slice_context {
	uint32_t nal_type;
	uint32_t dependent_slices;
	uint32_t num_extra_slice_header_bits;
	uint32_t output_flag_present;
	uint32_t cabac_init_present;
	uint32_t num_ref_idx_default_minus1[2];
	uint32_t lists_modification_present;
	uint32_t weighted_pred;
	uint32_t weighted_bipred;
	uint32_t slice_chroma_qp_offsets_present;
	uint32_t deblocking_override_enabled;
	uint32_t pps_deblocking_disabled;
	uint32_t loop_filter_across_slices;
	uint32_t tiles;
	uint32_t entropy_sync;
	uint32_t slice_header_extension_present;
	uint32_t separate_colour_plane;
	uint32_t log2_max_poc_lsb;
	uint32_t num_st_rps;
	uint32_t long_term_present;
	uint32_t num_lt_sps;
	uint32_t temporal_mvp;
	uint32_t sao;
	uint32_t chroma_array_type;
	uint32_t pic_size_ctbs;
	uint32_t num_pic_total_curr;
};

/*
 * One slice_segment_header() (7.3.6.1) with ref_pic_lists_modification()
 * (7.3.6.2) and pred_weight_table() (7.3.6.3), every field named after
 * its syntax element; list X is index X, entry i index i, chroma
 * component j index j.
 *
 * Inferred values the conditions use when an element is absent:
 * dependent_slice_segment_flag 0 (written only when first_in_pic is 0 and
 * the PPS has dependent_slices), slice_temporal_mvp_enabled_flag 0 (for
 * an IDR picture or an SPS without it), slice_sao_luma_flag and
 * slice_sao_chroma_flag 0, num_ref_idx_lX_active_minus1 the PPS default
 * (without num_ref_idx_active_override_flag), collocated_from_l0_flag 1
 * (other than for B), deblocking_filter_override_flag 0,
 * slice_deblocking_filter_disabled_flag the PPS's (without the override).
 *
 * reserved_flags holds slice_reserved_flag[ i ] as bit i; st_rps is
 * written as st_ref_pic_set( num_short_term_ref_pic_sets ) when
 * st_rps_sps is 0; lt_idx_sps[ i ] for i below num_long_term_sps,
 * poc_lsb_lt[ i ] and used_by_curr_pic_lt[ i ] for the others.  Every
 * entry_point_offset_minus1[ i ] is written as entry_point_offset_minus1
 * in offset_len_minus1 + 1 bits (at most 32).  extension_data[] are the
 * slice_segment_header_extension_data_byte.  The header ends with
 * byte_alignment(): alignment_one as alignment_bit_equal_to_one (1 in a
 * valid header) and the zero bits to the byte boundary, each written as
 * alignment_zero (0 in a valid header).
 */
struct h265w_slice {
	uint32_t first_in_pic;
	uint32_t no_output_of_prior_pics;
	uint32_t pps_id;
	uint32_t dependent;
	uint32_t address;
	uint32_t reserved_flags;
	uint32_t slice_type;
	uint32_t pic_output;
	uint32_t colour_plane_id;
	uint32_t poc_lsb;
	uint32_t st_rps_sps;
	struct h265w_st_rps st_rps;
	uint32_t st_rps_idx;
	uint32_t num_long_term_sps;
	uint32_t num_long_term_pics;
	uint32_t lt_idx_sps[H265W_MAX_LT];
	uint32_t poc_lsb_lt[H265W_MAX_LT];
	uint32_t used_by_curr_pic_lt[H265W_MAX_LT];
	uint32_t delta_poc_msb_present[H265W_MAX_LT];
	uint32_t delta_poc_msb_cycle_lt[H265W_MAX_LT];
	uint32_t temporal_mvp;
	uint32_t sao_luma;
	uint32_t sao_chroma;
	uint32_t num_ref_idx_override;
	uint32_t num_ref_idx_active_minus1[2];
	uint32_t list_mod[2];
	uint32_t list_entry[2][H265W_MAX_REFS];
	uint32_t mvd_l1_zero;
	uint32_t cabac_init;
	uint32_t collocated_from_l0;
	uint32_t collocated_ref_idx;
	uint32_t luma_log2_weight_denom;
	int32_t delta_chroma_log2_weight_denom;
	uint32_t luma_weight_flag[2][H265W_MAX_REFS];
	uint32_t chroma_weight_flag[2][H265W_MAX_REFS];
	int32_t delta_luma_weight[2][H265W_MAX_REFS];
	int32_t luma_offset[2][H265W_MAX_REFS];
	int32_t delta_chroma_weight[2][H265W_MAX_REFS][2];
	int32_t delta_chroma_offset[2][H265W_MAX_REFS][2];
	uint32_t five_minus_max_num_merge_cand;
	int32_t slice_qp_delta;
	int32_t cb_qp_offset;
	int32_t cr_qp_offset;
	uint32_t deblocking_override;
	uint32_t deblocking_disabled;
	int32_t beta_offset_div2;
	int32_t tc_offset_div2;
	uint32_t loop_filter_across_slices;
	uint32_t num_entry_point_offsets;
	uint32_t offset_len_minus1;
	uint32_t entry_point_offset_minus1;
	uint32_t extension_length;
	uint8_t extension_data[H265W_MAX_EXTENSION];
	uint32_t alignment_one;
	uint32_t alignment_zero;
};

/*
 * An Annex B byte stream being built in a buffer the test owns (B.2):
 * each NAL unit is written after a start code of start_code_bytes bytes
 * (4, a zero_byte and start_code_prefix_one_3bytes, by default; 3 leaves
 * the zero_byte out).  overflow is set when a NAL unit did not fit (it is
 * then not written).
 */
struct h265w_stream {
	uint8_t *data;
	size_t size;
	size_t capacity;
	unsigned start_code_bytes;
	int overflow;
};

/* Returns Ceil( Log2( value ) ) (5.7): 0 for value 0 and 1. */
static __inline unsigned
h265w_ceil_log2(
	uint32_t value)
{
	unsigned bits;

	/* The fewest bits whose range reaches value. */
	bits = 0U;
	while (bits < 32U && ((uint64_t)1U << bits) < (uint64_t)value)
		bits++;

	/* Succeeded: the bit length. */
	return bits;
}

/* Writes the count (0..64) low bits of value, most significant first. */
static __inline void
h265w_u64(
	struct bitwriter *writer,
	uint64_t value,
	unsigned count)
{
	/* A count above 32 is written as its high part, then 32 low bits. */
	if (count > 32U) {
		bitwriter_u(writer, (uint32_t)(value >> 32), count - 32U);
		bitwriter_u(writer, (uint32_t)(value & 0xFFFFFFFFU), 32U);
	} else {
		bitwriter_u(writer, (uint32_t)value, count);
	}
}

/* Writes one profile part of profile_tier_level() (7.3.3): 88 bits. */
static __inline void
h265w_profile(
	struct bitwriter *writer,
	const struct h265w_profile *profile)
{
	unsigned index;

	/* The profile space, tier and profile. */
	bitwriter_u(writer, profile->profile_space, 2U);
	bitwriter_u(writer, profile->tier, 1U);
	bitwriter_u(writer, profile->profile_idc, 5U);

	/* The 32 compatibility flags, flag[ 0 ] first. */
	for (index = 0U; index < 32U; index++)
		bitwriter_u(writer, (profile->compatibility >> index) & 1U, 1U);

	/* The four source and constraint flags. */
	bitwriter_u(writer, profile->progressive, 1U);
	bitwriter_u(writer, profile->interlaced, 1U);
	bitwriter_u(writer, profile->non_packed, 1U);
	bitwriter_u(writer, profile->frame_only, 1U);

	/* The 43 constraint and reserved bits and the inbld or reserved bit. */
	h265w_u64(writer, profile->bits43, 43U);
	bitwriter_u(writer, profile->bit44, 1U);
}

/* Writes profile_tier_level( 1, max_sub_layers_minus1 ) (7.3.3). */
static __inline void
h265w_ptl(
	struct bitwriter *writer,
	const struct h265w_ptl *ptl,
	unsigned max_sub_layers_minus1)
{
	unsigned index;
	unsigned count;

	/* The sub-layer arrays hold at most six entries below the highest. */
	count = max_sub_layers_minus1;
	if (count > H265W_MAX_SUB_LAYERS - 1U)
		count = H265W_MAX_SUB_LAYERS - 1U;

	/* The general profile and level. */
	h265w_profile(writer, &ptl->general);
	bitwriter_u(writer, ptl->level_idc, 8U);

	/* The present flag pairs of the sub-layers below the highest. */
	for (index = 0U; index < count; index++) {
		bitwriter_u(writer, ptl->profile_present[index], 1U);
		bitwriter_u(writer, ptl->level_present[index], 1U);
	}

	/* The reserved pairs up to eight entries, when there are sub-layers. */
	if (count > 0U) {
		for (index = count; index < 8U; index++)
			bitwriter_u(writer, ptl->reserved_2bits[index], 2U);
	}

	/* Each present sub-layer profile and level. */
	for (index = 0U; index < count; index++) {
		if (ptl->profile_present[index] != 0U)
			h265w_profile(writer, &ptl->sub_profile[index]);
		if (ptl->level_present[index] != 0U)
			bitwriter_u(writer, ptl->sub_level_idc[index], 8U);
	}
}

/*
 * Writes scaling_list_data() (7.3.4).  coefNum entries of delta_coef
 * are written for a list with pred_mode 1.
 */
static __inline void
h265w_scaling(
	struct bitwriter *writer,
	const struct h265w_scaling *scaling)
{
	unsigned size_id;
	unsigned matrix_id;
	unsigned coefficients;
	unsigned index;

	/* Every list the syntax codes: six per size, two of the 32x32 size. */
	for (size_id = 0U; size_id < 4U; size_id++) {
		for (matrix_id = 0U; matrix_id < 6U; matrix_id += (size_id == 3U) ? 3U : 1U) {
			bitwriter_u(writer, scaling->pred_mode[size_id][matrix_id], 1U);

			/* A predicted list names only its reference. */
			if (scaling->pred_mode[size_id][matrix_id] == 0U) {
				bitwriter_ue(writer, scaling->pred_matrix_id_delta[size_id][matrix_id]);
				continue;
			}

			/* A signalled list: the DC of the larger sizes, then the deltas. */
			coefficients = (size_id == 0U) ? 16U : 64U;
			if (size_id > 1U)
				bitwriter_se(writer, scaling->dc_coef_minus8[size_id - 2U][matrix_id]);

			/* One delta per coefficient, in the coded order. */
			for (index = 0U; index < coefficients; index++)
				bitwriter_se(writer, scaling->delta_coef[size_id][matrix_id][index]);
		}
	}
}

/*
 * Fills scaling with signalled lists (pred_mode 1 everywhere) that give
 * the lists list[ sizeId ][ matrixId ][ i ] and the DC values dc[ sizeId
 * - 2 ][ matrixId ]: each delta is the one of -128..127 that the
 * nextCoef recurrence of 7.3.4 turns into the wanted value.  Entries the
 * syntax does not code are left 0.  Every wanted value must be 1..255 for
 * the lists to be valid (the writer does not check).
 */
static __inline void
h265w_scaling_from_lists(
	struct h265w_scaling *scaling,
	const uint8_t list[4][6][64],
	const uint8_t dc[2][6])
{
	unsigned size_id;
	unsigned matrix_id;
	unsigned coefficients;
	unsigned index;
	int32_t next;
	int32_t delta;

	/* Starts from no list and no delta. */
	memset(scaling, 0, sizeof(*scaling));

	/* Every list the syntax codes, as a signalled one. */
	for (size_id = 0U; size_id < 4U; size_id++) {
		for (matrix_id = 0U; matrix_id < 6U; matrix_id += (size_id == 3U) ? 3U : 1U) {
			scaling->pred_mode[size_id][matrix_id] = 1U;
			coefficients = (size_id == 0U) ? 16U : 64U;

			/* The recurrence starts at 8, or at the DC for the larger sizes. */
			next = 8;
			if (size_id > 1U) {
				scaling->dc_coef_minus8[size_id - 2U][matrix_id] = (int32_t)dc[size_id - 2U][matrix_id] - 8;
				next = (int32_t)dc[size_id - 2U][matrix_id];
			}

			/* Each delta, brought into -128..127 modulo 256. */
			for (index = 0U; index < coefficients; index++) {
				delta = ((int32_t)list[size_id][matrix_id][index] - next + 256) % 256;
				if (delta > 127)
					delta -= 256;
				scaling->delta_coef[size_id][matrix_id][index] = delta;
				next = (int32_t)list[size_id][matrix_id][index];
			}
		}
	}
}

/* Writes sub_layer_hrd_parameters() (E.2.3) for count CPB specifications. */
static __inline void
h265w_sub_layer_hrd(
	struct bitwriter *writer,
	const struct h265w_sub_layer_hrd *hrd,
	unsigned count,
	uint32_t sub_pic_present)
{
	unsigned index;

	/* Each CPB specification: the rate and size, the DU ones, the CBR flag. */
	for (index = 0U; index < count && index < H265W_MAX_CPB; index++) {
		bitwriter_ue(writer, hrd->bit_rate_value_minus1[index]);
		bitwriter_ue(writer, hrd->cpb_size_value_minus1[index]);
		if (sub_pic_present != 0U) {
			bitwriter_ue(writer, hrd->cpb_size_du_value_minus1[index]);
			bitwriter_ue(writer, hrd->bit_rate_du_value_minus1[index]);
		}
		bitwriter_u(writer, hrd->cbr_flag[index], 1U);
	}
}

/* Writes hrd_parameters( common_present, max_sub_layers_minus1 ) (E.2.2). */
static __inline void
h265w_hrd(
	struct bitwriter *writer,
	const struct h265w_hrd *hrd,
	int common_present,
	unsigned max_sub_layers_minus1)
{
	unsigned index;
	uint32_t within_cvs;
	uint32_t low_delay;
	uint32_t cpb_count;

	/* The common part, when present. */
	if (common_present) {
		bitwriter_u(writer, hrd->nal_present, 1U);
		bitwriter_u(writer, hrd->vcl_present, 1U);

		/* The fields of a CPB, when either kind of HRD is present. */
		if (hrd->nal_present != 0U || hrd->vcl_present != 0U) {
			bitwriter_u(writer, hrd->sub_pic_present, 1U);
			if (hrd->sub_pic_present != 0U) {
				bitwriter_u(writer, hrd->tick_divisor_minus2, 8U);
				bitwriter_u(writer, hrd->du_cpb_removal_delay_increment_length_minus1, 5U);
				bitwriter_u(writer, hrd->sub_pic_cpb_params_in_pic_timing_sei, 1U);
				bitwriter_u(writer, hrd->dpb_output_delay_du_length_minus1, 5U);
			}

			/* The scales and the lengths of the timing fields. */
			bitwriter_u(writer, hrd->bit_rate_scale, 4U);
			bitwriter_u(writer, hrd->cpb_size_scale, 4U);
			if (hrd->sub_pic_present != 0U)
				bitwriter_u(writer, hrd->cpb_size_du_scale, 4U);
			bitwriter_u(writer, hrd->initial_cpb_removal_delay_length_minus1, 5U);
			bitwriter_u(writer, hrd->au_cpb_removal_delay_length_minus1, 5U);
			bitwriter_u(writer, hrd->dpb_output_delay_length_minus1, 5U);
		}
	}

	/* Each sub-layer: the picture rate, the CPB count, the sub-layer HRDs. */
	for (index = 0U; index <= max_sub_layers_minus1 && index < H265W_MAX_SUB_LAYERS; index++) {
		bitwriter_u(writer, hrd->fixed_pic_rate_general[index], 1U);
		within_cvs = 1U;
		if (hrd->fixed_pic_rate_general[index] == 0U) {
			bitwriter_u(writer, hrd->fixed_pic_rate_within_cvs[index], 1U);
			within_cvs = hrd->fixed_pic_rate_within_cvs[index];
		}

		/* The elemental duration of a fixed rate, else the low delay flag. */
		low_delay = 0U;
		if (within_cvs != 0U) {
			bitwriter_ue(writer, hrd->elemental_duration_in_tc_minus1[index]);
		} else {
			bitwriter_u(writer, hrd->low_delay_hrd[index], 1U);
			low_delay = hrd->low_delay_hrd[index];
		}

		/* The CPB count, inferred 1 under low delay. */
		cpb_count = 1U;
		if (low_delay == 0U) {
			bitwriter_ue(writer, hrd->cpb_cnt_minus1[index]);
			cpb_count = hrd->cpb_cnt_minus1[index] + 1U;
		}

		/* The NAL and VCL sub-layer parameters, as the flags say. */
		if (hrd->nal_present != 0U)
			h265w_sub_layer_hrd(writer, &hrd->nal[index], cpb_count, hrd->sub_pic_present);
		if (hrd->vcl_present != 0U)
			h265w_sub_layer_hrd(writer, &hrd->vcl[index], cpb_count, hrd->sub_pic_present);
	}
}

/* Writes vui_parameters() (E.2.1). */
static __inline void
h265w_vui(
	struct bitwriter *writer,
	const struct h265w_vui *vui,
	unsigned max_sub_layers_minus1)
{
	unsigned index;

	/* The sample aspect ratio, explicit for EXTENDED_SAR (255). */
	bitwriter_u(writer, vui->aspect_ratio_present, 1U);
	if (vui->aspect_ratio_present != 0U) {
		bitwriter_u(writer, vui->aspect_ratio_idc, 8U);
		if (vui->aspect_ratio_idc == 255U) {
			bitwriter_u(writer, vui->sar_width, 16U);
			bitwriter_u(writer, vui->sar_height, 16U);
		}
	}

	/* The overscan. */
	bitwriter_u(writer, vui->overscan_present, 1U);
	if (vui->overscan_present != 0U)
		bitwriter_u(writer, vui->overscan_appropriate, 1U);

	/* The video signal type and the colour description. */
	bitwriter_u(writer, vui->signal_present, 1U);
	if (vui->signal_present != 0U) {
		bitwriter_u(writer, vui->video_format, 3U);
		bitwriter_u(writer, vui->full_range, 1U);
		bitwriter_u(writer, vui->colour_present, 1U);
		if (vui->colour_present != 0U) {
			bitwriter_u(writer, vui->colour_primaries, 8U);
			bitwriter_u(writer, vui->transfer_characteristics, 8U);
			bitwriter_u(writer, vui->matrix_coeffs, 8U);
		}
	}

	/* The chroma sample locations. */
	bitwriter_u(writer, vui->chroma_loc_present, 1U);
	if (vui->chroma_loc_present != 0U) {
		bitwriter_ue(writer, vui->chroma_loc_top);
		bitwriter_ue(writer, vui->chroma_loc_bottom);
	}

	/* The three flags of the picture structure. */
	bitwriter_u(writer, vui->neutral_chroma, 1U);
	bitwriter_u(writer, vui->field_seq, 1U);
	bitwriter_u(writer, vui->frame_field_info_present, 1U);

	/* The default display window: left, right, top, bottom. */
	bitwriter_u(writer, vui->default_display_window, 1U);
	if (vui->default_display_window != 0U) {
		for (index = 0U; index < 4U; index++)
			bitwriter_ue(writer, vui->def_disp_win_offset[index]);
	}

	/* The timing, with the HRD parameters inside it. */
	bitwriter_u(writer, vui->timing_present, 1U);
	if (vui->timing_present != 0U) {
		bitwriter_u(writer, vui->num_units_in_tick, 32U);
		bitwriter_u(writer, vui->time_scale, 32U);
		bitwriter_u(writer, vui->poc_proportional, 1U);
		if (vui->poc_proportional != 0U)
			bitwriter_ue(writer, vui->num_ticks_poc_diff_one_minus1);
		bitwriter_u(writer, vui->hrd_present, 1U);
		if (vui->hrd_present != 0U)
			h265w_hrd(writer, &vui->hrd, 1, max_sub_layers_minus1);
	}

	/* The bitstream restrictions. */
	bitwriter_u(writer, vui->bitstream_restriction, 1U);
	if (vui->bitstream_restriction != 0U) {
		bitwriter_u(writer, vui->tiles_fixed_structure, 1U);
		bitwriter_u(writer, vui->motion_vectors_over_pic_boundaries, 1U);
		bitwriter_u(writer, vui->restricted_ref_pic_lists, 1U);
		bitwriter_ue(writer, vui->min_spatial_segmentation_idc);
		bitwriter_ue(writer, vui->max_bytes_per_pic_denom);
		bitwriter_ue(writer, vui->max_bits_per_min_cu_denom);
		bitwriter_ue(writer, vui->log2_max_mv_length_horizontal);
		bitwriter_ue(writer, vui->log2_max_mv_length_vertical);
	}
}

/*
 * Writes st_ref_pic_set( index ) (7.3.7) of an SPS with num_sets
 * num_short_term_ref_pic_sets (index == num_sets for a slice header's).
 */
static __inline void
h265w_st_rps(
	struct bitwriter *writer,
	const struct h265w_st_rps *rps,
	unsigned index,
	unsigned num_sets)
{
	unsigned entry;
	uint32_t predicted;

	/* The prediction flag, absent (0) for the first set. */
	predicted = 0U;
	if (index != 0U) {
		bitwriter_u(writer, rps->inter_rps_pred, 1U);
		predicted = rps->inter_rps_pred;
	}

	/* A predicted set: its reference, the delta and the flag pairs. */
	if (predicted != 0U) {
		if (index == num_sets)
			bitwriter_ue(writer, rps->delta_idx_minus1);
		bitwriter_u(writer, rps->delta_rps_sign, 1U);
		bitwriter_ue(writer, rps->abs_delta_rps_minus1);

		/* One pair for each entry of the reference set and one more. */
		for (entry = 0U; entry <= rps->ref_num_delta_pocs && entry < H265W_MAX_RPS_FLAGS; entry++) {
			bitwriter_u(writer, rps->used_by_curr_pic_flag[entry], 1U);
			if (rps->used_by_curr_pic_flag[entry] == 0U)
				bitwriter_u(writer, rps->use_delta_flag[entry], 1U);
		}

		return;
	}

	/* An explicit set: the counts, then each entry's delta and flag. */
	bitwriter_ue(writer, rps->num_negative_pics);
	bitwriter_ue(writer, rps->num_positive_pics);
	for (entry = 0U; entry < rps->num_negative_pics && entry < H265W_MAX_RPS; entry++) {
		bitwriter_ue(writer, rps->delta_poc_s0_minus1[entry]);
		bitwriter_u(writer, rps->used_by_curr_pic_s0_flag[entry], 1U);
	}

	/* The positive entries follow the negative ones. */
	for (entry = 0U; entry < rps->num_positive_pics && entry < H265W_MAX_RPS; entry++) {
		bitwriter_ue(writer, rps->delta_poc_s1_minus1[entry]);
		bitwriter_u(writer, rps->used_by_curr_pic_s1_flag[entry], 1U);
	}
}

/* Writes count (at most 32) low bits of data as extension data flags. */
static __inline void
h265w_extension_data(
	struct bitwriter *writer,
	uint32_t data,
	uint32_t count)
{
	/* The flags, most significant first, as one field. */
	if (count > 32U)
		count = 32U;
	bitwriter_u(writer, data, (unsigned)count);
}

/* Writes video_parameter_set_rbsp() (7.3.2.1) with its trailing bits. */
static __inline void
h265w_vps(
	struct bitwriter *writer,
	const struct h265w_vps *vps)
{
	unsigned index;
	unsigned layer;
	unsigned first;
	int common;

	/* The fixed fields up to the profile, tier and level. */
	bitwriter_u(writer, vps->vps_id, 4U);
	bitwriter_u(writer, vps->base_layer_internal, 1U);
	bitwriter_u(writer, vps->base_layer_available, 1U);
	bitwriter_u(writer, vps->max_layers_minus1, 6U);
	bitwriter_u(writer, vps->max_sub_layers_minus1, 3U);
	bitwriter_u(writer, vps->temporal_id_nesting, 1U);
	bitwriter_u(writer, vps->reserved_0xffff_16bits, 16U);
	h265w_ptl(writer, &vps->ptl, vps->max_sub_layers_minus1);

	/* The sub-layer ordering, of every sub-layer or the highest only. */
	bitwriter_u(writer, vps->sub_layer_ordering_info_present, 1U);
	first = (vps->sub_layer_ordering_info_present != 0U) ? 0U : vps->max_sub_layers_minus1;
	for (index = first; index <= vps->max_sub_layers_minus1 && index < H265W_MAX_SUB_LAYERS; index++) {
		bitwriter_ue(writer, vps->max_dec_pic_buffering_minus1[index]);
		bitwriter_ue(writer, vps->max_num_reorder_pics[index]);
		bitwriter_ue(writer, vps->max_latency_increase_plus1[index]);
	}

	/* The layer sets: one flag per layer id of each set after the first. */
	bitwriter_u(writer, vps->max_layer_id, 6U);
	bitwriter_ue(writer, vps->num_layer_sets_minus1);
	for (index = 1U; index <= vps->num_layer_sets_minus1; index++) {
		for (layer = 0U; layer <= vps->max_layer_id; layer++)
			bitwriter_u(writer, vps->layer_id_included, 1U);
	}

	/* The timing, with the HRD parameters inside it. */
	bitwriter_u(writer, vps->timing_present, 1U);
	if (vps->timing_present != 0U) {
		bitwriter_u(writer, vps->num_units_in_tick, 32U);
		bitwriter_u(writer, vps->time_scale, 32U);
		bitwriter_u(writer, vps->poc_proportional, 1U);
		if (vps->poc_proportional != 0U)
			bitwriter_ue(writer, vps->num_ticks_poc_diff_one_minus1);
		bitwriter_ue(writer, vps->num_hrd_parameters);

		/* Each HRD: its layer set, its common part flag (after the first), itself. */
		for (index = 0U; index < vps->num_hrd_parameters && index < H265W_VPS_HRDS; index++) {
			bitwriter_ue(writer, vps->hrd_layer_set_idx[index]);
			common = 1;
			if (index > 0U) {
				bitwriter_u(writer, vps->cprms_present[index], 1U);
				common = (vps->cprms_present[index] != 0U) ? 1 : 0;
			}
			h265w_hrd(writer, &vps->hrd[index], common, vps->max_sub_layers_minus1);
		}
	}

	/* The extension, then the trailing bits. */
	bitwriter_u(writer, vps->extension, 1U);
	if (vps->extension != 0U)
		h265w_extension_data(writer, vps->extension_data, vps->extension_data_bits);
	bitwriter_trailing(writer);
}

/* Writes seq_parameter_set_rbsp() (7.3.2.2) with its trailing bits. */
static __inline void
h265w_sps(
	struct bitwriter *writer,
	const struct h265w_sps *sps)
{
	unsigned index;
	unsigned first;
	unsigned lsb_bits;

	/* The ids, the sub-layers and the profile, tier and level. */
	bitwriter_u(writer, sps->vps_id, 4U);
	bitwriter_u(writer, sps->max_sub_layers_minus1, 3U);
	bitwriter_u(writer, sps->temporal_id_nesting, 1U);
	h265w_ptl(writer, &sps->ptl, sps->max_sub_layers_minus1);
	bitwriter_ue(writer, sps->sps_id);

	/* The format and the size. */
	bitwriter_ue(writer, sps->chroma_format_idc);
	if (sps->chroma_format_idc == 3U)
		bitwriter_u(writer, sps->separate_colour_plane, 1U);
	bitwriter_ue(writer, sps->width);
	bitwriter_ue(writer, sps->height);

	/* The conformance window: left, right, top, bottom. */
	bitwriter_u(writer, sps->conformance_window, 1U);
	if (sps->conformance_window != 0U) {
		for (index = 0U; index < 4U; index++)
			bitwriter_ue(writer, sps->conf_win_offset[index]);
	}

	/* The bit depths and the order count length. */
	bitwriter_ue(writer, sps->bit_depth_luma_minus8);
	bitwriter_ue(writer, sps->bit_depth_chroma_minus8);
	bitwriter_ue(writer, sps->log2_max_poc_lsb_minus4);

	/* The sub-layer ordering, of every sub-layer or the highest only. */
	bitwriter_u(writer, sps->sub_layer_ordering_info_present, 1U);
	first = (sps->sub_layer_ordering_info_present != 0U) ? 0U : sps->max_sub_layers_minus1;
	for (index = first; index <= sps->max_sub_layers_minus1 && index < H265W_MAX_SUB_LAYERS; index++) {
		bitwriter_ue(writer, sps->max_dec_pic_buffering_minus1[index]);
		bitwriter_ue(writer, sps->max_num_reorder_pics[index]);
		bitwriter_ue(writer, sps->max_latency_increase_plus1[index]);
	}

	/* The coding and transform block sizes. */
	bitwriter_ue(writer, sps->log2_min_cb_minus3);
	bitwriter_ue(writer, sps->log2_diff_max_min_cb);
	bitwriter_ue(writer, sps->log2_min_tb_minus2);
	bitwriter_ue(writer, sps->log2_diff_max_min_tb);
	bitwriter_ue(writer, sps->max_th_depth_inter);
	bitwriter_ue(writer, sps->max_th_depth_intra);

	/* The scaling lists. */
	bitwriter_u(writer, sps->scaling_list_enabled, 1U);
	if (sps->scaling_list_enabled != 0U) {
		bitwriter_u(writer, sps->scaling_list_data_present, 1U);
		if (sps->scaling_list_data_present != 0U)
			h265w_scaling(writer, &sps->scaling);
	}

	/* The tool flags and PCM. */
	bitwriter_u(writer, sps->amp, 1U);
	bitwriter_u(writer, sps->sao, 1U);
	bitwriter_u(writer, sps->pcm, 1U);
	if (sps->pcm != 0U) {
		bitwriter_u(writer, sps->pcm_bit_depth_luma_minus1, 4U);
		bitwriter_u(writer, sps->pcm_bit_depth_chroma_minus1, 4U);
		bitwriter_ue(writer, sps->log2_min_pcm_minus3);
		bitwriter_ue(writer, sps->log2_diff_max_min_pcm);
		bitwriter_u(writer, sps->pcm_loop_filter_disabled, 1U);
	}

	/* The short-term reference picture sets. */
	bitwriter_ue(writer, sps->num_st_rps);
	for (index = 0U; index < sps->num_st_rps && index < H265W_MAX_ST_RPS; index++)
		h265w_st_rps(writer, &sps->st_rps[index], index, sps->num_st_rps);

	/* The long-term pictures, each order count in log2_max_poc_lsb bits. */
	bitwriter_u(writer, sps->long_term_present, 1U);
	if (sps->long_term_present != 0U) {
		bitwriter_ue(writer, sps->num_lt_sps);
		lsb_bits = sps->log2_max_poc_lsb_minus4 + 4U;
		if (lsb_bits > 32U)
			lsb_bits = 32U;
		for (index = 0U; index < sps->num_lt_sps && index < H265W_MAX_LT_SPS; index++) {
			bitwriter_u(writer, sps->lt_poc_lsb[index], lsb_bits);
			bitwriter_u(writer, sps->lt_used[index], 1U);
		}
	}

	/* The last tool flags and the VUI. */
	bitwriter_u(writer, sps->temporal_mvp, 1U);
	bitwriter_u(writer, sps->strong_intra_smoothing, 1U);
	bitwriter_u(writer, sps->vui_present, 1U);
	if (sps->vui_present != 0U)
		h265w_vui(writer, &sps->vui, sps->max_sub_layers_minus1);

	/* The extension flags. */
	bitwriter_u(writer, sps->extension_present, 1U);
	if (sps->extension_present != 0U) {
		bitwriter_u(writer, sps->range_extension_flag, 1U);
		bitwriter_u(writer, sps->multilayer_extension_flag, 1U);
		bitwriter_u(writer, sps->extension_3d_flag, 1U);
		bitwriter_u(writer, sps->scc_extension_flag, 1U);
		bitwriter_u(writer, sps->extension_4bits, 4U);
	}

	/* The nine range extension flags (7.3.2.2.2), bit 0 first. */
	if (sps->extension_present != 0U && sps->range_extension_flag != 0U) {
		for (index = 0U; index < 9U; index++)
			bitwriter_u(writer, (sps->range_extension >> index) & 1U, 1U);
	}

	/* The extension data, then the trailing bits. */
	if (sps->extension_present != 0U && sps->extension_4bits != 0U)
		h265w_extension_data(writer, sps->extension_data, sps->extension_data_bits);
	bitwriter_trailing(writer);
}

/* Writes pic_parameter_set_rbsp() (7.3.2.3) with its trailing bits. */
static __inline void
h265w_pps(
	struct bitwriter *writer,
	const struct h265w_pps *pps)
{
	unsigned index;

	/* The ids and the slice header controls. */
	bitwriter_ue(writer, pps->pps_id);
	bitwriter_ue(writer, pps->sps_id);
	bitwriter_u(writer, pps->dependent_slices, 1U);
	bitwriter_u(writer, pps->output_flag_present, 1U);
	bitwriter_u(writer, pps->num_extra_slice_header_bits, 3U);
	bitwriter_u(writer, pps->sign_data_hiding, 1U);
	bitwriter_u(writer, pps->cabac_init_present, 1U);

	/* The default list sizes and the QP. */
	bitwriter_ue(writer, pps->num_ref_idx_l0_default_minus1);
	bitwriter_ue(writer, pps->num_ref_idx_l1_default_minus1);
	bitwriter_se(writer, pps->init_qp_minus26);
	bitwriter_u(writer, pps->constrained_intra, 1U);
	bitwriter_u(writer, pps->transform_skip, 1U);
	bitwriter_u(writer, pps->cu_qp_delta, 1U);
	if (pps->cu_qp_delta != 0U)
		bitwriter_ue(writer, pps->diff_cu_qp_delta_depth);
	bitwriter_se(writer, pps->cb_qp_offset);
	bitwriter_se(writer, pps->cr_qp_offset);

	/* The tool flags. */
	bitwriter_u(writer, pps->slice_chroma_qp_offsets_present, 1U);
	bitwriter_u(writer, pps->weighted_pred, 1U);
	bitwriter_u(writer, pps->weighted_bipred, 1U);
	bitwriter_u(writer, pps->transquant_bypass, 1U);
	bitwriter_u(writer, pps->tiles, 1U);
	bitwriter_u(writer, pps->entropy_sync, 1U);

	/* The tiles: the counts, the explicit sizes, the loop filter flag. */
	if (pps->tiles != 0U) {
		bitwriter_ue(writer, pps->num_tile_columns_minus1);
		bitwriter_ue(writer, pps->num_tile_rows_minus1);
		bitwriter_u(writer, pps->uniform_spacing, 1U);
		if (pps->uniform_spacing == 0U) {
			for (index = 0U; index < pps->num_tile_columns_minus1 && index < H265W_MAX_TILES; index++)
				bitwriter_ue(writer, pps->column_width_minus1[index]);
			for (index = 0U; index < pps->num_tile_rows_minus1 && index < H265W_MAX_TILES; index++)
				bitwriter_ue(writer, pps->row_height_minus1[index]);
		}
		bitwriter_u(writer, pps->loop_filter_across_tiles, 1U);
	}

	/* The deblocking filter controls. */
	bitwriter_u(writer, pps->loop_filter_across_slices, 1U);
	bitwriter_u(writer, pps->deblocking_control_present, 1U);
	if (pps->deblocking_control_present != 0U) {
		bitwriter_u(writer, pps->deblocking_override_enabled, 1U);
		bitwriter_u(writer, pps->deblocking_disabled, 1U);
		if (pps->deblocking_disabled == 0U) {
			bitwriter_se(writer, pps->beta_offset_div2);
			bitwriter_se(writer, pps->tc_offset_div2);
		}
	}

	/* The scaling lists and the last controls. */
	bitwriter_u(writer, pps->scaling_list_present, 1U);
	if (pps->scaling_list_present != 0U)
		h265w_scaling(writer, &pps->scaling);
	bitwriter_u(writer, pps->lists_modification_present, 1U);
	bitwriter_ue(writer, pps->log2_parallel_merge_level_minus2);
	bitwriter_u(writer, pps->slice_header_extension_present, 1U);

	/* The extension flags. */
	bitwriter_u(writer, pps->extension_present, 1U);
	if (pps->extension_present != 0U) {
		bitwriter_u(writer, pps->range_extension_flag, 1U);
		bitwriter_u(writer, pps->multilayer_extension_flag, 1U);
		bitwriter_u(writer, pps->extension_3d_flag, 1U);
		bitwriter_u(writer, pps->scc_extension_flag, 1U);
		bitwriter_u(writer, pps->extension_4bits, 4U);
	}

	/* The range extension (7.3.2.3.2). */
	if (pps->extension_present != 0U && pps->range_extension_flag != 0U) {
		if (pps->transform_skip != 0U)
			bitwriter_ue(writer, pps->log2_max_transform_skip_block_size_minus2);
		bitwriter_u(writer, pps->cross_component_prediction, 1U);
		bitwriter_u(writer, pps->chroma_qp_offset_list_enabled, 1U);

		/* The chroma QP offset list. */
		if (pps->chroma_qp_offset_list_enabled != 0U) {
			bitwriter_ue(writer, pps->diff_cu_chroma_qp_offset_depth);
			bitwriter_ue(writer, pps->chroma_qp_offset_list_len_minus1);
			for (index = 0U; index <= pps->chroma_qp_offset_list_len_minus1 && index < H265W_MAX_QP_OFFSETS; index++) {
				bitwriter_se(writer, pps->cb_qp_offset_list[index]);
				bitwriter_se(writer, pps->cr_qp_offset_list[index]);
			}
		}

		/* The SAO offset scales. */
		bitwriter_ue(writer, pps->log2_sao_offset_scale_luma);
		bitwriter_ue(writer, pps->log2_sao_offset_scale_chroma);
	}

	/* The extension data, then the trailing bits. */
	if (pps->extension_present != 0U && pps->extension_4bits != 0U)
		h265w_extension_data(writer, pps->extension_data, pps->extension_data_bits);
	bitwriter_trailing(writer);
}

/*
 * Fills a slice header context from the writer structures of the SPS and
 * PPS the header uses and its nal_unit_type.  PicSizeInCtbsY comes from
 * the sizes (7-10 to 7-19 with CtbLog2SizeY = log2_min_cb_minus3 + 3 +
 * log2_diff_max_min_cb); num_pic_total_curr is left 0 for the test to set.
 */
static __inline void
h265w_slice_context_from(
	struct h265w_slice_context *context,
	const struct h265w_sps *sps,
	const struct h265w_pps *pps,
	uint32_t nal_type)
{
	uint32_t ctb_log2;
	uint64_t ctb_size;
	uint64_t width_ctbs;
	uint64_t height_ctbs;

	/* Starts from nothing present. */
	memset(context, 0, sizeof(*context));
	context->nal_type = nal_type;

	/* The PPS's part. */
	context->dependent_slices = pps->dependent_slices;
	context->num_extra_slice_header_bits = pps->num_extra_slice_header_bits;
	context->output_flag_present = pps->output_flag_present;
	context->cabac_init_present = pps->cabac_init_present;
	context->num_ref_idx_default_minus1[0] = pps->num_ref_idx_l0_default_minus1;
	context->num_ref_idx_default_minus1[1] = pps->num_ref_idx_l1_default_minus1;
	context->lists_modification_present = pps->lists_modification_present;
	context->weighted_pred = pps->weighted_pred;
	context->weighted_bipred = pps->weighted_bipred;
	context->slice_chroma_qp_offsets_present = pps->slice_chroma_qp_offsets_present;
	context->deblocking_override_enabled = (pps->deblocking_control_present != 0U) ? pps->deblocking_override_enabled : 0U;
	context->pps_deblocking_disabled = (pps->deblocking_control_present != 0U) ? pps->deblocking_disabled : 0U;
	context->loop_filter_across_slices = pps->loop_filter_across_slices;
	context->tiles = pps->tiles;
	context->entropy_sync = pps->entropy_sync;
	context->slice_header_extension_present = pps->slice_header_extension_present;

	/* The SPS's part. */
	context->separate_colour_plane = (sps->chroma_format_idc == 3U) ? sps->separate_colour_plane : 0U;
	context->log2_max_poc_lsb = sps->log2_max_poc_lsb_minus4 + 4U;
	context->num_st_rps = sps->num_st_rps;
	context->long_term_present = sps->long_term_present;
	context->num_lt_sps = (sps->long_term_present != 0U) ? sps->num_lt_sps : 0U;
	context->temporal_mvp = sps->temporal_mvp;
	context->sao = sps->sao;
	context->chroma_array_type = (context->separate_colour_plane != 0U) ? 0U : sps->chroma_format_idc;

	/* PicSizeInCtbsY from the luma size and the CTB size, rounded up. */
	ctb_log2 = sps->log2_min_cb_minus3 + 3U + sps->log2_diff_max_min_cb;
	if (ctb_log2 > 31U)
		ctb_log2 = 31U;
	ctb_size = (uint64_t)1U << ctb_log2;
	width_ctbs = ((uint64_t)sps->width + ctb_size - 1U) / ctb_size;
	height_ctbs = ((uint64_t)sps->height + ctb_size - 1U) / ctb_size;
	context->pic_size_ctbs = (uint32_t)(width_ctbs * height_ctbs);
}

/*
 * Writes the pred_weight_table() of one list (7.3.6.3): the luma flags,
 * the chroma flags (ChromaArrayType not 0), then each entry's weights.
 */
static __inline void
h265w_weights_list(
	struct bitwriter *writer,
	const struct h265w_slice *slice,
	unsigned list,
	unsigned count,
	uint32_t chroma_array_type)
{
	unsigned index;
	unsigned component;

	/* Every entry's luma flag, then every entry's chroma flag. */
	for (index = 0U; index < count; index++)
		bitwriter_u(writer, slice->luma_weight_flag[list][index], 1U);
	if (chroma_array_type != 0U) {
		for (index = 0U; index < count; index++)
			bitwriter_u(writer, slice->chroma_weight_flag[list][index], 1U);
	}

	/* Each entry's weights and offsets, as its flags say. */
	for (index = 0U; index < count; index++) {
		if (slice->luma_weight_flag[list][index] != 0U) {
			bitwriter_se(writer, slice->delta_luma_weight[list][index]);
			bitwriter_se(writer, slice->luma_offset[list][index]);
		}

		/* The two chroma components. */
		if (chroma_array_type != 0U && slice->chroma_weight_flag[list][index] != 0U) {
			for (component = 0U; component < 2U; component++) {
				bitwriter_se(writer, slice->delta_chroma_weight[list][index][component]);
				bitwriter_se(writer, slice->delta_chroma_offset[list][index][component]);
			}
		}
	}
}

/*
 * Writes slice_segment_header() (7.3.6.1 to 7.3.6.3) up to and including
 * byte_alignment(); the writer is then at the first bit of
 * slice_segment_data(), byte aligned when alignment_one and
 * alignment_zero are 1 and 0.
 */
static __inline void
h265w_slice(
	struct bitwriter *writer,
	const struct h265w_slice *slice,
	const struct h265w_slice_context *context)
{
	unsigned index;
	unsigned count[2];
	uint32_t dependent;
	uint32_t idr;
	uint32_t tmvp;
	uint32_t sao_luma;
	uint32_t sao_chroma;
	uint32_t collocated_from_l0;
	uint32_t deblocking_override;
	uint32_t deblocking_disabled;
	unsigned lsb_bits;
	unsigned offset_bits;

	/* The first fields: the first flag, the IRAP flag, the PPS. */
	bitwriter_u(writer, slice->first_in_pic, 1U);
	if (context->nal_type >= 16U && context->nal_type <= 23U)
		bitwriter_u(writer, slice->no_output_of_prior_pics, 1U);
	bitwriter_ue(writer, slice->pps_id);

	/* The dependent flag and the address of a segment that is not the first. */
	dependent = 0U;
	if (slice->first_in_pic == 0U) {
		if (context->dependent_slices != 0U) {
			bitwriter_u(writer, slice->dependent, 1U);
			dependent = slice->dependent;
		}
		bitwriter_u(writer, slice->address, h265w_ceil_log2(context->pic_size_ctbs));
	}

	/* The fields an independent segment carries. */
	if (dependent == 0U) {
		lsb_bits = context->log2_max_poc_lsb;
		if (lsb_bits > 32U)
			lsb_bits = 32U;
		for (index = 0U; index < context->num_extra_slice_header_bits; index++)
			bitwriter_u(writer, (slice->reserved_flags >> index) & 1U, 1U);
		bitwriter_ue(writer, slice->slice_type);
		if (context->output_flag_present != 0U)
			bitwriter_u(writer, slice->pic_output, 1U);
		if (context->separate_colour_plane != 0U)
			bitwriter_u(writer, slice->colour_plane_id, 2U);

		/* The order count and the reference picture set of a picture that is not IDR. */
		idr = (context->nal_type == H265W_NAL_IDR_W_RADL || context->nal_type == H265W_NAL_IDR_N_LP) ? 1U : 0U;
		tmvp = 0U;
		if (idr == 0U) {
			bitwriter_u(writer, slice->poc_lsb, lsb_bits);
			bitwriter_u(writer, slice->st_rps_sps, 1U);
			if (slice->st_rps_sps == 0U) {
				h265w_st_rps(writer, &slice->st_rps, context->num_st_rps, context->num_st_rps);
			} else if (context->num_st_rps > 1U) {
				bitwriter_u(writer, slice->st_rps_idx, h265w_ceil_log2(context->num_st_rps));
			}

			/* The long-term entries: from the SPS, then signalled. */
			if (context->long_term_present != 0U) {
				if (context->num_lt_sps > 0U)
					bitwriter_ue(writer, slice->num_long_term_sps);
				bitwriter_ue(writer, slice->num_long_term_pics);
				for (index = 0U; index < slice->num_long_term_sps + slice->num_long_term_pics && index < H265W_MAX_LT; index++) {
					if (index < slice->num_long_term_sps) {
						if (context->num_lt_sps > 1U)
							bitwriter_u(writer, slice->lt_idx_sps[index], h265w_ceil_log2(context->num_lt_sps));
					} else {
						bitwriter_u(writer, slice->poc_lsb_lt[index], lsb_bits);
						bitwriter_u(writer, slice->used_by_curr_pic_lt[index], 1U);
					}

					/* The most significant part of the order count, when present. */
					bitwriter_u(writer, slice->delta_poc_msb_present[index], 1U);
					if (slice->delta_poc_msb_present[index] != 0U)
						bitwriter_ue(writer, slice->delta_poc_msb_cycle_lt[index]);
				}
			}

			/* The temporal motion vector prediction. */
			if (context->temporal_mvp != 0U) {
				bitwriter_u(writer, slice->temporal_mvp, 1U);
				tmvp = slice->temporal_mvp;
			}
		}

		/* SAO, chroma only with a chroma array. */
		sao_luma = 0U;
		sao_chroma = 0U;
		if (context->sao != 0U) {
			bitwriter_u(writer, slice->sao_luma, 1U);
			sao_luma = slice->sao_luma;
			if (context->chroma_array_type != 0U) {
				bitwriter_u(writer, slice->sao_chroma, 1U);
				sao_chroma = slice->sao_chroma;
			}
		}

		/* The inter part of a P or B slice. */
		if (slice->slice_type == H265W_SLICE_P || slice->slice_type == H265W_SLICE_B) {
			count[0] = context->num_ref_idx_default_minus1[0] + 1U;
			count[1] = context->num_ref_idx_default_minus1[1] + 1U;
			bitwriter_u(writer, slice->num_ref_idx_override, 1U);
			if (slice->num_ref_idx_override != 0U) {
				bitwriter_ue(writer, slice->num_ref_idx_active_minus1[0]);
				count[0] = slice->num_ref_idx_active_minus1[0] + 1U;
				if (slice->slice_type == H265W_SLICE_B) {
					bitwriter_ue(writer, slice->num_ref_idx_active_minus1[1]);
					count[1] = slice->num_ref_idx_active_minus1[1] + 1U;
				}
			}
			if (count[0] > H265W_MAX_REFS)
				count[0] = H265W_MAX_REFS;
			if (count[1] > H265W_MAX_REFS)
				count[1] = H265W_MAX_REFS;

			/* The list modification (7.3.6.2), each entry in Ceil( Log2( NumPicTotalCurr ) ) bits. */
			if (context->lists_modification_present != 0U && context->num_pic_total_curr > 1U) {
				bitwriter_u(writer, slice->list_mod[0], 1U);
				if (slice->list_mod[0] != 0U) {
					for (index = 0U; index < count[0]; index++)
						bitwriter_u(writer, slice->list_entry[0][index], h265w_ceil_log2(context->num_pic_total_curr));
				}
				if (slice->slice_type == H265W_SLICE_B) {
					bitwriter_u(writer, slice->list_mod[1], 1U);
					if (slice->list_mod[1] != 0U) {
						for (index = 0U; index < count[1]; index++)
							bitwriter_u(writer, slice->list_entry[1][index], h265w_ceil_log2(context->num_pic_total_curr));
					}
				}
			}

			/* The B flag, the CABAC flag and the collocated picture. */
			if (slice->slice_type == H265W_SLICE_B)
				bitwriter_u(writer, slice->mvd_l1_zero, 1U);
			if (context->cabac_init_present != 0U)
				bitwriter_u(writer, slice->cabac_init, 1U);
			if (tmvp != 0U) {
				collocated_from_l0 = 1U;
				if (slice->slice_type == H265W_SLICE_B) {
					bitwriter_u(writer, slice->collocated_from_l0, 1U);
					collocated_from_l0 = slice->collocated_from_l0;
				}
				if ((collocated_from_l0 != 0U && count[0] > 1U) ||
				    (collocated_from_l0 == 0U && count[1] > 1U))
					bitwriter_ue(writer, slice->collocated_ref_idx);
			}

			/* The weights (7.3.6.3), every flag present (no entry has the current order count). */
			if ((context->weighted_pred != 0U && slice->slice_type == H265W_SLICE_P) ||
			    (context->weighted_bipred != 0U && slice->slice_type == H265W_SLICE_B)) {
				bitwriter_ue(writer, slice->luma_log2_weight_denom);
				if (context->chroma_array_type != 0U)
					bitwriter_se(writer, slice->delta_chroma_log2_weight_denom);
				h265w_weights_list(writer, slice, 0U, count[0], context->chroma_array_type);
				if (slice->slice_type == H265W_SLICE_B)
					h265w_weights_list(writer, slice, 1U, count[1], context->chroma_array_type);
			}

			/* The merge candidates. */
			bitwriter_ue(writer, slice->five_minus_max_num_merge_cand);
		}

		/* The QP and the chroma QP offsets. */
		bitwriter_se(writer, slice->slice_qp_delta);
		if (context->slice_chroma_qp_offsets_present != 0U) {
			bitwriter_se(writer, slice->cb_qp_offset);
			bitwriter_se(writer, slice->cr_qp_offset);
		}

		/* The deblocking override. */
		deblocking_override = 0U;
		deblocking_disabled = context->pps_deblocking_disabled;
		if (context->deblocking_override_enabled != 0U) {
			bitwriter_u(writer, slice->deblocking_override, 1U);
			deblocking_override = slice->deblocking_override;
		}
		if (deblocking_override != 0U) {
			bitwriter_u(writer, slice->deblocking_disabled, 1U);
			deblocking_disabled = slice->deblocking_disabled;
			if (deblocking_disabled == 0U) {
				bitwriter_se(writer, slice->beta_offset_div2);
				bitwriter_se(writer, slice->tc_offset_div2);
			}
		}

		/* The loop filter across slices, when any in-loop filter runs. */
		if (context->loop_filter_across_slices != 0U &&
		    (sao_luma != 0U || sao_chroma != 0U || deblocking_disabled == 0U))
			bitwriter_u(writer, slice->loop_filter_across_slices, 1U);
	}

	/* The entry points of tiles and wavefronts. */
	if (context->tiles != 0U || context->entropy_sync != 0U) {
		bitwriter_ue(writer, slice->num_entry_point_offsets);
		if (slice->num_entry_point_offsets > 0U) {
			bitwriter_ue(writer, slice->offset_len_minus1);
			offset_bits = (slice->offset_len_minus1 < 32U) ? slice->offset_len_minus1 + 1U : 32U;
			for (index = 0U; index < slice->num_entry_point_offsets && writer->overflow == 0; index++)
				bitwriter_u(writer, slice->entry_point_offset_minus1, offset_bits);
		}
	}

	/* The header extension bytes. */
	if (context->slice_header_extension_present != 0U) {
		bitwriter_ue(writer, slice->extension_length);
		for (index = 0U; index < slice->extension_length && index < H265W_MAX_EXTENSION; index++)
			bitwriter_u(writer, slice->extension_data[index], 8U);
	}

	/* byte_alignment() (7.3.2.12): the one bit, then zero bits to the boundary. */
	bitwriter_u(writer, slice->alignment_one, 1U);
	while (writer->bits % 8U != 0U && writer->overflow == 0)
		bitwriter_u(writer, slice->alignment_zero, 1U);
}

/*
 * Makes a NAL unit (7.3.1): the two-byte header (forbidden_zero_bit 0,
 * nal_unit_type, nuh_layer_id, nuh_temporal_id_plus1, 7.3.1.2) then the
 * RBSP with an emulation_prevention_three_byte inserted wherever two zero
 * bytes of the payload would be followed by a byte of 0..3, and after a
 * final zero byte (7.4.2).  Returns the bytes written to out (capacity
 * bytes), or 0 when they do not fit.
 */
static __inline size_t
h265w_nal(
	uint32_t type,
	uint32_t layer_id,
	uint32_t temporal_id_plus1,
	const uint8_t *rbsp,
	size_t size,
	uint8_t *out,
	size_t capacity)
{
	size_t used;
	size_t index;
	unsigned zeros;

	/* The header: 0, six bits of type, six of layer, three of temporal id. */
	if (capacity < 2U)
		return 0U;
	out[0] = (uint8_t)(((type & 0x3FU) << 1) | ((layer_id >> 5) & 1U));
	out[1] = (uint8_t)(((layer_id & 0x1FU) << 3) | (temporal_id_plus1 & 7U));
	used = 2U;

	/* The RBSP bytes, escaped where the payload would hold 0x000000..0x000003. */
	zeros = 0U;
	for (index = 0U; index < size; index++) {
		if (zeros == 2U && rbsp[index] <= 3U) {
			if (used == capacity)
				return 0U;
			out[used] = 3U;
			used++;
			zeros = 0U;
		}

		/* The byte itself, counting the zeros that run up to it. */
		if (used == capacity)
			return 0U;
		out[used] = rbsp[index];
		used++;
		if (rbsp[index] == 0U) {
			zeros++;
		} else {
			zeros = 0U;
		}
	}

	/* A final zero byte (a cabac_zero_word) is followed by 0x03. */
	if (size != 0U && rbsp[size - 1U] == 0U) {
		if (used == capacity)
			return 0U;
		out[used] = 3U;
		used++;
	}

	/* Succeeded: the NAL unit's length. */
	return used;
}

/* Starts an Annex B byte stream in buffer (capacity bytes), with four-byte start codes. */
static __inline void
h265w_stream_init(
	struct h265w_stream *stream,
	uint8_t *buffer,
	size_t capacity)
{
	/* Nothing written yet. */
	stream->data = buffer;
	stream->size = 0U;
	stream->capacity = capacity;
	stream->start_code_bytes = 4U;
	stream->overflow = 0;
}

/* Appends one NAL unit made from rbsp (size bytes) after a start code (B.2). */
static __inline void
h265w_stream_bytes(
	struct h265w_stream *stream,
	uint32_t type,
	uint32_t layer_id,
	uint32_t temporal_id_plus1,
	const uint8_t *rbsp,
	size_t size)
{
	size_t start;
	size_t written;
	unsigned index;

	/* The start code: zero bytes, then the 0x01 that ends the prefix. */
	start = (stream->start_code_bytes == 3U) ? 3U : 4U;
	if (stream->capacity - stream->size < start) {
		stream->overflow = 1;
		return;
	}
	for (index = 0U; index + 1U < start; index++)
		stream->data[stream->size + index] = 0U;
	stream->data[stream->size + start - 1U] = 1U;

	/* The NAL unit after it; nothing is kept when it does not fit. */
	written = h265w_nal(type,
	    layer_id,
	    temporal_id_plus1,
	    rbsp,
	    size,
	    stream->data + stream->size + start,
	    stream->capacity - stream->size - start);
	if (written == 0U) {
		stream->overflow = 1;
		return;
	}
	stream->size += start + written;
}

/* Appends one NAL unit whose RBSP is the bytes of writer. */
static __inline void
h265w_stream_rbsp(
	struct h265w_stream *stream,
	uint32_t type,
	uint32_t layer_id,
	uint32_t temporal_id_plus1,
	const struct bitwriter *writer)
{
	/* The written bytes, the last partial byte padded with zeros. */
	h265w_stream_bytes(stream, type, layer_id, temporal_id_plus1, writer->data, bitwriter_bytes(writer));
}

/* Sets profile to the Main profile (general_profile_idc 1, compatible with Main and Main 10), progressive frames. */
static __inline void
h265w_profile_main(
	struct h265w_profile *profile)
{
	/* Main, flags 1 and 2, progressive frames only, the other bits 0. */
	memset(profile, 0, sizeof(*profile));
	profile->profile_idc = 1U;
	profile->compatibility = (1U << 1) | (1U << 2);
	profile->progressive = 1U;
	profile->frame_only = 1U;
}

/*
 * Fills vps with a valid VPS for one layer and one sub-layer: id 0, the
 * base layer internal and available, Main at level 3.1 (93),
 * vps_reserved_0xffff_16bits 0xFFFF, max_dec_pic_buffering_minus1 4, no
 * timing, no extension.
 */
static __inline void
h265w_vps_default(
	struct h265w_vps *vps)
{
	/* Everything absent first. */
	memset(vps, 0, sizeof(*vps));

	/* The layer and the profile. */
	vps->base_layer_internal = 1U;
	vps->base_layer_available = 1U;
	vps->temporal_id_nesting = 1U;
	vps->reserved_0xffff_16bits = 0xFFFFU;
	h265w_profile_main(&vps->ptl.general);
	vps->ptl.level_idc = 93U;

	/* The buffering of the one sub-layer. */
	vps->sub_layer_ordering_info_present = 1U;
	vps->max_dec_pic_buffering_minus1[0] = 4U;
}

/*
 * Fills sps with a valid SPS: id 0 of VPS 0, one sub-layer, Main at level
 * 3.1, 4:2:0 8-bit 64x64 luma samples, CTB 16 (MinCbLog2SizeY 3,
 * CtbLog2SizeY 4), transform blocks 4..16 with depths 1, 8 order count
 * bits, max_dec_pic_buffering_minus1 4, no reorder, no latency limit,
 * every tool and set absent.
 */
static __inline void
h265w_sps_default(
	struct h265w_sps *sps)
{
	/* Everything absent first. */
	memset(sps, 0, sizeof(*sps));

	/* The sub-layer and the profile. */
	sps->temporal_id_nesting = 1U;
	h265w_profile_main(&sps->ptl.general);
	sps->ptl.level_idc = 93U;

	/* The format and the size. */
	sps->chroma_format_idc = 1U;
	sps->width = 64U;
	sps->height = 64U;
	sps->log2_max_poc_lsb_minus4 = 4U;

	/* The buffering of the one sub-layer. */
	sps->sub_layer_ordering_info_present = 1U;
	sps->max_dec_pic_buffering_minus1[0] = 4U;

	/* The block sizes. */
	sps->log2_diff_max_min_cb = 1U;
	sps->log2_diff_max_min_tb = 2U;
	sps->max_th_depth_inter = 1U;
	sps->max_th_depth_intra = 1U;
}

/*
 * Fills pps with a valid PPS: id 0 of SPS 0, one reference of each list
 * by default, init_qp 26, every tool absent, the loop filter across
 * slices on.
 */
static __inline void
h265w_pps_default(
	struct h265w_pps *pps)
{
	/* Everything absent first; then the one flag that is on. */
	memset(pps, 0, sizeof(*pps));
	pps->loop_filter_across_slices = 1U;
}

/*
 * Fills slice with the header of an independent first slice segment of
 * an I picture of PPS 0 (slice_qp_delta 0, the merge candidates 5, a
 * valid byte_alignment()).
 */
static __inline void
h265w_slice_default(
	struct h265w_slice *slice)
{
	/* Everything absent first. */
	memset(slice, 0, sizeof(*slice));

	/* The first segment of an I picture. */
	slice->first_in_pic = 1U;
	slice->slice_type = H265W_SLICE_I;
	slice->pic_output = 1U;
	slice->collocated_from_l0 = 1U;
	slice->alignment_one = 1U;
}

#endif
