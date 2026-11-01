/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Tests of h265-std: the StdVideoH265 structures made from the parse.
 *
 * The inputs (h265sw_vps, h265sw_sps, h265sw_pps, h265_picture and
 * h265_dpb_plan) are filled by hand in this file, so no other module of
 * the parse is needed.  Every expected value is written out here from the
 * minus and difference forms of the syntax of H.265 (7.3.2.1, 7.3.2.2,
 * 7.3.2.3, E.2.1, E.2.2), from the derivation of the short-term sets
 * (7-61 to 7-71, solved by hand for the syntax elements), from the
 * general_level_idc values (30 times the level number of Table A.8, 7.4.4)
 * and from the Vulkan Video structure definitions; never from the code
 * under test.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "userland/desktop/libmedia/h265-std.h"

/* The number of random inputs of the robustness checks. */
#define RANDOM_INPUTS 3000U

/* The number of general_level_idc values of the level table, and the Vulkan level of each. */
#define LEVEL_CASES 44U

/* The state of the pseudo-random generator, fixed so a failure repeats. */
static uint32_t random_state = 0x2545F491U;

/* The inputs and outputs, too large for the stack of a check. */
static struct h265sw_vps vps_in;
static struct h265_std_vps vps_out;
static struct h265sw_sps sps_in;
static struct h265_std_sps sps_out;
static struct h265sw_pps pps_in;
static struct h265_std_pps pps_out;
static struct h265_picture picture_in;
static struct h265_dpb_plan plan_in;

/* One level case: a general_level_idc and the Vulkan level it maps to. */
struct level_case {
	unsigned idc;
	StdVideoH265LevelIdc level;
};

/*
 * The level cases: the thirteen values of Table A.8 (30 times 1, 2, 2.1,
 * 3, 3.1, 4, 4.1, 5, 5.1, 5.2, 6, 6.1, 6.2), each value just below and just
 * above a level (which goes to the next level at or above), 0, and the
 * values above 186 (6.2).
 */
static const struct level_case level_cases[LEVEL_CASES] = {
	{30U, STD_VIDEO_H265_LEVEL_IDC_1_0},
	{60U, STD_VIDEO_H265_LEVEL_IDC_2_0},
	{63U, STD_VIDEO_H265_LEVEL_IDC_2_1},
	{90U, STD_VIDEO_H265_LEVEL_IDC_3_0},
	{93U, STD_VIDEO_H265_LEVEL_IDC_3_1},
	{120U, STD_VIDEO_H265_LEVEL_IDC_4_0},
	{123U, STD_VIDEO_H265_LEVEL_IDC_4_1},
	{150U, STD_VIDEO_H265_LEVEL_IDC_5_0},
	{153U, STD_VIDEO_H265_LEVEL_IDC_5_1},
	{156U, STD_VIDEO_H265_LEVEL_IDC_5_2},
	{180U, STD_VIDEO_H265_LEVEL_IDC_6_0},
	{183U, STD_VIDEO_H265_LEVEL_IDC_6_1},
	{186U, STD_VIDEO_H265_LEVEL_IDC_6_2},
	{0U, STD_VIDEO_H265_LEVEL_IDC_1_0},
	{1U, STD_VIDEO_H265_LEVEL_IDC_1_0},
	{29U, STD_VIDEO_H265_LEVEL_IDC_1_0},
	{31U, STD_VIDEO_H265_LEVEL_IDC_2_0},
	{59U, STD_VIDEO_H265_LEVEL_IDC_2_0},
	{61U, STD_VIDEO_H265_LEVEL_IDC_2_1},
	{62U, STD_VIDEO_H265_LEVEL_IDC_2_1},
	{64U, STD_VIDEO_H265_LEVEL_IDC_3_0},
	{89U, STD_VIDEO_H265_LEVEL_IDC_3_0},
	{91U, STD_VIDEO_H265_LEVEL_IDC_3_1},
	{92U, STD_VIDEO_H265_LEVEL_IDC_3_1},
	{94U, STD_VIDEO_H265_LEVEL_IDC_4_0},
	{100U, STD_VIDEO_H265_LEVEL_IDC_4_0},
	{119U, STD_VIDEO_H265_LEVEL_IDC_4_0},
	{121U, STD_VIDEO_H265_LEVEL_IDC_4_1},
	{124U, STD_VIDEO_H265_LEVEL_IDC_5_0},
	{149U, STD_VIDEO_H265_LEVEL_IDC_5_0},
	{151U, STD_VIDEO_H265_LEVEL_IDC_5_1},
	{154U, STD_VIDEO_H265_LEVEL_IDC_5_2},
	{155U, STD_VIDEO_H265_LEVEL_IDC_5_2},
	{157U, STD_VIDEO_H265_LEVEL_IDC_6_0},
	{179U, STD_VIDEO_H265_LEVEL_IDC_6_0},
	{181U, STD_VIDEO_H265_LEVEL_IDC_6_1},
	{182U, STD_VIDEO_H265_LEVEL_IDC_6_1},
	{184U, STD_VIDEO_H265_LEVEL_IDC_6_2},
	{185U, STD_VIDEO_H265_LEVEL_IDC_6_2},
	{187U, STD_VIDEO_H265_LEVEL_IDC_6_2},
	{189U, STD_VIDEO_H265_LEVEL_IDC_6_2},
	{255U, STD_VIDEO_H265_LEVEL_IDC_6_2},
	{256U, STD_VIDEO_H265_LEVEL_IDC_6_2},
	{0xFFFFFFFFU, STD_VIDEO_H265_LEVEL_IDC_6_2}
};

/* Static function declarations. */
static uint32_t random_next(void);
static void random_fill(void *memory, size_t size);
static void ptl_fill(struct h265sw_ptl *ptl);
static void hrd_fill(struct h265sw_hrd *hrd);
static void scaling_fill(struct h265sw_scaling_list *list, unsigned seed);
static void vui_fill(struct h265sw_vui *vui);
static void sps_fill(struct h265sw_sps *sps);
static void pps_fill(struct h265sw_pps *pps);
static void sub_layer_hrd_check(const StdVideoH265SubLayerHrdParameters *out, unsigned i, int vcl);
static void hrd_check(const StdVideoH265HrdParameters *out, const StdVideoH265SubLayerHrdParameters *nal, const StdVideoH265SubLayerHrdParameters *vcl, int nal_present, int vcl_present);
static void scaling_check(const StdVideoH265ScalingLists *out, const struct h265sw_scaling_list *in);
static void test_level(void);
static void test_profile(void);
static void test_ptl_flags(void);
static void test_hrd(void);
static void test_vps(void);
static void test_sps_values(void);
static void test_sps_flags(void);
static void test_sps_pointers(void);
static void test_vui(void);
static void test_scaling(void);
static void test_short_term_sets(void);
static void test_short_term_limits(void);
static void test_pps_values(void);
static void test_pps_flags(void);
static void test_pps_tiles(void);
static void test_picture(void);
static void test_picture_lists(void);
static void test_references(void);
static void test_random(void);

/* Steps the pseudo-random generator. */
static uint32_t
random_next(void)
{
	/* A 32-bit xorshift step; the sequence is the same on every run. */
	random_state ^= random_state << 13;
	random_state ^= random_state >> 17;
	random_state ^= random_state << 5;

	return random_state;
}

/* Fills memory with pseudo-random bytes. */
static void
random_fill(
	void *memory,
	size_t size)
{
	unsigned char *bytes;
	size_t i;

	/* Writes one byte of the generator's output for every byte of memory. */
	bytes = (unsigned char *)memory;
	for (i = 0U; i < size; i++)
		bytes[i] = (unsigned char)(random_next() >> 11);
}

/* Fills a profile, tier and level with the values of the tests below (Main 10, tier 1, level 4.1). */
static void
ptl_fill(
	struct h265sw_ptl *ptl)
{
	/* Writes the profile, tier, source flags and level. */
	memset(ptl, 0, sizeof(*ptl));
	ptl->profile_idc = 2U;
	ptl->compatibility = (1U << 2) | (1U << 3);
	ptl->tier = 1U;
	ptl->progressive = 1;
	ptl->interlaced = 0;
	ptl->non_packed = 1;
	ptl->frame_only = 0;
	ptl->level_idc = 123U;
}

/*
 * Fills an HRD parameters structure with distinct values: the per-sub-layer
 * flags have different patterns, the CPB values depend on the sub-layer
 * and the CPB index, and both HRD kinds are present.
 */
static void
hrd_fill(
	struct h265sw_hrd *hrd)
{
	unsigned i;
	unsigned j;

	/* Clears the structure and writes the common part. */
	memset(hrd, 0, sizeof(*hrd));
	hrd->nal_hrd_present = 1;
	hrd->vcl_hrd_present = 1;
	hrd->sub_pic_hrd_params_present = 1;
	hrd->tick_divisor_minus2 = 11U;
	hrd->du_cpb_removal_delay_increment_length_minus1 = 12U;
	hrd->sub_pic_cpb_params_in_pic_timing_sei = 1;
	hrd->dpb_output_delay_du_length_minus1 = 13U;
	hrd->bit_rate_scale = 4U;
	hrd->cpb_size_scale = 5U;
	hrd->cpb_size_du_scale = 6U;
	hrd->initial_cpb_removal_delay_length_minus1 = 21U;
	hrd->au_cpb_removal_delay_length_minus1 = 22U;
	hrd->dpb_output_delay_length_minus1 = 24U;

	/*
	 * The per-sub-layer flags: fixed_pic_rate_general 1,0,1,0,0,0,1 (bits
	 * 0, 2 and 6), fixed_pic_rate_within_cvs 0,1,1,0,0,1,0 (bits 1, 2 and
	 * 5), low_delay_hrd 1,1,0,0,0,0,1 (bits 0, 1 and 6).  The flags are
	 * stored as 1 here, and as 2 for one to show any non-zero value counts.
	 */
	hrd->fixed_pic_rate_general[0] = 1U;
	hrd->fixed_pic_rate_general[2] = 1U;
	hrd->fixed_pic_rate_general[6] = 1U;
	hrd->fixed_pic_rate_within_cvs[1] = 1U;
	hrd->fixed_pic_rate_within_cvs[2] = 1U;
	hrd->fixed_pic_rate_within_cvs[5] = 2U;
	hrd->low_delay_hrd[0] = 1U;
	hrd->low_delay_hrd[1] = 1U;
	hrd->low_delay_hrd[6] = 1U;

	/* Writes the CPB count and the elemental duration of each sub-layer. */
	for (i = 0U; i < H265SW_MAX_SUB_LAYERS; i++) {
		hrd->cpb_cnt_minus1[i] = (uint8_t)(i + 3U);
		hrd->elemental_duration_in_tc_minus1[i] = (uint16_t)(100U + 2U * i);
	}

	/* Writes distinct CPB values for the NAL and the VCL parameters of each sub-layer. */
	for (i = 0U; i < H265SW_MAX_SUB_LAYERS; i++) {
		for (j = 0U; j < H265SW_MAX_CPB; j++) {
			hrd->nal[i].bit_rate_value_minus1[j] = 1000000U * (i + 1U) + 100U * j + 1U;
			hrd->nal[i].cpb_size_value_minus1[j] = 1000000U * (i + 1U) + 100U * j + 2U;
			hrd->nal[i].cpb_size_du_value_minus1[j] = 1000000U * (i + 1U) + 100U * j + 3U;
			hrd->nal[i].bit_rate_du_value_minus1[j] = 1000000U * (i + 1U) + 100U * j + 4U;
			hrd->vcl[i].bit_rate_value_minus1[j] = 2000000U * (i + 1U) + 100U * j + 5U;
			hrd->vcl[i].cpb_size_value_minus1[j] = 2000000U * (i + 1U) + 100U * j + 6U;
			hrd->vcl[i].cpb_size_du_value_minus1[j] = 2000000U * (i + 1U) + 100U * j + 7U;
			hrd->vcl[i].bit_rate_du_value_minus1[j] = 2000000U * (i + 1U) + 100U * j + 8U;
		}

		hrd->nal[i].cbr_flags = 0xA5000000U + i;
		hrd->vcl[i].cbr_flags = 0x5A000000U + i;
	}
}

/*
 * Fills scaling lists with distinct values: entry i of list [sizeId][matrixId]
 * is a function of the three, the 32x32 lists exist for matrixId 0 and 3
 * only (the others are 0, as the parse leaves them), and the DC values are
 * distinct.
 */
static void
scaling_fill(
	struct h265sw_scaling_list *list,
	unsigned seed)
{
	unsigned size_id;
	unsigned matrix_id;
	unsigned i;
	unsigned length;

	/* Clears the lists and writes every list the syntax codes. */
	memset(list, 0, sizeof(*list));
	for (size_id = 0U; size_id < 4U; size_id++) {
		for (matrix_id = 0U; matrix_id < 6U; matrix_id++) {
			/* Skips the matrices of the 32x32 size that the syntax does not code. */
			if (size_id == 3U && matrix_id != 0U && matrix_id != 3U)
				continue;

			length = 64U;
			if (size_id == 0U)
				length = 16U;

			for (i = 0U; i < length; i++)
				list->list[size_id][matrix_id][i] = (uint8_t)(1U + ((seed + size_id * 37U + matrix_id * 11U + i * 3U) % 250U));
		}
	}

	/* Writes the DC values of the 16x16 lists and of the two coded 32x32 lists. */
	for (matrix_id = 0U; matrix_id < 6U; matrix_id++)
		list->dc[0][matrix_id] = (uint8_t)(100U + seed + matrix_id);
	list->dc[1][0] = (uint8_t)(200U + seed);
	list->dc[1][3] = (uint8_t)(203U + seed);
}

/* Fills a video usability information structure with distinct values and every flag set. */
static void
vui_fill(
	struct h265sw_vui *vui)
{
	/* Clears the structure and writes the aspect ratio: EXTENDED_SAR 4:3, with a derived ratio of 8:6. */
	memset(vui, 0, sizeof(*vui));
	vui->aspect_ratio_present = 1;
	vui->aspect_ratio_idc = 255U;
	vui->signalled_sar_width = 4U;
	vui->signalled_sar_height = 3U;
	vui->sar_width = 8U;
	vui->sar_height = 6U;

	/* Writes the flags and the fields of the signal type and the chroma location. */
	vui->overscan_present = 1;
	vui->overscan_appropriate = 1;
	vui->signal_present = 1;
	vui->video_format = 3U;
	vui->full_range = 1;
	vui->colour_present = 1;
	vui->colour_primaries = 9U;
	vui->transfer_characteristics = 16U;
	vui->matrix_coeffs = 9U;
	vui->chroma_loc_present = 1;
	vui->chroma_sample_loc_type_top_field = 2U;
	vui->chroma_sample_loc_type_bottom_field = 4U;
	vui->neutral_chroma_indication = 1;
	vui->field_seq = 1;
	vui->frame_field_info_present = 1;

	/* Writes the default display window. */
	vui->default_display_window = 1;
	vui->def_disp_win_left_offset = 11U;
	vui->def_disp_win_right_offset = 12U;
	vui->def_disp_win_top_offset = 13U;
	vui->def_disp_win_bottom_offset = 16888U;

	/* Writes the timing information and the bitstream restriction. */
	vui->timing_present = 1;
	vui->num_units_in_tick = 1001U;
	vui->time_scale = 60000U;
	vui->poc_proportional_to_timing = 1;
	vui->num_ticks_poc_diff_one_minus1 = 0xFFFFFFFEU;
	vui->hrd_present = 1;
	hrd_fill(&vui->hrd);
	vui->bitstream_restriction = 1;
	vui->tiles_fixed_structure = 1;
	vui->motion_vectors_over_pic_boundaries = 1;
	vui->restricted_ref_pic_lists = 1;
	vui->min_spatial_segmentation_idc = 4095U;
	vui->max_bytes_per_pic_denom = 7U;
	vui->max_bits_per_min_cu_denom = 8U;
	vui->log2_max_mv_length_horizontal = 14U;
	vui->log2_max_mv_length_vertical = 13U;
}

/*
 * Fills an SPS with distinct values (a 1920x1088 10-bit 4:2:0 sequence with
 * three sub-layers, PCM, a conformance window, two long-term pictures and
 * no short-term sets) and the optional parts off; the tests switch them on.
 */
static void
sps_fill(
	struct h265sw_sps *sps)
{
	unsigned i;

	/* Clears the SPS and writes the identifiers and the picture format. */
	memset(sps, 0, sizeof(*sps));
	sps->vps_id = 9U;
	sps->max_sub_layers_minus1 = 2U;
	ptl_fill(&sps->ptl);
	sps->id = 14U;
	sps->chroma_format_idc = 1U;
	sps->width = 1920U;
	sps->height = 1088U;
	sps->bit_depth_luma = 10U;
	sps->bit_depth_chroma = 12U;
	sps->log2_max_poc_lsb = 12U;

