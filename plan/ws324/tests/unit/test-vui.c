/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Tests of h265sw-vui: vui_parameters() (E.2.1, E.3.1), hrd_parameters()
 * (E.2.2, E.3.2) and sub_layer_hrd_parameters() (E.2.3, E.3.3).
 *
 * The inputs are written by the syntax writer of h265write.h from the
 * syntax tables of Annex E.  The expected values are the values written
 * and, where an element is absent, the inferred values of E.3 written out
 * here by hand; the sample aspect ratios are the rows of Table E.1 as text
 * of the table (aspect_ratio_idc, width, height) typed in this file.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "h265write.h"
#include "userland/desktop/libmedia/h265sw-vui.h"

/* The number of random inputs of the round-trip and the robustness checks. */
#define ROUND_TRIP_INPUTS 4000U
#define RANDOM_INPUTS 100000U

/* The largest value of an Exp-Golomb coded element: codeNum 2^32 - 2. */
#define UE_MAX 0xFFFFFFFEU

/* The state of the pseudo-random generator, fixed so a failure repeats. */
static uint32_t random_state = 0x6C078965U;

/* The writer, the syntax values and the read results, too large for the stack of a check. */
static struct bitwriter writer;
static struct h265w_vui vui_syntax;
static struct h265w_hrd hrd_syntax;
static struct h265w_hrd hrd_previous_syntax;
static struct h265sw_vui vui_result;
static struct h265sw_hrd hrd_result;
static struct h265sw_hrd hrd_previous_result;
static struct h265sw_sub_layer_hrd zero_sub_layer;

/* The bits of the last written structure before padding, and its bytes. */
static size_t last_bits;
static size_t last_bytes;

/*
 * The sample aspect ratios of Table E.1: aspect_ratio_idc 1..16 and the
 * two terms of the ratio, as in the "Sample aspect ratio" column.
 */
static const unsigned table_e1[16][3] = {
	{1U, 1U, 1U},
	{2U, 12U, 11U},
	{3U, 10U, 11U},
	{4U, 16U, 11U},
	{5U, 40U, 33U},
	{6U, 24U, 11U},
	{7U, 20U, 11U},
	{8U, 32U, 11U},
	{9U, 80U, 33U},
	{10U, 18U, 11U},
	{11U, 15U, 11U},
	{12U, 64U, 33U},
	{13U, 160U, 99U},
	{14U, 4U, 3U},
	{15U, 3U, 2U},
	{16U, 2U, 1U}
};

/* Static function declarations. */
static uint32_t random_next(void);
static uint32_t random_ue(void);
static void hrd_random(struct h265w_hrd *hrd, unsigned max_sub, unsigned nal, unsigned vcl, unsigned sub_pic);
static void vui_random(struct h265w_vui *vui, unsigned max_sub);
static int hrd_roundtrip(const struct h265w_hrd *hrd, const struct h265w_hrd *common, int common_present, unsigned max_sub, const struct h265sw_hrd *previous);
static int vui_roundtrip(const struct h265w_vui *vui, unsigned max_sub, size_t cut);
static void hrd_compare(const struct h265w_hrd *hrd, const struct h265w_hrd *common, const struct h265sw_hrd *read, unsigned max_sub);
static void vui_compare(const struct h265w_vui *vui, const struct h265sw_vui *read, unsigned max_sub);
static void check_absent(void);
static void check_sar(void);
static void check_vui_round_trip(void);
static void check_vui_ranges(void);
static void check_hrd_combinations(void);
static void check_hrd_common(void);
static void check_hrd_ranges(void);
static void check_truncated(void);
static void check_random(void);

/* Returns the next pseudo-random number (xorshift32). */
static uint32_t
random_next(void)
{
	/* The three shifts of the generator. */
	random_state ^= random_state << 13;
	random_state ^= random_state >> 17;
	random_state ^= random_state << 5;

	/* Succeeded: the next number. */
	return random_state;
}

/* Returns a pseudo-random ue(v) value: a third small, a third mid-sized, a third of any size up to the largest. */
static uint32_t
random_ue(void)
{
	uint32_t kind;
	uint32_t value;

	/* Picks the size class, and the value in it. */
	kind = random_next() % 3U;
	if (kind == 0U) {
		value = random_next() % 16U;
	} else if (kind == 1U) {
		value = random_next() % 65536U;
	} else {
		value = random_next();
		if (value == 0xFFFFFFFFU)
			value = UE_MAX;
	}

	/* Succeeded: a codable value. */
	return value;
}

/* Fills hrd with random valid values for a given presence of the NAL, VCL and sub-picture parts. */
static void
hrd_random(
	struct h265w_hrd *hrd,
	unsigned max_sub,
	unsigned nal,
	unsigned vcl,
	unsigned sub_pic)
{
	unsigned sub_layer;
	unsigned index;

	/* The common part. */
	memset(hrd, 0, sizeof(*hrd));
	hrd->nal_present = nal;
	hrd->vcl_present = vcl;
	hrd->sub_pic_present = sub_pic;
	hrd->tick_divisor_minus2 = random_next() % 256U;
	hrd->du_cpb_removal_delay_increment_length_minus1 = random_next() % 32U;
	hrd->sub_pic_cpb_params_in_pic_timing_sei = random_next() % 2U;
	hrd->dpb_output_delay_du_length_minus1 = random_next() % 32U;
	hrd->bit_rate_scale = random_next() % 16U;
	hrd->cpb_size_scale = random_next() % 16U;
	hrd->cpb_size_du_scale = random_next() % 16U;
	hrd->initial_cpb_removal_delay_length_minus1 = random_next() % 32U;
	hrd->au_cpb_removal_delay_length_minus1 = random_next() % 32U;
	hrd->dpb_output_delay_length_minus1 = random_next() % 32U;

	/* Each sub-layer, with the CPB count and elemental duration in range and every ue value anywhere. */
	for (sub_layer = 0U; sub_layer <= max_sub; sub_layer++) {
		hrd->fixed_pic_rate_general[sub_layer] = random_next() % 2U;
		hrd->fixed_pic_rate_within_cvs[sub_layer] = random_next() % 2U;
		hrd->low_delay_hrd[sub_layer] = random_next() % 2U;
		hrd->elemental_duration_in_tc_minus1[sub_layer] = random_next() % 2048U;
		if ((random_next() % 4U) == 0U)
			hrd->elemental_duration_in_tc_minus1[sub_layer] = 2047U;
		hrd->cpb_cnt_minus1[sub_layer] = random_next() % 32U;
		if ((random_next() % 4U) == 0U)
			hrd->cpb_cnt_minus1[sub_layer] = 31U;
		for (index = 0U; index < H265W_MAX_CPB; index++) {
			hrd->nal[sub_layer].bit_rate_value_minus1[index] = random_ue();
			hrd->nal[sub_layer].cpb_size_value_minus1[index] = random_ue();
			hrd->nal[sub_layer].cpb_size_du_value_minus1[index] = random_ue();
			hrd->nal[sub_layer].bit_rate_du_value_minus1[index] = random_ue();
			hrd->nal[sub_layer].cbr_flag[index] = random_next() % 2U;
			hrd->vcl[sub_layer].bit_rate_value_minus1[index] = random_ue();
			hrd->vcl[sub_layer].cpb_size_value_minus1[index] = random_ue();
			hrd->vcl[sub_layer].cpb_size_du_value_minus1[index] = random_ue();
			hrd->vcl[sub_layer].bit_rate_du_value_minus1[index] = random_ue();
			hrd->vcl[sub_layer].cbr_flag[index] = random_next() % 2U;
		}
	}
}

