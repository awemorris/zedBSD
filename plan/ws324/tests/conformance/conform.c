/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host tool of the H.265 parse conformance run: reads one Annex B
 * bitstream (a file, or the standard input with "-") through h265-stream,
 * h265-dpb (17 slots, every output released at once) and h265-std, and
 * writes what they made as text for compare.py.
 *
 * Lines:
 *   @PIC index nal tid poc pic_output no_rasl first after_eos noprior irap idr vps sps pps generation slices
 *   @SKIP reason count            pictures or NAL units not returned before the next @PIC (or the end)
 *   @VPS id index / @SPS id index / @PPS id index
 *                                 a parameter set a picture uses, when it is new or its content changed
 *   @SH index                     the first slice segment header of picture index
 *   name value                    a syntax element of the section above, named as in ITU-T H.265 7.3
 *                                 (and as ffmpeg's trace_headers names it), emitted exactly where the
 *                                 syntax has it, from the values the parse kept
 *   =name value                   a derived value of the section above (compare.py derives its own)
 *   @OUT poc index                a picture output by the DPB, in output order
 *   @DPB what value               DPB events: missing, discarded, evicted, error
 *   @STD what text                StdVideo self-check failures (h265-std and the round trip)
 *   @COUNT name value             the counts at the end, and the checks made
 *   @ERROR text                   a call that failed
 *
 * The StdVideo round trip (the WS322 T1 form): every VPS, SPS and PPS
 * dumped is made into StdVideo with h265-std, written back as a NAL unit
 * RBSP with h265write.h from the StdVideo values only (plus the few values
 * StdVideo does not carry, which are named), read again with h265sw-ps,
 * and compared with the first parse.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "h265write.h"
#include "userland/desktop/libmedia/h265-dpb.h"
#include "userland/desktop/libmedia/h265-std.h"
#include "userland/desktop/libmedia/h265-stream.h"

/* The slots of the DPB: one more than the pictures (design 8.4). */
#define CONFORM_SLOTS 17U

/* The largest bitstream read (the largest conformance stream is below 10 MB). */
#define CONFORM_MAX_INPUT (256U * 1024U * 1024U)

/* Checks made by the StdVideo self-check, and failures found. */
static unsigned long std_checks;
static unsigned long std_failures;

/* The last dumped copy of each parameter set (dump only when new or changed). */
static int vps_dumped[H265SW_VPS_IDS];
static int sps_dumped[H265SW_SPS_IDS];
static int pps_dumped[H265SW_PPS_IDS];
static struct h265sw_vps vps_last[H265SW_VPS_IDS];
static struct h265sw_sps sps_last[H265SW_SPS_IDS];
static struct h265sw_pps pps_last[H265SW_PPS_IDS];

/* The writer of the round trip (64 KiB, kept off the stack). */
static struct bitwriter writer;

static void emit(const char *name, long value);
static void emit1(const char *name, unsigned i, long value);
static void dump_ptl(const struct h265sw_ptl *ptl);
static void dump_sub_layer_hrd(const struct h265sw_sub_layer_hrd *sub, unsigned count, int sub_pic);
static void dump_hrd(const struct h265sw_hrd *hrd, int common, unsigned max_sub);
static void dump_vui(const struct h265sw_vui *vui, unsigned max_sub);
static void dump_st_syntax(const struct h265sw_st_rps_syntax *syntax, unsigned index, unsigned num_sets, const struct h265sw_st_rps *sets);
static void dump_st_derived(const char *name, unsigned index, const struct h265sw_st_rps *rps);
static void dump_scaling(const struct h265sw_scaling_list *list);
static void dump_vps(const struct h265sw_vps *vps, uint64_t index);
static void dump_sps(const struct h265sw_sps *sps, uint64_t index);
static void dump_pps(const struct h265sw_pps *pps, uint64_t index);
static void dump_weights(const struct h265sw_slice_header *header, unsigned chroma);
static void dump_slice(const struct h265_picture *picture, const struct h265sw_sps *sps, const struct h265sw_pps *pps);
static void std_fail(const char *what, const char *text, unsigned long value);
static void std_compare(const char *what, const void *a, const void *b, size_t size);
static void std_to_ptl(const StdVideoH265ProfileTierLevel *std, const struct h265sw_ptl *orig, struct h265w_ptl *out);
static void std_to_hrd(const StdVideoH265HrdParameters *std, unsigned max_sub, struct h265w_hrd *out);
static void std_to_scaling(const StdVideoH265ScalingLists *std, struct h265w_scaling *out);
static void std_to_st_rps(const StdVideoH265ShortTermRefPicSet *sets, unsigned index, struct h265w_st_rps *out);
static void roundtrip_vps(const struct h265sw_vps *orig);
static void roundtrip_sps(const struct h265sw_sps *orig);
static void roundtrip_pps(const struct h265sw_pps *orig, const struct h265sw_sps *sps);
static void check_picture(const struct h265_picture *picture, const struct h265_dpb_plan *plan);
static void print_outputs(const struct h265_dpb_output *output, unsigned count, struct h265_dpb *dpb);
static void print_skips(const struct h265_stream_counts *before, const struct h265_stream_counts *after);
static int read_input(const char *path, uint8_t **data, size_t *size);

/* Writes one syntax element of the current section. */
static void
emit(
	const char *name,
	long value)
{
	printf("%s %ld\n", name, value);
}

/* Writes one element of a one-index array (name[i]). */
static void
emit1(
	const char *name,
	unsigned i,
	long value)
{
	printf("%s[%u] %ld\n", name, i, value);
}

/* Writes the general part of profile_tier_level() (7.3.3) that the parse keeps. */
static void
dump_ptl(
	const struct h265sw_ptl *ptl)
{
	unsigned j;

	/* The profile, its 32 compatibility flags, the four source flags and the level. */
	emit("general_profile_space", (long)ptl->profile_space);
	emit("general_tier_flag", (long)ptl->tier);
	emit("general_profile_idc", (long)ptl->profile_idc);
	for (j = 0U; j < 32U; j++)
		emit1("general_profile_compatibility_flag", j, (long)((ptl->compatibility >> j) & 1U));
	emit("general_progressive_source_flag", (long)ptl->progressive);
	emit("general_interlaced_source_flag", (long)ptl->interlaced);
	emit("general_non_packed_constraint_flag", (long)ptl->non_packed);
	emit("general_frame_only_constraint_flag", (long)ptl->frame_only);
	emit("general_level_idc", (long)ptl->level_idc);
}

/* Writes one sub_layer_hrd_parameters() (E.2.3) of count CPBs. */
static void
dump_sub_layer_hrd(
	const struct h265sw_sub_layer_hrd *sub,
	unsigned count,
	int sub_pic)
{
	unsigned i;

	/* Each CPB's rate and size, the DU values with sub_pic_hrd_params_present_flag. */
	for (i = 0U; i < count; i++) {
		emit1("bit_rate_value_minus1", i, (long)sub->bit_rate_value_minus1[i]);
		emit1("cpb_size_value_minus1", i, (long)sub->cpb_size_value_minus1[i]);
		if (sub_pic != 0) {
			emit1("cpb_size_du_value_minus1", i, (long)sub->cpb_size_du_value_minus1[i]);
			emit1("bit_rate_du_value_minus1", i, (long)sub->bit_rate_du_value_minus1[i]);
		}

		emit1("cbr_flag", i, (long)((sub->cbr_flags >> i) & 1U));
	}
}

/* Writes hrd_parameters( common, max_sub ) (E.2.2). */
static void
dump_hrd(
	const struct h265sw_hrd *hrd,
	int common,
	unsigned max_sub)
{
	unsigned i;
	unsigned within;
	unsigned low_delay;

	/* The common part. */
	if (common != 0) {
		emit("nal_hrd_parameters_present_flag", (long)hrd->nal_hrd_present);
		emit("vcl_hrd_parameters_present_flag", (long)hrd->vcl_hrd_present);
		if (hrd->nal_hrd_present != 0 || hrd->vcl_hrd_present != 0) {
			emit("sub_pic_hrd_params_present_flag", (long)hrd->sub_pic_hrd_params_present);
			if (hrd->sub_pic_hrd_params_present != 0) {
				emit("tick_divisor_minus2", (long)hrd->tick_divisor_minus2);
				emit("du_cpb_removal_delay_increment_length_minus1", (long)hrd->du_cpb_removal_delay_increment_length_minus1);
				emit("sub_pic_cpb_params_in_pic_timing_sei_flag", (long)hrd->sub_pic_cpb_params_in_pic_timing_sei);
				emit("dpb_output_delay_du_length_minus1", (long)hrd->dpb_output_delay_du_length_minus1);
			}

			emit("bit_rate_scale", (long)hrd->bit_rate_scale);
			emit("cpb_size_scale", (long)hrd->cpb_size_scale);
			if (hrd->sub_pic_hrd_params_present != 0)
				emit("cpb_size_du_scale", (long)hrd->cpb_size_du_scale);
			emit("initial_cpb_removal_delay_length_minus1", (long)hrd->initial_cpb_removal_delay_length_minus1);
			emit("au_cpb_removal_delay_length_minus1", (long)hrd->au_cpb_removal_delay_length_minus1);
			emit("dpb_output_delay_length_minus1", (long)hrd->dpb_output_delay_length_minus1);
		}
	}

	/* Each sub-layer: the rate flags (with their inferences), the CPB count and the sub-layer HRDs. */
	for (i = 0U; i <= max_sub && i < H265SW_MAX_SUB_LAYERS; i++) {
		emit1("fixed_pic_rate_general_flag", i, (long)hrd->fixed_pic_rate_general[i]);
		within = 1U;
		if (hrd->fixed_pic_rate_general[i] == 0U) {
			emit1("fixed_pic_rate_within_cvs_flag", i, (long)hrd->fixed_pic_rate_within_cvs[i]);
			within = hrd->fixed_pic_rate_within_cvs[i];
		}

		low_delay = 0U;
		if (within != 0U) {
			emit1("elemental_duration_in_tc_minus1", i, (long)hrd->elemental_duration_in_tc_minus1[i]);
		} else {
			emit1("low_delay_hrd_flag", i, (long)hrd->low_delay_hrd[i]);
			low_delay = hrd->low_delay_hrd[i];
		}

		if (low_delay == 0U)
			emit1("cpb_cnt_minus1", i, (long)hrd->cpb_cnt_minus1[i]);
		if (hrd->nal_hrd_present != 0)
			dump_sub_layer_hrd(&hrd->nal[i], hrd->cpb_cnt_minus1[i] + 1U, hrd->sub_pic_hrd_params_present);
		if (hrd->vcl_hrd_present != 0)
			dump_sub_layer_hrd(&hrd->vcl[i], hrd->cpb_cnt_minus1[i] + 1U, hrd->sub_pic_hrd_params_present);
	}
}

/* Writes vui_parameters() (E.2.1). */
static void
dump_vui(
	const struct h265sw_vui *vui,
	unsigned max_sub)
{
	/* The aspect ratio, overscan, video signal and chroma location. */
	emit("aspect_ratio_info_present_flag", (long)vui->aspect_ratio_present);
	if (vui->aspect_ratio_present != 0) {
		emit("aspect_ratio_idc", (long)vui->aspect_ratio_idc);
		if (vui->aspect_ratio_idc == 255U) {
			emit("sar_width", (long)vui->signalled_sar_width);
			emit("sar_height", (long)vui->signalled_sar_height);
		}
	}

	emit("overscan_info_present_flag", (long)vui->overscan_present);
	if (vui->overscan_present != 0)
		emit("overscan_appropriate_flag", (long)vui->overscan_appropriate);
	emit("video_signal_type_present_flag", (long)vui->signal_present);
	if (vui->signal_present != 0) {
		emit("video_format", (long)vui->video_format);
		emit("video_full_range_flag", (long)vui->full_range);
		emit("colour_description_present_flag", (long)vui->colour_present);
		if (vui->colour_present != 0) {
			emit("colour_primaries", (long)vui->colour_primaries);
			emit("transfer_characteristics", (long)vui->transfer_characteristics);
			emit("matrix_coefficients", (long)vui->matrix_coeffs);
		}
	}

	emit("chroma_loc_info_present_flag", (long)vui->chroma_loc_present);
	if (vui->chroma_loc_present != 0) {
		emit("chroma_sample_loc_type_top_field", (long)vui->chroma_sample_loc_type_top_field);
		emit("chroma_sample_loc_type_bottom_field", (long)vui->chroma_sample_loc_type_bottom_field);
	}

	/* The field and window flags. */
	emit("neutral_chroma_indication_flag", (long)vui->neutral_chroma_indication);
	emit("field_seq_flag", (long)vui->field_seq);
	emit("frame_field_info_present_flag", (long)vui->frame_field_info_present);
	emit("default_display_window_flag", (long)vui->default_display_window);
	if (vui->default_display_window != 0) {
		emit("def_disp_win_left_offset", (long)vui->def_disp_win_left_offset);
		emit("def_disp_win_right_offset", (long)vui->def_disp_win_right_offset);
		emit("def_disp_win_top_offset", (long)vui->def_disp_win_top_offset);
		emit("def_disp_win_bottom_offset", (long)vui->def_disp_win_bottom_offset);
	}

	/* The timing and its HRD. */
	emit("vui_timing_info_present_flag", (long)vui->timing_present);
	if (vui->timing_present != 0) {
		emit("vui_num_units_in_tick", (long)vui->num_units_in_tick);
		emit("vui_time_scale", (long)vui->time_scale);
		emit("vui_poc_proportional_to_timing_flag", (long)vui->poc_proportional_to_timing);
		if (vui->poc_proportional_to_timing != 0)
			emit("vui_num_ticks_poc_diff_one_minus1", (long)vui->num_ticks_poc_diff_one_minus1);
		emit("vui_hrd_parameters_present_flag", (long)vui->hrd_present);
		if (vui->hrd_present != 0)
			dump_hrd(&vui->hrd, 1, max_sub);
	}

	/* The bitstream restrictions. */
	emit("bitstream_restriction_flag", (long)vui->bitstream_restriction);
	if (vui->bitstream_restriction != 0) {
		emit("tiles_fixed_structure_flag", (long)vui->tiles_fixed_structure);
		emit("motion_vectors_over_pic_boundaries_flag", (long)vui->motion_vectors_over_pic_boundaries);
		emit("restricted_ref_pic_lists_flag", (long)vui->restricted_ref_pic_lists);
		emit("min_spatial_segmentation_idc", (long)vui->min_spatial_segmentation_idc);
		emit("max_bytes_per_pic_denom", (long)vui->max_bytes_per_pic_denom);
		emit("max_bits_per_min_cu_denom", (long)vui->max_bits_per_min_cu_denom);
		emit("log2_max_mv_length_horizontal", (long)vui->log2_max_mv_length_horizontal);
		emit("log2_max_mv_length_vertical", (long)vui->log2_max_mv_length_vertical);
	}
}

/*
 * Writes st_ref_pic_set( index ) (7.3.7) as signalled.  num_sets is
 * num_short_term_ref_pic_sets; sets are the derived sets of the SPS, whose
 * counts give NumDeltaPocs[ RefRpsIdx ] (7-60) for a predicted set.
 */
static void
dump_st_syntax(
	const struct h265sw_st_rps_syntax *syntax,
	unsigned index,
	unsigned num_sets,
	const struct h265sw_st_rps *sets)
{
	unsigned j;
	unsigned ref;
	unsigned count;

	/* The prediction flag, read for every set but the first. */
	if (index != 0U)
		emit("inter_ref_pic_set_prediction_flag", (long)syntax->inter_rps_pred);

	/* A predicted set: its reference and the flag pairs of each entry of the reference, and one more. */
	if (syntax->inter_rps_pred != 0) {
		if (index == num_sets)
			emit("delta_idx_minus1", (long)syntax->delta_idx_minus1);
		emit("delta_rps_sign", (long)syntax->delta_rps_sign);
		emit("abs_delta_rps_minus1", (long)syntax->abs_delta_rps_minus1);
		ref = index - (syntax->delta_idx_minus1 + 1U);
		count = 0U;
		if (ref < index && ref < H265SW_MAX_ST_RPS - 1U)
			count = sets[ref].num_negative + sets[ref].num_positive;
		for (j = 0U; j <= count && j < 32U; j++) {
			emit1("used_by_curr_pic_flag", j, (long)((syntax->used_by_curr_pic_flags >> j) & 1U));
			if (((syntax->used_by_curr_pic_flags >> j) & 1U) == 0U)
				emit1("use_delta_flag", j, (long)((syntax->use_delta_flags >> j) & 1U));
		}

		return;
	}

	/* An explicit set: the counts, then each entry's delta and flag. */
	emit("num_negative_pics", (long)syntax->num_negative_pics);
	emit("num_positive_pics", (long)syntax->num_positive_pics);
	for (j = 0U; j < syntax->num_negative_pics && j < H265SW_MAX_RPS; j++) {
		emit1("delta_poc_s0_minus1", j, (long)syntax->delta_poc_s0_minus1[j]);
		emit1("used_by_curr_pic_s0_flag", j, (long)((syntax->used_by_curr_pic_s0_flags >> j) & 1U));
	}

	for (j = 0U; j < syntax->num_positive_pics && j < H265SW_MAX_RPS; j++) {
		emit1("delta_poc_s1_minus1", j, (long)syntax->delta_poc_s1_minus1[j]);
		emit1("used_by_curr_pic_s1_flag", j, (long)((syntax->used_by_curr_pic_s1_flags >> j) & 1U));
	}
}

/* Writes a derived short-term set (7.4.8) as "=name[index] neg pos delta:used ...". */
static void
dump_st_derived(
	const char *name,
	unsigned index,
	const struct h265sw_st_rps *rps)
{
	unsigned i;

	/* The counts, then DeltaPocS0 and DeltaPocS1 in order with their used flags. */
	printf("=%s[%u] %u %u", name, index, rps->num_negative, rps->num_positive);
	for (i = 0U; i < rps->num_negative + rps->num_positive && i < H265SW_MAX_RPS; i++)
		printf(" %ld:%u", (long)rps->delta_poc[i], (unsigned)rps->used[i]);
	printf("\n");
}

/* Writes the scaling lists in use (7.4.5) as derived values. */
static void
dump_scaling(
	const struct h265sw_scaling_list *list)
{
	unsigned size_id;
	unsigned matrix_id;
	unsigned count;
	unsigned i;

	/* Every list the syntax codes, then the DC values of the 16x16 and 32x32 sizes. */
	for (size_id = 0U; size_id < 4U; size_id++) {
		for (matrix_id = 0U; matrix_id < 6U; matrix_id += (size_id == 3U) ? 3U : 1U) {
			count = (size_id == 0U) ? 16U : 64U;
			printf("=ScalingList[%u][%u]", size_id, matrix_id);
			for (i = 0U; i < count; i++)
				printf(" %u", (unsigned)list->list[size_id][matrix_id][i]);
			printf("\n");
			if (size_id >= 2U)
				printf("=ScalingDC[%u][%u] %u\n", size_id, matrix_id, (unsigned)list->dc[size_id - 2U][matrix_id]);
		}
	}
}

/* Writes video_parameter_set_rbsp() (7.3.2.1) up to the first HRD. */
static void
dump_vps(
	const struct h265sw_vps *vps,
	uint64_t index)
{
	unsigned i;
	unsigned first;

	/* The section, the ids, the layers and the profile. */
	printf("@VPS %u %llu\n", vps->id, (unsigned long long)index);
	emit("vps_video_parameter_set_id", (long)vps->id);
	emit("vps_base_layer_internal_flag", (long)vps->base_layer_internal);
	emit("vps_base_layer_available_flag", (long)vps->base_layer_available);
	emit("vps_max_layers_minus1", (long)vps->max_layers_minus1);
	emit("vps_max_sub_layers_minus1", (long)vps->max_sub_layers_minus1);
	emit("vps_temporal_id_nesting_flag", (long)vps->temporal_id_nesting);
	dump_ptl(&vps->ptl);

	/* The sub-layer ordering, written from the first signalled sub-layer. */
	emit("vps_sub_layer_ordering_info_present_flag", (long)vps->sub_layer_ordering_info_present);
	first = (vps->sub_layer_ordering_info_present != 0) ? 0U : vps->max_sub_layers_minus1;
	for (i = first; i <= vps->max_sub_layers_minus1 && i < H265SW_MAX_SUB_LAYERS; i++) {
		emit1("vps_max_dec_pic_buffering_minus1", i, (long)vps->max_dec_pic_buffering_minus1[i]);
		emit1("vps_max_num_reorder_pics", i, (long)vps->max_num_reorder_pics[i]);
		emit1("vps_max_latency_increase_plus1", i, (long)vps->max_latency_increase_plus1[i]);
	}

	/* The layer sets, the timing and the first HRD. */
	emit("vps_max_layer_id", (long)vps->max_layer_id);
	emit("vps_num_layer_sets_minus1", (long)vps->num_layer_sets_minus1);
	emit("vps_timing_info_present_flag", (long)vps->timing_present);
	if (vps->timing_present != 0) {
		emit("vps_num_units_in_tick", (long)vps->num_units_in_tick);
		emit("vps_time_scale", (long)vps->time_scale);
		emit("vps_poc_proportional_to_timing_flag", (long)vps->poc_proportional_to_timing);
		if (vps->poc_proportional_to_timing != 0)
			emit("vps_num_ticks_poc_diff_one_minus1", (long)vps->num_ticks_poc_diff_one_minus1);
		emit("vps_num_hrd_parameters", (long)vps->num_hrd_parameters);
		if (vps->num_hrd_parameters != 0U)
			dump_hrd(&vps->hrd, 1, vps->max_sub_layers_minus1);
	}

	emit("vps_extension_flag", (long)vps->extension);
}

/* Writes seq_parameter_set_rbsp() (7.3.2.2.1) and the derived sets and lists. */
static void
dump_sps(
	const struct h265sw_sps *sps,
	uint64_t index)
{
	unsigned i;
	unsigned first;
	static const char *const range_names[9] = {
		"transform_skip_rotation_enabled_flag",
		"transform_skip_context_enabled_flag",
		"implicit_rdpcm_enabled_flag",
		"explicit_rdpcm_enabled_flag",
		"extended_precision_processing_flag",
		"intra_smoothing_disabled_flag",
		"high_precision_offsets_enabled_flag",
		"persistent_rice_adaptation_enabled_flag",
		"cabac_bypass_alignment_enabled_flag"
	};

	/* The section, the ids, the sub-layers and the profile. */
	printf("@SPS %u %llu\n", sps->id, (unsigned long long)index);
	emit("sps_video_parameter_set_id", (long)sps->vps_id);
	emit("sps_max_sub_layers_minus1", (long)sps->max_sub_layers_minus1);
	emit("sps_temporal_id_nesting_flag", (long)sps->temporal_id_nesting);
	dump_ptl(&sps->ptl);
	emit("sps_seq_parameter_set_id", (long)sps->id);

	/* The format, the size and the window. */
	emit("chroma_format_idc", (long)sps->chroma_format_idc);
	if (sps->chroma_format_idc == 3U)
		emit("separate_colour_plane_flag", (long)sps->separate_colour_plane);
	emit("pic_width_in_luma_samples", (long)sps->width);
	emit("pic_height_in_luma_samples", (long)sps->height);
	emit("conformance_window_flag", (long)sps->conformance_window);
	if (sps->conformance_window != 0) {
		emit("conf_win_left_offset", (long)sps->conf_win_offset[0]);
		emit("conf_win_right_offset", (long)sps->conf_win_offset[1]);
		emit("conf_win_top_offset", (long)sps->conf_win_offset[2]);
		emit("conf_win_bottom_offset", (long)sps->conf_win_offset[3]);
	}

	/* The bit depths, the order count bits and the sub-layer ordering. */
	emit("bit_depth_luma_minus8", (long)sps->bit_depth_luma - 8L);
	emit("bit_depth_chroma_minus8", (long)sps->bit_depth_chroma - 8L);
	emit("log2_max_pic_order_cnt_lsb_minus4", (long)sps->log2_max_poc_lsb - 4L);
	emit("sps_sub_layer_ordering_info_present_flag", (long)sps->sub_layer_ordering_info_present);
	first = (sps->sub_layer_ordering_info_present != 0) ? 0U : sps->max_sub_layers_minus1;
	for (i = first; i <= sps->max_sub_layers_minus1 && i < H265SW_MAX_SUB_LAYERS; i++) {
		emit1("sps_max_dec_pic_buffering_minus1", i, (long)sps->max_dec_pic_buffering_minus1[i]);
		emit1("sps_max_num_reorder_pics", i, (long)sps->max_num_reorder_pics[i]);
		emit1("sps_max_latency_increase_plus1", i, (long)sps->max_latency_increase_plus1[i]);
	}

	/* The block sizes, in the minus and diff forms of the syntax. */
	emit("log2_min_luma_coding_block_size_minus3", (long)sps->min_cb_log2 - 3L);
	emit("log2_diff_max_min_luma_coding_block_size", (long)sps->ctb_log2 - (long)sps->min_cb_log2);
	emit("log2_min_luma_transform_block_size_minus2", (long)sps->min_tb_log2 - 2L);
	emit("log2_diff_max_min_luma_transform_block_size", (long)sps->max_tb_log2 - (long)sps->min_tb_log2);
	emit("max_transform_hierarchy_depth_inter", (long)sps->max_th_depth_inter);
	emit("max_transform_hierarchy_depth_intra", (long)sps->max_th_depth_intra);

	/* The scaling lists (the ones in use as derived values). */
	emit("scaling_list_enabled_flag", (long)sps->scaling_list_enabled);
	if (sps->scaling_list_enabled != 0) {
		emit("sps_scaling_list_data_present_flag", (long)sps->scaling_list_data_present);
		dump_scaling(&sps->scaling);
	}

	/* The coding tools and PCM. */
	emit("amp_enabled_flag", (long)sps->amp);
	emit("sample_adaptive_offset_enabled_flag", (long)sps->sao);
	emit("pcm_enabled_flag", (long)sps->pcm);
	if (sps->pcm != 0) {
		emit("pcm_sample_bit_depth_luma_minus1", (long)sps->pcm_bit_depth_luma - 1L);
		emit("pcm_sample_bit_depth_chroma_minus1", (long)sps->pcm_bit_depth_chroma - 1L);
		emit("log2_min_pcm_luma_coding_block_size_minus3", (long)sps->pcm_min_log2 - 3L);
		emit("log2_diff_max_min_pcm_luma_coding_block_size", (long)sps->pcm_max_log2 - (long)sps->pcm_min_log2);
		emit("pcm_loop_filter_disabled_flag", (long)sps->pcm_loop_filter_disabled);
	}

	/* The short-term sets as signalled, and each derived. */
	emit("num_short_term_ref_pic_sets", (long)sps->num_st_rps);
	for (i = 0U; i < sps->num_st_rps && i < H265SW_MAX_ST_RPS - 1U; i++)
		dump_st_syntax(&sps->st_syntax[i], i, sps->num_st_rps, sps->st_rps);
	for (i = 0U; i < sps->num_st_rps && i < H265SW_MAX_ST_RPS - 1U; i++)
		dump_st_derived("StRps", i, &sps->st_rps[i]);

	/* The long-term candidates. */
	emit("long_term_ref_pics_present_flag", (long)sps->long_term_present);
	if (sps->long_term_present != 0) {
		emit("num_long_term_ref_pics_sps", (long)sps->num_lt_sps);
		for (i = 0U; i < sps->num_lt_sps && i < H265SW_MAX_LT_SPS; i++) {
			emit1("lt_ref_pic_poc_lsb_sps", i, (long)sps->lt_poc_lsb[i]);
			emit1("used_by_curr_pic_lt_sps_flag", i, (long)sps->lt_used[i]);
		}
	}

	/* The last tools, the VUI and the extension flags. */
	emit("sps_temporal_mvp_enabled_flag", (long)sps->temporal_mvp);
	emit("strong_intra_smoothing_enabled_flag", (long)sps->strong_intra_smoothing);
	emit("vui_parameters_present_flag", (long)sps->vui_present);
	if (sps->vui_present != 0)
		dump_vui(&sps->vui, sps->max_sub_layers_minus1);
	emit("sps_extension_present_flag", (long)sps->extension_present);
	if (sps->extension_present != 0) {
		emit("sps_range_extension_flag", (long)sps->range_extension_present);
		emit("sps_multilayer_extension_flag", 0L);
		emit("sps_3d_extension_flag", 0L);
		emit("sps_scc_extension_flag", 0L);
	}

	/* The nine flags of sps_range_extension(). */
	if (sps->range_extension_present != 0) {
		for (i = 0U; i < 9U; i++)
			emit(range_names[i], (long)((sps->range_extension >> i) & 1U));
	}
}

/* Writes pic_parameter_set_rbsp() (7.3.2.3.1). */
static void
dump_pps(
	const struct h265sw_pps *pps,
	uint64_t index)
{
	unsigned i;

	/* The section, the ids and the slice header controls. */
	printf("@PPS %u %llu\n", pps->id, (unsigned long long)index);
	emit("pps_pic_parameter_set_id", (long)pps->id);
	emit("pps_seq_parameter_set_id", (long)pps->sps_id);
	emit("dependent_slice_segments_enabled_flag", (long)pps->dependent_slices);
	emit("output_flag_present_flag", (long)pps->output_flag_present);
	emit("num_extra_slice_header_bits", (long)pps->num_extra_slice_header_bits);
	emit("sign_data_hiding_enabled_flag", (long)pps->sign_data_hiding);
	emit("cabac_init_present_flag", (long)pps->cabac_init_present);
	emit("num_ref_idx_l0_default_active_minus1", (long)pps->num_ref_idx_default[0] - 1L);
	emit("num_ref_idx_l1_default_active_minus1", (long)pps->num_ref_idx_default[1] - 1L);

	/* The QP and the coding tools. */
	emit("init_qp_minus26", (long)pps->init_qp - 26L);
	emit("constrained_intra_pred_flag", (long)pps->constrained_intra);
	emit("transform_skip_enabled_flag", (long)pps->transform_skip);
	emit("cu_qp_delta_enabled_flag", (long)pps->cu_qp_delta);
	if (pps->cu_qp_delta != 0)
		emit("diff_cu_qp_delta_depth", (long)pps->diff_cu_qp_delta_depth);
	emit("pps_cb_qp_offset", (long)pps->cb_qp_offset);
	emit("pps_cr_qp_offset", (long)pps->cr_qp_offset);
	emit("pps_slice_chroma_qp_offsets_present_flag", (long)pps->slice_chroma_qp_offsets_present);
	emit("weighted_pred_flag", (long)pps->weighted_pred);
	emit("weighted_bipred_flag", (long)pps->weighted_bipred);
	emit("transquant_bypass_enabled_flag", (long)pps->transquant_bypass);

	/* The tiles and the wavefronts. */
	emit("tiles_enabled_flag", (long)pps->tiles);
	emit("entropy_coding_sync_enabled_flag", (long)pps->entropy_sync);
	if (pps->tiles != 0) {
		emit("num_tile_columns_minus1", (long)pps->tile_columns - 1L);
		emit("num_tile_rows_minus1", (long)pps->tile_rows - 1L);
		emit("uniform_spacing_flag", (long)pps->uniform_spacing);
		if (pps->uniform_spacing == 0) {
			for (i = 0U; i + 1U < pps->tile_columns && i < H265SW_MAX_TILE_COLS; i++)
				emit1("column_width_minus1", i, (long)pps->column_width[i] - 1L);
			for (i = 0U; i + 1U < pps->tile_rows && i < H265SW_MAX_TILE_ROWS; i++)
				emit1("row_height_minus1", i, (long)pps->row_height[i] - 1L);
		}

		emit("loop_filter_across_tiles_enabled_flag", (long)pps->loop_filter_across_tiles);
	}

	/* The loop filters. */
	emit("pps_loop_filter_across_slices_enabled_flag", (long)pps->loop_filter_across_slices);
	emit("deblocking_filter_control_present_flag", (long)pps->deblocking_control_present);
	if (pps->deblocking_control_present != 0) {
		emit("deblocking_filter_override_enabled_flag", (long)pps->deblocking_override_enabled);
		emit("pps_deblocking_filter_disabled_flag", (long)pps->deblocking_disabled);
		if (pps->deblocking_disabled == 0) {
			emit("pps_beta_offset_div2", (long)pps->beta_offset_div2);
			emit("pps_tc_offset_div2", (long)pps->tc_offset_div2);
		}
	}

	/* The scaling lists, the rest and the extension flags. */
	emit("pps_scaling_list_data_present_flag", (long)pps->scaling_list_present);
	if (pps->scaling_list_present != 0)
		dump_scaling(&pps->scaling);
	emit("lists_modification_present_flag", (long)pps->lists_modification_present);
	emit("log2_parallel_merge_level_minus2", (long)pps->log2_parallel_merge_level - 2L);
	emit("slice_segment_header_extension_present_flag", (long)pps->slice_header_extension_present);
	emit("pps_extension_present_flag", (long)pps->extension_present);
	if (pps->extension_present != 0) {
		emit("pps_range_extension_flag", (long)pps->range_extension_present);
		emit("pps_multilayer_extension_flag", 0L);
		emit("pps_3d_extension_flag", 0L);
		emit("pps_scc_extension_flag", 0L);
	}
}

/* Writes the derived weights of pred_weight_table() (7.4.7.3). */
static void
dump_weights(
	const struct h265sw_slice_header *header,
	unsigned chroma)
{
	unsigned list;
	unsigned lists;
	unsigned i;
	unsigned j;
	const struct h265sw_weights *weights;

	/* The denominators: the luma one as signalled, the chroma one as a delta. */
	weights = &header->weights;
	emit("luma_log2_weight_denom", (long)weights->luma_log2_denom);
	if (chroma != 0U)
		emit("delta_chroma_log2_weight_denom", (long)weights->chroma_log2_denom - (long)weights->luma_log2_denom);

	/* Each entry of list 0 and, for a B slice, list 1. */
	lists = (header->slice_type == H265SW_SLICE_B) ? 2U : 1U;
	for (list = 0U; list < lists; list++) {
		for (i = 0U; i < header->num_ref_idx[list] && i < H265SW_MAX_REFS; i++) {
			printf("=LumaWeightL%u[%u] %d\n", list, i, (int)weights->luma_weight[list][i]);
			printf("=LumaOffsetL%u[%u] %d\n", list, i, (int)weights->luma_offset[list][i]);
			for (j = 0U; chroma != 0U && j < 2U; j++) {
				printf("=ChromaWeightL%u[%u][%u] %d\n", list, i, j, (int)weights->chroma_weight[list][i][j]);
				printf("=ChromaOffsetL%u[%u][%u] %d\n", list, i, j, (int)weights->chroma_offset[list][i][j]);
			}
		}
	}
}

/* Writes slice_segment_header() (7.3.6.1) of the first segment of a picture. */
static void
dump_slice(
	const struct h265_picture *picture,
	const struct h265sw_sps *sps,
	const struct h265sw_pps *pps)
{
	const struct h265sw_slice_header *header;
	unsigned chroma;
	unsigned i;
	unsigned count;
	uint32_t previous;
	int idr;
	int inter;
	int collocated;

	/* The section and the elements every segment has. */
	header = &picture->header;
	chroma = (sps->separate_colour_plane != 0) ? 0U : sps->chroma_format_idc;
	idr = (header->nal_type == H265SW_NAL_IDR_W_RADL || header->nal_type == H265SW_NAL_IDR_N_LP);
	printf("@SH %llu\n", (unsigned long long)picture->index);
	emit("first_slice_segment_in_pic_flag", (long)header->first_in_pic);
	if (header->nal_type >= H265SW_NAL_BLA_W_LP && header->nal_type <= H265SW_NAL_RSV_IRAP_23)
		emit("no_output_of_prior_pics_flag", (long)header->no_output_of_prior_pics);
	emit("slice_pic_parameter_set_id", (long)header->pps_id);

	/* The reserved flags, the type, the output flag and the colour plane. */
	for (i = 0U; i < pps->num_extra_slice_header_bits; i++)
		emit1("slice_reserved_flag", i, (long)((header->reserved_flags >> i) & 1U));
	emit("slice_type", (long)header->slice_type);
	if (pps->output_flag_present != 0)
		emit("pic_output_flag", (long)header->pic_output);
	if (sps->separate_colour_plane != 0)
		emit("colour_plane_id", (long)header->colour_plane_id);

	/* The order count, the short-term set and the long-term pictures of a non-IDR picture. */
	if (idr == 0) {
		emit("slice_pic_order_cnt_lsb", (long)header->poc_lsb);
		emit("short_term_ref_pic_set_sps_flag", (long)header->st_rps_sps);
		if (header->st_rps_sps == 0) {
			dump_st_syntax(&header->st_syntax, sps->num_st_rps, sps->num_st_rps, sps->st_rps);
			printf("=StRpsBits %u\n", header->st_syntax.bits);
		} else if (sps->num_st_rps > 1U) {
			emit("short_term_ref_pic_set_idx", (long)header->st_rps_idx);
		}

		dump_st_derived("StRpsSlice", 0U, &header->st_rps);
		if (sps->long_term_present != 0) {
			if (sps->num_lt_sps > 0U)
				emit("num_long_term_sps", (long)header->num_long_term_sps);
			emit("num_long_term_pics", (long)header->num_long_term_pics);
			count = header->num_long_term_sps + header->num_long_term_pics;
			for (i = 0U; i < count && i < H265SW_MAX_RPS; i++) {
				if (i < header->num_long_term_sps) {
					if (sps->num_lt_sps > 1U)
						emit1("lt_idx_sps", i, (long)header->lt_idx_sps[i]);
				} else {
					emit1("poc_lsb_lt", i, (long)header->lt_poc_lsb[i]);
					emit1("used_by_curr_pic_lt_flag", i, (long)header->lt_used[i]);
				}

				emit1("delta_poc_msb_present_flag", i, (long)header->lt_msb_present[i]);
				if (header->lt_msb_present[i] != 0U) {
					previous = 0U;
					if (i != 0U && i != header->num_long_term_sps)
						previous = header->lt_msb_cycle[i - 1U];
					emit1("delta_poc_msb_cycle_lt", i, (long)(header->lt_msb_cycle[i] - previous));
				}

				printf("=PocLsbLt[%u] %lu\n", i, (unsigned long)header->lt_poc_lsb[i]);
				printf("=UsedByCurrPicLt[%u] %u\n", i, (unsigned)header->lt_used[i]);
				printf("=DeltaPocMsbCycleLt[%u] %lu\n", i, (unsigned long)header->lt_msb_cycle[i]);
			}
		}

		if (sps->temporal_mvp != 0)
			emit("slice_temporal_mvp_enabled_flag", (long)header->temporal_mvp);
	}

	/* SAO. */
	if (sps->sao != 0) {
		emit("slice_sao_luma_flag", (long)header->sao_luma);
		if (chroma != 0U)
			emit("slice_sao_chroma_flag", (long)header->sao_chroma);
	}

	/* The inter part: list sizes, modification, collocated picture, weights, merge candidates. */
	inter = (header->slice_type == H265SW_SLICE_P || header->slice_type == H265SW_SLICE_B);
	if (inter != 0) {
		printf("=NumRefIdxActive[0] %u\n", header->num_ref_idx[0]);
		if (header->slice_type == H265SW_SLICE_B)
			printf("=NumRefIdxActive[1] %u\n", header->num_ref_idx[1]);
		printf("=NumPicTotalCurr %u\n", header->num_pic_total_curr);
		if (pps->lists_modification_present != 0 && header->num_pic_total_curr > 1U) {
			emit("ref_pic_list_modification_flag_l0", (long)header->list_mod[0]);
			for (i = 0U; header->list_mod[0] != 0 && i < header->num_ref_idx[0]; i++)
				emit1("list_entry_l0", i, (long)header->list_entry[0][i]);
			if (header->slice_type == H265SW_SLICE_B) {
				emit("ref_pic_list_modification_flag_l1", (long)header->list_mod[1]);
				for (i = 0U; header->list_mod[1] != 0 && i < header->num_ref_idx[1]; i++)
					emit1("list_entry_l1", i, (long)header->list_entry[1][i]);
			}
		}

		if (header->slice_type == H265SW_SLICE_B)
			emit("mvd_l1_zero_flag", (long)header->mvd_l1_zero);
		if (pps->cabac_init_present != 0)
			emit("cabac_init_flag", (long)header->cabac_init);
		if (header->temporal_mvp != 0) {
			if (header->slice_type == H265SW_SLICE_B)
				emit("collocated_from_l0_flag", (long)header->collocated_from_l0);
			collocated = (header->collocated_from_l0 != 0 && header->num_ref_idx[0] > 1U) ||
			    (header->collocated_from_l0 == 0 && header->num_ref_idx[1] > 1U);
			if (collocated != 0)
				emit("collocated_ref_idx", (long)header->collocated_ref_idx);
		}

		if ((pps->weighted_pred != 0 && header->slice_type == H265SW_SLICE_P) ||
		    (pps->weighted_bipred != 0 && header->slice_type == H265SW_SLICE_B))
			dump_weights(header, chroma);
		emit("five_minus_max_num_merge_cand", 5L - (long)header->max_merge_cand);
	}

	/* The QP and its chroma offsets. */
	emit("slice_qp_delta", (long)header->qp - (long)pps->init_qp);
	if (pps->slice_chroma_qp_offsets_present != 0) {
		emit("slice_cb_qp_offset", (long)header->cb_qp_offset);
		emit("slice_cr_qp_offset", (long)header->cr_qp_offset);
	}

	/* The deblocking override and the filter across slices. */
	if (pps->deblocking_override_enabled != 0)
		emit("deblocking_filter_override_flag", (long)header->deblocking_override);
	if (header->deblocking_override != 0) {
		emit("slice_deblocking_filter_disabled_flag", (long)header->deblocking_disabled);
		if (header->deblocking_disabled == 0) {
			emit("slice_beta_offset_div2", (long)header->beta_offset_div2);
			emit("slice_tc_offset_div2", (long)header->tc_offset_div2);
		}
	}

	if (pps->loop_filter_across_slices != 0 &&
	    (header->sao_luma != 0 || header->sao_chroma != 0 || header->deblocking_disabled == 0))
		emit("slice_loop_filter_across_slices_enabled_flag", (long)header->loop_filter_across_slices);

	/* The entry points and where the slice data starts. */
	if (pps->tiles != 0 || pps->entropy_sync != 0) {
		emit("num_entry_point_offsets", (long)header->num_entry_points);
		if (header->num_entry_points > 0U)
			emit("offset_len_minus1", (long)header->offset_len - 1L);
	}

	printf("=DataOffset %lu\n", (unsigned long)header->data_offset);
}

/* Records one StdVideo self-check failure. */
static void
std_fail(
	const char *what,
	const char *text,
	unsigned long value)
{
	printf("@STD %s %s %lu\n", what, text, value);
	std_failures++;
}

/* Compares a structure read back with the first parse, byte by byte. */
static void
std_compare(
	const char *what,
	const void *a,
	const void *b,
	size_t size)
{
	const uint8_t *x;
	const uint8_t *y;
	size_t i;

	/* One check per structure; a difference names its first byte offset. */
	std_checks++;
	x = (const uint8_t *)a;
	y = (const uint8_t *)b;
	for (i = 0U; i < size; i++) {
		if (x[i] != y[i]) {
			std_fail(what, "differs-at-byte", (unsigned long)i);
			return;
		}
	}
}

/*
 * Writes the profile_tier_level() of a StdVideo PTL.  StdVideo has no
 * general_profile_space and no compatibility flags: they come from the
 * first parse (orig), as does the level when it is off the table.
 */
static void
std_to_ptl(
	const StdVideoH265ProfileTierLevel *std,
	const struct h265sw_ptl *orig,
	struct h265w_ptl *out)
{
	/* The general part; the sub-layer parts are absent (the parse keeps none). */
	memset(out, 0, sizeof(*out));
	out->general.profile_space = orig->profile_space;
	out->general.tier = std->flags.general_tier_flag;
	out->general.profile_idc = (uint32_t)std->general_profile_idc;
	out->general.compatibility = orig->compatibility;
	out->general.progressive = std->flags.general_progressive_source_flag;
	out->general.interlaced = std->flags.general_interlaced_source_flag;
	out->general.non_packed = std->flags.general_non_packed_constraint_flag;
	out->general.frame_only = std->flags.general_frame_only_constraint_flag;
	out->level_idc = orig->level_idc;
}

/* Writes the hrd_parameters() of a StdVideo HRD. */
static void
std_to_hrd(
	const StdVideoH265HrdParameters *std,
	unsigned max_sub,
	struct h265w_hrd *out)
{
	unsigned i;
	unsigned j;

	/* The common part. */
	memset(out, 0, sizeof(*out));
	out->nal_present = std->flags.nal_hrd_parameters_present_flag;
	out->vcl_present = std->flags.vcl_hrd_parameters_present_flag;
	out->sub_pic_present = std->flags.sub_pic_hrd_params_present_flag;
	out->tick_divisor_minus2 = std->tick_divisor_minus2;
	out->du_cpb_removal_delay_increment_length_minus1 = std->du_cpb_removal_delay_increment_length_minus1;
	out->sub_pic_cpb_params_in_pic_timing_sei = std->flags.sub_pic_cpb_params_in_pic_timing_sei_flag;
	out->dpb_output_delay_du_length_minus1 = std->dpb_output_delay_du_length_minus1;
	out->bit_rate_scale = std->bit_rate_scale;
	out->cpb_size_scale = std->cpb_size_scale;
	out->cpb_size_du_scale = std->cpb_size_du_scale;
	out->initial_cpb_removal_delay_length_minus1 = std->initial_cpb_removal_delay_length_minus1;
	out->au_cpb_removal_delay_length_minus1 = std->au_cpb_removal_delay_length_minus1;
	out->dpb_output_delay_length_minus1 = std->dpb_output_delay_length_minus1;

	/* Each sub-layer's flags, count and CPB values. */
	for (i = 0U; i <= max_sub && i < H265W_MAX_SUB_LAYERS; i++) {
		out->fixed_pic_rate_general[i] = (std->flags.fixed_pic_rate_general_flag >> i) & 1U;
		out->fixed_pic_rate_within_cvs[i] = (std->flags.fixed_pic_rate_within_cvs_flag >> i) & 1U;
		out->low_delay_hrd[i] = (std->flags.low_delay_hrd_flag >> i) & 1U;
		out->elemental_duration_in_tc_minus1[i] = std->elemental_duration_in_tc_minus1[i];
		out->cpb_cnt_minus1[i] = std->cpb_cnt_minus1[i];
		for (j = 0U; j <= std->cpb_cnt_minus1[i] && j < H265W_MAX_CPB; j++) {
			if (std->pSubLayerHrdParametersNal != NULL) {
				out->nal[i].bit_rate_value_minus1[j] = std->pSubLayerHrdParametersNal[i].bit_rate_value_minus1[j];
				out->nal[i].cpb_size_value_minus1[j] = std->pSubLayerHrdParametersNal[i].cpb_size_value_minus1[j];
				out->nal[i].cpb_size_du_value_minus1[j] = std->pSubLayerHrdParametersNal[i].cpb_size_du_value_minus1[j];
				out->nal[i].bit_rate_du_value_minus1[j] = std->pSubLayerHrdParametersNal[i].bit_rate_du_value_minus1[j];
				out->nal[i].cbr_flag[j] = (std->pSubLayerHrdParametersNal[i].cbr_flag >> j) & 1U;
			}

			if (std->pSubLayerHrdParametersVcl != NULL) {
				out->vcl[i].bit_rate_value_minus1[j] = std->pSubLayerHrdParametersVcl[i].bit_rate_value_minus1[j];
				out->vcl[i].cpb_size_value_minus1[j] = std->pSubLayerHrdParametersVcl[i].cpb_size_value_minus1[j];
				out->vcl[i].cpb_size_du_value_minus1[j] = std->pSubLayerHrdParametersVcl[i].cpb_size_du_value_minus1[j];
				out->vcl[i].bit_rate_du_value_minus1[j] = std->pSubLayerHrdParametersVcl[i].bit_rate_du_value_minus1[j];
				out->vcl[i].cbr_flag[j] = (std->pSubLayerHrdParametersVcl[i].cbr_flag >> j) & 1U;
			}
		}
	}
}

/* Writes scaling_list_data() that gives the StdVideo lists (32x32 index k is matrixId 3k). */
static void
std_to_scaling(
	const StdVideoH265ScalingLists *std,
	struct h265w_scaling *out)
{
	static uint8_t list[4][6][64];
	static uint8_t dc[2][6];
	unsigned m;

	/* The lists in the parse's layout, then the signalled form that gives them. */
	memset(list, 0, sizeof(list));
	memset(dc, 0, sizeof(dc));
	for (m = 0U; m < 6U; m++) {
		memcpy(list[0][m], std->ScalingList4x4[m], 16U);
		memcpy(list[1][m], std->ScalingList8x8[m], 64U);
		memcpy(list[2][m], std->ScalingList16x16[m], 64U);
		dc[0][m] = std->ScalingListDCCoef16x16[m];
	}

	memcpy(list[3][0], std->ScalingList32x32[0], 64U);
	memcpy(list[3][3], std->ScalingList32x32[1], 64U);
	dc[1][0] = std->ScalingListDCCoef32x32[0];
	dc[1][3] = std->ScalingListDCCoef32x32[1];
	h265w_scaling_from_lists(out, (const uint8_t (*)[6][64])list, (const uint8_t (*)[6])dc);
}

/*
 * Writes st_ref_pic_set( index ) from StdVideo sets: the prediction syntax
 * of a predicted set (NumDeltaPocs[ RefRpsIdx ] from the referenced
 * set's explicit form, h265-std.h), the explicit set otherwise.
 */
static void
std_to_st_rps(
	const StdVideoH265ShortTermRefPicSet *sets,
	unsigned index,
	struct h265w_st_rps *out)
{
	const StdVideoH265ShortTermRefPicSet *set;
	unsigned ref;
	unsigned j;

	/* The flag and, for a predicted set, its reference and flag pairs. */
	memset(out, 0, sizeof(*out));
	set = &sets[index];
	out->inter_rps_pred = set->flags.inter_ref_pic_set_prediction_flag;
	if (out->inter_rps_pred != 0U) {
		out->delta_idx_minus1 = set->delta_idx_minus1;
		out->delta_rps_sign = set->flags.delta_rps_sign;
		out->abs_delta_rps_minus1 = set->abs_delta_rps_minus1;
		ref = index - (set->delta_idx_minus1 + 1U);
		out->ref_num_delta_pocs = (uint32_t)sets[ref].num_negative_pics + sets[ref].num_positive_pics;
		for (j = 0U; j <= out->ref_num_delta_pocs && j < H265W_MAX_RPS_FLAGS; j++) {
			out->used_by_curr_pic_flag[j] = (set->used_by_curr_pic_flag >> j) & 1U;
			out->use_delta_flag[j] = (set->use_delta_flag >> j) & 1U;
		}

		return;
	}

	/* An explicit set. */
	out->num_negative_pics = set->num_negative_pics;
	out->num_positive_pics = set->num_positive_pics;
	for (j = 0U; j < H265W_MAX_RPS && j < STD_VIDEO_H265_MAX_DPB_SIZE; j++) {
		out->delta_poc_s0_minus1[j] = set->delta_poc_s0_minus1[j];
		out->used_by_curr_pic_s0_flag[j] = (set->used_by_curr_pic_s0_flag >> j) & 1U;
		out->delta_poc_s1_minus1[j] = set->delta_poc_s1_minus1[j];
		out->used_by_curr_pic_s1_flag[j] = (set->used_by_curr_pic_s1_flag >> j) & 1U;
	}
}

/*
 * The VPS round trip.  StdVideo does not carry vps_base_layer_internal_flag,
 * vps_base_layer_available_flag, vps_max_layers_minus1, vps_max_layer_id,
 * the layer sets or vps_extension_flag: they come from the first parse, the
 * layer sets are written as none and the HRD as one, and the read-back
 * structure takes the first parse's counts before the comparison.
 */
static void
roundtrip_vps(
	const struct h265sw_vps *orig)
{
	struct h265_std_vps *std;
	struct h265w_vps *w;
	struct h265sw_ps_store *store;
	struct h265sw_vps copy;
	struct vcodec_bitreader bits;
	unsigned i;
	int error;

	/* The StdVideo form. */
	std = calloc(1U, sizeof(*std));
	w = calloc(1U, sizeof(*w));
	store = calloc(1U, sizeof(*store));
	if (std == NULL || w == NULL || store == NULL) {
		printf("@ERROR out of memory\n");
		exit(2);
	}

	error = h265_std_vps(orig, std);
	std_checks++;
	if (error != 0) {
		std_fail("vps", "h265_std_vps", (unsigned long)error);
		goto done;
	}

	/* The syntax from the StdVideo values. */
	w->vps_id = std->vps.vps_video_parameter_set_id;
	w->base_layer_internal = (uint32_t)orig->base_layer_internal;
	w->base_layer_available = (uint32_t)orig->base_layer_available;
	w->max_layers_minus1 = orig->max_layers_minus1;
	w->max_sub_layers_minus1 = std->vps.vps_max_sub_layers_minus1;
	w->temporal_id_nesting = std->vps.flags.vps_temporal_id_nesting_flag;
	w->reserved_0xffff_16bits = 0xFFFFU;
	std_to_ptl(std->vps.pProfileTierLevel, &orig->ptl, &w->ptl);
	w->sub_layer_ordering_info_present = std->vps.flags.vps_sub_layer_ordering_info_present_flag;
	for (i = 0U; i < H265W_MAX_SUB_LAYERS; i++) {
		w->max_dec_pic_buffering_minus1[i] = std->vps.pDecPicBufMgr->max_dec_pic_buffering_minus1[i];
		w->max_num_reorder_pics[i] = std->vps.pDecPicBufMgr->max_num_reorder_pics[i];
		w->max_latency_increase_plus1[i] = std->vps.pDecPicBufMgr->max_latency_increase_plus1[i];
	}

	w->max_layer_id = orig->max_layer_id;
	w->timing_present = std->vps.flags.vps_timing_info_present_flag;
	w->num_units_in_tick = std->vps.vps_num_units_in_tick;
	w->time_scale = std->vps.vps_time_scale;
	w->poc_proportional = std->vps.flags.vps_poc_proportional_to_timing_flag;
	w->num_ticks_poc_diff_one_minus1 = std->vps.vps_num_ticks_poc_diff_one_minus1;
	if (std->vps.pHrdParameters != NULL) {
		w->num_hrd_parameters = 1U;
		w->cprms_present[0] = 1U;
		std_to_hrd(std->vps.pHrdParameters, w->max_sub_layers_minus1, &w->hrd[0]);
	}

	/* Written and read again. */
	bitwriter_init(&writer);
	h265w_vps(&writer, w);
	h265sw_ps_init(store);
	vcodec_bitreader_init(&bits, writer.data, bitwriter_bytes(&writer));
	error = h265sw_ps_read_vps(store, &bits);
	std_checks++;
	if (error != 0 || store->vps_present[orig->id] == 0) {
		std_fail("vps", "read-back", (unsigned long)error);
		goto done;
	}

	/* The values StdVideo does not carry, then the comparison. */
	copy = store->vps[orig->id];
	copy.num_layer_sets_minus1 = orig->num_layer_sets_minus1;
	copy.num_hrd_parameters = orig->num_hrd_parameters;
	copy.extension = orig->extension;
	std_compare("vps", &copy, orig, sizeof(copy));

done:
	free(store);
	free(w);
	free(std);
}

/*
 * The SPS round trip.  StdVideo does not carry general_profile_space, the
 * compatibility flags, a level off Table A.8 or a profile other than
 * those it names: they come from the first parse (std_to_ptl), and the
 * read-back structure takes the first parse's profile_idc when h265-std
 * mapped it (a compatible profile, h265_std_ptl).
 */
static void
roundtrip_sps(
	const struct h265sw_sps *orig)
{
	struct h265_std_sps *std;
	struct h265w_sps *w;
	struct h265sw_ps_store *store;
	struct h265sw_sps *copy;
	const StdVideoH265SequenceParameterSet *s;
	const StdVideoH265SequenceParameterSetVui *v;
	struct vcodec_bitreader bits;
	unsigned i;
	int error;

	/* The StdVideo form. */
	std = calloc(1U, sizeof(*std));
	w = calloc(1U, sizeof(*w));
	store = calloc(1U, sizeof(*store));
	copy = calloc(1U, sizeof(*copy));
	if (std == NULL || w == NULL || store == NULL || copy == NULL) {
		printf("@ERROR out of memory\n");
		exit(2);
	}

	error = h265_std_sps(orig, std);
	std_checks++;
	if (error != 0) {
		std_fail("sps", "h265_std_sps", (unsigned long)error);
		goto done;
	}

	/* The ids, the profile, the format, the size and the window. */
	s = &std->sps;
	w->vps_id = s->sps_video_parameter_set_id;
	w->max_sub_layers_minus1 = s->sps_max_sub_layers_minus1;
	w->temporal_id_nesting = s->flags.sps_temporal_id_nesting_flag;
	std_to_ptl(s->pProfileTierLevel, &orig->ptl, &w->ptl);
	w->sps_id = s->sps_seq_parameter_set_id;
	w->chroma_format_idc = (uint32_t)s->chroma_format_idc;
	w->separate_colour_plane = s->flags.separate_colour_plane_flag;
	w->width = s->pic_width_in_luma_samples;
	w->height = s->pic_height_in_luma_samples;
	w->conformance_window = s->flags.conformance_window_flag;
	w->conf_win_offset[0] = s->conf_win_left_offset;
	w->conf_win_offset[1] = s->conf_win_right_offset;
	w->conf_win_offset[2] = s->conf_win_top_offset;
	w->conf_win_offset[3] = s->conf_win_bottom_offset;

	/* The bit depths, the order count and the sub-layer buffering. */
	w->bit_depth_luma_minus8 = s->bit_depth_luma_minus8;
	w->bit_depth_chroma_minus8 = s->bit_depth_chroma_minus8;
	w->log2_max_poc_lsb_minus4 = s->log2_max_pic_order_cnt_lsb_minus4;
	w->sub_layer_ordering_info_present = s->flags.sps_sub_layer_ordering_info_present_flag;
	for (i = 0U; i < H265W_MAX_SUB_LAYERS; i++) {
		w->max_dec_pic_buffering_minus1[i] = s->pDecPicBufMgr->max_dec_pic_buffering_minus1[i];
		w->max_num_reorder_pics[i] = s->pDecPicBufMgr->max_num_reorder_pics[i];
		w->max_latency_increase_plus1[i] = s->pDecPicBufMgr->max_latency_increase_plus1[i];
	}

	/* The block sizes, the scaling lists, the tools and PCM. */
	w->log2_min_cb_minus3 = s->log2_min_luma_coding_block_size_minus3;
	w->log2_diff_max_min_cb = s->log2_diff_max_min_luma_coding_block_size;
	w->log2_min_tb_minus2 = s->log2_min_luma_transform_block_size_minus2;
	w->log2_diff_max_min_tb = s->log2_diff_max_min_luma_transform_block_size;
	w->max_th_depth_inter = s->max_transform_hierarchy_depth_inter;
	w->max_th_depth_intra = s->max_transform_hierarchy_depth_intra;
	w->scaling_list_enabled = s->flags.scaling_list_enabled_flag;
	w->scaling_list_data_present = s->flags.sps_scaling_list_data_present_flag;
	if (w->scaling_list_data_present != 0U && s->pScalingLists != NULL)
		std_to_scaling(s->pScalingLists, &w->scaling);
	w->amp = s->flags.amp_enabled_flag;
	w->sao = s->flags.sample_adaptive_offset_enabled_flag;
	w->pcm = s->flags.pcm_enabled_flag;
	w->pcm_bit_depth_luma_minus1 = s->pcm_sample_bit_depth_luma_minus1;
	w->pcm_bit_depth_chroma_minus1 = s->pcm_sample_bit_depth_chroma_minus1;
	w->log2_min_pcm_minus3 = s->log2_min_pcm_luma_coding_block_size_minus3;
	w->log2_diff_max_min_pcm = s->log2_diff_max_min_pcm_luma_coding_block_size;
	w->pcm_loop_filter_disabled = s->flags.pcm_loop_filter_disabled_flag;

	/* The short-term sets and the long-term candidates. */
	w->num_st_rps = s->num_short_term_ref_pic_sets;
	for (i = 0U; i < w->num_st_rps && i < H265W_MAX_ST_RPS; i++)
		std_to_st_rps(s->pShortTermRefPicSet, i, &w->st_rps[i]);
	w->long_term_present = s->flags.long_term_ref_pics_present_flag;
	w->num_lt_sps = s->num_long_term_ref_pics_sps;
	for (i = 0U; i < w->num_lt_sps && i < H265W_MAX_LT_SPS && s->pLongTermRefPicsSps != NULL; i++) {
		w->lt_poc_lsb[i] = s->pLongTermRefPicsSps->lt_ref_pic_poc_lsb_sps[i];
		w->lt_used[i] = (s->pLongTermRefPicsSps->used_by_curr_pic_lt_sps_flag >> i) & 1U;
	}

	/* The last tools and the VUI. */
	w->temporal_mvp = s->flags.sps_temporal_mvp_enabled_flag;
	w->strong_intra_smoothing = s->flags.strong_intra_smoothing_enabled_flag;
	w->vui_present = s->flags.vui_parameters_present_flag;
	v = s->pSequenceParameterSetVui;
	if (w->vui_present != 0U && v != NULL) {
		w->vui.aspect_ratio_present = v->flags.aspect_ratio_info_present_flag;
		w->vui.aspect_ratio_idc = (uint32_t)v->aspect_ratio_idc;
		w->vui.sar_width = v->sar_width;
		w->vui.sar_height = v->sar_height;
		w->vui.overscan_present = v->flags.overscan_info_present_flag;
		w->vui.overscan_appropriate = v->flags.overscan_appropriate_flag;
		w->vui.signal_present = v->flags.video_signal_type_present_flag;
		w->vui.video_format = v->video_format;
		w->vui.full_range = v->flags.video_full_range_flag;
		w->vui.colour_present = v->flags.colour_description_present_flag;
		w->vui.colour_primaries = v->colour_primaries;
		w->vui.transfer_characteristics = v->transfer_characteristics;
		w->vui.matrix_coeffs = v->matrix_coeffs;
		w->vui.chroma_loc_present = v->flags.chroma_loc_info_present_flag;
		w->vui.chroma_loc_top = v->chroma_sample_loc_type_top_field;
		w->vui.chroma_loc_bottom = v->chroma_sample_loc_type_bottom_field;
		w->vui.neutral_chroma = v->flags.neutral_chroma_indication_flag;
		w->vui.field_seq = v->flags.field_seq_flag;
		w->vui.frame_field_info_present = v->flags.frame_field_info_present_flag;
		w->vui.default_display_window = v->flags.default_display_window_flag;
		w->vui.def_disp_win_offset[0] = v->def_disp_win_left_offset;
		w->vui.def_disp_win_offset[1] = v->def_disp_win_right_offset;
		w->vui.def_disp_win_offset[2] = v->def_disp_win_top_offset;
		w->vui.def_disp_win_offset[3] = v->def_disp_win_bottom_offset;
		w->vui.timing_present = v->flags.vui_timing_info_present_flag;
		w->vui.num_units_in_tick = v->vui_num_units_in_tick;
		w->vui.time_scale = v->vui_time_scale;
		w->vui.poc_proportional = v->flags.vui_poc_proportional_to_timing_flag;
		w->vui.num_ticks_poc_diff_one_minus1 = v->vui_num_ticks_poc_diff_one_minus1;
		w->vui.hrd_present = v->flags.vui_hrd_parameters_present_flag;
		if (v->pHrdParameters != NULL)
			std_to_hrd(v->pHrdParameters, w->max_sub_layers_minus1, &w->vui.hrd);
		w->vui.bitstream_restriction = v->flags.bitstream_restriction_flag;
		w->vui.tiles_fixed_structure = v->flags.tiles_fixed_structure_flag;
		w->vui.motion_vectors_over_pic_boundaries = v->flags.motion_vectors_over_pic_boundaries_flag;
		w->vui.restricted_ref_pic_lists = v->flags.restricted_ref_pic_lists_flag;
		w->vui.min_spatial_segmentation_idc = v->min_spatial_segmentation_idc;
		w->vui.max_bytes_per_pic_denom = v->max_bytes_per_pic_denom;
		w->vui.max_bits_per_min_cu_denom = v->max_bits_per_min_cu_denom;
		w->vui.log2_max_mv_length_horizontal = v->log2_max_mv_length_horizontal;
		w->vui.log2_max_mv_length_vertical = v->log2_max_mv_length_vertical;
	}

	/* The extension flags and the range extension's nine flags. */
	w->extension_present = s->flags.sps_extension_present_flag;
	w->range_extension_flag = s->flags.sps_range_extension_flag;
	w->range_extension =
	    ((uint32_t)s->flags.transform_skip_rotation_enabled_flag << 0) |
	    ((uint32_t)s->flags.transform_skip_context_enabled_flag << 1) |
	    ((uint32_t)s->flags.implicit_rdpcm_enabled_flag << 2) |
	    ((uint32_t)s->flags.explicit_rdpcm_enabled_flag << 3) |
	    ((uint32_t)s->flags.extended_precision_processing_flag << 4) |
	    ((uint32_t)s->flags.intra_smoothing_disabled_flag << 5) |
	    ((uint32_t)s->flags.high_precision_offsets_enabled_flag << 6) |
	    ((uint32_t)s->flags.persistent_rice_adaptation_enabled_flag << 7) |
	    ((uint32_t)s->flags.cabac_bypass_alignment_enabled_flag << 8);

	/* Written and read again. */
	bitwriter_init(&writer);
	h265w_sps(&writer, w);
	h265sw_ps_init(store);
	vcodec_bitreader_init(&bits, writer.data, bitwriter_bytes(&writer));
	error = h265sw_ps_read_sps(store, &bits);
	std_checks++;
	if (error != 0 || store->sps_present[orig->id] == 0) {
		std_fail("sps", "read-back", (unsigned long)error);
		goto done;
	}

	/* The profile h265-std may have mapped, then the comparison. */
	*copy = store->sps[orig->id];
	if ((unsigned)s->pProfileTierLevel->general_profile_idc != orig->ptl.profile_idc) {
		printf("@STD sps profile-mapped %u\n", orig->ptl.profile_idc);
		copy->ptl.profile_idc = orig->ptl.profile_idc;
	}

	std_compare("sps", copy, orig, sizeof(*copy));

done:
	free(copy);
	free(store);
	free(w);
	free(std);
}

/* The PPS round trip (StdVideo carries every field the parse keeps). */
static void
roundtrip_pps(
	const struct h265sw_pps *orig,
	const struct h265sw_sps *sps)
{
	struct h265_std_pps *std;
	struct h265w_pps *w;
	struct h265sw_ps_store *store;
	const StdVideoH265PictureParameterSet *p;
	struct vcodec_bitreader bits;
	unsigned i;
	int error;

	/* The StdVideo form. */
	std = calloc(1U, sizeof(*std));
	w = calloc(1U, sizeof(*w));
	store = calloc(1U, sizeof(*store));
	if (std == NULL || w == NULL || store == NULL) {
		printf("@ERROR out of memory\n");
		exit(2);
	}

	error = h265_std_pps(orig, sps, std);
	std_checks++;
	if (error != 0) {
		std_fail("pps", "h265_std_pps", (unsigned long)error);
		goto done;
	}

	/* The ids, the header controls, the QP and the tools. */
	p = &std->pps;
	std_checks++;
	if (p->sps_video_parameter_set_id != sps->vps_id)
		std_fail("pps", "sps_video_parameter_set_id", (unsigned long)p->sps_video_parameter_set_id);
	w->pps_id = p->pps_pic_parameter_set_id;
	w->sps_id = p->pps_seq_parameter_set_id;
	w->dependent_slices = p->flags.dependent_slice_segments_enabled_flag;
	w->output_flag_present = p->flags.output_flag_present_flag;
	w->num_extra_slice_header_bits = p->num_extra_slice_header_bits;
	w->sign_data_hiding = p->flags.sign_data_hiding_enabled_flag;
	w->cabac_init_present = p->flags.cabac_init_present_flag;
	w->num_ref_idx_l0_default_minus1 = p->num_ref_idx_l0_default_active_minus1;
	w->num_ref_idx_l1_default_minus1 = p->num_ref_idx_l1_default_active_minus1;
	w->init_qp_minus26 = p->init_qp_minus26;
	w->constrained_intra = p->flags.constrained_intra_pred_flag;
	w->transform_skip = p->flags.transform_skip_enabled_flag;
	w->cu_qp_delta = p->flags.cu_qp_delta_enabled_flag;
	w->diff_cu_qp_delta_depth = p->diff_cu_qp_delta_depth;
	w->cb_qp_offset = p->pps_cb_qp_offset;
	w->cr_qp_offset = p->pps_cr_qp_offset;
	w->slice_chroma_qp_offsets_present = p->flags.pps_slice_chroma_qp_offsets_present_flag;
	w->weighted_pred = p->flags.weighted_pred_flag;
	w->weighted_bipred = p->flags.weighted_bipred_flag;
	w->transquant_bypass = p->flags.transquant_bypass_enabled_flag;

	/* The tiles, the loop filters and the deblocking controls. */
	w->tiles = p->flags.tiles_enabled_flag;
	w->entropy_sync = p->flags.entropy_coding_sync_enabled_flag;
	w->num_tile_columns_minus1 = p->num_tile_columns_minus1;
	w->num_tile_rows_minus1 = p->num_tile_rows_minus1;
	w->uniform_spacing = p->flags.uniform_spacing_flag;
	for (i = 0U; i < H265W_MAX_TILES && i < STD_VIDEO_H265_CHROMA_QP_OFFSET_TILE_COLS_LIST_SIZE; i++)
		w->column_width_minus1[i] = p->column_width_minus1[i];
	for (i = 0U; i < H265W_MAX_TILES && i < STD_VIDEO_H265_CHROMA_QP_OFFSET_TILE_ROWS_LIST_SIZE; i++)
		w->row_height_minus1[i] = p->row_height_minus1[i];
	w->loop_filter_across_tiles = p->flags.loop_filter_across_tiles_enabled_flag;
	w->loop_filter_across_slices = p->flags.pps_loop_filter_across_slices_enabled_flag;
	w->deblocking_control_present = p->flags.deblocking_filter_control_present_flag;
	w->deblocking_override_enabled = p->flags.deblocking_filter_override_enabled_flag;
	w->deblocking_disabled = p->flags.pps_deblocking_filter_disabled_flag;
	w->beta_offset_div2 = p->pps_beta_offset_div2;
	w->tc_offset_div2 = p->pps_tc_offset_div2;

	/* The scaling lists, the rest and the extension flags. */
	w->scaling_list_present = p->flags.pps_scaling_list_data_present_flag;
	if (w->scaling_list_present != 0U && p->pScalingLists != NULL)
		std_to_scaling(p->pScalingLists, &w->scaling);
	w->lists_modification_present = p->flags.lists_modification_present_flag;
	w->log2_parallel_merge_level_minus2 = p->log2_parallel_merge_level_minus2;
	w->slice_header_extension_present = p->flags.slice_segment_header_extension_present_flag;
	w->extension_present = p->flags.pps_extension_present_flag;
	w->range_extension_flag = p->flags.pps_range_extension_flag;

	/* Written and read again, then the comparison. */
	bitwriter_init(&writer);
	h265w_pps(&writer, w);
	h265sw_ps_init(store);
	vcodec_bitreader_init(&bits, writer.data, bitwriter_bytes(&writer));
	error = h265sw_ps_read_pps(store, &bits);
	std_checks++;
	if (error != 0 || store->pps_present[orig->id] == 0) {
		std_fail("pps", "read-back", (unsigned long)error);
		goto done;
	}

	std_compare("pps", &store->pps[orig->id], orig, sizeof(*orig));

done:
	free(store);
	free(w);
	free(std);
}

/* Checks the picture information h265-std makes against the picture and plan. */
static void
check_picture(
	const struct h265_picture *picture,
	const struct h265_dpb_plan *plan)
{
	StdVideoDecodeH265PictureInfo info;
	StdVideoDecodeH265ReferenceInfo refs[H265_DPB_PICTURES];
	StdVideoDecodeH265ReferenceInfo setup;
	unsigned bits;
	unsigned i;
	int error;

	/* The picture information. */
	error = h265_std_picture(picture, plan, &info);
	std_checks++;
	if (error != 0) {
		std_fail("picture", "h265_std_picture", (unsigned long)error);
		return;
	}

	/* Its fields against the picture (h265-std.h). */
	bits = (picture->header.st_rps_sps == 0) ? picture->header.st_syntax.bits : 0U;
	std_checks++;
	if (info.PicOrderCntVal != picture->poc ||
	    info.flags.IrapPicFlag != (picture->irap != 0) ||
	    info.flags.IdrPicFlag != (picture->idr != 0) ||
	    info.flags.IsReference != 1U ||
	    info.sps_video_parameter_set_id != picture->vps_id ||
	    info.pps_seq_parameter_set_id != picture->sps_id ||
	    info.pps_pic_parameter_set_id != picture->pps_id ||
	    info.flags.short_term_ref_pic_set_sps_flag != (picture->header.st_rps_sps != 0) ||
	    info.NumBitsForSTRefPicSetInSlice != bits)
		std_fail("picture", "fields", (unsigned long)picture->index);

	/* The Curr sets as slots. */
	std_checks++;
	for (i = 0U; i < STD_VIDEO_DECODE_H265_REF_PIC_SET_LIST_SIZE; i++) {
		if (i < plan->num_st_before && info.RefPicSetStCurrBefore[i] != (uint8_t)plan->st_before[i])
			std_fail("picture", "st_before", (unsigned long)picture->index);
		if (i >= plan->num_st_before && info.RefPicSetStCurrBefore[i] != STD_VIDEO_H265_NO_REFERENCE_PICTURE)
			std_fail("picture", "st_before-pad", (unsigned long)picture->index);
		if (i < plan->num_st_after && info.RefPicSetStCurrAfter[i] != (uint8_t)plan->st_after[i])
			std_fail("picture", "st_after", (unsigned long)picture->index);
		if (i < plan->num_lt_curr && info.RefPicSetLtCurr[i] != (uint8_t)plan->lt_curr[i])
			std_fail("picture", "lt_curr", (unsigned long)picture->index);
	}

	/* The reference and setup information. */
	h265_std_references(plan, refs);
	std_checks++;
	for (i = 0U; i < plan->reference_count && i < H265_DPB_PICTURES; i++) {
		if (refs[i].PicOrderCntVal != plan->reference_poc[i] ||
		    refs[i].flags.used_for_long_term_reference != plan->reference_long[i] ||
		    refs[i].flags.unused_for_reference != 0U)
			std_fail("picture", "reference", (unsigned long)picture->index);
	}

	h265_std_setup(picture->poc, &setup);
	std_checks++;
	if (setup.PicOrderCntVal != picture->poc || setup.flags.used_for_long_term_reference != 0U)
		std_fail("picture", "setup", (unsigned long)picture->index);
}

/* Prints the pictures output in output order and releases their slots at once. */
static void
print_outputs(
	const struct h265_dpb_output *output,
	unsigned count,
	struct h265_dpb *dpb)
{
	unsigned i;

	/* Each output: its order count and decode index, then its slot is free. */
	for (i = 0U; i < count; i++) {
		printf("@OUT %ld %llu\n", (long)output[i].poc, (unsigned long long)output[i].id);
		h265_dpb_release(dpb, output[i].slot);
	}
}

/* Prints what the reader did not return since the last call. */
static void
print_skips(
	const struct h265_stream_counts *before,
	const struct h265_stream_counts *after)
{
	/* One line per reason whose count grew. */
	if (after->before_irap != before->before_irap)
		printf("@SKIP before_irap %llu\n", (unsigned long long)(after->before_irap - before->before_irap));
	if (after->rasl_skipped != before->rasl_skipped)
		printf("@SKIP rasl_skipped %llu\n", (unsigned long long)(after->rasl_skipped - before->rasl_skipped));
	if (after->reserved_type != before->reserved_type)
		printf("@SKIP reserved_type %llu\n", (unsigned long long)(after->reserved_type - before->reserved_type));
	if (after->other_layer != before->other_layer)
		printf("@SKIP other_layer %llu\n", (unsigned long long)(after->other_layer - before->other_layer));
	if (after->refused_sets != before->refused_sets)
		printf("@SKIP refused_sets %llu\n", (unsigned long long)(after->refused_sets - before->refused_sets));
	if (after->broken_pictures != before->broken_pictures)
		printf("@SKIP broken_pictures %llu\n", (unsigned long long)(after->broken_pictures - before->broken_pictures));
}

/* Reads a whole file ("-" for the standard input) into a new buffer. */
static int
read_input(
	const char *path,
	uint8_t **data,
	size_t *size)
{
	FILE *file;
	uint8_t *buffer;
	size_t length;
	size_t got;

	/* The file. */
	file = stdin;
	if (strcmp(path, "-") != 0)
		file = fopen(path, "rb");
	if (file == NULL)
		return errno;

	/* Its bytes, up to the bound. */
	buffer = malloc(CONFORM_MAX_INPUT);
	if (buffer == NULL)
		return ENOMEM;
	length = 0U;
	do {
		got = fread(buffer + length, 1U, CONFORM_MAX_INPUT - length, file);
		length += got;
	} while (got != 0U && length < CONFORM_MAX_INPUT);

	if (file != stdin)
		fclose(file);
	*data = buffer;
	*size = length;
	return 0;
}

int
main(
	int argc,
	char **argv)
{
	struct h265_stream *stream;
	struct h265_picture *picture;
	struct h265_dpb dpb;
	struct h265_dpb_plan plan;
	struct h265_dpb_output output[H265_DPB_MAX_OUTPUT];
	struct h265_stream_counts counts;
	const struct h265sw_ps_store *store;
	const struct h265sw_sps *sps;
	const struct h265sw_pps *pps;
	uint8_t *data;
	size_t size;
	unsigned count;
	unsigned long missing;
	unsigned long discarded;
	unsigned long evicted;
	unsigned long dpb_errors;
	int error;

	/* The input and the reader. */
	if (argc != 2) {
		fprintf(stderr, "usage: conform <bitstream>|-\n");
		return 2;
	}

	data = NULL;
	size = 0U;
	error = read_input(argv[1], &data, &size);
	if (error != 0) {
		printf("@ERROR read %d\n", error);
		return 2;
	}

	stream = calloc(1U, sizeof(*stream));
	picture = calloc(1U, sizeof(*picture));
	if (stream == NULL || picture == NULL) {
		printf("@ERROR out of memory\n");
		return 2;
	}

	h265_stream_init(stream);
	h265_stream_input(stream, data, size);
	h265_dpb_init(&dpb, CONFORM_SLOTS);
	store = &stream->store;
	missing = 0U;
	discarded = 0U;
	evicted = 0U;
	dpb_errors = 0U;

	/* Each picture in decoding order. */
	for (;;) {
		counts = stream->counts;
		error = h265_stream_next(stream, picture);
		print_skips(&counts, &stream->counts);
		if (error == ENOENT)
			break;
		if (error != 0) {
			printf("@ERROR h265_stream_next %d\n", error);
			continue;
		}

		/* The picture and the parameter sets it uses, when new or changed. */
		sps = &store->sps[picture->sps_id];
		pps = &store->pps[picture->pps_id];
		printf("@PIC %llu %u %u %ld %d %d %d %d %d %d %d %u %u %u %u %u\n",
		    (unsigned long long)picture->index, picture->nal_type, picture->temporal_id, (long)picture->poc,
		    picture->pic_output, picture->no_rasl_output, picture->first, picture->after_eos,
		    picture->no_output_of_prior_pics, picture->irap, picture->idr, picture->vps_id, picture->sps_id,
		    picture->pps_id, picture->generation, picture->slice_count);
		if (store->vps_present[picture->vps_id] != 0 &&
		    (vps_dumped[picture->vps_id] == 0 ||
		    memcmp(&vps_last[picture->vps_id], &store->vps[picture->vps_id], sizeof(struct h265sw_vps)) != 0)) {
			vps_last[picture->vps_id] = store->vps[picture->vps_id];
			vps_dumped[picture->vps_id] = 1;
			dump_vps(&store->vps[picture->vps_id], picture->index);
			roundtrip_vps(&store->vps[picture->vps_id]);
		}

		if (sps_dumped[picture->sps_id] == 0 || memcmp(&sps_last[picture->sps_id], sps, sizeof(*sps)) != 0) {
			sps_last[picture->sps_id] = *sps;
			sps_dumped[picture->sps_id] = 1;
			dump_sps(sps, picture->index);
			roundtrip_sps(sps);
		}

		if (pps_dumped[picture->pps_id] == 0 || memcmp(&pps_last[picture->pps_id], pps, sizeof(*pps)) != 0) {
			pps_last[picture->pps_id] = *pps;
			pps_dumped[picture->pps_id] = 1;
			dump_pps(pps, picture->index);
			roundtrip_pps(pps, sps);
		}

		dump_slice(picture, sps, pps);

		/*
		 * The marking and bumping before the picture (C.5.2.2).  An end of
		 * sequence before it needs nothing more: the picture after it is an
		 * IRAP picture with NoRaslOutputFlag 1, whose step 1 outputs the
		 * prior pictures by bumping or, with NoOutputOfPriorPicsFlag 1 (a CRA
		 * picture, or no_output_of_prior_pics_flag), empties them without
		 * output, as the reference decoder does; emptying the buffer with
		 * output first (h265_dpb_flush) would output pictures C.5.2.2 drops.
		 */
		error = h265_dpb_begin(&dpb, sps, &picture->rps, picture->irap != 0 && picture->no_rasl_output != 0,
		    picture->no_output_of_prior_pics, &plan);
		if (error != 0) {
			printf("@DPB error begin %d %llu\n", error, (unsigned long long)picture->index);
			dpb_errors++;
			if (error == EINVAL)
				print_outputs(plan.output, plan.output_count, &dpb);
			continue;
		}

		print_outputs(plan.output, plan.output_count, &dpb);
		if (plan.missing != 0U) {
			printf("@DPB missing %u %llu\n", plan.missing, (unsigned long long)picture->index);
			missing += plan.missing;
		}

		if (plan.discarded != 0U) {
			printf("@DPB discarded %u %llu\n", plan.discarded, (unsigned long long)picture->index);
			discarded += plan.discarded;
		}

		if (plan.evicted != 0U) {
			printf("@DPB evicted %u %llu\n", plan.evicted, (unsigned long long)picture->index);
			evicted += plan.evicted;
		}

		/* The slot, the StdVideo picture information, then the store after the "decode" (C.5.2.3). */
		error = h265_dpb_take_slot(&dpb, &plan);
		if (error != 0) {
			printf("@DPB error take_slot %d %llu\n", error, (unsigned long long)picture->index);
			dpb_errors++;
			h265_dpb_abort(&dpb);
			continue;
		}

		check_picture(picture, &plan);
		error = h265_dpb_end(&dpb, &plan, picture->poc, picture->pic_output, picture->index, output, &count);
		if (error != 0) {
			printf("@DPB error end %d %llu\n", error, (unsigned long long)picture->index);
			dpb_errors++;
			h265_dpb_abort(&dpb);
			continue;
		}

		print_outputs(output, count, &dpb);
	}

	/* The end of the stream: the rest in output order. */
	error = h265_dpb_flush(&dpb, output, &count);
	if (error != 0) {
		printf("@DPB error flush %d\n", error);
		dpb_errors++;
	}

	print_outputs(output, count, &dpb);

	/* The counts. */
	printf("@COUNT returned %llu\n", (unsigned long long)stream->returned);
	printf("@COUNT before_irap %llu\n", (unsigned long long)stream->counts.before_irap);
	printf("@COUNT rasl_skipped %llu\n", (unsigned long long)stream->counts.rasl_skipped);
	printf("@COUNT reserved_type %llu\n", (unsigned long long)stream->counts.reserved_type);
	printf("@COUNT other_layer %llu\n", (unsigned long long)stream->counts.other_layer);
	printf("@COUNT refused_sets %llu\n", (unsigned long long)stream->counts.refused_sets);
	printf("@COUNT broken_pictures %llu\n", (unsigned long long)stream->counts.broken_pictures);
	printf("@COUNT missing %lu\n", missing);
	printf("@COUNT discarded %lu\n", discarded);
	printf("@COUNT evicted %lu\n", evicted);
	printf("@COUNT dpb_errors %lu\n", dpb_errors);
	printf("@COUNT std_checks %lu\n", std_checks);
	printf("@COUNT std_failures %lu\n", std_failures);
	if (stream->reason != NULL)
		printf("@COUNT last_reason %s\n", stream->reason);

	/* The reader's memory. */
	h265_stream_destroy(stream);
	free(picture);
	free(stream);
	free(data);
	return 0;
}