	/* Writes the conformance window offsets: left, right, top, bottom. */
	sps->conformance_window = 1;
	sps->conf_win_offset[0] = 1U;
	sps->conf_win_offset[1] = 2U;
	sps->conf_win_offset[2] = 3U;
	sps->conf_win_offset[3] = 4U;

	/* Writes the buffering limits of the seven sub-layers; the three the SPS has, and nonzero values above to show all seven are copied. */
	sps->sub_layer_ordering_info_present = 1;
	for (i = 0U; i < H265SW_MAX_SUB_LAYERS; i++) {
		sps->max_dec_pic_buffering_minus1[i] = 3U + 2U * i;
		sps->max_num_reorder_pics[i] = i;
		sps->max_latency_increase_plus1[i] = 1000U * (i + 1U);
	}

	sps->max_latency_increase_plus1[6] = 0xFFFFFFFEU;

	/* Writes the block sizes: minimum coding block 16, CTB 64, transform blocks 8 to 32. */
	sps->min_cb_log2 = 4U;
	sps->ctb_log2 = 6U;
	sps->min_tb_log2 = 3U;
	sps->max_tb_log2 = 5U;
	sps->max_th_depth_inter = 2U;
	sps->max_th_depth_intra = 3U;

	/* Writes the PCM sizes: sample bit depths 7 and 5, coding block sizes 16 to 32. */
	sps->pcm = 1;
	sps->pcm_bit_depth_luma = 7U;
	sps->pcm_bit_depth_chroma = 5U;
	sps->pcm_min_log2 = 4U;
	sps->pcm_max_log2 = 5U;
	sps->pcm_loop_filter_disabled = 1;

	/* Writes the long-term pictures of the SPS: entry 0 used, entry 1 not, entry 2 used. */
	sps->long_term_present = 1;
	sps->num_lt_sps = 3U;
	sps->lt_poc_lsb[0] = 0x111U;
	sps->lt_poc_lsb[1] = 0x222U;
	sps->lt_poc_lsb[2] = 0xFFFFU;
	sps->lt_used[0] = 1U;
	sps->lt_used[1] = 0U;
	sps->lt_used[2] = 1U;
}

/* Fills a PPS with distinct values that name the SPS of sps_fill and the optional parts off. */
static void
pps_fill(
	struct h265sw_pps *pps)
{
	/* Clears the PPS and writes the identifiers and the slice header defaults. */
	memset(pps, 0, sizeof(*pps));
	pps->id = 37U;
	pps->sps_id = 14U;
	pps->num_extra_slice_header_bits = 5U;
	pps->num_ref_idx_default[0] = 15U;
	pps->num_ref_idx_default[1] = 2U;

	/* Writes the quantization and deblocking fields. */
	pps->init_qp = 40;
	pps->diff_cu_qp_delta_depth = 3U;
	pps->cb_qp_offset = -12;
	pps->cr_qp_offset = 11;
	pps->beta_offset_div2 = -6;
	pps->tc_offset_div2 = 5;
	pps->log2_parallel_merge_level = 6U;

	/* Writes the single tile of a picture without tiles. */
	pps->tile_columns = 1U;
	pps->tile_rows = 1U;
}

/* Checks one sub-layer's CPB values against the values hrd_fill wrote. */
static void
sub_layer_hrd_check(
	const StdVideoH265SubLayerHrdParameters *out,
	unsigned i,
	int vcl)
{
	unsigned base;
	unsigned j;
	uint32_t cbr;
	int good;

	/* Chooses the values of the NAL or the VCL parameters. */
	base = 1000000U * (i + 1U);
	cbr = 0xA5000000U + i;
	if (vcl != 0) {
		base = 2000000U * (i + 1U);
		cbr = 0x5A000000U + i;
	}

	/* Compares every CPB specification's four values. */
	good = 1;
	for (j = 0U; j < H265SW_MAX_CPB; j++) {
		unsigned add = 1U;

		if (vcl != 0)
			add = 5U;

		if (out->bit_rate_value_minus1[j] != base + 100U * j + add)
			good = 0;
		if (out->cpb_size_value_minus1[j] != base + 100U * j + add + 1U)
			good = 0;
		if (out->cpb_size_du_value_minus1[j] != base + 100U * j + add + 2U)
			good = 0;
		if (out->bit_rate_du_value_minus1[j] != base + 100U * j + add + 3U)
			good = 0;
	}

	CHECK(good);
	CHECK(out->cbr_flag == cbr);
}

/* Checks an HRD parameters structure and its sub-layer arrays against the values hrd_fill wrote. */
static void
hrd_check(
	const StdVideoH265HrdParameters *out,
	const StdVideoH265SubLayerHrdParameters *nal,
	const StdVideoH265SubLayerHrdParameters *vcl,
	int nal_present,
	int vcl_present)
{
	unsigned i;

	/* Compares the flags: the bit masks are the patterns hrd_fill wrote. */
	CHECK(out->flags.nal_hrd_parameters_present_flag == (unsigned)nal_present);
	CHECK(out->flags.vcl_hrd_parameters_present_flag == (unsigned)vcl_present);
	CHECK(out->flags.sub_pic_hrd_params_present_flag == 1U);
	CHECK(out->flags.sub_pic_cpb_params_in_pic_timing_sei_flag == 1U);
	CHECK(out->flags.fixed_pic_rate_general_flag == 0x45U);
	CHECK(out->flags.fixed_pic_rate_within_cvs_flag == 0x26U);
	CHECK(out->flags.low_delay_hrd_flag == 0x43U);

	/* Compares the common fields. */
	CHECK(out->tick_divisor_minus2 == 11U);
	CHECK(out->du_cpb_removal_delay_increment_length_minus1 == 12U);
	CHECK(out->dpb_output_delay_du_length_minus1 == 13U);
	CHECK(out->bit_rate_scale == 4U);
	CHECK(out->cpb_size_scale == 5U);
	CHECK(out->cpb_size_du_scale == 6U);
	CHECK(out->initial_cpb_removal_delay_length_minus1 == 21U);
	CHECK(out->au_cpb_removal_delay_length_minus1 == 22U);
	CHECK(out->dpb_output_delay_length_minus1 == 24U);
	CHECK(out->reserved[0] == 0U);
	CHECK(out->reserved[1] == 0U);
	CHECK(out->reserved[2] == 0U);

	/* Compares the per-sub-layer values. */
	for (i = 0U; i < 7U; i++) {
		CHECK(out->cpb_cnt_minus1[i] == i + 3U);
		CHECK(out->elemental_duration_in_tc_minus1[i] == 100U + 2U * i);
	}

	/* Compares the pointers and the sub-layer arrays the present flags ask for. */
	if (nal_present != 0) {
		CHECK(out->pSubLayerHrdParametersNal == nal);
		for (i = 0U; i < 7U; i++)
			sub_layer_hrd_check(&nal[i], i, 0);
	} else {
		CHECK(out->pSubLayerHrdParametersNal == NULL);
	}

	if (vcl_present != 0) {
		CHECK(out->pSubLayerHrdParametersVcl == vcl);
		for (i = 0U; i < 7U; i++)
			sub_layer_hrd_check(&vcl[i], i, 1);
	} else {
		CHECK(out->pSubLayerHrdParametersVcl == NULL);
	}
}

/* Checks Vulkan scaling lists against the parse's lists: the 32x32 entry n is the syntax's matrixId 3 * n. */
static void
scaling_check(
	const StdVideoH265ScalingLists *out,
	const struct h265sw_scaling_list *in)
{
	unsigned m;
	unsigned i;
	int good;

	/* Compares the 4x4 lists (16 entries). */
	good = 1;
	for (m = 0U; m < 6U; m++) {
		for (i = 0U; i < 16U; i++) {
			if (out->ScalingList4x4[m][i] != in->list[0][m][i])
				good = 0;
		}
	}

	CHECK(good);

	/* Compares the 8x8 lists and the 16x16 lists (64 entries each) and the 16x16 DC values. */
	good = 1;
	for (m = 0U; m < 6U; m++) {
		for (i = 0U; i < 64U; i++) {
			if (out->ScalingList8x8[m][i] != in->list[1][m][i])
				good = 0;
			if (out->ScalingList16x16[m][i] != in->list[2][m][i])
				good = 0;
		}

		if (out->ScalingListDCCoef16x16[m] != in->dc[0][m])
			good = 0;
	}

	CHECK(good);

	/* Compares the two 32x32 lists with the syntax's matrices 0 and 3, and the DC values. */
	good = 1;
	for (i = 0U; i < 64U; i++) {
		if (out->ScalingList32x32[0][i] != in->list[3][0][i])
			good = 0;
		if (out->ScalingList32x32[1][i] != in->list[3][3][i])
			good = 0;
	}

	CHECK(good);
	CHECK(out->ScalingListDCCoef32x32[0] == in->dc[1][0]);
	CHECK(out->ScalingListDCCoef32x32[1] == in->dc[1][3]);
}

/* Tests the level mapping through h265_std_ptl: the thirteen levels, the values between them, 0 and the values above 6.2. */
static void
test_level(void)
{
	struct h265sw_ptl ptl;
	StdVideoH265ProfileTierLevel out;
	unsigned i;
	int error;

	/* Maps each level value with a valid profile. */
	for (i = 0U; i < LEVEL_CASES; i++) {
		memset(&ptl, 0, sizeof(ptl));
		ptl.profile_idc = 1U;
		ptl.level_idc = level_cases[i].idc;

		error = h265_std_ptl(&ptl, &out);
		CHECK(error == 0);
		CHECK(out.general_level_idc == level_cases[i].level);
	}
}

/* Tests the profile choice: the five signalled profiles, the compatibility flags, and the refusal. */
static void
test_profile(void)
{
	struct h265sw_ptl ptl;
	StdVideoH265ProfileTierLevel out;
	StdVideoH265ProfileTierLevel untouched;
	unsigned char pattern[sizeof(out)];
	unsigned i;
	int error;

	/* The signalled profiles that the Vulkan enumeration names are copied, whatever the flags say. */
	memset(&ptl, 0, sizeof(ptl));
	ptl.profile_idc = 1U;
	error = h265_std_ptl(&ptl, &out);
	CHECK(error == 0);
	CHECK(out.general_profile_idc == STD_VIDEO_H265_PROFILE_IDC_MAIN);

	ptl.profile_idc = 2U;
	error = h265_std_ptl(&ptl, &out);
	CHECK(error == 0);
	CHECK(out.general_profile_idc == STD_VIDEO_H265_PROFILE_IDC_MAIN_10);

	ptl.profile_idc = 3U;
	error = h265_std_ptl(&ptl, &out);
	CHECK(error == 0);
	CHECK(out.general_profile_idc == STD_VIDEO_H265_PROFILE_IDC_MAIN_STILL_PICTURE);

	ptl.profile_idc = 4U;
	ptl.compatibility = 1U << 1;
	error = h265_std_ptl(&ptl, &out);
	CHECK(error == 0);
	CHECK(out.general_profile_idc == STD_VIDEO_H265_PROFILE_IDC_FORMAT_RANGE_EXTENSIONS);

	ptl.profile_idc = 9U;
	ptl.compatibility = 1U << 3;
	error = h265_std_ptl(&ptl, &out);
	CHECK(error == 0);
	CHECK(out.general_profile_idc == STD_VIDEO_H265_PROFILE_IDC_SCC_EXTENSIONS);

	/* A profile 0 or 5 stream takes the lowest of flags 1, 2 and 3 that is set. */
	ptl.profile_idc = 0U;
	ptl.compatibility = 1U << 1;
	error = h265_std_ptl(&ptl, &out);
	CHECK(error == 0);
	CHECK(out.general_profile_idc == STD_VIDEO_H265_PROFILE_IDC_MAIN);

	ptl.compatibility = 1U << 2;
	error = h265_std_ptl(&ptl, &out);
	CHECK(error == 0);
	CHECK(out.general_profile_idc == STD_VIDEO_H265_PROFILE_IDC_MAIN_10);

	ptl.compatibility = 1U << 3;
	error = h265_std_ptl(&ptl, &out);
	CHECK(error == 0);
	CHECK(out.general_profile_idc == STD_VIDEO_H265_PROFILE_IDC_MAIN_STILL_PICTURE);

	ptl.profile_idc = 5U;
	ptl.compatibility = (1U << 2) | (1U << 3);
	error = h265_std_ptl(&ptl, &out);
	CHECK(error == 0);
	CHECK(out.general_profile_idc == STD_VIDEO_H265_PROFILE_IDC_MAIN_10);

	ptl.compatibility = (1U << 1) | (1U << 2) | (1U << 3);
	error = h265_std_ptl(&ptl, &out);
	CHECK(error == 0);
	CHECK(out.general_profile_idc == STD_VIDEO_H265_PROFILE_IDC_MAIN);

	ptl.compatibility = (1U << 1) | (1U << 3);
	error = h265_std_ptl(&ptl, &out);
	CHECK(error == 0);
	CHECK(out.general_profile_idc == STD_VIDEO_H265_PROFILE_IDC_MAIN);

	ptl.profile_idc = 10U;
	ptl.compatibility = 1U << 2;
	error = h265_std_ptl(&ptl, &out);
	CHECK(error == 0);
	CHECK(out.general_profile_idc == STD_VIDEO_H265_PROFILE_IDC_MAIN_10);

	/* The other flags (0, and 4 to 31) do not choose a profile, and the refusal leaves out as it was. */
	memset(pattern, 0xA5, sizeof(pattern));
	memcpy(&untouched, pattern, sizeof(untouched));

	for (i = 0U; i < 32U; i++) {
		/* Flags 1, 2 and 3 are the usable ones; every other single flag is refused. */
		if (i >= 1U && i <= 3U)
			continue;

		memset(&ptl, 0, sizeof(ptl));
		ptl.profile_idc = 5U;
		ptl.compatibility = (uint32_t)1U << i;
		memcpy(&out, pattern, sizeof(out));
		error = h265_std_ptl(&ptl, &out);
		CHECK(error == ENOTSUP);
		CHECK(memcmp(&out, &untouched, sizeof(out)) == 0);
	}

	memset(&ptl, 0, sizeof(ptl));
	ptl.profile_idc = 0U;
	ptl.compatibility = 0xFFFFFFF1U;
	memcpy(&out, pattern, sizeof(out));
	error = h265_std_ptl(&ptl, &out);
	CHECK(error == ENOTSUP);
	CHECK(memcmp(&out, &untouched, sizeof(out)) == 0);

	ptl.profile_idc = 255U;
	ptl.compatibility = 0U;
	error = h265_std_ptl(&ptl, &out);
	CHECK(error == ENOTSUP);
}

/* Tests the tier and the four source flags one at a time and together. */
static void
test_ptl_flags(void)
{
	struct h265sw_ptl ptl;
	StdVideoH265ProfileTierLevel out;
	StdVideoH265ProfileTierLevel expected;
	unsigned k;
	int error;

	/* Sets one flag at a time and checks that exactly that flag is set in the output. */
	for (k = 0U; k < 5U; k++) {
		memset(&ptl, 0, sizeof(ptl));
		ptl.profile_idc = 1U;
		ptl.level_idc = 93U;

		if (k == 0U)
			ptl.tier = 1U;
		if (k == 1U)
			ptl.progressive = 1;
		if (k == 2U)
			ptl.interlaced = 1;
		if (k == 3U)
			ptl.non_packed = 1;
		if (k == 4U)
			ptl.frame_only = 1;

		error = h265_std_ptl(&ptl, &out);
		CHECK(error == 0);
		CHECK(out.flags.general_tier_flag == (k == 0U));
		CHECK(out.flags.general_progressive_source_flag == (k == 1U));
		CHECK(out.flags.general_interlaced_source_flag == (k == 2U));
		CHECK(out.flags.general_non_packed_constraint_flag == (k == 3U));
		CHECK(out.flags.general_frame_only_constraint_flag == (k == 4U));
		CHECK(out.general_level_idc == STD_VIDEO_H265_LEVEL_IDC_3_1);
	}

	/* The whole output is defined: it equals a cleared structure with the seven fields set. */
	memset(&ptl, 0, sizeof(ptl));
	ptl.profile_idc = 2U;
	ptl.level_idc = 93U;
	ptl.tier = 1U;
	ptl.non_packed = 1;
	memset(&out, 0xCC, sizeof(out));
	error = h265_std_ptl(&ptl, &out);
	CHECK(error == 0);
	memset(&expected, 0, sizeof(expected));
	expected.flags.general_tier_flag = 1U;
	expected.flags.general_non_packed_constraint_flag = 1U;
	expected.general_profile_idc = STD_VIDEO_H265_PROFILE_IDC_MAIN_10;
	expected.general_level_idc = STD_VIDEO_H265_LEVEL_IDC_3_1;
	CHECK(memcmp(&out, &expected, sizeof(out)) == 0);

	/* A flag stored as another non-zero value is still 1. */
	memset(&ptl, 0, sizeof(ptl));
	ptl.profile_idc = 1U;
	ptl.progressive = 7;
	ptl.frame_only = -1;
	error = h265_std_ptl(&ptl, &out);
	CHECK(error == 0);
	CHECK(out.flags.general_progressive_source_flag == 1U);
	CHECK(out.flags.general_frame_only_constraint_flag == 1U);
}