/* Fills vui with random valid values, every branch chosen at random. */
static void
vui_random(
	struct h265w_vui *vui,
	unsigned max_sub)
{
	unsigned index;
	unsigned kind;

	/* The aspect ratio: a table index, the explicit one, or a reserved one. */
	memset(vui, 0, sizeof(*vui));
	vui->aspect_ratio_present = random_next() % 2U;
	kind = random_next() % 4U;
	if (kind == 0U) {
		vui->aspect_ratio_idc = random_next() % 17U;
	} else if (kind == 1U) {
		vui->aspect_ratio_idc = 255U;
	} else if (kind == 2U) {
		vui->aspect_ratio_idc = 17U + random_next() % 238U;
	} else {
		vui->aspect_ratio_idc = 1U + random_next() % 16U;
	}
	vui->sar_width = random_next() % 65536U;
	vui->sar_height = random_next() % 65536U;
	if ((random_next() % 5U) == 0U)
		vui->sar_width = 0U;
	if ((random_next() % 5U) == 0U)
		vui->sar_height = 0U;

	/* Overscan, the video signal type and the colour description. */
	vui->overscan_present = random_next() % 2U;
	vui->overscan_appropriate = random_next() % 2U;
	vui->signal_present = random_next() % 2U;
	vui->video_format = random_next() % 8U;
	vui->full_range = random_next() % 2U;
	vui->colour_present = random_next() % 2U;
	vui->colour_primaries = random_next() % 256U;
	vui->transfer_characteristics = random_next() % 256U;
	vui->matrix_coeffs = random_next() % 256U;

	/* The chroma location, the three picture structure flags and the default display window. */
	vui->chroma_loc_present = random_next() % 2U;
	vui->chroma_loc_top = random_next() % 6U;
	vui->chroma_loc_bottom = random_next() % 6U;
	vui->neutral_chroma = random_next() % 2U;
	vui->field_seq = random_next() % 2U;
	vui->frame_field_info_present = random_next() % 2U;
	vui->default_display_window = random_next() % 2U;
	for (index = 0U; index < 4U; index++)
		vui->def_disp_win_offset[index] = random_next() % (H265SW_MAX_SIDE + 1U);

	/* The timing, with the proportion and the HRD parameters inside it. */
	vui->timing_present = random_next() % 2U;
	vui->num_units_in_tick = 1U + random_next() % 0xFFFFFFFFU;
	vui->time_scale = 1U + random_next() % 0xFFFFFFFFU;
	vui->poc_proportional = random_next() % 2U;
	vui->num_ticks_poc_diff_one_minus1 = random_ue();
	vui->hrd_present = random_next() % 2U;
	hrd_random(&vui->hrd, max_sub, random_next() % 2U, random_next() % 2U, random_next() % 2U);

	/* The bitstream restriction. */
	vui->bitstream_restriction = random_next() % 2U;
	vui->tiles_fixed_structure = random_next() % 2U;
	vui->motion_vectors_over_pic_boundaries = random_next() % 2U;
	vui->restricted_ref_pic_lists = random_next() % 2U;
	vui->min_spatial_segmentation_idc = random_next() % 4096U;
	vui->max_bytes_per_pic_denom = random_next() % 17U;
	vui->max_bits_per_min_cu_denom = random_next() % 17U;
	vui->log2_max_mv_length_horizontal = random_next() % 16U;
	vui->log2_max_mv_length_vertical = random_next() % 16U;
}

/*
 * Writes hrd as hrd_parameters( common_present, max_sub ) and reads it into
 * hrd_result with previous; common is the structure the common fields come
 * from when they are not written.  Returns the reader's answer.
 */
static int
hrd_roundtrip(
	const struct h265w_hrd *hrd,
	const struct h265w_hrd *common,
	int common_present,
	unsigned max_sub,
	const struct h265sw_hrd *previous)
{
	struct vcodec_bitreader bits;

	/* UNUSED: the writer takes the presence flags from hrd itself. */
	UNUSED_PARAMETER(common);

	/* Writes the syntax. */
	bitwriter_init(&writer);
	h265w_hrd(&writer, hrd, common_present, max_sub);
	last_bits = writer.bits;
	last_bytes = bitwriter_bytes(&writer);

	/* Reads it into a structure that holds a value the reader must overwrite. */
	memset(&hrd_result, 0xCC, sizeof(hrd_result));
	vcodec_bitreader_init(&bits, writer.data, last_bytes);

	/* Succeeded: the reader's answer. */
	return h265sw_vui_read_hrd(&hrd_result, &bits, common_present, max_sub, previous);
}

/* Writes vui and reads it into vui_result, using only the first cut bytes when cut is shorter. */
static int
vui_roundtrip(
	const struct h265w_vui *vui,
	unsigned max_sub,
	size_t cut)
{
	struct vcodec_bitreader bits;
	size_t bytes;

	/* Writes the syntax. */
	bitwriter_init(&writer);
	h265w_vui(&writer, vui, max_sub);
	last_bits = writer.bits;
	last_bytes = bitwriter_bytes(&writer);
	bytes = last_bytes;
	if (cut < bytes)
		bytes = cut;

	/* Reads it into a structure that holds a value the reader must overwrite. */
	memset(&vui_result, 0xCC, sizeof(vui_result));
	vcodec_bitreader_init(&bits, writer.data, bytes);

	/* Succeeded: the reader's answer. */
	return h265sw_vui_read(&vui_result, &bits, max_sub);
}

/*
 * Checks a read HRD against the syntax values written and the inferences of
 * E.3.2.  The common fields are expected from common, the sub-layer fields
 * from hrd.
 */
static void
hrd_compare(
	const struct h265w_hrd *hrd,
	const struct h265w_hrd *common,
	const struct h265sw_hrd *read,
	unsigned max_sub)
{
	unsigned sub_layer;
	unsigned index;
	unsigned any;
	unsigned sub_pic;
	unsigned general;
	unsigned within;
	unsigned low_delay;
	unsigned count;
	uint32_t cbr;

	/* The presence flags are the ones the sub-layers were written with. */
	CHECK((unsigned)read->nal_hrd_present == common->nal_present);
	CHECK((unsigned)read->vcl_hrd_present == common->vcl_present);
	any = (common->nal_present != 0U || common->vcl_present != 0U) ? 1U : 0U;
	sub_pic = (any != 0U) ? common->sub_pic_present : 0U;
	CHECK((unsigned)read->sub_pic_hrd_params_present == sub_pic);

	/* The common fields: as written when present, 0 for the parts that are absent, 23 for the three lengths. */
	if (sub_pic != 0U) {
		CHECK(read->tick_divisor_minus2 == common->tick_divisor_minus2);
		CHECK(read->du_cpb_removal_delay_increment_length_minus1 == common->du_cpb_removal_delay_increment_length_minus1);
		CHECK((unsigned)read->sub_pic_cpb_params_in_pic_timing_sei == common->sub_pic_cpb_params_in_pic_timing_sei);
		CHECK(read->dpb_output_delay_du_length_minus1 == common->dpb_output_delay_du_length_minus1);
		CHECK(read->cpb_size_du_scale == common->cpb_size_du_scale);
	} else {
		CHECK(read->tick_divisor_minus2 == 0U);
		CHECK(read->du_cpb_removal_delay_increment_length_minus1 == 0U);
		CHECK(read->sub_pic_cpb_params_in_pic_timing_sei == 0);
		CHECK(read->dpb_output_delay_du_length_minus1 == 0U);
		CHECK(read->cpb_size_du_scale == 0U);
	}
	if (any != 0U) {
		CHECK(read->bit_rate_scale == common->bit_rate_scale);
		CHECK(read->cpb_size_scale == common->cpb_size_scale);
		CHECK(read->initial_cpb_removal_delay_length_minus1 == common->initial_cpb_removal_delay_length_minus1);
		CHECK(read->au_cpb_removal_delay_length_minus1 == common->au_cpb_removal_delay_length_minus1);
		CHECK(read->dpb_output_delay_length_minus1 == common->dpb_output_delay_length_minus1);
	} else {
		CHECK(read->bit_rate_scale == 0U);
		CHECK(read->cpb_size_scale == 0U);
		CHECK(read->initial_cpb_removal_delay_length_minus1 == 23U);
		CHECK(read->au_cpb_removal_delay_length_minus1 == 23U);
		CHECK(read->dpb_output_delay_length_minus1 == 23U);
	}

	/* Each sub-layer. */
	for (sub_layer = 0U; sub_layer < H265SW_MAX_SUB_LAYERS; sub_layer++) {
		if (sub_layer > max_sub) {
			/* Sub-layers above the last are all 0. */
			CHECK(read->fixed_pic_rate_general[sub_layer] == 0U);
			CHECK(read->fixed_pic_rate_within_cvs[sub_layer] == 0U);
			CHECK(read->low_delay_hrd[sub_layer] == 0U);
			CHECK(read->elemental_duration_in_tc_minus1[sub_layer] == 0U);
			CHECK(read->cpb_cnt_minus1[sub_layer] == 0U);
			CHECK(memcmp(&read->nal[sub_layer], &zero_sub_layer, sizeof(zero_sub_layer)) == 0);
			CHECK(memcmp(&read->vcl[sub_layer], &zero_sub_layer, sizeof(zero_sub_layer)) == 0);
			continue;
		}

		/* What E.3.2 infers: the within-CVS flag is 1 under the general one, the duration and the low delay flag are absent otherwise. */
		general = hrd->fixed_pic_rate_general[sub_layer];
		within = (general != 0U) ? 1U : hrd->fixed_pic_rate_within_cvs[sub_layer];
		low_delay = (within != 0U) ? 0U : hrd->low_delay_hrd[sub_layer];
		count = (low_delay != 0U) ? 1U : hrd->cpb_cnt_minus1[sub_layer] + 1U;
		CHECK(read->fixed_pic_rate_general[sub_layer] == general);
		CHECK(read->fixed_pic_rate_within_cvs[sub_layer] == within);
		CHECK(read->low_delay_hrd[sub_layer] == low_delay);
		CHECK(read->elemental_duration_in_tc_minus1[sub_layer] == ((within != 0U) ? hrd->elemental_duration_in_tc_minus1[sub_layer] : 0U));
		CHECK(read->cpb_cnt_minus1[sub_layer] == count - 1U);

		/* The CPB specifications of the NAL part, then the VCL part: as written, 0 past the count and when absent. */
		for (index = 0U; index < H265SW_MAX_CPB; index++) {
			if (index < count && common->nal_present != 0U) {
				CHECK(read->nal[sub_layer].bit_rate_value_minus1[index] == hrd->nal[sub_layer].bit_rate_value_minus1[index]);
				CHECK(read->nal[sub_layer].cpb_size_value_minus1[index] == hrd->nal[sub_layer].cpb_size_value_minus1[index]);
				if (sub_pic != 0U) {
					CHECK(read->nal[sub_layer].cpb_size_du_value_minus1[index] == hrd->nal[sub_layer].cpb_size_du_value_minus1[index]);
					CHECK(read->nal[sub_layer].bit_rate_du_value_minus1[index] == hrd->nal[sub_layer].bit_rate_du_value_minus1[index]);
				} else {
					CHECK(read->nal[sub_layer].cpb_size_du_value_minus1[index] == 0U);
					CHECK(read->nal[sub_layer].bit_rate_du_value_minus1[index] == 0U);
				}
				cbr = (read->nal[sub_layer].cbr_flags >> index) & 1U;
				CHECK(cbr == hrd->nal[sub_layer].cbr_flag[index]);
			} else {
				CHECK(read->nal[sub_layer].bit_rate_value_minus1[index] == 0U);
				CHECK(read->nal[sub_layer].cpb_size_value_minus1[index] == 0U);
				CHECK(read->nal[sub_layer].cpb_size_du_value_minus1[index] == 0U);
				CHECK(read->nal[sub_layer].bit_rate_du_value_minus1[index] == 0U);
				CHECK(((read->nal[sub_layer].cbr_flags >> index) & 1U) == 0U);
			}
			if (index < count && common->vcl_present != 0U) {
				CHECK(read->vcl[sub_layer].bit_rate_value_minus1[index] == hrd->vcl[sub_layer].bit_rate_value_minus1[index]);
				CHECK(read->vcl[sub_layer].cpb_size_value_minus1[index] == hrd->vcl[sub_layer].cpb_size_value_minus1[index]);
				if (sub_pic != 0U) {
					CHECK(read->vcl[sub_layer].cpb_size_du_value_minus1[index] == hrd->vcl[sub_layer].cpb_size_du_value_minus1[index]);
					CHECK(read->vcl[sub_layer].bit_rate_du_value_minus1[index] == hrd->vcl[sub_layer].bit_rate_du_value_minus1[index]);
				} else {
					CHECK(read->vcl[sub_layer].cpb_size_du_value_minus1[index] == 0U);
					CHECK(read->vcl[sub_layer].bit_rate_du_value_minus1[index] == 0U);
				}
				cbr = (read->vcl[sub_layer].cbr_flags >> index) & 1U;
				CHECK(cbr == hrd->vcl[sub_layer].cbr_flag[index]);
			} else {
				CHECK(read->vcl[sub_layer].bit_rate_value_minus1[index] == 0U);
				CHECK(read->vcl[sub_layer].cpb_size_value_minus1[index] == 0U);
				CHECK(read->vcl[sub_layer].cpb_size_du_value_minus1[index] == 0U);
				CHECK(read->vcl[sub_layer].bit_rate_du_value_minus1[index] == 0U);
				CHECK(((read->vcl[sub_layer].cbr_flags >> index) & 1U) == 0U);
			}
		}
	}
}