/* Tests the HRD parameters through a VPS: which present flags give which sub-layer arrays. */
static void
test_hrd(void)
{
	unsigned combination;
	int nal;
	int vcl;
	int error;

	/* Makes the VPS with each combination of the NAL and VCL present flags. */
	for (combination = 0U; combination < 4U; combination++) {
		nal = (int)(combination & 1U);
		vcl = (int)((combination >> 1) & 1U);

		memset(&vps_in, 0, sizeof(vps_in));
		ptl_fill(&vps_in.ptl);
		vps_in.num_hrd_parameters = 1U;
		hrd_fill(&vps_in.hrd);
		vps_in.hrd.nal_hrd_present = nal;
		vps_in.hrd.vcl_hrd_present = vcl;

		error = h265_std_vps(&vps_in, &vps_out);
		CHECK(error == 0);
		CHECK(vps_out.vps.pHrdParameters == &vps_out.hrd);
		hrd_check(&vps_out.hrd, vps_out.hrd_nal, vps_out.hrd_vcl, nal, vcl);
	}

	/* The two sub-picture flags are independent. */
	vps_in.hrd.sub_pic_hrd_params_present = 1;
	vps_in.hrd.sub_pic_cpb_params_in_pic_timing_sei = 0;
	error = h265_std_vps(&vps_in, &vps_out);
	CHECK(error == 0);
	CHECK(vps_out.hrd.flags.sub_pic_hrd_params_present_flag == 1U);
	CHECK(vps_out.hrd.flags.sub_pic_cpb_params_in_pic_timing_sei_flag == 0U);

	vps_in.hrd.sub_pic_hrd_params_present = 0;
	vps_in.hrd.sub_pic_cpb_params_in_pic_timing_sei = 1;
	error = h265_std_vps(&vps_in, &vps_out);
	CHECK(error == 0);
	CHECK(vps_out.hrd.flags.sub_pic_hrd_params_present_flag == 0U);
	CHECK(vps_out.hrd.flags.sub_pic_cpb_params_in_pic_timing_sei_flag == 1U);

	/* The sub-layer arrays are not copied when their present flag is 0: the arrays stay cleared. */
	vps_in.hrd.nal_hrd_present = 0;
	vps_in.hrd.vcl_hrd_present = 0;
	error = h265_std_vps(&vps_in, &vps_out);
	CHECK(error == 0);
	CHECK(vps_out.hrd_nal[0].bit_rate_value_minus1[0] == 0U);
	CHECK(vps_out.hrd_vcl[6].cbr_flag == 0U);
}

/* Tests a VPS: every field, the pointers, the optional HRD structure and the refusal. */
static void
test_vps(void)
{
	unsigned i;
	int error;
	struct h265_std_vps untouched;
	unsigned char pattern[sizeof(vps_out)];

	/* Fills a VPS with distinct values; three sub-layers, and values above them to show the seven are copied. */
	memset(&vps_in, 0, sizeof(vps_in));
	vps_in.id = 5U;
	vps_in.base_layer_internal = 1;
	vps_in.base_layer_available = 1;
	vps_in.max_layers_minus1 = 3U;
	vps_in.max_sub_layers_minus1 = 2U;
	vps_in.temporal_id_nesting = 1;
	ptl_fill(&vps_in.ptl);
	vps_in.sub_layer_ordering_info_present = 1;
	for (i = 0U; i < 7U; i++) {
		vps_in.max_dec_pic_buffering_minus1[i] = 3U + 2U * i;
		vps_in.max_num_reorder_pics[i] = i;
		vps_in.max_latency_increase_plus1[i] = 10U * (i + 1U);
	}

	vps_in.max_latency_increase_plus1[6] = 0xFFFFFFFEU;
	vps_in.timing_present = 1;
	vps_in.num_units_in_tick = 1001U;
	vps_in.time_scale = 60000U;
	vps_in.poc_proportional_to_timing = 1;
	vps_in.num_ticks_poc_diff_one_minus1 = 0x12345678U;
	vps_in.num_hrd_parameters = 0U;
	hrd_fill(&vps_in.hrd);

	/* Makes the StdVideo form. */
	memset(&vps_out, 0xCC, sizeof(vps_out));
	error = h265_std_vps(&vps_in, &vps_out);
	CHECK(error == 0);

	/* The flags, the identifiers and the timing are copied. */
	CHECK(vps_out.vps.flags.vps_temporal_id_nesting_flag == 1U);
	CHECK(vps_out.vps.flags.vps_sub_layer_ordering_info_present_flag == 1U);
	CHECK(vps_out.vps.flags.vps_timing_info_present_flag == 1U);
	CHECK(vps_out.vps.flags.vps_poc_proportional_to_timing_flag == 1U);
	CHECK(vps_out.vps.vps_video_parameter_set_id == 5U);
	CHECK(vps_out.vps.vps_max_sub_layers_minus1 == 2U);
	CHECK(vps_out.vps.reserved1 == 0U);
	CHECK(vps_out.vps.reserved2 == 0U);
	CHECK(vps_out.vps.reserved3 == 0U);
	CHECK(vps_out.vps.vps_num_units_in_tick == 1001U);
	CHECK(vps_out.vps.vps_time_scale == 60000U);
	CHECK(vps_out.vps.vps_num_ticks_poc_diff_one_minus1 == 0x12345678U);

	/* The profile, tier and level are copied: Main 10, tier 1, level 4.1. */
	CHECK(vps_out.ptl.general_profile_idc == STD_VIDEO_H265_PROFILE_IDC_MAIN_10);
	CHECK(vps_out.ptl.general_level_idc == STD_VIDEO_H265_LEVEL_IDC_4_1);
	CHECK(vps_out.ptl.flags.general_tier_flag == 1U);
	CHECK(vps_out.ptl.flags.general_progressive_source_flag == 1U);
	CHECK(vps_out.ptl.flags.general_interlaced_source_flag == 0U);
	CHECK(vps_out.ptl.flags.general_non_packed_constraint_flag == 1U);
	CHECK(vps_out.ptl.flags.general_frame_only_constraint_flag == 0U);

	/* All seven sub-layers of the buffering limits are copied. */
	for (i = 0U; i < 7U; i++) {
		CHECK(vps_out.dpb.max_dec_pic_buffering_minus1[i] == 3U + 2U * i);
		CHECK(vps_out.dpb.max_num_reorder_pics[i] == i);
	}

	CHECK(vps_out.dpb.max_latency_increase_plus1[0] == 10U);
	CHECK(vps_out.dpb.max_latency_increase_plus1[1] == 20U);
	CHECK(vps_out.dpb.max_latency_increase_plus1[2] == 30U);
	CHECK(vps_out.dpb.max_latency_increase_plus1[3] == 40U);
	CHECK(vps_out.dpb.max_latency_increase_plus1[4] == 50U);
	CHECK(vps_out.dpb.max_latency_increase_plus1[5] == 60U);
	CHECK(vps_out.dpb.max_latency_increase_plus1[6] == 0xFFFFFFFEU);

	/* The pointers name the structures inside the output; there is no HRD structure without HRD parameters. */
	CHECK(vps_out.vps.pProfileTierLevel == &vps_out.ptl);
	CHECK(vps_out.vps.pDecPicBufMgr == &vps_out.dpb);
	CHECK(vps_out.vps.pHrdParameters == NULL);

	/* With HRD parameters the pointer names the HRD structure. */
	vps_in.num_hrd_parameters = 1U;
	error = h265_std_vps(&vps_in, &vps_out);
	CHECK(error == 0);
	CHECK(vps_out.vps.pHrdParameters == &vps_out.hrd);

	vps_in.num_hrd_parameters = 7U;
	error = h265_std_vps(&vps_in, &vps_out);
	CHECK(error == 0);
	CHECK(vps_out.vps.pHrdParameters == &vps_out.hrd);

	/* The flags are written as 0 or 1 whatever non-zero value the parse kept. */
	vps_in.temporal_id_nesting = 9;
	error = h265_std_vps(&vps_in, &vps_out);
	CHECK(error == 0);
	CHECK(vps_out.vps.flags.vps_temporal_id_nesting_flag == 1U);

	/* Each flag alone sets only its bit. */
	vps_in.temporal_id_nesting = 1;
	vps_in.sub_layer_ordering_info_present = 0;
	vps_in.timing_present = 0;
	vps_in.poc_proportional_to_timing = 0;
	error = h265_std_vps(&vps_in, &vps_out);
	CHECK(error == 0);
	CHECK(vps_out.vps.flags.vps_temporal_id_nesting_flag == 1U);
	CHECK(vps_out.vps.flags.vps_sub_layer_ordering_info_present_flag == 0U);
	CHECK(vps_out.vps.flags.vps_timing_info_present_flag == 0U);
	CHECK(vps_out.vps.flags.vps_poc_proportional_to_timing_flag == 0U);

	vps_in.temporal_id_nesting = 0;
	vps_in.sub_layer_ordering_info_present = 0;
	vps_in.timing_present = 0;
	vps_in.poc_proportional_to_timing = 1;
	error = h265_std_vps(&vps_in, &vps_out);
	CHECK(error == 0);
	CHECK(vps_out.vps.flags.vps_temporal_id_nesting_flag == 0U);
	CHECK(vps_out.vps.flags.vps_sub_layer_ordering_info_present_flag == 0U);
	CHECK(vps_out.vps.flags.vps_timing_info_present_flag == 0U);
	CHECK(vps_out.vps.flags.vps_poc_proportional_to_timing_flag == 1U);

	vps_in.poc_proportional_to_timing = 0;
	vps_in.timing_present = 1;
	error = h265_std_vps(&vps_in, &vps_out);
	CHECK(error == 0);
	CHECK(vps_out.vps.flags.vps_timing_info_present_flag == 1U);
	CHECK(vps_out.vps.flags.vps_poc_proportional_to_timing_flag == 0U);

	vps_in.timing_present = 0;
	vps_in.sub_layer_ordering_info_present = 1;
	error = h265_std_vps(&vps_in, &vps_out);
	CHECK(error == 0);
	CHECK(vps_out.vps.flags.vps_sub_layer_ordering_info_present_flag == 1U);
	CHECK(vps_out.vps.flags.vps_timing_info_present_flag == 0U);

	/* A profile that is refused leaves the output as it was. */
	vps_in.ptl.profile_idc = 0U;
	vps_in.ptl.compatibility = 0U;
	memset(pattern, 0x5A, sizeof(pattern));
	memcpy(&vps_out, pattern, sizeof(vps_out));
	memcpy(&untouched, pattern, sizeof(untouched));
	error = h265_std_vps(&vps_in, &vps_out);
	CHECK(error == ENOTSUP);
	CHECK(memcmp(&vps_out, &untouched, sizeof(vps_out)) == 0);
}

/* Tests the values of an SPS: sizes, ids, minus and difference forms, PCM, conformance window, buffering. */
static void
test_sps_values(void)
{
	unsigned i;
	int error;

	/* Makes the SPS with PCM on. */
	sps_fill(&sps_in);
	memset(&sps_out, 0xCC, sizeof(sps_out));
	error = h265_std_sps(&sps_in, &sps_out);
	CHECK(error == 0);

	/* The picture format and the identifiers. */
	CHECK(sps_out.sps.chroma_format_idc == STD_VIDEO_H265_CHROMA_FORMAT_IDC_420);
	CHECK(sps_out.sps.pic_width_in_luma_samples == 1920U);
	CHECK(sps_out.sps.pic_height_in_luma_samples == 1088U);
	CHECK(sps_out.sps.sps_video_parameter_set_id == 9U);
	CHECK(sps_out.sps.sps_max_sub_layers_minus1 == 2U);
	CHECK(sps_out.sps.sps_seq_parameter_set_id == 14U);

	/* The bit depths and the order count length in the minus forms: 10 - 8, 12 - 8, 12 - 4. */
	CHECK(sps_out.sps.bit_depth_luma_minus8 == 2U);
	CHECK(sps_out.sps.bit_depth_chroma_minus8 == 4U);
	CHECK(sps_out.sps.log2_max_pic_order_cnt_lsb_minus4 == 8U);

	/* The block sizes: minimum coding block 2^4 (4 - 3), CTB 2^6 (6 - 4 = 2), transform blocks 2^3 (3 - 2) to 2^5 (5 - 3 = 2). */
	CHECK(sps_out.sps.log2_min_luma_coding_block_size_minus3 == 1U);
	CHECK(sps_out.sps.log2_diff_max_min_luma_coding_block_size == 2U);
	CHECK(sps_out.sps.log2_min_luma_transform_block_size_minus2 == 1U);
	CHECK(sps_out.sps.log2_diff_max_min_luma_transform_block_size == 2U);
	CHECK(sps_out.sps.max_transform_hierarchy_depth_inter == 2U);
	CHECK(sps_out.sps.max_transform_hierarchy_depth_intra == 3U);

	/* The counts of the sets of the SPS. */
	CHECK(sps_out.sps.num_short_term_ref_pic_sets == 0U);
	CHECK(sps_out.sps.num_long_term_ref_pics_sps == 3U);

	/* The PCM fields in the minus forms: 7 - 1, 5 - 1, 4 - 3, 5 - 4. */
	CHECK(sps_out.sps.pcm_sample_bit_depth_luma_minus1 == 6U);
	CHECK(sps_out.sps.pcm_sample_bit_depth_chroma_minus1 == 4U);
	CHECK(sps_out.sps.log2_min_pcm_luma_coding_block_size_minus3 == 1U);
	CHECK(sps_out.sps.log2_diff_max_min_pcm_luma_coding_block_size == 1U);

	/* The conformance window as signalled. */
	CHECK(sps_out.sps.conf_win_left_offset == 1U);
	CHECK(sps_out.sps.conf_win_right_offset == 2U);
	CHECK(sps_out.sps.conf_win_top_offset == 3U);
	CHECK(sps_out.sps.conf_win_bottom_offset == 4U);

	/* The reserved fields and the screen content fields are 0. */
	CHECK(sps_out.sps.reserved1 == 0U);
	CHECK(sps_out.sps.reserved2 == 0U);
	CHECK(sps_out.sps.palette_max_size == 0U);
	CHECK(sps_out.sps.delta_palette_max_predictor_size == 0U);
	CHECK(sps_out.sps.motion_vector_resolution_control_idc == 0U);
	CHECK(sps_out.sps.sps_num_palette_predictor_initializers_minus1 == 0U);
	CHECK(sps_out.sps.pPredictorPaletteEntries == NULL);

	/* The profile, tier and level, and all seven sub-layers of the buffering limits. */
	CHECK(sps_out.ptl.general_profile_idc == STD_VIDEO_H265_PROFILE_IDC_MAIN_10);
	CHECK(sps_out.ptl.general_level_idc == STD_VIDEO_H265_LEVEL_IDC_4_1);
	for (i = 0U; i < 7U; i++) {
		CHECK(sps_out.dpb.max_dec_pic_buffering_minus1[i] == 3U + 2U * i);
		CHECK(sps_out.dpb.max_num_reorder_pics[i] == i);
		if (i < 6U)
			CHECK(sps_out.dpb.max_latency_increase_plus1[i] == 1000U * (i + 1U));
	}

	CHECK(sps_out.dpb.max_latency_increase_plus1[6] == 0xFFFFFFFEU);

	/* The long-term entries: the lsb of each, and the used flags as a mask (entries 0 and 2). */
	CHECK(sps_out.lt.lt_ref_pic_poc_lsb_sps[0] == 0x111U);
	CHECK(sps_out.lt.lt_ref_pic_poc_lsb_sps[1] == 0x222U);
	CHECK(sps_out.lt.lt_ref_pic_poc_lsb_sps[2] == 0xFFFFU);
	CHECK(sps_out.lt.lt_ref_pic_poc_lsb_sps[3] == 0U);
	CHECK(sps_out.lt.used_by_curr_pic_lt_sps_flag == 0x5U);

	/* With PCM off the PCM fields are 0 although the input holds values. */
	sps_in.pcm = 0;
	error = h265_std_sps(&sps_in, &sps_out);
	CHECK(error == 0);
	CHECK(sps_out.sps.flags.pcm_enabled_flag == 0U);
	CHECK(sps_out.sps.pcm_sample_bit_depth_luma_minus1 == 0U);
	CHECK(sps_out.sps.pcm_sample_bit_depth_chroma_minus1 == 0U);
	CHECK(sps_out.sps.log2_min_pcm_luma_coding_block_size_minus3 == 0U);
	CHECK(sps_out.sps.log2_diff_max_min_pcm_luma_coding_block_size == 0U);

	/* A 4:4:4 8-bit sequence at the other end of the ranges: CTB 16, 8x8 coding blocks, 4x4 transform blocks. */
	sps_fill(&sps_in);
	sps_in.chroma_format_idc = 3U;
	sps_in.bit_depth_luma = 8U;
	sps_in.bit_depth_chroma = 8U;
	sps_in.log2_max_poc_lsb = 4U;
	sps_in.min_cb_log2 = 3U;
	sps_in.ctb_log2 = 4U;
	sps_in.min_tb_log2 = 2U;
	sps_in.max_tb_log2 = 4U;
	sps_in.pcm_bit_depth_luma = 1U;
	sps_in.pcm_bit_depth_chroma = 1U;
	sps_in.pcm_min_log2 = 3U;
	sps_in.pcm_max_log2 = 3U;
	error = h265_std_sps(&sps_in, &sps_out);
	CHECK(error == 0);
	CHECK(sps_out.sps.chroma_format_idc == STD_VIDEO_H265_CHROMA_FORMAT_IDC_444);
	CHECK(sps_out.sps.bit_depth_luma_minus8 == 0U);
	CHECK(sps_out.sps.bit_depth_chroma_minus8 == 0U);
	CHECK(sps_out.sps.log2_max_pic_order_cnt_lsb_minus4 == 0U);
	CHECK(sps_out.sps.log2_min_luma_coding_block_size_minus3 == 0U);
	CHECK(sps_out.sps.log2_diff_max_min_luma_coding_block_size == 1U);
	CHECK(sps_out.sps.log2_min_luma_transform_block_size_minus2 == 0U);
	CHECK(sps_out.sps.log2_diff_max_min_luma_transform_block_size == 2U);
	CHECK(sps_out.sps.pcm_sample_bit_depth_luma_minus1 == 0U);
	CHECK(sps_out.sps.pcm_sample_bit_depth_chroma_minus1 == 0U);
	CHECK(sps_out.sps.log2_min_pcm_luma_coding_block_size_minus3 == 0U);
	CHECK(sps_out.sps.log2_diff_max_min_pcm_luma_coding_block_size == 0U);

	/* Monochrome and 4:2:2 map to their own values. */
	sps_in.chroma_format_idc = 0U;
	error = h265_std_sps(&sps_in, &sps_out);
	CHECK(error == 0);
	CHECK(sps_out.sps.chroma_format_idc == STD_VIDEO_H265_CHROMA_FORMAT_IDC_MONOCHROME);

	sps_in.chroma_format_idc = 2U;
	error = h265_std_sps(&sps_in, &sps_out);
	CHECK(error == 0);
	CHECK(sps_out.sps.chroma_format_idc == STD_VIDEO_H265_CHROMA_FORMAT_IDC_422);

	/* A profile that is refused leaves the output as it was. */
	sps_in.ptl.profile_idc = 0U;
	sps_in.ptl.compatibility = 0U;
	memset(&sps_out, 0x5A, sizeof(sps_out));
	error = h265_std_sps(&sps_in, &sps_out);
	CHECK(error == ENOTSUP);
	CHECK(sps_out.sps.pic_width_in_luma_samples == 0x5A5A5A5AU);
}

/*
 * The index of each flag of an SPS in the order of StdVideoH265SpsFlags:
 * the 16 flags before the range extension flags (sps_range_extension_flag
 * last), the 9 range extension flags, and the 5 screen content flags.
 */
#define SPS_FLAG_COUNT 30U

/* Reads the 30 flags of an SPS in the Vulkan order. */
static void
sps_flag_bits(
	const StdVideoH265SpsFlags *f,
	unsigned *bits)
{
	/* Reads each flag by name. */
	bits[0] = f->sps_temporal_id_nesting_flag;
	bits[1] = f->separate_colour_plane_flag;
	bits[2] = f->conformance_window_flag;
	bits[3] = f->sps_sub_layer_ordering_info_present_flag;
	bits[4] = f->scaling_list_enabled_flag;
	bits[5] = f->sps_scaling_list_data_present_flag;
	bits[6] = f->amp_enabled_flag;
	bits[7] = f->sample_adaptive_offset_enabled_flag;
	bits[8] = f->pcm_enabled_flag;
	bits[9] = f->pcm_loop_filter_disabled_flag;
	bits[10] = f->long_term_ref_pics_present_flag;
	bits[11] = f->sps_temporal_mvp_enabled_flag;
	bits[12] = f->strong_intra_smoothing_enabled_flag;
	bits[13] = f->vui_parameters_present_flag;
	bits[14] = f->sps_extension_present_flag;
	bits[15] = f->sps_range_extension_flag;
	bits[16] = f->transform_skip_rotation_enabled_flag;
	bits[17] = f->transform_skip_context_enabled_flag;
	bits[18] = f->implicit_rdpcm_enabled_flag;
	bits[19] = f->explicit_rdpcm_enabled_flag;
	bits[20] = f->extended_precision_processing_flag;
	bits[21] = f->intra_smoothing_disabled_flag;
	bits[22] = f->high_precision_offsets_enabled_flag;
	bits[23] = f->persistent_rice_adaptation_enabled_flag;
	bits[24] = f->cabac_bypass_alignment_enabled_flag;
	bits[25] = f->sps_scc_extension_flag;
	bits[26] = f->sps_curr_pic_ref_enabled_flag;
	bits[27] = f->palette_mode_enabled_flag;
	bits[28] = f->sps_palette_predictor_initializers_present_flag;
	bits[29] = f->intra_boundary_filtering_disabled_flag;
}

/* Tests the SPS flags one at a time (the base flags and the nine range extension bits) and the screen content flags. */
static void
test_sps_flags(void)
{
	unsigned bits[SPS_FLAG_COUNT];
	unsigned k;
	unsigned j;
	int good;
	int error;

	/* Sets one input flag at a time and checks that exactly the matching output flag is set. */
	for (k = 0U; k < 25U; k++) {
		memset(&sps_in, 0, sizeof(sps_in));
		ptl_fill(&sps_in.ptl);
		sps_in.min_cb_log2 = 3U;
		sps_in.ctb_log2 = 4U;
		sps_in.min_tb_log2 = 2U;
		sps_in.max_tb_log2 = 3U;
		sps_in.bit_depth_luma = 8U;
		sps_in.bit_depth_chroma = 8U;
		sps_in.log2_max_poc_lsb = 4U;

		if (k == 0U)
			sps_in.temporal_id_nesting = 1;
		if (k == 1U)
			sps_in.separate_colour_plane = 1;
		if (k == 2U)
			sps_in.conformance_window = 1;
		if (k == 3U)
			sps_in.sub_layer_ordering_info_present = 1;
		if (k == 4U)
			sps_in.scaling_list_enabled = 1;
		if (k == 5U)
			sps_in.scaling_list_data_present = 1;
		if (k == 6U)
			sps_in.amp = 1;
		if (k == 7U)
			sps_in.sao = 1;
		if (k == 8U)
			sps_in.pcm = 1;
		if (k == 9U)
			sps_in.pcm_loop_filter_disabled = 1;
		if (k == 10U)
			sps_in.long_term_present = 1;
		if (k == 11U)
			sps_in.temporal_mvp = 1;
		if (k == 12U)
			sps_in.strong_intra_smoothing = 1;
		if (k == 13U)
			sps_in.vui_present = 1;
		if (k == 14U)
			sps_in.extension_present = 1;
		if (k == 15U)
			sps_in.range_extension_present = 1;
		if (k >= 16U)
			sps_in.range_extension = (unsigned)1U << (k - 16U);

		if (k == 8U) {
			sps_in.pcm_bit_depth_luma = 1U;
			sps_in.pcm_bit_depth_chroma = 1U;
			sps_in.pcm_min_log2 = 3U;
			sps_in.pcm_max_log2 = 3U;
		}

		error = h265_std_sps(&sps_in, &sps_out);
		CHECK(error == 0);

		sps_flag_bits(&sps_out.sps.flags, bits);
		good = 1;
		for (j = 0U; j < SPS_FLAG_COUNT; j++) {
			if (bits[j] != (unsigned)(j == k))
				good = 0;
		}

		CHECK(good);
	}

	/* The bits of range_extension above the ninth are not flags. */
	memset(&sps_in, 0, sizeof(sps_in));
	ptl_fill(&sps_in.ptl);
	sps_in.min_cb_log2 = 3U;
	sps_in.ctb_log2 = 4U;
	sps_in.min_tb_log2 = 2U;
	sps_in.max_tb_log2 = 3U;
	sps_in.bit_depth_luma = 8U;
	sps_in.bit_depth_chroma = 8U;
	sps_in.log2_max_poc_lsb = 4U;
	sps_in.range_extension = 0xFFFFFE00U;
	error = h265_std_sps(&sps_in, &sps_out);
	CHECK(error == 0);
	sps_flag_bits(&sps_out.sps.flags, bits);
	good = 1;
	for (j = 0U; j < SPS_FLAG_COUNT; j++) {
		if (bits[j] != 0U)
			good = 0;
	}

	CHECK(good);

	/* All nine range extension bits set: the nine flags are 1, the range extension flag itself follows its own field. */
	sps_in.range_extension = 0x1FFU;
	sps_in.range_extension_present = 1;
	error = h265_std_sps(&sps_in, &sps_out);
	CHECK(error == 0);
	sps_flag_bits(&sps_out.sps.flags, bits);
	good = 1;
	for (j = 0U; j < SPS_FLAG_COUNT; j++) {
		unsigned expected = 0U;

		if (j >= 15U && j <= 24U)
			expected = 1U;

		if (bits[j] != expected)
			good = 0;
	}

	CHECK(good);

	/* Flags stored as other non-zero values are 1. */
	memset(&sps_in, 0, sizeof(sps_in));
	ptl_fill(&sps_in.ptl);
	sps_in.min_cb_log2 = 3U;
	sps_in.ctb_log2 = 4U;
	sps_in.min_tb_log2 = 2U;
	sps_in.max_tb_log2 = 3U;
	sps_in.bit_depth_luma = 8U;
	sps_in.bit_depth_chroma = 8U;
	sps_in.log2_max_poc_lsb = 4U;
	sps_in.amp = 2;
	sps_in.sao = -1;
	error = h265_std_sps(&sps_in, &sps_out);
	CHECK(error == 0);
	CHECK(sps_out.sps.flags.amp_enabled_flag == 1U);
	CHECK(sps_out.sps.flags.sample_adaptive_offset_enabled_flag == 1U);
}

/* Tests which pointers of an SPS are set, and that each names a structure inside the output. */
static void
test_sps_pointers(void)
{
	int error;

	/* An SPS with nothing optional: only the profile and the buffering pointers are set. */
	sps_fill(&sps_in);
	sps_in.long_term_present = 0;
	sps_in.num_lt_sps = 0U;
	error = h265_std_sps(&sps_in, &sps_out);
	CHECK(error == 0);
	CHECK(sps_out.sps.pProfileTierLevel == &sps_out.ptl);
	CHECK(sps_out.sps.pDecPicBufMgr == &sps_out.dpb);
	CHECK(sps_out.sps.pScalingLists == NULL);
	CHECK(sps_out.sps.pShortTermRefPicSet == NULL);
	CHECK(sps_out.sps.pLongTermRefPicsSps == NULL);
	CHECK(sps_out.sps.pSequenceParameterSetVui == NULL);
	CHECK(sps_out.lt.used_by_curr_pic_lt_sps_flag == 0U);

	/* Long-term pictures present: the pointer names the long-term structure, also when the SPS lists none. */
	sps_in.long_term_present = 1;
	error = h265_std_sps(&sps_in, &sps_out);
	CHECK(error == 0);
	CHECK(sps_out.sps.pLongTermRefPicsSps == &sps_out.lt);

	sps_in.num_lt_sps = 0U;
	error = h265_std_sps(&sps_in, &sps_out);
	CHECK(error == 0);
	CHECK(sps_out.sps.pLongTermRefPicsSps == &sps_out.lt);
	CHECK(sps_out.lt.used_by_curr_pic_lt_sps_flag == 0U);
	CHECK(sps_out.lt.lt_ref_pic_poc_lsb_sps[0] == 0U);

	/* All 32 long-term entries: the mask holds the entry 31 flag, and each lsb is copied. */
	sps_in.num_lt_sps = 32U;
	sps_in.lt_poc_lsb[31] = 0xABCDU;
	sps_in.lt_used[31] = 1U;
	error = h265_std_sps(&sps_in, &sps_out);
	CHECK(error == 0);
	CHECK(sps_out.lt.lt_ref_pic_poc_lsb_sps[31] == 0xABCDU);
	CHECK((sps_out.lt.used_by_curr_pic_lt_sps_flag & 0x80000000U) != 0U);
	CHECK(sps_out.lt.used_by_curr_pic_lt_sps_flag == 0x80000005U);

	/* A count above 32 is cut to the 32 entries the array holds. */
	sps_in.num_lt_sps = 40U;
	error = h265_std_sps(&sps_in, &sps_out);
	CHECK(error == 0);
	CHECK(sps_out.lt.lt_ref_pic_poc_lsb_sps[31] == 0xABCDU);
	CHECK(sps_out.lt.used_by_curr_pic_lt_sps_flag == 0x80000005U);

	/* Short-term sets: the pointer names the array of the output when there is at least one. */
	sps_in.num_st_rps = 1U;
	error = h265_std_sps(&sps_in, &sps_out);
	CHECK(error == 0);
	CHECK(sps_out.sps.pShortTermRefPicSet == sps_out.st_rps);
	CHECK(sps_out.sps.num_short_term_ref_pic_sets == 1U);

	/* VUI present without HRD: the VUI pointer is set and its HRD pointer is NULL. */
	sps_in.vui_present = 1;
	vui_fill(&sps_in.vui);
	sps_in.vui.hrd_present = 0;
	error = h265_std_sps(&sps_in, &sps_out);
	CHECK(error == 0);
	CHECK(sps_out.sps.pSequenceParameterSetVui == &sps_out.vui);
	CHECK(sps_out.vui.pHrdParameters == NULL);

	/* VUI with HRD parameters: the HRD pointer names the HRD structure of the output and its arrays. */
	sps_in.vui.hrd_present = 1;
	error = h265_std_sps(&sps_in, &sps_out);
	CHECK(error == 0);
	CHECK(sps_out.sps.pSequenceParameterSetVui == &sps_out.vui);
	CHECK(sps_out.vui.pHrdParameters == &sps_out.hrd);
	hrd_check(&sps_out.hrd, sps_out.hrd_nal, sps_out.hrd_vcl, 1, 1);

	/* An SPS without a VUI has no VUI pointer even when the input holds VUI values and an HRD flag. */
	sps_in.vui_present = 0;
	error = h265_std_sps(&sps_in, &sps_out);
	CHECK(error == 0);
	CHECK(sps_out.sps.pSequenceParameterSetVui == NULL);

	/* The output is made again in other memory, and its pointers name that memory. */
	{
		struct h265_std_sps *copy;

		copy = (struct h265_std_sps *)malloc(sizeof(*copy));
		CHECK(copy != NULL);
		if (copy != NULL) {
			sps_in.vui_present = 1;
			error = h265_std_sps(&sps_in, copy);
			CHECK(error == 0);
			CHECK(copy->sps.pProfileTierLevel == &copy->ptl);
			CHECK(copy->sps.pDecPicBufMgr == &copy->dpb);
			CHECK(copy->sps.pShortTermRefPicSet == copy->st_rps);
			CHECK(copy->sps.pLongTermRefPicsSps == &copy->lt);
			CHECK(copy->sps.pSequenceParameterSetVui == &copy->vui);
			CHECK(copy->vui.pHrdParameters == &copy->hrd);
			CHECK(copy->hrd.pSubLayerHrdParametersNal == copy->hrd_nal);
			CHECK(copy->hrd.pSubLayerHrdParametersVcl == copy->hrd_vcl);
			free(copy);
		}
	}
}