/* Checks a read VUI against the syntax values written and the inferences of E.3.1. */
static void
vui_compare(
	const struct h265w_vui *vui,
	const struct h265sw_vui *read,
	unsigned max_sub)
{
	unsigned index;
	unsigned expected_width;
	unsigned expected_height;

	/* The aspect ratio and the derived sample aspect ratio (Table E.1 and E.3.1). */
	CHECK((unsigned)read->aspect_ratio_present == vui->aspect_ratio_present);
	expected_width = 0U;
	expected_height = 0U;
	if (vui->aspect_ratio_present != 0U) {
		CHECK(read->aspect_ratio_idc == vui->aspect_ratio_idc);
		if (vui->aspect_ratio_idc == 255U) {
			CHECK(read->signalled_sar_width == vui->sar_width);
			CHECK(read->signalled_sar_height == vui->sar_height);
			if (vui->sar_width != 0U && vui->sar_height != 0U) {
				expected_width = vui->sar_width;
				expected_height = vui->sar_height;
			}
		} else {
			CHECK(read->signalled_sar_width == 0U);
			CHECK(read->signalled_sar_height == 0U);
			for (index = 0U; index < 16U; index++) {
				if (table_e1[index][0] == vui->aspect_ratio_idc) {
					expected_width = table_e1[index][1];
					expected_height = table_e1[index][2];
				}
			}
		}
	} else {
		CHECK(read->aspect_ratio_idc == 0U);
		CHECK(read->signalled_sar_width == 0U);
		CHECK(read->signalled_sar_height == 0U);
	}
	CHECK(read->sar_width == expected_width);
	CHECK(read->sar_height == expected_height);

	/* The overscan. */
	CHECK((unsigned)read->overscan_present == vui->overscan_present);
	CHECK((unsigned)read->overscan_appropriate == ((vui->overscan_present != 0U) ? vui->overscan_appropriate : 0U));

	/* The video signal type: 5 and 0 when absent, the colour description 2 when absent. */
	CHECK((unsigned)read->signal_present == vui->signal_present);
	if (vui->signal_present != 0U) {
		CHECK(read->video_format == vui->video_format);
		CHECK((unsigned)read->full_range == vui->full_range);
		CHECK((unsigned)read->colour_present == vui->colour_present);
	} else {
		CHECK(read->video_format == 5U);
		CHECK(read->full_range == 0);
		CHECK(read->colour_present == 0);
	}
	if (vui->signal_present != 0U && vui->colour_present != 0U) {
		CHECK(read->colour_primaries == vui->colour_primaries);
		CHECK(read->transfer_characteristics == vui->transfer_characteristics);
		CHECK(read->matrix_coeffs == vui->matrix_coeffs);
	} else {
		CHECK(read->colour_primaries == 2U);
		CHECK(read->transfer_characteristics == 2U);
		CHECK(read->matrix_coeffs == 2U);
	}

	/* The chroma location. */
	CHECK((unsigned)read->chroma_loc_present == vui->chroma_loc_present);
	CHECK(read->chroma_sample_loc_type_top_field == ((vui->chroma_loc_present != 0U) ? vui->chroma_loc_top : 0U));
	CHECK(read->chroma_sample_loc_type_bottom_field == ((vui->chroma_loc_present != 0U) ? vui->chroma_loc_bottom : 0U));

	/* The three picture structure flags and the default display window, left, right, top, bottom. */
	CHECK((unsigned)read->neutral_chroma_indication == vui->neutral_chroma);
	CHECK((unsigned)read->field_seq == vui->field_seq);
	CHECK((unsigned)read->frame_field_info_present == vui->frame_field_info_present);
	CHECK((unsigned)read->default_display_window == vui->default_display_window);
	CHECK(read->def_disp_win_left_offset == ((vui->default_display_window != 0U) ? vui->def_disp_win_offset[0] : 0U));
	CHECK(read->def_disp_win_right_offset == ((vui->default_display_window != 0U) ? vui->def_disp_win_offset[1] : 0U));
	CHECK(read->def_disp_win_top_offset == ((vui->default_display_window != 0U) ? vui->def_disp_win_offset[2] : 0U));
	CHECK(read->def_disp_win_bottom_offset == ((vui->default_display_window != 0U) ? vui->def_disp_win_offset[3] : 0U));

	/* The timing information. */
	CHECK((unsigned)read->timing_present == vui->timing_present);
	if (vui->timing_present != 0U) {
		CHECK(read->num_units_in_tick == vui->num_units_in_tick);
		CHECK(read->time_scale == vui->time_scale);
		CHECK((unsigned)read->poc_proportional_to_timing == vui->poc_proportional);
		CHECK(read->num_ticks_poc_diff_one_minus1 == ((vui->poc_proportional != 0U) ? vui->num_ticks_poc_diff_one_minus1 : 0U));
		CHECK((unsigned)read->hrd_present == vui->hrd_present);
	} else {
		CHECK(read->num_units_in_tick == 0U);
		CHECK(read->time_scale == 0U);
		CHECK(read->poc_proportional_to_timing == 0);
		CHECK(read->num_ticks_poc_diff_one_minus1 == 0U);
		CHECK(read->hrd_present == 0);
	}

	/* The HRD: as written when present, all 0 when absent. */
	if (vui->timing_present != 0U && vui->hrd_present != 0U) {
		hrd_compare(&vui->hrd, &vui->hrd, &read->hrd, max_sub);
	} else {
		memset(&hrd_previous_result, 0, sizeof(hrd_previous_result));
		CHECK(memcmp(&read->hrd, &hrd_previous_result, sizeof(read->hrd)) == 0);
	}

	/* The bitstream restriction, with the inferred values when absent. */
	CHECK((unsigned)read->bitstream_restriction == vui->bitstream_restriction);
	if (vui->bitstream_restriction != 0U) {
		CHECK((unsigned)read->tiles_fixed_structure == vui->tiles_fixed_structure);
		CHECK((unsigned)read->motion_vectors_over_pic_boundaries == vui->motion_vectors_over_pic_boundaries);
		CHECK((unsigned)read->restricted_ref_pic_lists == vui->restricted_ref_pic_lists);
		CHECK(read->min_spatial_segmentation_idc == vui->min_spatial_segmentation_idc);
		CHECK(read->max_bytes_per_pic_denom == vui->max_bytes_per_pic_denom);
		CHECK(read->max_bits_per_min_cu_denom == vui->max_bits_per_min_cu_denom);
		CHECK(read->log2_max_mv_length_horizontal == vui->log2_max_mv_length_horizontal);
		CHECK(read->log2_max_mv_length_vertical == vui->log2_max_mv_length_vertical);
	} else {
		CHECK(read->tiles_fixed_structure == 0);
		CHECK(read->motion_vectors_over_pic_boundaries == 1);
		CHECK(read->restricted_ref_pic_lists == 0);
		CHECK(read->min_spatial_segmentation_idc == 0U);
		CHECK(read->max_bytes_per_pic_denom == 2U);
		CHECK(read->max_bits_per_min_cu_denom == 1U);
		CHECK(read->log2_max_mv_length_horizontal == 15U);
		CHECK(read->log2_max_mv_length_vertical == 15U);
	}
}

/* Checks a VUI with every element absent: ten flag bits and all the inferred values. */
static void
check_absent(void)
{
	unsigned max_sub;
	int error;

	/* The same for every sub-layer count. */
	for (max_sub = 0U; max_sub < 7U; max_sub++) {
		memset(&vui_syntax, 0, sizeof(vui_syntax));
		error = vui_roundtrip(&vui_syntax, max_sub, (size_t)-1);
		CHECK(error == 0);
		CHECK(last_bits == 10U);
		vui_compare(&vui_syntax, &vui_result, max_sub);

		/* The inferred values, spelled out from E.3.1. */
		CHECK(vui_result.video_format == 5U);
		CHECK(vui_result.colour_primaries == 2U);
		CHECK(vui_result.transfer_characteristics == 2U);
		CHECK(vui_result.matrix_coeffs == 2U);
		CHECK(vui_result.motion_vectors_over_pic_boundaries == 1);
		CHECK(vui_result.max_bytes_per_pic_denom == 2U);
		CHECK(vui_result.max_bits_per_min_cu_denom == 1U);
		CHECK(vui_result.log2_max_mv_length_horizontal == 15U);
		CHECK(vui_result.log2_max_mv_length_vertical == 15U);
		CHECK(vui_result.sar_width == 0U);
		CHECK(vui_result.sar_height == 0U);
	}
}