/* Tests the video usability information: the flags, the signalled sample aspect ratio and the fields. */
static void
test_vui(void)
{
	int error;
	unsigned k;
	unsigned bits[18];
	unsigned j;
	int good;

	/* Makes an SPS with every VUI flag set and distinct values. */
	sps_fill(&sps_in);
	sps_in.vui_present = 1;
	vui_fill(&sps_in.vui);
	error = h265_std_sps(&sps_in, &sps_out);
	CHECK(error == 0);

	/* The aspect ratio is the signalled one: EXTENDED_SAR with 4 and 3, not the derived 8 and 6. */
	CHECK(sps_out.vui.aspect_ratio_idc == STD_VIDEO_H265_ASPECT_RATIO_IDC_EXTENDED_SAR);
	CHECK(sps_out.vui.sar_width == 4U);
	CHECK(sps_out.vui.sar_height == 3U);

	/* The other fields. */
	CHECK(sps_out.vui.video_format == 3U);
	CHECK(sps_out.vui.colour_primaries == 9U);
	CHECK(sps_out.vui.transfer_characteristics == 16U);
	CHECK(sps_out.vui.matrix_coeffs == 9U);
	CHECK(sps_out.vui.chroma_sample_loc_type_top_field == 2U);
	CHECK(sps_out.vui.chroma_sample_loc_type_bottom_field == 4U);
	CHECK(sps_out.vui.reserved1 == 0U);
	CHECK(sps_out.vui.reserved2 == 0U);
	CHECK(sps_out.vui.def_disp_win_left_offset == 11U);
	CHECK(sps_out.vui.def_disp_win_right_offset == 12U);
	CHECK(sps_out.vui.def_disp_win_top_offset == 13U);
	CHECK(sps_out.vui.def_disp_win_bottom_offset == 16888U);
	CHECK(sps_out.vui.vui_num_units_in_tick == 1001U);
	CHECK(sps_out.vui.vui_time_scale == 60000U);
	CHECK(sps_out.vui.vui_num_ticks_poc_diff_one_minus1 == 0xFFFFFFFEU);
	CHECK(sps_out.vui.min_spatial_segmentation_idc == 4095U);
	CHECK(sps_out.vui.reserved3 == 0U);
	CHECK(sps_out.vui.max_bytes_per_pic_denom == 7U);
	CHECK(sps_out.vui.max_bits_per_min_cu_denom == 8U);
	CHECK(sps_out.vui.log2_max_mv_length_horizontal == 14U);
	CHECK(sps_out.vui.log2_max_mv_length_vertical == 13U);

	/* A sample aspect ratio that is not EXTENDED_SAR: the signalled sizes are 0 whatever the derived ratio is. */
	sps_in.vui.aspect_ratio_idc = 1U;
	sps_in.vui.signalled_sar_width = 0U;
	sps_in.vui.signalled_sar_height = 0U;
	sps_in.vui.sar_width = 1U;
	sps_in.vui.sar_height = 1U;
	error = h265_std_sps(&sps_in, &sps_out);
	CHECK(error == 0);
	CHECK(sps_out.vui.aspect_ratio_idc == STD_VIDEO_H265_ASPECT_RATIO_IDC_SQUARE);
	CHECK(sps_out.vui.sar_width == 0U);
	CHECK(sps_out.vui.sar_height == 0U);

	/* Sets one VUI flag at a time and checks that exactly the matching output flag is set. */
	for (k = 0U; k < 18U; k++) {
		memset(&sps_in.vui, 0, sizeof(sps_in.vui));
		if (k == 0U)
			sps_in.vui.aspect_ratio_present = 1;
		if (k == 1U)
			sps_in.vui.overscan_present = 1;
		if (k == 2U)
			sps_in.vui.overscan_appropriate = 1;
		if (k == 3U)
			sps_in.vui.signal_present = 1;
		if (k == 4U)
			sps_in.vui.full_range = 1;
		if (k == 5U)
			sps_in.vui.colour_present = 1;
		if (k == 6U)
			sps_in.vui.chroma_loc_present = 1;
		if (k == 7U)
			sps_in.vui.neutral_chroma_indication = 1;
		if (k == 8U)
			sps_in.vui.field_seq = 1;
		if (k == 9U)
			sps_in.vui.frame_field_info_present = 1;
		if (k == 10U)
			sps_in.vui.default_display_window = 1;
		if (k == 11U)
			sps_in.vui.timing_present = 1;
		if (k == 12U)
			sps_in.vui.poc_proportional_to_timing = 1;
		if (k == 13U)
			sps_in.vui.hrd_present = 1;
		if (k == 14U)
			sps_in.vui.bitstream_restriction = 1;
		if (k == 15U)
			sps_in.vui.tiles_fixed_structure = 1;
		if (k == 16U)
			sps_in.vui.motion_vectors_over_pic_boundaries = 1;
		if (k == 17U)
			sps_in.vui.restricted_ref_pic_lists = 1;

		error = h265_std_sps(&sps_in, &sps_out);
		CHECK(error == 0);

		bits[0] = sps_out.vui.flags.aspect_ratio_info_present_flag;
		bits[1] = sps_out.vui.flags.overscan_info_present_flag;
		bits[2] = sps_out.vui.flags.overscan_appropriate_flag;
		bits[3] = sps_out.vui.flags.video_signal_type_present_flag;
		bits[4] = sps_out.vui.flags.video_full_range_flag;
		bits[5] = sps_out.vui.flags.colour_description_present_flag;
		bits[6] = sps_out.vui.flags.chroma_loc_info_present_flag;
		bits[7] = sps_out.vui.flags.neutral_chroma_indication_flag;
		bits[8] = sps_out.vui.flags.field_seq_flag;
		bits[9] = sps_out.vui.flags.frame_field_info_present_flag;
		bits[10] = sps_out.vui.flags.default_display_window_flag;
		bits[11] = sps_out.vui.flags.vui_timing_info_present_flag;
		bits[12] = sps_out.vui.flags.vui_poc_proportional_to_timing_flag;
		bits[13] = sps_out.vui.flags.vui_hrd_parameters_present_flag;
		bits[14] = sps_out.vui.flags.bitstream_restriction_flag;
		bits[15] = sps_out.vui.flags.tiles_fixed_structure_flag;
		bits[16] = sps_out.vui.flags.motion_vectors_over_pic_boundaries_flag;
		bits[17] = sps_out.vui.flags.restricted_ref_pic_lists_flag;

		good = 1;
		for (j = 0U; j < 18U; j++) {
			if (bits[j] != (unsigned)(j == k))
				good = 0;
		}

		CHECK(good);
	}
}

/* Tests the scaling lists of an SPS and a PPS: the mapping of sizes and matrices, and the pointer rules. */
static void
test_scaling(void)
{
	int error;

	/* Scaling lists enabled with signalled data: the lists are copied. */
	sps_fill(&sps_in);
	sps_in.scaling_list_enabled = 1;
	sps_in.scaling_list_data_present = 1;
	scaling_fill(&sps_in.scaling, 5U);
	error = h265_std_sps(&sps_in, &sps_out);
	CHECK(error == 0);
	CHECK(sps_out.sps.pScalingLists == &sps_out.scaling);
	scaling_check(&sps_out.scaling, &sps_in.scaling);

	/* Enabled without data: the default lists the parse made (here a different fill) are the ones used. */
	sps_in.scaling_list_data_present = 0;
	scaling_fill(&sps_in.scaling, 77U);
	error = h265_std_sps(&sps_in, &sps_out);
	CHECK(error == 0);
	CHECK(sps_out.sps.pScalingLists == &sps_out.scaling);
	scaling_check(&sps_out.scaling, &sps_in.scaling);

	/* Not enabled: no pointer, although the input holds lists. */
	sps_in.scaling_list_enabled = 0;
	error = h265_std_sps(&sps_in, &sps_out);
	CHECK(error == 0);
	CHECK(sps_out.sps.pScalingLists == NULL);

	/* A PPS with lists points at its own copy; without them it has no pointer. */
	pps_fill(&pps_in);
	pps_in.scaling_list_present = 1;
	scaling_fill(&pps_in.scaling, 33U);
	error = h265_std_pps(&pps_in, &sps_in, &pps_out);
	CHECK(error == 0);
	CHECK(pps_out.pps.pScalingLists == &pps_out.scaling);
	scaling_check(&pps_out.scaling, &pps_in.scaling);

	pps_in.scaling_list_present = 0;
	error = h265_std_pps(&pps_in, &sps_in, &pps_out);
	CHECK(error == 0);
	CHECK(pps_out.pps.pScalingLists == NULL);
	CHECK(pps_out.pps.pPredictorPaletteEntries == NULL);
}

/*
 * Tests the short-term sets: an explicit set as signalled, and a predicted
 * set with its syntax and the derived set written explicitly.
 *
 * The explicit set 0 has DeltaPocS0 = -1, -4 (delta_poc_s0_minus1 = 0, 2;
 * the first used, the second not) and DeltaPocS1 = 2 (delta_poc_s1_minus1
 * = 1, used).
 *
 * The predicted set 1 is predicted from set 0 with deltaRps -1
 * (delta_rps_sign 1, abs_delta_rps_minus1 0): the entries of set 0 and
 * deltaRps itself shifted by deltaRps are -2, -5, 1 and -1; with the used
 * flags 1, 0, 1, 1 for the entries j = 0 to 3 the derived set is
 * DeltaPocS0 = -1, -2, -5 (used 1, 1, 0) and DeltaPocS1 = 1 (used 1), so
 * delta_poc_s0_minus1 = 0, 0, 2 (the steps 1, 1, 3 minus 1),
 * delta_poc_s1_minus1 = 0, used_by_curr_pic_s0_flag = 3 and
 * used_by_curr_pic_s1_flag = 1.
 */