/* Checks the derived sample aspect ratio for every aspect_ratio_idc against Table E.1. */
static void
check_sar(void)
{
	unsigned idc;
	unsigned index;
	unsigned width;
	unsigned height;
	int error;

	/* Every index with the ratio present; the signalled terms are junk that must be ignored unless 255. */
	for (idc = 0U; idc < 256U; idc++) {
		memset(&vui_syntax, 0, sizeof(vui_syntax));
		vui_syntax.aspect_ratio_present = 1U;
		vui_syntax.aspect_ratio_idc = idc;
		vui_syntax.sar_width = 7U;
		vui_syntax.sar_height = 5U;
		error = vui_roundtrip(&vui_syntax, 0U, (size_t)-1);
		CHECK(error == 0);
		CHECK(vui_result.aspect_ratio_idc == idc);

		/* Table E.1, 1..16; 255 is the signalled 7:5; 0 and 17..254 are unspecified. */
		width = 0U;
		height = 0U;
		for (index = 0U; index < 16U; index++) {
			if (table_e1[index][0] == idc) {
				width = table_e1[index][1];
				height = table_e1[index][2];
			}
		}
		if (idc == 255U) {
			width = 7U;
			height = 5U;
		}
		CHECK(vui_result.sar_width == width);
		CHECK(vui_result.sar_height == height);
	}

	/* The explicit ratio with a zero term is unspecified, whichever term it is. */
	for (index = 0U; index < 4U; index++) {
		memset(&vui_syntax, 0, sizeof(vui_syntax));
		vui_syntax.aspect_ratio_present = 1U;
		vui_syntax.aspect_ratio_idc = 255U;
		vui_syntax.sar_width = (index & 1U) ? 3U : 0U;
		vui_syntax.sar_height = (index & 2U) ? 4U : 0U;
		error = vui_roundtrip(&vui_syntax, 0U, (size_t)-1);
		CHECK(error == 0);
		CHECK(vui_result.signalled_sar_width == vui_syntax.sar_width);
		CHECK(vui_result.signalled_sar_height == vui_syntax.sar_height);
		if (index == 3U) {
			CHECK(vui_result.sar_width == 3U);
			CHECK(vui_result.sar_height == 4U);
		} else {
			CHECK(vui_result.sar_width == 0U);
			CHECK(vui_result.sar_height == 0U);
		}
	}

	/* The largest terms. */
	memset(&vui_syntax, 0, sizeof(vui_syntax));
	vui_syntax.aspect_ratio_present = 1U;
	vui_syntax.aspect_ratio_idc = 255U;
	vui_syntax.sar_width = 65535U;
	vui_syntax.sar_height = 65534U;
	error = vui_roundtrip(&vui_syntax, 0U, (size_t)-1);
	CHECK(error == 0);
	CHECK(vui_result.sar_width == 65535U);
	CHECK(vui_result.sar_height == 65534U);

	/* A ratio index with the present flag 0 is not read: the flag decides. */
	memset(&vui_syntax, 0, sizeof(vui_syntax));
	vui_syntax.aspect_ratio_idc = 3U;
	error = vui_roundtrip(&vui_syntax, 0U, (size_t)-1);
	CHECK(error == 0);
	CHECK(vui_result.sar_width == 0U);
	CHECK(vui_result.sar_height == 0U);
}

/* Checks random complete VUIs, every branch, and the single-branch cases of the list of the Phase. */
static void
check_vui_round_trip(void)
{
	unsigned round;
	unsigned max_sub;
	int error;

	/* Random VUIs with HRD parameters of random shape. */
	for (round = 0U; round < ROUND_TRIP_INPUTS; round++) {
		max_sub = random_next() % 7U;
		vui_random(&vui_syntax, max_sub);
		error = vui_roundtrip(&vui_syntax, max_sub, (size_t)-1);
		CHECK(error == 0);
		if (error == 0)
			vui_compare(&vui_syntax, &vui_result, max_sub);
	}

	/* Every present flag alone, and all of them together. */
	for (round = 0U; round < 10U; round++) {
		vui_random(&vui_syntax, 2U);
		vui_syntax.aspect_ratio_present = (round == 0U || round == 9U) ? 1U : 0U;
		vui_syntax.overscan_present = (round == 1U || round == 9U) ? 1U : 0U;
		vui_syntax.signal_present = (round == 2U || round == 9U) ? 1U : 0U;
		vui_syntax.chroma_loc_present = (round == 3U || round == 9U) ? 1U : 0U;
		vui_syntax.default_display_window = (round == 4U || round == 9U) ? 1U : 0U;
		vui_syntax.timing_present = (round == 5U || round == 9U) ? 1U : 0U;
		vui_syntax.hrd_present = (round == 6U || round == 9U) ? 1U : 0U;
		vui_syntax.poc_proportional = (round == 7U || round == 9U) ? 1U : 0U;
		vui_syntax.bitstream_restriction = (round == 8U || round == 9U) ? 1U : 0U;
		if (round == 6U)
			vui_syntax.timing_present = 1U;
		vui_syntax.colour_present = (round == 2U || round == 9U) ? 1U : 0U;
		error = vui_roundtrip(&vui_syntax, 2U, (size_t)-1);
		CHECK(error == 0);
		if (error == 0)
			vui_compare(&vui_syntax, &vui_result, 2U);
	}

	/* The signal type present without the colour description, and the reserved video_format values kept as signalled. */
	memset(&vui_syntax, 0, sizeof(vui_syntax));
	vui_syntax.signal_present = 1U;
	vui_syntax.video_format = 7U;
	vui_syntax.full_range = 1U;
	error = vui_roundtrip(&vui_syntax, 0U, (size_t)-1);
	CHECK(error == 0);
	CHECK(vui_result.video_format == 7U);
	CHECK(vui_result.full_range == 1);
	CHECK(vui_result.colour_present == 0);
	CHECK(vui_result.colour_primaries == 2U);
	vui_syntax.colour_present = 1U;
	vui_syntax.colour_primaries = 255U;
	vui_syntax.transfer_characteristics = 0U;
	vui_syntax.matrix_coeffs = 1U;
	error = vui_roundtrip(&vui_syntax, 0U, (size_t)-1);
	CHECK(error == 0);
	CHECK(vui_result.colour_primaries == 255U);
	CHECK(vui_result.transfer_characteristics == 0U);
	CHECK(vui_result.matrix_coeffs == 1U);

	/* The timing with no HRD, and the largest timing values and POC proportion. */
	memset(&vui_syntax, 0, sizeof(vui_syntax));
	vui_syntax.timing_present = 1U;
	vui_syntax.num_units_in_tick = 0xFFFFFFFFU;
	vui_syntax.time_scale = 0xFFFFFFFFU;
	vui_syntax.poc_proportional = 1U;
	vui_syntax.num_ticks_poc_diff_one_minus1 = UE_MAX;
	error = vui_roundtrip(&vui_syntax, 0U, (size_t)-1);
	CHECK(error == 0);
	CHECK(vui_result.num_units_in_tick == 0xFFFFFFFFU);
	CHECK(vui_result.time_scale == 0xFFFFFFFFU);
	CHECK(vui_result.num_ticks_poc_diff_one_minus1 == UE_MAX);
	CHECK(vui_result.hrd_present == 0);
}