static void
test_short_term_sets(void)
{
	int error;
	unsigned i;

	/* Fills the two sets: the derived sets and the syntax as signalled. */
	sps_fill(&sps_in);
	sps_in.num_st_rps = 2U;

	sps_in.st_rps[0].num_negative = 2U;
	sps_in.st_rps[0].num_positive = 1U;
	sps_in.st_rps[0].delta_poc[0] = -1;
	sps_in.st_rps[0].delta_poc[1] = -4;
	sps_in.st_rps[0].delta_poc[2] = 2;
	sps_in.st_rps[0].used[0] = 1U;
	sps_in.st_rps[0].used[1] = 0U;
	sps_in.st_rps[0].used[2] = 1U;
	sps_in.st_syntax[0].inter_rps_pred = 0;
	sps_in.st_syntax[0].num_negative_pics = 2U;
	sps_in.st_syntax[0].num_positive_pics = 1U;
	sps_in.st_syntax[0].delta_poc_s0_minus1[0] = 0U;
	sps_in.st_syntax[0].delta_poc_s0_minus1[1] = 2U;
	sps_in.st_syntax[0].delta_poc_s1_minus1[0] = 1U;
	sps_in.st_syntax[0].used_by_curr_pic_s0_flags = 1U;
	sps_in.st_syntax[0].used_by_curr_pic_s1_flags = 1U;
	sps_in.st_syntax[0].bits = 17U;

	sps_in.st_rps[1].num_negative = 3U;
	sps_in.st_rps[1].num_positive = 1U;
	sps_in.st_rps[1].delta_poc[0] = -1;
	sps_in.st_rps[1].delta_poc[1] = -2;
	sps_in.st_rps[1].delta_poc[2] = -5;
	sps_in.st_rps[1].delta_poc[3] = 1;
	sps_in.st_rps[1].used[0] = 1U;
	sps_in.st_rps[1].used[1] = 1U;
	sps_in.st_rps[1].used[2] = 0U;
	sps_in.st_rps[1].used[3] = 1U;
	sps_in.st_syntax[1].inter_rps_pred = 1;
	sps_in.st_syntax[1].delta_idx_minus1 = 0U;
	sps_in.st_syntax[1].delta_rps_sign = 1;
	sps_in.st_syntax[1].abs_delta_rps_minus1 = 0U;
	sps_in.st_syntax[1].ref_num_delta_pocs = 3U;
	sps_in.st_syntax[1].used_by_curr_pic_flags = 0xDU;
	sps_in.st_syntax[1].use_delta_flags = 0xFU;
	sps_in.st_syntax[1].bits = 9U;

	error = h265_std_sps(&sps_in, &sps_out);
	CHECK(error == 0);
	CHECK(sps_out.sps.num_short_term_ref_pic_sets == 2U);
	CHECK(sps_out.sps.pShortTermRefPicSet == sps_out.st_rps);

	/* The explicit set is as signalled, and the prediction syntax is 0. */
	CHECK(sps_out.st_rps[0].flags.inter_ref_pic_set_prediction_flag == 0U);
	CHECK(sps_out.st_rps[0].flags.delta_rps_sign == 0U);
	CHECK(sps_out.st_rps[0].delta_idx_minus1 == 0U);
	CHECK(sps_out.st_rps[0].use_delta_flag == 0U);
	CHECK(sps_out.st_rps[0].abs_delta_rps_minus1 == 0U);
	CHECK(sps_out.st_rps[0].used_by_curr_pic_flag == 0U);
	CHECK(sps_out.st_rps[0].num_negative_pics == 2U);
	CHECK(sps_out.st_rps[0].num_positive_pics == 1U);
	CHECK(sps_out.st_rps[0].delta_poc_s0_minus1[0] == 0U);
	CHECK(sps_out.st_rps[0].delta_poc_s0_minus1[1] == 2U);
	CHECK(sps_out.st_rps[0].delta_poc_s0_minus1[2] == 0U);
	CHECK(sps_out.st_rps[0].delta_poc_s1_minus1[0] == 1U);
	CHECK(sps_out.st_rps[0].delta_poc_s1_minus1[1] == 0U);
	CHECK(sps_out.st_rps[0].used_by_curr_pic_s0_flag == 1U);
	CHECK(sps_out.st_rps[0].used_by_curr_pic_s1_flag == 1U);
	CHECK(sps_out.st_rps[0].reserved1 == 0U);
	CHECK(sps_out.st_rps[0].reserved2 == 0U);
	CHECK(sps_out.st_rps[0].reserved3 == 0U);

	/* The predicted set carries the prediction syntax. */
	CHECK(sps_out.st_rps[1].flags.inter_ref_pic_set_prediction_flag == 1U);
	CHECK(sps_out.st_rps[1].flags.delta_rps_sign == 1U);
	CHECK(sps_out.st_rps[1].delta_idx_minus1 == 0U);
	CHECK(sps_out.st_rps[1].use_delta_flag == 0xFU);
	CHECK(sps_out.st_rps[1].abs_delta_rps_minus1 == 0U);
	CHECK(sps_out.st_rps[1].used_by_curr_pic_flag == 0xDU);

	/* The predicted set also carries the derived set written explicitly. */
	CHECK(sps_out.st_rps[1].num_negative_pics == 3U);
	CHECK(sps_out.st_rps[1].num_positive_pics == 1U);
	CHECK(sps_out.st_rps[1].delta_poc_s0_minus1[0] == 0U);
	CHECK(sps_out.st_rps[1].delta_poc_s0_minus1[1] == 0U);
	CHECK(sps_out.st_rps[1].delta_poc_s0_minus1[2] == 2U);
	CHECK(sps_out.st_rps[1].delta_poc_s0_minus1[3] == 0U);
	CHECK(sps_out.st_rps[1].delta_poc_s1_minus1[0] == 0U);
	CHECK(sps_out.st_rps[1].delta_poc_s1_minus1[1] == 0U);
	CHECK(sps_out.st_rps[1].used_by_curr_pic_s0_flag == 3U);
	CHECK(sps_out.st_rps[1].used_by_curr_pic_s1_flag == 1U);

	/* The entries of the sets that the SPS does not have stay 0. */
	CHECK(sps_out.st_rps[2].num_negative_pics == 0U);
	CHECK(sps_out.st_rps[63].delta_poc_s0_minus1[15] == 0U);

	/* A positive-only predicted set with a mixed used pattern: DeltaPocS1 = 3, 5, 9, 10 (used 0, 1, 1, 0). */
	sps_in.num_st_rps = 1U;
	memset(&sps_in.st_rps[0], 0, sizeof(sps_in.st_rps[0]));
	memset(&sps_in.st_syntax[0], 0, sizeof(sps_in.st_syntax[0]));
	sps_in.st_rps[0].num_negative = 0U;
	sps_in.st_rps[0].num_positive = 4U;
	sps_in.st_rps[0].delta_poc[0] = 3;
	sps_in.st_rps[0].delta_poc[1] = 5;
	sps_in.st_rps[0].delta_poc[2] = 9;
	sps_in.st_rps[0].delta_poc[3] = 10;
	sps_in.st_rps[0].used[0] = 0U;
	sps_in.st_rps[0].used[1] = 1U;
	sps_in.st_rps[0].used[2] = 1U;
	sps_in.st_rps[0].used[3] = 0U;
	sps_in.st_syntax[0].inter_rps_pred = 1;
	sps_in.st_syntax[0].delta_idx_minus1 = 2U;
	sps_in.st_syntax[0].delta_rps_sign = 0;
	sps_in.st_syntax[0].abs_delta_rps_minus1 = 300U;
	sps_in.st_syntax[0].used_by_curr_pic_flags = 0x1234U;
	sps_in.st_syntax[0].use_delta_flags = 0x4321U;
	error = h265_std_sps(&sps_in, &sps_out);
	CHECK(error == 0);
	CHECK(sps_out.st_rps[0].flags.inter_ref_pic_set_prediction_flag == 1U);
	CHECK(sps_out.st_rps[0].flags.delta_rps_sign == 0U);
	CHECK(sps_out.st_rps[0].delta_idx_minus1 == 2U);
	CHECK(sps_out.st_rps[0].abs_delta_rps_minus1 == 300U);
	CHECK(sps_out.st_rps[0].used_by_curr_pic_flag == 0x1234U);
	CHECK(sps_out.st_rps[0].use_delta_flag == 0x4321U);
	CHECK(sps_out.st_rps[0].num_negative_pics == 0U);
	CHECK(sps_out.st_rps[0].num_positive_pics == 4U);
	CHECK(sps_out.st_rps[0].delta_poc_s1_minus1[0] == 2U);
	CHECK(sps_out.st_rps[0].delta_poc_s1_minus1[1] == 1U);
	CHECK(sps_out.st_rps[0].delta_poc_s1_minus1[2] == 3U);
	CHECK(sps_out.st_rps[0].delta_poc_s1_minus1[3] == 0U);
	CHECK(sps_out.st_rps[0].used_by_curr_pic_s1_flag == 6U);
	CHECK(sps_out.st_rps[0].used_by_curr_pic_s0_flag == 0U);

	/* A negative-only predicted set: DeltaPocS0 = -7, -8, -20 (used 1, 0, 1). */
	memset(&sps_in.st_rps[0], 0, sizeof(sps_in.st_rps[0]));
	sps_in.st_rps[0].num_negative = 3U;
	sps_in.st_rps[0].delta_poc[0] = -7;
	sps_in.st_rps[0].delta_poc[1] = -8;
	sps_in.st_rps[0].delta_poc[2] = -20;
	sps_in.st_rps[0].used[0] = 1U;
	sps_in.st_rps[0].used[1] = 0U;
	sps_in.st_rps[0].used[2] = 1U;
	error = h265_std_sps(&sps_in, &sps_out);
	CHECK(error == 0);
	CHECK(sps_out.st_rps[0].delta_poc_s0_minus1[0] == 6U);
	CHECK(sps_out.st_rps[0].delta_poc_s0_minus1[1] == 0U);
	CHECK(sps_out.st_rps[0].delta_poc_s0_minus1[2] == 11U);
	CHECK(sps_out.st_rps[0].used_by_curr_pic_s0_flag == 5U);
	CHECK(sps_out.st_rps[0].used_by_curr_pic_s1_flag == 0U);

	/* An empty predicted set. */
	memset(&sps_in.st_rps[0], 0, sizeof(sps_in.st_rps[0]));
	error = h265_std_sps(&sps_in, &sps_out);
	CHECK(error == 0);
	CHECK(sps_out.st_rps[0].num_negative_pics == 0U);
	CHECK(sps_out.st_rps[0].num_positive_pics == 0U);

	/* The full set of 15 entries (the most the parse keeps): 8 negative and 7 positive, the used flag of entry 7 and entry 6. */
	memset(&sps_in.st_rps[0], 0, sizeof(sps_in.st_rps[0]));
	sps_in.st_rps[0].num_negative = 8U;
	sps_in.st_rps[0].num_positive = 7U;
	for (i = 0U; i < 8U; i++)
		sps_in.st_rps[0].delta_poc[i] = -(int32_t)(i + 1U);
	for (i = 0U; i < 7U; i++)
		sps_in.st_rps[0].delta_poc[8U + i] = (int32_t)(i + 1U);
	sps_in.st_rps[0].used[7] = 1U;
	sps_in.st_rps[0].used[8U + 6U] = 1U;
	error = h265_std_sps(&sps_in, &sps_out);
	CHECK(error == 0);
	CHECK(sps_out.st_rps[0].num_negative_pics == 8U);
	CHECK(sps_out.st_rps[0].num_positive_pics == 7U);
	CHECK(sps_out.st_rps[0].delta_poc_s0_minus1[7] == 0U);
	CHECK(sps_out.st_rps[0].used_by_curr_pic_s0_flag == 0x80U);
	CHECK(sps_out.st_rps[0].used_by_curr_pic_s1_flag == 0x40U);

	/* Sixteen entries in all (8 + 8) still fit the arrays; the explicit form takes 16 in one list as well. */
	sps_in.st_rps[0].num_positive = 8U;
	sps_in.st_rps[0].delta_poc[15] = 8;
	sps_in.st_rps[0].used[15] = 1U;
	error = h265_std_sps(&sps_in, &sps_out);
	CHECK(error == 0);
	CHECK(sps_out.st_rps[0].num_positive_pics == 8U);
	CHECK(sps_out.st_rps[0].delta_poc_s1_minus1[7] == 0U);
	CHECK(sps_out.st_rps[0].used_by_curr_pic_s1_flag == 0xC0U);

	memset(&sps_in.st_syntax[0], 0, sizeof(sps_in.st_syntax[0]));
	sps_in.st_syntax[0].num_negative_pics = 16U;
	sps_in.st_syntax[0].delta_poc_s0_minus1[15] = 123U;
	sps_in.st_syntax[0].used_by_curr_pic_s0_flags = 0x8000U;
	error = h265_std_sps(&sps_in, &sps_out);
	CHECK(error == 0);
	CHECK(sps_out.st_rps[0].num_negative_pics == 16U);
	CHECK(sps_out.st_rps[0].delta_poc_s0_minus1[15] == 123U);
	CHECK(sps_out.st_rps[0].used_by_curr_pic_s0_flag == 0x8000U);

	memset(&sps_in.st_syntax[0], 0, sizeof(sps_in.st_syntax[0]));
	sps_in.st_syntax[0].num_positive_pics = 16U;
	sps_in.st_syntax[0].delta_poc_s1_minus1[15] = 321U;
	sps_in.st_syntax[0].used_by_curr_pic_s1_flags = 0x8001U;
	error = h265_std_sps(&sps_in, &sps_out);
	CHECK(error == 0);
	CHECK(sps_out.st_rps[0].num_positive_pics == 16U);
	CHECK(sps_out.st_rps[0].delta_poc_s1_minus1[15] == 321U);
	CHECK(sps_out.st_rps[0].used_by_curr_pic_s1_flag == 0x8001U);

	/* Sixty-four explicit sets keep their order in the array. */
	sps_fill(&sps_in);
	sps_in.num_st_rps = 64U;
	for (i = 0U; i < 64U; i++) {
		sps_in.st_syntax[i].num_negative_pics = 1U;
		sps_in.st_syntax[i].delta_poc_s0_minus1[0] = (uint16_t)(i * 1000U);
	}

	error = h265_std_sps(&sps_in, &sps_out);
	CHECK(error == 0);
	CHECK(sps_out.sps.num_short_term_ref_pic_sets == 64U);
	for (i = 0U; i < 64U; i++)
		CHECK(sps_out.st_rps[i].delta_poc_s0_minus1[0] == (uint16_t)(i * 1000U));
}

/* Tests the limits of the explicit form of a predicted set, and the counts that do not fit. */
static void
test_short_term_limits(void)
{
	int error;
	unsigned i;

	/* Sets up one predicted set; the first negative step is varied around 65536. */
	sps_fill(&sps_in);
	sps_in.num_st_rps = 1U;
	sps_in.st_syntax[0].inter_rps_pred = 1;
	sps_in.st_rps[0].num_negative = 2U;

	/* The first step -65536 gives delta_poc_s0_minus1 65535 (the largest); the second step of 65536 does too. */
	sps_in.st_rps[0].delta_poc[0] = -65536;
	sps_in.st_rps[0].delta_poc[1] = -131072;
	error = h265_std_sps(&sps_in, &sps_out);
	CHECK(error == 0);
	CHECK(sps_out.st_rps[0].delta_poc_s0_minus1[0] == 65535U);
	CHECK(sps_out.st_rps[0].delta_poc_s0_minus1[1] == 65535U);

	/* A first step of 65537 does not fit. */
	sps_in.st_rps[0].delta_poc[0] = -65537;
	sps_in.st_rps[0].delta_poc[1] = -65538;
	error = h265_std_sps(&sps_in, &sps_out);
	CHECK(error == EINVAL);

	/* A second step of 65537 does not fit. */
	sps_in.st_rps[0].delta_poc[0] = -1;
	sps_in.st_rps[0].delta_poc[1] = -65538;
	error = h265_std_sps(&sps_in, &sps_out);
	CHECK(error == EINVAL);

	/* The second step 65536 (the first -1, the second -65537) fits. */
	sps_in.st_rps[0].delta_poc[1] = -65537;
	error = h265_std_sps(&sps_in, &sps_out);
	CHECK(error == 0);
	CHECK(sps_out.st_rps[0].delta_poc_s0_minus1[1] == 65535U);

	/* Entries that do not decrease are refused: equal, then increasing. */
	sps_in.st_rps[0].delta_poc[0] = -2;
	sps_in.st_rps[0].delta_poc[1] = -2;
	error = h265_std_sps(&sps_in, &sps_out);
	CHECK(error == EINVAL);

	sps_in.st_rps[0].delta_poc[0] = -3;
	sps_in.st_rps[0].delta_poc[1] = -2;
	error = h265_std_sps(&sps_in, &sps_out);
	CHECK(error == EINVAL);

	/* A first negative entry that is not negative (0) is refused: the step would be -1. */
	sps_in.st_rps[0].delta_poc[0] = 0;
	sps_in.st_rps[0].delta_poc[1] = -3;
	error = h265_std_sps(&sps_in, &sps_out);
	CHECK(error == EINVAL);

	/* The same limits for the positive entries: steps 65536 fit, 65537 does not. */
	memset(&sps_in.st_rps[0], 0, sizeof(sps_in.st_rps[0]));
	sps_in.st_rps[0].num_positive = 2U;
	sps_in.st_rps[0].delta_poc[0] = 65536;
	sps_in.st_rps[0].delta_poc[1] = 131072;
	error = h265_std_sps(&sps_in, &sps_out);
	CHECK(error == 0);
	CHECK(sps_out.st_rps[0].delta_poc_s1_minus1[0] == 65535U);
	CHECK(sps_out.st_rps[0].delta_poc_s1_minus1[1] == 65535U);

	sps_in.st_rps[0].delta_poc[0] = 65537;
	sps_in.st_rps[0].delta_poc[1] = 65538;
	error = h265_std_sps(&sps_in, &sps_out);
	CHECK(error == EINVAL);

	sps_in.st_rps[0].delta_poc[0] = 1;
	sps_in.st_rps[0].delta_poc[1] = 65538;
	error = h265_std_sps(&sps_in, &sps_out);
	CHECK(error == EINVAL);

	sps_in.st_rps[0].delta_poc[0] = 5;
	sps_in.st_rps[0].delta_poc[1] = 5;
	error = h265_std_sps(&sps_in, &sps_out);
	CHECK(error == EINVAL);

	sps_in.st_rps[0].delta_poc[0] = 0;
	sps_in.st_rps[0].delta_poc[1] = 4;
	error = h265_std_sps(&sps_in, &sps_out);
	CHECK(error == EINVAL);

	/* Counts that do not fit the arrays are refused: 17 in one list, and 9 + 8 together. */
	memset(&sps_in.st_rps[0], 0, sizeof(sps_in.st_rps[0]));
	sps_in.st_rps[0].num_negative = 17U;
	error = h265_std_sps(&sps_in, &sps_out);
	CHECK(error == EINVAL);

	memset(&sps_in.st_rps[0], 0, sizeof(sps_in.st_rps[0]));
	sps_in.st_rps[0].num_positive = 17U;
	error = h265_std_sps(&sps_in, &sps_out);
	CHECK(error == EINVAL);

	memset(&sps_in.st_rps[0], 0, sizeof(sps_in.st_rps[0]));
	sps_in.st_rps[0].num_negative = 9U;
	sps_in.st_rps[0].num_positive = 8U;
	for (i = 0U; i < 9U; i++)
		sps_in.st_rps[0].delta_poc[i] = -(int32_t)(i + 1U);
	for (i = 0U; i < 7U; i++)
		sps_in.st_rps[0].delta_poc[9U + i] = (int32_t)(i + 1U);
	error = h265_std_sps(&sps_in, &sps_out);
	CHECK(error == EINVAL);

	/* Sixteen entries in one list fit, with valid steps. */
	memset(&sps_in.st_rps[0], 0, sizeof(sps_in.st_rps[0]));
	sps_in.st_rps[0].num_negative = 16U;
	for (i = 0U; i < 16U; i++)
		sps_in.st_rps[0].delta_poc[i] = -(int32_t)(i + 1U);
	error = h265_std_sps(&sps_in, &sps_out);
	CHECK(error == 0);
	CHECK(sps_out.st_rps[0].num_negative_pics == 16U);
	CHECK(sps_out.st_rps[0].delta_poc_s0_minus1[15] == 0U);

	memset(&sps_in.st_rps[0], 0, sizeof(sps_in.st_rps[0]));
	sps_in.st_rps[0].num_positive = 16U;
	for (i = 0U; i < 16U; i++)
		sps_in.st_rps[0].delta_poc[i] = (int32_t)(i + 1U);
	sps_in.st_rps[0].used[15] = 1U;
	error = h265_std_sps(&sps_in, &sps_out);
	CHECK(error == 0);
	CHECK(sps_out.st_rps[0].num_positive_pics == 16U);
	CHECK(sps_out.st_rps[0].used_by_curr_pic_s1_flag == 0x8000U);

	/* An explicit set with counts above the arrays is refused as well. */
	memset(&sps_in.st_syntax[0], 0, sizeof(sps_in.st_syntax[0]));
	sps_in.st_syntax[0].num_negative_pics = 17U;
	error = h265_std_sps(&sps_in, &sps_out);
	CHECK(error == EINVAL);

	memset(&sps_in.st_syntax[0], 0, sizeof(sps_in.st_syntax[0]));
	sps_in.st_syntax[0].num_positive_pics = 17U;
	error = h265_std_sps(&sps_in, &sps_out);
	CHECK(error == EINVAL);

	/* 65 sets do not fit, 64 do. */
	sps_fill(&sps_in);
	sps_in.num_st_rps = 65U;
	error = h265_std_sps(&sps_in, &sps_out);
	CHECK(error == EINVAL);

	sps_in.num_st_rps = 64U;
	error = h265_std_sps(&sps_in, &sps_out);
	CHECK(error == 0);
}

/* Tests the values of a PPS: ids (the VPS id from the SPS), the minus forms, the offsets and the tile counts. */
static void
test_pps_values(void)
{
	int error;
	unsigned i;
	int qp;

	/* Makes the PPS of the SPS (id 14, VPS id 9). */
	sps_fill(&sps_in);
	pps_fill(&pps_in);
	memset(&pps_out, 0xCC, sizeof(pps_out));
	error = h265_std_pps(&pps_in, &sps_in, &pps_out);
	CHECK(error == 0);

	/* The three ids of the key. */
	CHECK(pps_out.pps.pps_pic_parameter_set_id == 37U);
	CHECK(pps_out.pps.pps_seq_parameter_set_id == 14U);
	CHECK(pps_out.pps.sps_video_parameter_set_id == 9U);

	/* The slice header defaults in the minus forms: 15 - 1 and 2 - 1; init_qp 40 - 26. */
	CHECK(pps_out.pps.num_extra_slice_header_bits == 5U);
	CHECK(pps_out.pps.num_ref_idx_l0_default_active_minus1 == 14U);
	CHECK(pps_out.pps.num_ref_idx_l1_default_active_minus1 == 1U);
	CHECK(pps_out.pps.init_qp_minus26 == 14);

	/* The offsets, with their signs; the merge level 6 - 2. */
	CHECK(pps_out.pps.diff_cu_qp_delta_depth == 3U);
	CHECK(pps_out.pps.pps_cb_qp_offset == -12);
	CHECK(pps_out.pps.pps_cr_qp_offset == 11);
	CHECK(pps_out.pps.pps_beta_offset_div2 == -6);
	CHECK(pps_out.pps.pps_tc_offset_div2 == 5);
	CHECK(pps_out.pps.log2_parallel_merge_level_minus2 == 4U);

	/* Without tiles the tile counts and sizes are 0, and the fields of the extensions that are refused are 0. */
	CHECK(pps_out.pps.num_tile_columns_minus1 == 0U);
	CHECK(pps_out.pps.num_tile_rows_minus1 == 0U);
	CHECK(pps_out.pps.column_width_minus1[0] == 0U);
	CHECK(pps_out.pps.row_height_minus1[0] == 0U);
	CHECK(pps_out.pps.log2_max_transform_skip_block_size_minus2 == 0U);
	CHECK(pps_out.pps.diff_cu_chroma_qp_offset_depth == 0U);
	CHECK(pps_out.pps.chroma_qp_offset_list_len_minus1 == 0U);
	CHECK(pps_out.pps.cb_qp_offset_list[0] == 0);
	CHECK(pps_out.pps.cr_qp_offset_list[5] == 0);
	CHECK(pps_out.pps.log2_sao_offset_scale_luma == 0U);
	CHECK(pps_out.pps.log2_sao_offset_scale_chroma == 0U);
	CHECK(pps_out.pps.pps_act_y_qp_offset_plus5 == 0);
	CHECK(pps_out.pps.pps_act_cb_qp_offset_plus5 == 0);
	CHECK(pps_out.pps.pps_act_cr_qp_offset_plus3 == 0);
	CHECK(pps_out.pps.pps_num_palette_predictor_initializers == 0U);
	CHECK(pps_out.pps.luma_bit_depth_entry_minus8 == 0U);
	CHECK(pps_out.pps.chroma_bit_depth_entry_minus8 == 0U);
	CHECK(pps_out.pps.reserved1 == 0U);
	CHECK(pps_out.pps.reserved2 == 0U);
	CHECK(pps_out.pps.reserved3 == 0U);

	/* The init_qp range of the parse (-48..51) maps to init_qp_minus26 (-74..25), which an int8_t holds. */
	for (qp = -48; qp <= 51; qp++) {
		pps_in.init_qp = qp;
		error = h265_std_pps(&pps_in, &sps_in, &pps_out);
		CHECK(error == 0);
		CHECK(pps_out.pps.init_qp_minus26 == qp - 26);
	}

	/* The default list sizes at the other end: 1 and 1, merge level 2. */
	pps_in.num_ref_idx_default[0] = 1U;
	pps_in.num_ref_idx_default[1] = 1U;
	pps_in.log2_parallel_merge_level = 2U;
	error = h265_std_pps(&pps_in, &sps_in, &pps_out);
	CHECK(error == 0);
	CHECK(pps_out.pps.num_ref_idx_l0_default_active_minus1 == 0U);
	CHECK(pps_out.pps.num_ref_idx_l1_default_active_minus1 == 0U);
	CHECK(pps_out.pps.log2_parallel_merge_level_minus2 == 0U);

	/* A missing SPS and an SPS with another id are refused. */
	error = h265_std_pps(&pps_in, NULL, &pps_out);
	CHECK(error == EINVAL);

	sps_in.id = 13U;
	error = h265_std_pps(&pps_in, &sps_in, &pps_out);
	CHECK(error == EINVAL);

	sps_in.id = 15U;
	error = h265_std_pps(&pps_in, &sps_in, &pps_out);
	CHECK(error == EINVAL);

	/* Every id of the PPS and the SPS maps. */
	for (i = 0U; i < 16U; i++) {
		sps_in.id = i;
		sps_in.vps_id = 15U - i;
		pps_in.sps_id = i;
		pps_in.id = 63U - i;
		error = h265_std_pps(&pps_in, &sps_in, &pps_out);
		CHECK(error == 0);
		CHECK(pps_out.pps.pps_seq_parameter_set_id == i);
		CHECK(pps_out.pps.sps_video_parameter_set_id == 15U - i);
		CHECK(pps_out.pps.pps_pic_parameter_set_id == 63U - i);
	}
}

/* Tests the PPS flags one at a time. */
static void
test_pps_flags(void)
{
	unsigned bits[24];
	unsigned k;
	unsigned j;
	int good;
	int error;

	/* Sets one input flag at a time and checks that exactly the matching output flag is set. */
	sps_fill(&sps_in);
	for (k = 0U; k < 24U; k++) {
		pps_fill(&pps_in);

		if (k == 0U)
			pps_in.dependent_slices = 1;
		if (k == 1U)
			pps_in.output_flag_present = 1;
		if (k == 2U)
			pps_in.sign_data_hiding = 1;
		if (k == 3U)
			pps_in.cabac_init_present = 1;
		if (k == 4U)
			pps_in.constrained_intra = 1;
		if (k == 5U)
			pps_in.transform_skip = 1;
		if (k == 6U)
			pps_in.cu_qp_delta = 1;
		if (k == 7U)
			pps_in.slice_chroma_qp_offsets_present = 1;
		if (k == 8U)
			pps_in.weighted_pred = 1;
		if (k == 9U)
			pps_in.weighted_bipred = 1;
		if (k == 10U)
			pps_in.transquant_bypass = 1;
		if (k == 11U)
			pps_in.tiles = 1;
		if (k == 12U)
			pps_in.entropy_sync = 1;
		if (k == 13U)
			pps_in.uniform_spacing = 1;
		if (k == 14U)
			pps_in.loop_filter_across_tiles = 1;
		if (k == 15U)
			pps_in.loop_filter_across_slices = 1;
		if (k == 16U)
			pps_in.deblocking_control_present = 1;
		if (k == 17U)
			pps_in.deblocking_override_enabled = 1;
		if (k == 18U)
			pps_in.deblocking_disabled = 1;
		if (k == 19U)
			pps_in.scaling_list_present = 1;
		if (k == 20U)
			pps_in.lists_modification_present = 1;
		if (k == 21U)
			pps_in.slice_header_extension_present = 1;
		if (k == 22U)
			pps_in.extension_present = 1;
		if (k == 23U)
			pps_in.range_extension_present = 1;

		error = h265_std_pps(&pps_in, &sps_in, &pps_out);
		CHECK(error == 0);

		bits[0] = pps_out.pps.flags.dependent_slice_segments_enabled_flag;
		bits[1] = pps_out.pps.flags.output_flag_present_flag;
		bits[2] = pps_out.pps.flags.sign_data_hiding_enabled_flag;
		bits[3] = pps_out.pps.flags.cabac_init_present_flag;
		bits[4] = pps_out.pps.flags.constrained_intra_pred_flag;
		bits[5] = pps_out.pps.flags.transform_skip_enabled_flag;
		bits[6] = pps_out.pps.flags.cu_qp_delta_enabled_flag;
		bits[7] = pps_out.pps.flags.pps_slice_chroma_qp_offsets_present_flag;
		bits[8] = pps_out.pps.flags.weighted_pred_flag;
		bits[9] = pps_out.pps.flags.weighted_bipred_flag;
		bits[10] = pps_out.pps.flags.transquant_bypass_enabled_flag;
		bits[11] = pps_out.pps.flags.tiles_enabled_flag;
		bits[12] = pps_out.pps.flags.entropy_coding_sync_enabled_flag;
		bits[13] = pps_out.pps.flags.uniform_spacing_flag;
		bits[14] = pps_out.pps.flags.loop_filter_across_tiles_enabled_flag;
		bits[15] = pps_out.pps.flags.pps_loop_filter_across_slices_enabled_flag;
		bits[16] = pps_out.pps.flags.deblocking_filter_control_present_flag;
		bits[17] = pps_out.pps.flags.deblocking_filter_override_enabled_flag;
		bits[18] = pps_out.pps.flags.pps_deblocking_filter_disabled_flag;
		bits[19] = pps_out.pps.flags.pps_scaling_list_data_present_flag;
		bits[20] = pps_out.pps.flags.lists_modification_present_flag;
		bits[21] = pps_out.pps.flags.slice_segment_header_extension_present_flag;
		bits[22] = pps_out.pps.flags.pps_extension_present_flag;
		bits[23] = pps_out.pps.flags.pps_range_extension_flag;

		good = 1;
		for (j = 0U; j < 24U; j++) {
			if (bits[j] != (unsigned)(j == k))
				good = 0;
		}

		CHECK(good);

		/* The flags of the extensions that the parse refuses are 0. */
		CHECK(pps_out.pps.flags.cross_component_prediction_enabled_flag == 0U);
		CHECK(pps_out.pps.flags.chroma_qp_offset_list_enabled_flag == 0U);
		CHECK(pps_out.pps.flags.pps_curr_pic_ref_enabled_flag == 0U);
		CHECK(pps_out.pps.flags.residual_adaptive_colour_transform_enabled_flag == 0U);
		CHECK(pps_out.pps.flags.pps_slice_act_qp_offsets_present_flag == 0U);
		CHECK(pps_out.pps.flags.pps_palette_predictor_initializers_present_flag == 0U);
		CHECK(pps_out.pps.flags.monochrome_palette_flag == 0U);
	}
}

/* Tests the tile structure of a PPS: counts in the minus form, the signalled sizes, the uniform spacing and the limits. */
static void
test_pps_tiles(void)
{
	int error;
	unsigned i;
	int good;

	/* Four columns and three rows with signalled sizes: the last entry of each is the rest of the picture. */
	sps_fill(&sps_in);
	pps_fill(&pps_in);
	pps_in.tiles = 1;
	pps_in.uniform_spacing = 0;
	pps_in.tile_columns = 4U;
	pps_in.tile_rows = 3U;
	pps_in.column_width[0] = 10U;
	pps_in.column_width[1] = 20U;
	pps_in.column_width[2] = 1055U;
	pps_in.column_width[3] = 99U;
	pps_in.row_height[0] = 5U;
	pps_in.row_height[1] = 1U;
	pps_in.row_height[2] = 77U;
	error = h265_std_pps(&pps_in, &sps_in, &pps_out);
	CHECK(error == 0);
	CHECK(pps_out.pps.flags.tiles_enabled_flag == 1U);
	CHECK(pps_out.pps.num_tile_columns_minus1 == 3U);
	CHECK(pps_out.pps.num_tile_rows_minus1 == 2U);
	CHECK(pps_out.pps.column_width_minus1[0] == 9U);
	CHECK(pps_out.pps.column_width_minus1[1] == 19U);
	CHECK(pps_out.pps.column_width_minus1[2] == 1054U);
	CHECK(pps_out.pps.column_width_minus1[3] == 0U);
	CHECK(pps_out.pps.row_height_minus1[0] == 4U);
	CHECK(pps_out.pps.row_height_minus1[1] == 0U);
	CHECK(pps_out.pps.row_height_minus1[2] == 0U);

	/* Uniform spacing signals no sizes: they are 0 although the input holds values. */
	pps_in.uniform_spacing = 1;
	error = h265_std_pps(&pps_in, &sps_in, &pps_out);
	CHECK(error == 0);
	CHECK(pps_out.pps.num_tile_columns_minus1 == 3U);
	CHECK(pps_out.pps.num_tile_rows_minus1 == 2U);
	CHECK(pps_out.pps.column_width_minus1[0] == 0U);
	CHECK(pps_out.pps.column_width_minus1[1] == 0U);
	CHECK(pps_out.pps.row_height_minus1[0] == 0U);

	/* Without tiles the counts are 0 and the sizes are 0, although the input holds values. */
	pps_in.tiles = 0;
	pps_in.uniform_spacing = 0;
	error = h265_std_pps(&pps_in, &sps_in, &pps_out);
	CHECK(error == 0);
	CHECK(pps_out.pps.num_tile_columns_minus1 == 0U);
	CHECK(pps_out.pps.num_tile_rows_minus1 == 0U);
	CHECK(pps_out.pps.column_width_minus1[0] == 0U);
	CHECK(pps_out.pps.row_height_minus1[0] == 0U);

	/* The most columns (20) and rows (22): the 19 and 21 signalled sizes fill their arrays. */
	pps_in.tiles = 1;
	pps_in.tile_columns = 20U;
	pps_in.tile_rows = 22U;
	for (i = 0U; i < 20U; i++)
		pps_in.column_width[i] = 100U + i;
	for (i = 0U; i < 22U; i++)
		pps_in.row_height[i] = 200U + i;
	error = h265_std_pps(&pps_in, &sps_in, &pps_out);
	CHECK(error == 0);
	CHECK(pps_out.pps.num_tile_columns_minus1 == 19U);
	CHECK(pps_out.pps.num_tile_rows_minus1 == 21U);
	good = 1;
	for (i = 0U; i < 19U; i++) {
		if (pps_out.pps.column_width_minus1[i] != 99U + i)
			good = 0;
	}

	CHECK(good);
	good = 1;
	for (i = 0U; i < 21U; i++) {
		if (pps_out.pps.row_height_minus1[i] != 199U + i)
			good = 0;
	}

	CHECK(good);

	/* More columns or rows than the arrays hold are refused. */
	pps_in.tile_columns = 21U;
	pps_in.tile_rows = 22U;
	error = h265_std_pps(&pps_in, &sps_in, &pps_out);
	CHECK(error == EINVAL);

	pps_in.tile_columns = 20U;
	pps_in.tile_rows = 23U;
	error = h265_std_pps(&pps_in, &sps_in, &pps_out);
	CHECK(error == EINVAL);

	/* A single tile in each direction. */
	pps_in.tile_columns = 1U;
	pps_in.tile_rows = 1U;
	error = h265_std_pps(&pps_in, &sps_in, &pps_out);
	CHECK(error == 0);
	CHECK(pps_out.pps.num_tile_columns_minus1 == 0U);
	CHECK(pps_out.pps.column_width_minus1[0] == 0U);
}

/*
 * Tests the picture information: the flags, the ids and the three
 * situations of the short-term set (a set of the SPS, a set explicit in
 * the slice header, a set predicted in the slice header).
 */