/* Checks each value out of its range and at the edge of it. */
static void
check_vui_ranges(void)
{
	unsigned offsets[4];
	unsigned index;
	int error;

	/* chroma_sample_loc_type: 5 is valid, 6 is not, for either field. */
	for (index = 0U; index < 2U; index++) {
		memset(&vui_syntax, 0, sizeof(vui_syntax));
		vui_syntax.chroma_loc_present = 1U;
		if (index == 0U) {
			vui_syntax.chroma_loc_top = 5U;
		} else {
			vui_syntax.chroma_loc_bottom = 5U;
		}
		error = vui_roundtrip(&vui_syntax, 0U, (size_t)-1);
		CHECK(error == 0);
		if (index == 0U) {
			vui_syntax.chroma_loc_top = 6U;
		} else {
			vui_syntax.chroma_loc_bottom = 6U;
		}
		error = vui_roundtrip(&vui_syntax, 0U, (size_t)-1);
		CHECK(error == EINVAL);
	}

	/* The default display window offsets: the largest picture side is valid, one more is not, for each of the four. */
	for (index = 0U; index < 4U; index++) {
		memset(&vui_syntax, 0, sizeof(vui_syntax));
		vui_syntax.default_display_window = 1U;
		vui_syntax.def_disp_win_offset[index] = 16888U;
		error = vui_roundtrip(&vui_syntax, 0U, (size_t)-1);
		CHECK(error == 0);
		offsets[0] = vui_result.def_disp_win_left_offset;
		offsets[1] = vui_result.def_disp_win_right_offset;
		offsets[2] = vui_result.def_disp_win_top_offset;
		offsets[3] = vui_result.def_disp_win_bottom_offset;
		CHECK(offsets[index] == 16888U);
		vui_syntax.def_disp_win_offset[index] = 16889U;
		error = vui_roundtrip(&vui_syntax, 0U, (size_t)-1);
		CHECK(error == EINVAL);
		vui_syntax.def_disp_win_offset[index] = UE_MAX;
		error = vui_roundtrip(&vui_syntax, 0U, (size_t)-1);
		CHECK(error == EINVAL);
	}

	/* The timing values: 0 is refused for either, 1 is valid. */
	memset(&vui_syntax, 0, sizeof(vui_syntax));
	vui_syntax.timing_present = 1U;
	vui_syntax.num_units_in_tick = 1U;
	vui_syntax.time_scale = 1U;
	error = vui_roundtrip(&vui_syntax, 0U, (size_t)-1);
	CHECK(error == 0);
	vui_syntax.num_units_in_tick = 0U;
	error = vui_roundtrip(&vui_syntax, 0U, (size_t)-1);
	CHECK(error == EINVAL);
	vui_syntax.num_units_in_tick = 1U;
	vui_syntax.time_scale = 0U;
	error = vui_roundtrip(&vui_syntax, 0U, (size_t)-1);
	CHECK(error == EINVAL);

	/* The bitstream restriction values at their largest and one more. */
	memset(&vui_syntax, 0, sizeof(vui_syntax));
	vui_syntax.bitstream_restriction = 1U;
	vui_syntax.min_spatial_segmentation_idc = 4095U;
	vui_syntax.max_bytes_per_pic_denom = 16U;
	vui_syntax.max_bits_per_min_cu_denom = 16U;
	vui_syntax.log2_max_mv_length_horizontal = 15U;
	vui_syntax.log2_max_mv_length_vertical = 15U;
	error = vui_roundtrip(&vui_syntax, 0U, (size_t)-1);
	CHECK(error == 0);
	CHECK(vui_result.min_spatial_segmentation_idc == 4095U);
	CHECK(vui_result.max_bytes_per_pic_denom == 16U);
	CHECK(vui_result.max_bits_per_min_cu_denom == 16U);
	CHECK(vui_result.log2_max_mv_length_horizontal == 15U);
	CHECK(vui_result.log2_max_mv_length_vertical == 15U);
	vui_syntax.min_spatial_segmentation_idc = 4096U;
	error = vui_roundtrip(&vui_syntax, 0U, (size_t)-1);
	CHECK(error == EINVAL);
	vui_syntax.min_spatial_segmentation_idc = 0U;
	vui_syntax.max_bytes_per_pic_denom = 17U;
	error = vui_roundtrip(&vui_syntax, 0U, (size_t)-1);
	CHECK(error == EINVAL);
	vui_syntax.max_bytes_per_pic_denom = 0U;
	vui_syntax.max_bits_per_min_cu_denom = 17U;
	error = vui_roundtrip(&vui_syntax, 0U, (size_t)-1);
	CHECK(error == EINVAL);
	vui_syntax.max_bits_per_min_cu_denom = 0U;
	vui_syntax.log2_max_mv_length_horizontal = 16U;
	error = vui_roundtrip(&vui_syntax, 0U, (size_t)-1);
	CHECK(error == EINVAL);
	vui_syntax.log2_max_mv_length_horizontal = 0U;
	vui_syntax.log2_max_mv_length_vertical = 16U;
	error = vui_roundtrip(&vui_syntax, 0U, (size_t)-1);
	CHECK(error == EINVAL);

	/* A failing HRD inside the VUI fails the VUI. */
	memset(&vui_syntax, 0, sizeof(vui_syntax));
	vui_syntax.timing_present = 1U;
	vui_syntax.num_units_in_tick = 1U;
	vui_syntax.time_scale = 1U;
	vui_syntax.hrd_present = 1U;
	vui_syntax.hrd.nal_present = 1U;
	error = vui_roundtrip(&vui_syntax, 0U, (size_t)-1);
	CHECK(error == 0);
	vui_syntax.hrd.fixed_pic_rate_general[0] = 1U;
	vui_syntax.hrd.elemental_duration_in_tc_minus1[0] = 2048U;
	error = vui_roundtrip(&vui_syntax, 0U, (size_t)-1);
	CHECK(error == EINVAL);

	/* A sub-layer count above 6 is refused by the VUI reader, with no read made. */
	memset(&vui_syntax, 0, sizeof(vui_syntax));
	error = vui_roundtrip(&vui_syntax, 7U, (size_t)-1);
	CHECK(error == EINVAL);
}

/* Checks the HRD with every combination of NAL, VCL, sub-picture and sub-layer count, with random contents. */
static void
check_hrd_combinations(void)
{
	unsigned nal;
	unsigned vcl;
	unsigned sub_pic;
	unsigned max_sub;
	unsigned round;
	int error;

	/* Every combination three times with fresh random values. */
	for (nal = 0U; nal < 2U; nal++) {
		for (vcl = 0U; vcl < 2U; vcl++) {
			for (sub_pic = 0U; sub_pic < 2U; sub_pic++) {
				for (max_sub = 0U; max_sub < 7U; max_sub++) {
					for (round = 0U; round < 3U; round++) {
						hrd_random(&hrd_syntax, max_sub, nal, vcl, sub_pic);
						error = hrd_roundtrip(&hrd_syntax, &hrd_syntax, 1, max_sub, NULL);
						CHECK(error == 0);
						if (error == 0)
							hrd_compare(&hrd_syntax, &hrd_syntax, &hrd_result, max_sub);
					}
				}
			}
		}
	}

	/* Every pattern of the three flags of one sub-layer, with the CPB count at 0, 1 and 31. */
	for (round = 0U; round < 3U * 2U * 2U * 2U; round++) {
		hrd_random(&hrd_syntax, 0U, 1U, 1U, 0U);
		hrd_syntax.fixed_pic_rate_general[0] = round & 1U;
		hrd_syntax.fixed_pic_rate_within_cvs[0] = (round >> 1) & 1U;
		hrd_syntax.low_delay_hrd[0] = (round >> 2) & 1U;
		hrd_syntax.cpb_cnt_minus1[0] = (round >> 3) == 0U ? 0U : ((round >> 3) == 1U ? 1U : 31U);
		error = hrd_roundtrip(&hrd_syntax, &hrd_syntax, 1, 0U, NULL);
		CHECK(error == 0);
		if (error == 0)
			hrd_compare(&hrd_syntax, &hrd_syntax, &hrd_result, 0U);
	}

	/* Hand-checked bit counts: no NAL or VCL part and one sub-layer is 2 + 1 + 1 + 1 + 1 bits (general 0, within 0, low delay 0, ue(0) cpb_cnt). */
	memset(&hrd_syntax, 0, sizeof(hrd_syntax));
	error = hrd_roundtrip(&hrd_syntax, &hrd_syntax, 1, 0U, NULL);
	CHECK(error == 0);
	CHECK(last_bits == 6U);
	CHECK(hrd_result.initial_cpb_removal_delay_length_minus1 == 23U);
}

/* Checks the HRD whose common part is taken from the one before it, and the refusals of the arguments. */
static void
check_hrd_common(void)
{
	unsigned round;
	unsigned max_sub;
	unsigned previous_max;
	int error;

	/* A first HRD with a common part, then one without that takes it from the first. */
	for (round = 0U; round < 300U; round++) {
		previous_max = random_next() % 7U;
		hrd_random(&hrd_previous_syntax, previous_max, random_next() % 2U, random_next() % 2U, random_next() % 2U);
		error = hrd_roundtrip(&hrd_previous_syntax, &hrd_previous_syntax, 1, previous_max, NULL);
		CHECK(error == 0);
		memcpy(&hrd_previous_result, &hrd_result, sizeof(hrd_previous_result));

		/* The second one is written with the presence flags of the first and junk for the common fields it does not write. */
		max_sub = random_next() % 7U;
		hrd_random(&hrd_syntax, max_sub, hrd_previous_syntax.nal_present, hrd_previous_syntax.vcl_present, hrd_previous_syntax.sub_pic_present);
		error = hrd_roundtrip(&hrd_syntax, &hrd_previous_syntax, 0, max_sub, &hrd_previous_result);
		CHECK(error == 0);
		if (error == 0)
			hrd_compare(&hrd_syntax, &hrd_previous_syntax, &hrd_result, max_sub);
	}

	/* Without the previous HRD a structure without its common part is refused. */
	hrd_random(&hrd_syntax, 0U, 1U, 0U, 0U);
	error = hrd_roundtrip(&hrd_syntax, &hrd_syntax, 0, 0U, NULL);
	CHECK(error == EINVAL);

	/* The previous HRD is not looked at when the common part is present. */
	memset(&hrd_previous_result, 0xFF, sizeof(hrd_previous_result));
	error = hrd_roundtrip(&hrd_syntax, &hrd_syntax, 1, 0U, &hrd_previous_result);
	CHECK(error == 0);
	hrd_compare(&hrd_syntax, &hrd_syntax, &hrd_result, 0U);

	/* A sub-layer count above 6 is refused. */
	hrd_random(&hrd_syntax, 6U, 1U, 1U, 1U);
	error = hrd_roundtrip(&hrd_syntax, &hrd_syntax, 1, 7U, NULL);
	CHECK(error == EINVAL);
}

/* Checks the HRD ranges: CPB count, elemental duration and the value ranges of the rates and sizes. */
static void
check_hrd_ranges(void)
{
	int error;
	unsigned count;
	unsigned index;

	/* cpb_cnt_minus1: 0 and 31 are valid. */
	for (count = 0U; count < 2U; count++) {
		hrd_random(&hrd_syntax, 0U, 1U, 1U, 1U);
		hrd_syntax.fixed_pic_rate_general[0] = 0U;
		hrd_syntax.fixed_pic_rate_within_cvs[0] = 0U;
		hrd_syntax.low_delay_hrd[0] = 0U;
		hrd_syntax.cpb_cnt_minus1[0] = (count == 0U) ? 0U : 31U;
		error = hrd_roundtrip(&hrd_syntax, &hrd_syntax, 1, 0U, NULL);
		CHECK(error == 0);
		CHECK(hrd_result.cpb_cnt_minus1[0] == hrd_syntax.cpb_cnt_minus1[0]);
	}

	/* cpb_cnt_minus1 32 and larger are refused, with and without the entries that follow. */
	hrd_syntax.cpb_cnt_minus1[0] = 32U;
	error = hrd_roundtrip(&hrd_syntax, &hrd_syntax, 1, 0U, NULL);
	CHECK(error == EINVAL);
	hrd_syntax.cpb_cnt_minus1[0] = 1000U;
	error = hrd_roundtrip(&hrd_syntax, &hrd_syntax, 1, 0U, NULL);
	CHECK(error == EINVAL);
	hrd_syntax.cpb_cnt_minus1[0] = UE_MAX;
	error = hrd_roundtrip(&hrd_syntax, &hrd_syntax, 1, 0U, NULL);
	CHECK(error == EINVAL);

	/* A bad CPB count in a later sub-layer is refused as well. */
	hrd_random(&hrd_syntax, 3U, 0U, 0U, 0U);
	hrd_syntax.fixed_pic_rate_general[3] = 0U;
	hrd_syntax.fixed_pic_rate_within_cvs[3] = 0U;
	hrd_syntax.low_delay_hrd[3] = 0U;
	hrd_syntax.cpb_cnt_minus1[3] = 32U;
	error = hrd_roundtrip(&hrd_syntax, &hrd_syntax, 1, 3U, NULL);
	CHECK(error == EINVAL);

	/* elemental_duration_in_tc_minus1: 2047 is valid, 2048 and larger are not. */
	hrd_random(&hrd_syntax, 0U, 1U, 0U, 0U);
	hrd_syntax.fixed_pic_rate_general[0] = 1U;
	hrd_syntax.cpb_cnt_minus1[0] = 0U;
	hrd_syntax.elemental_duration_in_tc_minus1[0] = 2047U;
	error = hrd_roundtrip(&hrd_syntax, &hrd_syntax, 1, 0U, NULL);
	CHECK(error == 0);
	CHECK(hrd_result.elemental_duration_in_tc_minus1[0] == 2047U);
	hrd_syntax.elemental_duration_in_tc_minus1[0] = 2048U;
	error = hrd_roundtrip(&hrd_syntax, &hrd_syntax, 1, 0U, NULL);
	CHECK(error == EINVAL);
	hrd_syntax.elemental_duration_in_tc_minus1[0] = UE_MAX;
	error = hrd_roundtrip(&hrd_syntax, &hrd_syntax, 1, 0U, NULL);
	CHECK(error == EINVAL);

	/* The rates and sizes: 2^32 - 2 is valid in every field and kept. */
	hrd_random(&hrd_syntax, 0U, 1U, 1U, 1U);
	hrd_syntax.fixed_pic_rate_general[0] = 0U;
	hrd_syntax.fixed_pic_rate_within_cvs[0] = 0U;
	hrd_syntax.low_delay_hrd[0] = 0U;
	hrd_syntax.cpb_cnt_minus1[0] = 1U;
	for (index = 0U; index < 2U; index++) {
		hrd_syntax.nal[0].bit_rate_value_minus1[index] = UE_MAX;
		hrd_syntax.nal[0].cpb_size_value_minus1[index] = UE_MAX;
		hrd_syntax.nal[0].cpb_size_du_value_minus1[index] = UE_MAX;
		hrd_syntax.nal[0].bit_rate_du_value_minus1[index] = UE_MAX;
		hrd_syntax.vcl[0].bit_rate_value_minus1[index] = UE_MAX;
		hrd_syntax.vcl[0].cpb_size_value_minus1[index] = UE_MAX;
		hrd_syntax.vcl[0].cpb_size_du_value_minus1[index] = UE_MAX;
		hrd_syntax.vcl[0].bit_rate_du_value_minus1[index] = UE_MAX;
	}
	error = hrd_roundtrip(&hrd_syntax, &hrd_syntax, 1, 0U, NULL);
	CHECK(error == 0);
	hrd_compare(&hrd_syntax, &hrd_syntax, &hrd_result, 0U);
	CHECK(hrd_result.nal[0].bit_rate_value_minus1[1] == UE_MAX);
	CHECK(hrd_result.vcl[0].bit_rate_du_value_minus1[0] == UE_MAX);

	/* 2^32 - 1 has 32 leading zero bits, which no ue(v) of this reader holds: refused in each of the eight fields. */
	hrd_syntax.nal[0].bit_rate_value_minus1[1] = 0xFFFFFFFFU;
	error = hrd_roundtrip(&hrd_syntax, &hrd_syntax, 1, 0U, NULL);
	CHECK(error == EINVAL);
	hrd_syntax.nal[0].bit_rate_value_minus1[1] = UE_MAX;
	hrd_syntax.nal[0].cpb_size_value_minus1[0] = 0xFFFFFFFFU;
	error = hrd_roundtrip(&hrd_syntax, &hrd_syntax, 1, 0U, NULL);
	CHECK(error == EINVAL);
	hrd_syntax.nal[0].cpb_size_value_minus1[0] = UE_MAX;
	hrd_syntax.vcl[0].cpb_size_du_value_minus1[1] = 0xFFFFFFFFU;
	error = hrd_roundtrip(&hrd_syntax, &hrd_syntax, 1, 0U, NULL);
	CHECK(error == EINVAL);
	hrd_syntax.vcl[0].cpb_size_du_value_minus1[1] = UE_MAX;
	hrd_syntax.vcl[0].bit_rate_du_value_minus1[0] = 0xFFFFFFFFU;
	error = hrd_roundtrip(&hrd_syntax, &hrd_syntax, 1, 0U, NULL);
	CHECK(error == EINVAL);

	/* The constant bit rate flags land on the bit of their entry: entries 0, 5 and 31 set only. */
	hrd_random(&hrd_syntax, 0U, 1U, 0U, 0U);
	hrd_syntax.fixed_pic_rate_general[0] = 0U;
	hrd_syntax.fixed_pic_rate_within_cvs[0] = 0U;
	hrd_syntax.low_delay_hrd[0] = 0U;
	hrd_syntax.cpb_cnt_minus1[0] = 31U;
	for (index = 0U; index < 32U; index++)
		hrd_syntax.nal[0].cbr_flag[index] = (index == 0U || index == 5U || index == 31U) ? 1U : 0U;
	error = hrd_roundtrip(&hrd_syntax, &hrd_syntax, 1, 0U, NULL);
	CHECK(error == 0);
	CHECK(hrd_result.nal[0].cbr_flags == 0x80000021U);
}