static void
test_picture(void)
{
	StdVideoDecodeH265PictureInfo info;
	unsigned i;
	int error;
	int good;

	/* An IDR picture: no set at all, no references. */
	memset(&picture_in, 0, sizeof(picture_in));
	memset(&plan_in, 0, sizeof(plan_in));
	picture_in.irap = 1;
	picture_in.idr = 1;
	picture_in.vps_id = 3U;
	picture_in.sps_id = 12U;
	picture_in.pps_id = 40U;
	picture_in.poc = -5;
	picture_in.header.st_rps_sps = 0;
	memset(&info, 0xCC, sizeof(info));
	error = h265_std_picture(&picture_in, &plan_in, &info);
	CHECK(error == 0);
	CHECK(info.flags.IrapPicFlag == 1U);
	CHECK(info.flags.IdrPicFlag == 1U);
	CHECK(info.flags.IsReference == 1U);
	CHECK(info.flags.short_term_ref_pic_set_sps_flag == 0U);
	CHECK(info.sps_video_parameter_set_id == 3U);
	CHECK(info.pps_seq_parameter_set_id == 12U);
	CHECK(info.pps_pic_parameter_set_id == 40U);
	CHECK(info.NumDeltaPocsOfRefRpsIdx == 0U);
	CHECK(info.PicOrderCntVal == -5);
	CHECK(info.NumBitsForSTRefPicSetInSlice == 0U);
	CHECK(info.reserved == 0U);
	good = 1;
	for (i = 0U; i < 8U; i++) {
		if (info.RefPicSetStCurrBefore[i] != 0xFFU)
			good = 0;
		if (info.RefPicSetStCurrAfter[i] != 0xFFU)
			good = 0;
		if (info.RefPicSetLtCurr[i] != 0xFFU)
			good = 0;
	}

	CHECK(good);

	/* A CRA picture is an IRAP picture but not an IDR picture. */
	picture_in.irap = 1;
	picture_in.idr = 0;
	error = h265_std_picture(&picture_in, &plan_in, &info);
	CHECK(error == 0);
	CHECK(info.flags.IrapPicFlag == 1U);
	CHECK(info.flags.IdrPicFlag == 0U);

	/* A non-IRAP picture with a set of the SPS: no bits, and no entries of a predicted set, whatever the syntax holds. */
	picture_in.irap = 0;
	picture_in.idr = 0;
	picture_in.poc = 0x7FFFFFFF;
	picture_in.header.st_rps_sps = 1;
	picture_in.header.st_syntax.bits = 77U;
	picture_in.header.st_syntax.inter_rps_pred = 1;
	picture_in.header.st_syntax.ref_num_delta_pocs = 5U;
	error = h265_std_picture(&picture_in, &plan_in, &info);
	CHECK(error == 0);
	CHECK(info.flags.IrapPicFlag == 0U);
	CHECK(info.flags.IdrPicFlag == 0U);
	CHECK(info.flags.IsReference == 1U);
	CHECK(info.flags.short_term_ref_pic_set_sps_flag == 1U);
	CHECK(info.NumBitsForSTRefPicSetInSlice == 0U);
	CHECK(info.NumDeltaPocsOfRefRpsIdx == 0U);
	CHECK(info.PicOrderCntVal == 0x7FFFFFFF);

	/* An explicit set in the slice header: its bits, and no entries of a reference set. */
	picture_in.header.st_rps_sps = 0;
	picture_in.header.st_syntax.bits = 21U;
	picture_in.header.st_syntax.inter_rps_pred = 0;
	picture_in.header.st_syntax.ref_num_delta_pocs = 7U;
	picture_in.poc = -2147483647 - 1;
	error = h265_std_picture(&picture_in, &plan_in, &info);
	CHECK(error == 0);
	CHECK(info.flags.short_term_ref_pic_set_sps_flag == 0U);
	CHECK(info.NumBitsForSTRefPicSetInSlice == 21U);
	CHECK(info.NumDeltaPocsOfRefRpsIdx == 0U);
	CHECK(info.PicOrderCntVal == -2147483647 - 1);

	/* A predicted set in the slice header: its bits, and the number of entries of the set it refers to. */
	picture_in.header.st_syntax.bits = 33U;
	picture_in.header.st_syntax.inter_rps_pred = 1;
	picture_in.header.st_syntax.ref_num_delta_pocs = 6U;
	error = h265_std_picture(&picture_in, &plan_in, &info);
	CHECK(error == 0);
	CHECK(info.NumBitsForSTRefPicSetInSlice == 33U);
	CHECK(info.NumDeltaPocsOfRefRpsIdx == 6U);

	/* The picture and its flags are written as 0 or 1 whatever non-zero value the parse kept. */
	picture_in.irap = 5;
	picture_in.idr = -3;
	picture_in.header.st_rps_sps = 9;
	error = h265_std_picture(&picture_in, &plan_in, &info);
	CHECK(error == 0);
	CHECK(info.flags.IrapPicFlag == 1U);
	CHECK(info.flags.IdrPicFlag == 1U);
	CHECK(info.flags.short_term_ref_pic_set_sps_flag == 1U);

	/* The largest id values map as they are. */
	picture_in.vps_id = 15U;
	picture_in.sps_id = 15U;
	picture_in.pps_id = 63U;
	error = h265_std_picture(&picture_in, &plan_in, &info);
	CHECK(error == 0);
	CHECK(info.sps_video_parameter_set_id == 15U);
	CHECK(info.pps_seq_parameter_set_id == 15U);
	CHECK(info.pps_pic_parameter_set_id == 63U);
}

/* Tests the three Curr lists of the picture information: slots, missing entries, the end of each list and the limits. */
static void
test_picture_lists(void)
{
	StdVideoDecodeH265PictureInfo info;
	StdVideoDecodeH265PictureInfo before;
	unsigned i;
	int error;

	/* Two entries before, one after and three long-term, one of which is missing. */
	memset(&picture_in, 0, sizeof(picture_in));
	memset(&plan_in, 0, sizeof(plan_in));
	plan_in.num_st_before = 2U;
	plan_in.st_before[0] = 3;
	plan_in.st_before[1] = 5;
	plan_in.num_st_after = 1U;
	plan_in.st_after[0] = 7;
	plan_in.num_lt_curr = 3U;
	plan_in.lt_curr[0] = H265_DPB_NO_SLOT;
	plan_in.lt_curr[1] = 0;
	plan_in.lt_curr[2] = 31;

	/* Entries past each count hold values that must not appear: the lists end with "no reference". */
	for (i = 3U; i < 16U; i++) {
		plan_in.st_before[i] = 9;
		plan_in.st_after[i] = 9;
		plan_in.lt_curr[i] = 9;
	}

	plan_in.st_before[2] = 8;
	plan_in.st_after[1] = 8;
	plan_in.lt_curr[3] = 8;
	error = h265_std_picture(&picture_in, &plan_in, &info);
	CHECK(error == 0);
	CHECK(info.RefPicSetStCurrBefore[0] == 3U);
	CHECK(info.RefPicSetStCurrBefore[1] == 5U);
	CHECK(info.RefPicSetStCurrBefore[2] == 0xFFU);
	CHECK(info.RefPicSetStCurrBefore[7] == 0xFFU);
	CHECK(info.RefPicSetStCurrAfter[0] == 7U);
	CHECK(info.RefPicSetStCurrAfter[1] == 0xFFU);
	CHECK(info.RefPicSetStCurrAfter[7] == 0xFFU);
	CHECK(info.RefPicSetLtCurr[0] == 0xFFU);
	CHECK(info.RefPicSetLtCurr[1] == 0U);
	CHECK(info.RefPicSetLtCurr[2] == 31U);
	CHECK(info.RefPicSetLtCurr[3] == 0xFFU);
	CHECK(info.RefPicSetLtCurr[7] == 0xFFU);

	/* The most entries (8) in each list, each a distinct slot. */
	plan_in.num_st_before = 8U;
	plan_in.num_st_after = 8U;
	plan_in.num_lt_curr = 8U;
	for (i = 0U; i < 8U; i++) {
		plan_in.st_before[i] = (int)i;
		plan_in.st_after[i] = (int)(10U + i);
		plan_in.lt_curr[i] = (int)(20U + i);
	}

	error = h265_std_picture(&picture_in, &plan_in, &info);
	CHECK(error == 0);
	for (i = 0U; i < 8U; i++) {
		CHECK(info.RefPicSetStCurrBefore[i] == i);
		CHECK(info.RefPicSetStCurrAfter[i] == 10U + i);
		CHECK(info.RefPicSetLtCurr[i] == 20U + i);
	}

	/* A list of 9 entries is refused, whichever list it is, and the output is left as it was. */
	memset(&before, 0xA5, sizeof(before));
	plan_in.num_st_before = 9U;
	plan_in.num_st_after = 0U;
	plan_in.num_lt_curr = 0U;
	memset(&info, 0xA5, sizeof(info));
	error = h265_std_picture(&picture_in, &plan_in, &info);
	CHECK(error == ENOTSUP);
	CHECK(memcmp(&info, &before, sizeof(info)) == 0);

	plan_in.num_st_before = 0U;
	plan_in.num_st_after = 9U;
	error = h265_std_picture(&picture_in, &plan_in, &info);
	CHECK(error == ENOTSUP);
	CHECK(memcmp(&info, &before, sizeof(info)) == 0);

	plan_in.num_st_after = 0U;
	plan_in.num_lt_curr = 9U;
	error = h265_std_picture(&picture_in, &plan_in, &info);
	CHECK(error == ENOTSUP);
	CHECK(memcmp(&info, &before, sizeof(info)) == 0);

	plan_in.num_lt_curr = 16U;
	error = h265_std_picture(&picture_in, &plan_in, &info);
	CHECK(error == ENOTSUP);

	/* A slot of the largest usable value (254) maps; a negative slot is "no reference"; 255 and more are refused. */
	plan_in.num_lt_curr = 0U;
	plan_in.num_st_before = 3U;
	plan_in.st_before[0] = 254;
	plan_in.st_before[1] = -7;
	plan_in.st_before[2] = 1;
	error = h265_std_picture(&picture_in, &plan_in, &info);
	CHECK(error == 0);
	CHECK(info.RefPicSetStCurrBefore[0] == 254U);
	CHECK(info.RefPicSetStCurrBefore[1] == 0xFFU);
	CHECK(info.RefPicSetStCurrBefore[2] == 1U);

	plan_in.st_before[0] = 255;
	error = h265_std_picture(&picture_in, &plan_in, &info);
	CHECK(error == EINVAL);

	plan_in.st_before[0] = 256;
	error = h265_std_picture(&picture_in, &plan_in, &info);
	CHECK(error == EINVAL);

	/* The slots of the other two lists are checked the same way. */
	plan_in.st_before[0] = 1;
	plan_in.num_st_after = 1U;
	plan_in.st_after[0] = 255;
	error = h265_std_picture(&picture_in, &plan_in, &info);
	CHECK(error == EINVAL);

	plan_in.st_after[0] = 2;
	plan_in.num_lt_curr = 1U;
	plan_in.lt_curr[0] = 255;
	error = h265_std_picture(&picture_in, &plan_in, &info);
	CHECK(error == EINVAL);

	plan_in.lt_curr[0] = 254;
	error = h265_std_picture(&picture_in, &plan_in, &info);
	CHECK(error == 0);
	CHECK(info.RefPicSetStCurrAfter[0] == 2U);
	CHECK(info.RefPicSetLtCurr[0] == 254U);
}

/* Tests the reference information of the references and of the setup slot. */
static void
test_references(void)
{
	StdVideoDecodeH265ReferenceInfo info[20];
	unsigned i;
	int good;

	/* Three references: the order counts, and the long-term flag of the second. */
	memset(&plan_in, 0, sizeof(plan_in));
	plan_in.reference_count = 3U;
	plan_in.reference_slot[0] = 4;
	plan_in.reference_slot[1] = 9;
	plan_in.reference_slot[2] = 2;
	plan_in.reference_poc[0] = 10;
	plan_in.reference_poc[1] = -4;
	plan_in.reference_poc[2] = 2147483647;
	plan_in.reference_long[0] = 0U;
	plan_in.reference_long[1] = 1U;
	plan_in.reference_long[2] = 0U;
	memset(info, 0xCC, sizeof(info));
	h265_std_references(&plan_in, info);
	CHECK(info[0].PicOrderCntVal == 10);
	CHECK(info[1].PicOrderCntVal == -4);
	CHECK(info[2].PicOrderCntVal == 2147483647);
	CHECK(info[0].flags.used_for_long_term_reference == 0U);
	CHECK(info[1].flags.used_for_long_term_reference == 1U);
	CHECK(info[2].flags.used_for_long_term_reference == 0U);
	CHECK(info[0].flags.unused_for_reference == 0U);
	CHECK(info[1].flags.unused_for_reference == 0U);
	CHECK(info[2].flags.unused_for_reference == 0U);

	/* The entries after the count are not written. */
	good = 1;
	for (i = 3U; i < 20U; i++) {
		unsigned char *bytes = (unsigned char *)&info[i];
		size_t b;

		for (b = 0U; b < sizeof(info[i]); b++) {
			if (bytes[b] != 0xCCU)
				good = 0;
		}
	}

	CHECK(good);

	/* No references: nothing is written. */
	plan_in.reference_count = 0U;
	memset(info, 0xCC, sizeof(info));
	h265_std_references(&plan_in, info);
	CHECK(((unsigned char *)info)[0] == 0xCCU);

	/* The long-term flag is 1 for any non-zero value. */
	plan_in.reference_count = 1U;
	plan_in.reference_long[0] = 200U;
	h265_std_references(&plan_in, info);
	CHECK(info[0].flags.used_for_long_term_reference == 1U);

	/* A count above the plan's arrays is cut to 16 entries. */
	plan_in.reference_count = 40U;
	for (i = 0U; i < 16U; i++) {
		plan_in.reference_poc[i] = (int32_t)(100U + i);
		plan_in.reference_long[i] = (uint8_t)(i & 1U);
	}

	memset(info, 0xCC, sizeof(info));
	h265_std_references(&plan_in, info);
	good = 1;
	for (i = 0U; i < 16U; i++) {
		if (info[i].PicOrderCntVal != (int32_t)(100U + i))
			good = 0;
		if (info[i].flags.used_for_long_term_reference != (i & 1U))
			good = 0;
	}

	CHECK(good);
	CHECK(info[16].PicOrderCntVal == (int32_t)0xCCCCCCCCU);

	/* The setup slot: the order count, both flags 0. */
	memset(info, 0xCC, sizeof(info));
	h265_std_setup(-123456, info);
	CHECK(info[0].PicOrderCntVal == -123456);
	CHECK(info[0].flags.used_for_long_term_reference == 0U);
	CHECK(info[0].flags.unused_for_reference == 0U);

	h265_std_setup(2147483647, info);
	CHECK(info[0].PicOrderCntVal == 2147483647);
	CHECK(info[0].flags.used_for_long_term_reference == 0U);
	CHECK(info[0].flags.unused_for_reference == 0U);
}

/* Tests random inputs: no input crashes, and every answer is one of the documented ones. */
static void
test_random(void)
{
	StdVideoDecodeH265PictureInfo info;
	StdVideoDecodeH265ReferenceInfo references[H265_DPB_PICTURES];
	unsigned n;
	unsigned i;
	int error;
	int good;

	/* Makes the StdVideo forms of random structures. */
	good = 1;
	for (n = 0U; n < RANDOM_INPUTS; n++) {
		/* A random VPS. */
		random_fill(&vps_in, sizeof(vps_in));
		error = h265_std_vps(&vps_in, &vps_out);
		if (error != 0 && error != ENOTSUP)
			good = 0;
		if (error == 0 && vps_out.vps.pDecPicBufMgr != &vps_out.dpb)
			good = 0;

		/* A random SPS, with the counts that decide which arrays are read in range often enough to be tested. */
		random_fill(&sps_in, sizeof(sps_in));
		sps_in.num_st_rps = random_next() % 70U;
		sps_in.num_lt_sps = random_next() % 40U;
		for (i = 0U; i < sps_in.num_st_rps && i < 64U; i++) {
			sps_in.st_rps[i].num_negative = random_next() % 18U;
			sps_in.st_rps[i].num_positive = random_next() % 18U;
			sps_in.st_syntax[i].num_negative_pics = random_next() % 18U;
			sps_in.st_syntax[i].num_positive_pics = random_next() % 18U;
			sps_in.st_syntax[i].inter_rps_pred = (int)(random_next() & 1U);
			if ((random_next() & 3U) != 0U) {
				sps_in.st_rps[i].delta_poc[0] = -(int32_t)(random_next() % 40000U);
				sps_in.st_rps[i].delta_poc[1] = -(int32_t)(random_next() % 40000U);
			}
		}

		error = h265_std_sps(&sps_in, &sps_out);
		if (error != 0 && error != ENOTSUP && error != EINVAL)
			good = 0;
		if (error == 0 && sps_out.sps.pProfileTierLevel != &sps_out.ptl)
			good = 0;

		/* A random PPS of the random SPS (or of one with another id), with tile counts near their limits. */
		random_fill(&pps_in, sizeof(pps_in));
		pps_in.tile_columns = random_next() % 24U;
		pps_in.tile_rows = random_next() % 26U;
		if ((random_next() & 1U) != 0U)
			pps_in.sps_id = sps_in.id;
		error = h265_std_pps(&pps_in, &sps_in, &pps_out);
		if (error != 0 && error != EINVAL)
			good = 0;

		/* A random picture and plan, with the list counts near their limits and slots near the "no reference" value. */
		random_fill(&picture_in, sizeof(picture_in));
		random_fill(&plan_in, sizeof(plan_in));
		plan_in.num_st_before = random_next() % 11U;
		plan_in.num_st_after = random_next() % 11U;
		plan_in.num_lt_curr = random_next() % 11U;
		for (i = 0U; i < 16U; i++) {
			plan_in.st_before[i] = (int)(random_next() % 300U) - 3;
			plan_in.st_after[i] = (int)(random_next() % 300U) - 3;
			plan_in.lt_curr[i] = (int)(random_next() % 300U) - 3;
		}

		error = h265_std_picture(&picture_in, &plan_in, &info);
		if (error != 0 && error != ENOTSUP && error != EINVAL)
			good = 0;

		/* The references of the same plan, with a count that may be above the arrays. */
		plan_in.reference_count = random_next() % 50U;
		h265_std_references(&plan_in, references);
	}

	CHECK(good);
}

/* Runs every test and reports the count. */
int
main(void)
{
	test_level();
	test_profile();
	test_ptl_flags();
	test_hrd();
	test_vps();
	test_sps_values();
	test_sps_flags();
	test_sps_pointers();
	test_vui();
	test_scaling();
	test_short_term_sets();
	test_short_term_limits();
	test_pps_values();
	test_pps_flags();
	test_pps_tiles();
	test_picture();
	test_picture_lists();
	test_references();
	test_random();

	return check_finish("std");
}