/* Checks that every shorter RBSP is refused, for the VUI and the HRD. */
static void
check_truncated(void)
{
	unsigned round;
	unsigned max_sub;
	struct vcodec_bitreader bits_reader;
	size_t cut;
	size_t bits;
	size_t bytes;
	int error;

	/* Complete VUIs with every branch present, cut at every byte length. */
	for (round = 0U; round < 20U; round++) {
		max_sub = round % 7U;
		vui_random(&vui_syntax, max_sub);
		vui_syntax.aspect_ratio_present = 1U;
		vui_syntax.overscan_present = 1U;
		vui_syntax.signal_present = 1U;
		vui_syntax.colour_present = 1U;
		vui_syntax.chroma_loc_present = 1U;
		vui_syntax.default_display_window = 1U;
		vui_syntax.timing_present = 1U;
		vui_syntax.poc_proportional = 1U;
		vui_syntax.hrd_present = 1U;
		vui_syntax.bitstream_restriction = 1U;
		error = vui_roundtrip(&vui_syntax, max_sub, (size_t)-1);
		CHECK(error == 0);
		bits = last_bits;
		bytes = last_bytes;
		for (cut = 0U; cut < bytes; cut++) {
			error = vui_roundtrip(&vui_syntax, max_sub, cut);
			if (cut * 8U < bits) {
				CHECK(error == EINVAL);
			} else {
				CHECK(error == 0);
			}
		}
	}

	/* HRD parameters of every shape, cut at every byte length. */
	for (round = 0U; round < 40U; round++) {
		max_sub = round % 7U;
		hrd_random(&hrd_syntax, max_sub, 1U, round % 2U, (round / 2U) % 2U);
		error = hrd_roundtrip(&hrd_syntax, &hrd_syntax, 1, max_sub, NULL);
		CHECK(error == 0);
		bits = last_bits;
		bytes = last_bytes;
		for (cut = 0U; cut < bytes; cut++) {
			vcodec_bitreader_init(&bits_reader, writer.data, cut);
			error = h265sw_vui_read_hrd(&hrd_result, &bits_reader, 1, max_sub, NULL);
			if (cut * 8U < bits) {
				CHECK(error == EINVAL);
			} else {
				CHECK(error == 0);
			}
		}
	}
}

/* Checks that random bytes never crash the readers and only the documented errors come back. */
static void
check_random(void)
{
	uint8_t *data;
	struct vcodec_bitreader bits;
	size_t size;
	size_t index;
	unsigned round;
	unsigned bias;
	unsigned max_sub;
	int error;
	int allowed;

	/* Each input in an exactly sized heap block, so an out-of-bounds read is caught. */
	for (round = 0U; round < RANDOM_INPUTS; round++) {
		size = (size_t)(random_next() % 200U);
		data = (uint8_t *)malloc(size + 1U);
		CHECK(data != NULL);
		if (data == NULL)
			return;
		bias = 1U + random_next() % 4U;
		for (index = 0U; index < size; index++) {
			if ((random_next() % bias) == 0U) {
				data[index] = (uint8_t)((random_next() % 2U == 0U) ? 0x00U : 0xFFU);
			} else {
				data[index] = (uint8_t)random_next();
			}
		}

		/* The VUI reader, with sub-layer counts including the refused 7. */
		max_sub = random_next() % 8U;
		vcodec_bitreader_init(&bits, data, size);
		error = h265sw_vui_read(&vui_result, &bits, max_sub);
		allowed = (error == 0 || error == EINVAL);
		CHECK(allowed);

		/* The HRD reader, with and without a previous HRD. */
		max_sub = random_next() % 8U;
		vcodec_bitreader_init(&bits, data, size);
		if ((random_next() % 2U) == 0U) {
			error = h265sw_vui_read_hrd(&hrd_result, &bits, 1, max_sub, NULL);
		} else {
			error = h265sw_vui_read_hrd(&hrd_result, &bits, 0, max_sub, &hrd_previous_result);
		}
		allowed = (error == 0 || error == EINVAL);
		CHECK(allowed);

		free(data);
	}
}

int
main(void)
{
	/* The zeroed sub-layer HRD that absent parts are compared with. */
	memset(&zero_sub_layer, 0, sizeof(zero_sub_layer));
	memset(&hrd_previous_result, 0, sizeof(hrd_previous_result));

	/* Runs every group of checks. */
	check_absent();
	check_sar();
	check_vui_round_trip();
	check_vui_ranges();
	check_hrd_combinations();
	check_hrd_common();
	check_hrd_ranges();
	check_truncated();
	check_random();

	/* Reports the summary line. */
	return check_finish("vui");
}
