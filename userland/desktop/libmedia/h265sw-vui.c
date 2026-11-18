/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The video usability information and the hypothetical reference decoder
 * parameters of H.265 (E.2, E.3).
 */

#include "h265sw-vui.h"

#include <errno.h>
#include <string.h>

/* The inferred value of the three HRD length fields when they are absent (E.3.2: 23). */
#define VUI_HRD_DEFAULT_LENGTH_MINUS1 23U

/* The largest values of the HRD fields that have a stated range (E.3.2). */
#define VUI_ELEMENTAL_DURATION_MAX 2047U
#define VUI_CPB_CNT_MINUS1_MAX 31U

/* The most sub-layers of an HRD: maxNumSubLayersMinus1 is 0..6. */
#define VUI_MAX_SUB_LAYERS_MINUS1 6U

/* The inferred values of the bitstream restriction fields when absent (E.3.1). */
#define VUI_VIDEO_FORMAT_UNSPECIFIED 5U
#define VUI_COLOUR_UNSPECIFIED 2U
#define VUI_MAX_BYTES_PER_PIC_DENOM_DEFAULT 2U
#define VUI_MAX_BITS_PER_MIN_CU_DENOM_DEFAULT 1U
#define VUI_LOG2_MAX_MV_LENGTH_DEFAULT 15U

/* The largest values of the fields with a stated range (E.3.1). */
#define VUI_CHROMA_LOC_MAX 5U
#define VUI_MIN_SPATIAL_SEGMENTATION_MAX 4095U
#define VUI_DENOM_MAX 16U
#define VUI_LOG2_MAX_MV_LENGTH_MAX 15U

/* The last aspect_ratio_idc that Table E.1 gives a fixed ratio. */
#define VUI_ASPECT_RATIO_LAST 16U

/*
 * The sample aspect ratios of aspect_ratio_idc 1..16.
 *
 * Index 0 is unused (aspect_ratio_idc 0 is unspecified); width and height
 * are the two terms of the ratio.
 *
 * Values referenced: ITU-T H.265 (01/2026) Table E.1 (the "Sample aspect
 * ratio" column, aspect_ratio_idc 1..16).
 */
static const unsigned vui_aspect_ratio[VUI_ASPECT_RATIO_LAST + 1U][2] = {
	{0U, 0U},
	{1U, 1U},
	{12U, 11U},
	{10U, 11U},
	{16U, 11U},
	{40U, 33U},
	{24U, 11U},
	{20U, 11U},
	{32U, 11U},
	{80U, 33U},
	{18U, 11U},
	{15U, 11U},
	{64U, 33U},
	{160U, 99U},
	{4U, 3U},
	{3U, 2U},
	{2U, 1U}
};

/* Static function declarations. */
static int vui_sub_layer_hrd_read(struct h265sw_sub_layer_hrd *hrd, struct vcodec_bitreader *bits, unsigned cpb_count, int sub_pic);
static int vui_hrd_common_read(struct h265sw_hrd *hrd, struct vcodec_bitreader *bits);
static void vui_hrd_common_copy(struct h265sw_hrd *hrd, const struct h265sw_hrd *previous);
static void vui_sar_derive(struct h265sw_vui *vui);

/*
 * Reads hrd_parameters( common_present, max_sub_layers_minus1 ) into hrd
 * (E.2.2, E.2.3).
 *
 * The fields are checked as they are read, before any of them is used as a
 * count, so a broken read never lengthens a loop.
 */
int
h265sw_vui_read_hrd(
	struct h265sw_hrd *hrd,
	struct vcodec_bitreader *bits,
	int common_present,
	unsigned max_sub_layers_minus1,
	const struct h265sw_hrd *previous)
{
	unsigned sub_layer;
	unsigned general_flag;
	unsigned within_cvs_flag;
	unsigned low_delay_flag;
	uint32_t elemental_duration;
	uint32_t cpb_cnt_minus1;
	int error;

	/* Refuses missing objects and a sub-layer count outside 0..6. */
	if (hrd == NULL || bits == NULL)
		return EINVAL;
	if (max_sub_layers_minus1 > VUI_MAX_SUB_LAYERS_MINUS1)
		return EINVAL;

	/* A structure without its own common part takes the one of the HRD before it, which must exist and be another object. */
	if (common_present == 0) {
		if (previous == NULL)
			return EINVAL;
		if (previous == hrd)
			return EINVAL;
	}

	/* Clears the structure, then sets the values that stand for an absent length field. */
	memset(hrd, 0, sizeof(*hrd));
	hrd->initial_cpb_removal_delay_length_minus1 = VUI_HRD_DEFAULT_LENGTH_MINUS1;
	hrd->au_cpb_removal_delay_length_minus1 = VUI_HRD_DEFAULT_LENGTH_MINUS1;
	hrd->dpb_output_delay_length_minus1 = VUI_HRD_DEFAULT_LENGTH_MINUS1;

	/* Reads the common part, or takes it from the HRD this one shares it with. */
	if (common_present != 0) {
		error = vui_hrd_common_read(hrd, bits);
		if (error != 0)
			return error;
	} else {
		vui_hrd_common_copy(hrd, previous);
	}

	/* Reads the parameters of each sub-layer. */
	for (sub_layer = 0U; sub_layer <= max_sub_layers_minus1; sub_layer++) {
		/* Reads whether the picture rate is fixed, in general and within a coded video sequence. */
		general_flag = vcodec_bitreader_u1(bits);
		within_cvs_flag = 1U;
		if (general_flag == 0U)
			within_cvs_flag = vcodec_bitreader_u1(bits);

		/* A fixed rate gives the duration of a picture; otherwise the low delay mode is read. */
		elemental_duration = 0U;
		low_delay_flag = 0U;
		if (within_cvs_flag != 0U)
			elemental_duration = vcodec_bitreader_ue(bits);
		else
			low_delay_flag = vcodec_bitreader_u1(bits);

		/* Reads the number of alternative CPB specifications unless the low delay mode implies one. */
		cpb_cnt_minus1 = 0U;
		if (low_delay_flag == 0U)
			cpb_cnt_minus1 = vcodec_bitreader_ue(bits);
		if (bits->error != 0)
			return EINVAL;

		/* Refuses values outside their ranges (E.3.2). */
		if (elemental_duration > VUI_ELEMENTAL_DURATION_MAX)
			return EINVAL;
		if (cpb_cnt_minus1 > VUI_CPB_CNT_MINUS1_MAX)
			return EINVAL;

		/* Keeps what was read for this sub-layer. */
		hrd->fixed_pic_rate_general[sub_layer] = (uint8_t)general_flag;
		hrd->fixed_pic_rate_within_cvs[sub_layer] = (uint8_t)within_cvs_flag;
		hrd->low_delay_hrd[sub_layer] = (uint8_t)low_delay_flag;
		hrd->elemental_duration_in_tc_minus1[sub_layer] = (uint16_t)elemental_duration;
		hrd->cpb_cnt_minus1[sub_layer] = (uint8_t)cpb_cnt_minus1;

		/* Reads the CPB specifications of the NAL HRD, then of the VCL HRD, when present. */
		if (hrd->nal_hrd_present != 0) {
			error = vui_sub_layer_hrd_read(&hrd->nal[sub_layer], bits, cpb_cnt_minus1 + 1U, hrd->sub_pic_hrd_params_present);
			if (error != 0)
				return error;
		}
		if (hrd->vcl_hrd_present != 0) {
			error = vui_sub_layer_hrd_read(&hrd->vcl[sub_layer], bits, cpb_cnt_minus1 + 1U, hrd->sub_pic_hrd_params_present);
			if (error != 0)
				return error;
		}
	}

	/* Succeeded: every sub-layer is read. */
	return 0;
}

/*
 * Reads vui_parameters() into vui (E.2.1).
 *
 * Every present flag is read in the order of the syntax; a field that is
 * absent keeps the value E.3.1 infers for it.
 */
int
h265sw_vui_read(
	struct h265sw_vui *vui,
	struct vcodec_bitreader *bits,
	unsigned max_sub_layers_minus1)
{
	int error;

	/* Refuses missing objects and a sub-layer count outside 0..6. */
	if (vui == NULL || bits == NULL)
		return EINVAL;
	if (max_sub_layers_minus1 > VUI_MAX_SUB_LAYERS_MINUS1)
		return EINVAL;

	/* Clears the structure, then sets the values E.3.1 infers for absent elements. */
	memset(vui, 0, sizeof(*vui));
	vui->video_format = VUI_VIDEO_FORMAT_UNSPECIFIED;
	vui->colour_primaries = VUI_COLOUR_UNSPECIFIED;
	vui->transfer_characteristics = VUI_COLOUR_UNSPECIFIED;
	vui->matrix_coeffs = VUI_COLOUR_UNSPECIFIED;
	vui->motion_vectors_over_pic_boundaries = 1;
	vui->max_bytes_per_pic_denom = VUI_MAX_BYTES_PER_PIC_DENOM_DEFAULT;
	vui->max_bits_per_min_cu_denom = VUI_MAX_BITS_PER_MIN_CU_DENOM_DEFAULT;
	vui->log2_max_mv_length_horizontal = VUI_LOG2_MAX_MV_LENGTH_DEFAULT;
	vui->log2_max_mv_length_vertical = VUI_LOG2_MAX_MV_LENGTH_DEFAULT;

	/* Reads the sample aspect ratio: an index, and the two terms for the explicit index. */
	vui->aspect_ratio_present = (int)vcodec_bitreader_u1(bits);
	if (vui->aspect_ratio_present != 0) {
		vui->aspect_ratio_idc = vcodec_bitreader_u(bits, 8U);
		if (vui->aspect_ratio_idc == H265SW_EXTENDED_SAR) {
			vui->signalled_sar_width = vcodec_bitreader_u(bits, 16U);
			vui->signalled_sar_height = vcodec_bitreader_u(bits, 16U);
		}
	}

	/* Reads whether the picture suits display with overscan. */
	vui->overscan_present = (int)vcodec_bitreader_u1(bits);
	if (vui->overscan_present != 0)
		vui->overscan_appropriate = (int)vcodec_bitreader_u1(bits);

	/* Reads the video signal type, and the colour description inside it. */
	vui->signal_present = (int)vcodec_bitreader_u1(bits);
	if (vui->signal_present != 0) {
		vui->video_format = vcodec_bitreader_u(bits, 3U);
		vui->full_range = (int)vcodec_bitreader_u1(bits);
		vui->colour_present = (int)vcodec_bitreader_u1(bits);
		if (vui->colour_present != 0) {
			vui->colour_primaries = vcodec_bitreader_u(bits, 8U);
			vui->transfer_characteristics = vcodec_bitreader_u(bits, 8U);
			vui->matrix_coeffs = vcodec_bitreader_u(bits, 8U);
		}
	}
	if (bits->error != 0)
		return EINVAL;

	/* Reads the chroma sample locations, which are 0..5 (E.3.1). */
	vui->chroma_loc_present = (int)vcodec_bitreader_u1(bits);
	if (vui->chroma_loc_present != 0) {
		vui->chroma_sample_loc_type_top_field = vcodec_bitreader_ue(bits);
		vui->chroma_sample_loc_type_bottom_field = vcodec_bitreader_ue(bits);
		if (bits->error != 0)
			return EINVAL;
		if (vui->chroma_sample_loc_type_top_field > VUI_CHROMA_LOC_MAX)
			return EINVAL;
		if (vui->chroma_sample_loc_type_bottom_field > VUI_CHROMA_LOC_MAX)
			return EINVAL;
	}

	/* Reads the three flags of the picture structure. */
	vui->neutral_chroma_indication = (int)vcodec_bitreader_u1(bits);
	vui->field_seq = (int)vcodec_bitreader_u1(bits);
	vui->frame_field_info_present = (int)vcodec_bitreader_u1(bits);

	/* Reads the default display window, in chroma sample units, each offset within the largest picture side. */
	vui->default_display_window = (int)vcodec_bitreader_u1(bits);
	if (vui->default_display_window != 0) {
		vui->def_disp_win_left_offset = vcodec_bitreader_ue(bits);
		vui->def_disp_win_right_offset = vcodec_bitreader_ue(bits);
		vui->def_disp_win_top_offset = vcodec_bitreader_ue(bits);
		vui->def_disp_win_bottom_offset = vcodec_bitreader_ue(bits);
		if (bits->error != 0)
			return EINVAL;
		if (vui->def_disp_win_left_offset > H265SW_MAX_SIDE)
			return EINVAL;
		if (vui->def_disp_win_right_offset > H265SW_MAX_SIDE)
			return EINVAL;
		if (vui->def_disp_win_top_offset > H265SW_MAX_SIDE)
			return EINVAL;
		if (vui->def_disp_win_bottom_offset > H265SW_MAX_SIDE)
			return EINVAL;
	}

	/* Reads the timing information, and the HRD parameters inside it. */
	vui->timing_present = (int)vcodec_bitreader_u1(bits);
	if (vui->timing_present != 0) {
		vui->num_units_in_tick = vcodec_bitreader_u(bits, 32U);
		vui->time_scale = vcodec_bitreader_u(bits, 32U);
		if (bits->error != 0)
			return EINVAL;

		/* Both timing values shall be greater than 0 (E.3.1). */
		if (vui->num_units_in_tick == 0U)
			return EINVAL;
		if (vui->time_scale == 0U)
			return EINVAL;

		/* Reads whether the picture order count is proportional to time, and by how much. */
		vui->poc_proportional_to_timing = (int)vcodec_bitreader_u1(bits);
		if (vui->poc_proportional_to_timing != 0)
			vui->num_ticks_poc_diff_one_minus1 = vcodec_bitreader_ue(bits);

		/* Reads the HRD parameters, whose common part is always present here. */
		vui->hrd_present = (int)vcodec_bitreader_u1(bits);
		if (vui->hrd_present != 0) {
			error = h265sw_vui_read_hrd(&vui->hrd, bits, 1, max_sub_layers_minus1, NULL);
			if (error != 0)
				return error;
		}
	}
	if (bits->error != 0)
		return EINVAL;

	/* Reads the bitstream restrictions, each within its range (E.3.1). */
	vui->bitstream_restriction = (int)vcodec_bitreader_u1(bits);
	if (vui->bitstream_restriction != 0) {
		vui->tiles_fixed_structure = (int)vcodec_bitreader_u1(bits);
		vui->motion_vectors_over_pic_boundaries = (int)vcodec_bitreader_u1(bits);
		vui->restricted_ref_pic_lists = (int)vcodec_bitreader_u1(bits);
		vui->min_spatial_segmentation_idc = vcodec_bitreader_ue(bits);
		vui->max_bytes_per_pic_denom = vcodec_bitreader_ue(bits);
		vui->max_bits_per_min_cu_denom = vcodec_bitreader_ue(bits);
		vui->log2_max_mv_length_horizontal = vcodec_bitreader_ue(bits);
		vui->log2_max_mv_length_vertical = vcodec_bitreader_ue(bits);
		if (bits->error != 0)
			return EINVAL;
		if (vui->min_spatial_segmentation_idc > VUI_MIN_SPATIAL_SEGMENTATION_MAX)
			return EINVAL;
		if (vui->max_bytes_per_pic_denom > VUI_DENOM_MAX)
			return EINVAL;
		if (vui->max_bits_per_min_cu_denom > VUI_DENOM_MAX)
			return EINVAL;
		if (vui->log2_max_mv_length_horizontal > VUI_LOG2_MAX_MV_LENGTH_MAX)
			return EINVAL;
		if (vui->log2_max_mv_length_vertical > VUI_LOG2_MAX_MV_LENGTH_MAX)
			return EINVAL;
	}

	/* A failed read of the last flag leaves no later check to notice it, so the whole read is checked here. */
	if (bits->error != 0)
		return EINVAL;

	/* Derives the sample aspect ratio the index names. */
	vui_sar_derive(vui);

	/* Succeeded: the whole VUI is read. */
	return 0;
}

/*
 * Reads sub_layer_hrd_parameters() (E.2.3) for cpb_count CPB specifications.
 *
 * cpb_count is 1..32, already checked by the caller.  The DU fields are
 * read only for a sub-picture HRD, and stay 0 otherwise.
 */
static int
vui_sub_layer_hrd_read(
	struct h265sw_sub_layer_hrd *hrd,
	struct vcodec_bitreader *bits,
	unsigned cpb_count,
	int sub_pic)
{
	unsigned index;
	uint32_t cbr_flag;

	/* Reads each CPB specification: rates, sizes, and whether it is a constant bit rate. */
	for (index = 0U; index < cpb_count; index++) {
		hrd->bit_rate_value_minus1[index] = vcodec_bitreader_ue(bits);
		hrd->cpb_size_value_minus1[index] = vcodec_bitreader_ue(bits);
		if (sub_pic != 0) {
			hrd->cpb_size_du_value_minus1[index] = vcodec_bitreader_ue(bits);
			hrd->bit_rate_du_value_minus1[index] = vcodec_bitreader_ue(bits);
		}
		cbr_flag = vcodec_bitreader_u1(bits);
		if (bits->error != 0)
			return EINVAL;

		/* Keeps cbr_flag[ index ] as bit index of cbr_flags. */
		hrd->cbr_flags |= cbr_flag << index;
	}

	/* Succeeded: every CPB specification is read. */
	return 0;
}

/*
 * Reads the common part of hrd_parameters() (E.2.2, the commonInfPresentFlag
 * 1 branch).
 */
static int
vui_hrd_common_read(
	struct h265sw_hrd *hrd,
	struct vcodec_bitreader *bits)
{
	/* Reads whether the NAL and VCL HRD parameters are present. */
	hrd->nal_hrd_present = (int)vcodec_bitreader_u1(bits);
	hrd->vcl_hrd_present = (int)vcodec_bitreader_u1(bits);

	/* The rest of the common part is present only when one of the two is. */
	if (hrd->nal_hrd_present != 0 || hrd->vcl_hrd_present != 0) {
		/* Reads the sub-picture parameters, when the HRD can run at sub-picture level. */
		hrd->sub_pic_hrd_params_present = (int)vcodec_bitreader_u1(bits);
		if (hrd->sub_pic_hrd_params_present != 0) {
			hrd->tick_divisor_minus2 = vcodec_bitreader_u(bits, 8U);
			hrd->du_cpb_removal_delay_increment_length_minus1 = vcodec_bitreader_u(bits, 5U);
			hrd->sub_pic_cpb_params_in_pic_timing_sei = (int)vcodec_bitreader_u1(bits);
			hrd->dpb_output_delay_du_length_minus1 = vcodec_bitreader_u(bits, 5U);
		}

		/* Reads the scales of the rates and sizes. */
		hrd->bit_rate_scale = vcodec_bitreader_u(bits, 4U);
		hrd->cpb_size_scale = vcodec_bitreader_u(bits, 4U);
		if (hrd->sub_pic_hrd_params_present != 0)
			hrd->cpb_size_du_scale = vcodec_bitreader_u(bits, 4U);

		/* Reads the bit lengths of the delay fields of the SEI messages. */
		hrd->initial_cpb_removal_delay_length_minus1 = vcodec_bitreader_u(bits, 5U);
		hrd->au_cpb_removal_delay_length_minus1 = vcodec_bitreader_u(bits, 5U);
		hrd->dpb_output_delay_length_minus1 = vcodec_bitreader_u(bits, 5U);
	}
	if (bits->error != 0)
		return EINVAL;

	/* Succeeded: the common part is read. */
	return 0;
}

/*
 * Copies the common part of an HRD from the one it shares it with (7.4.3.1:
 * an hrd_parameters() without its own common part).
 */
static void
vui_hrd_common_copy(
	struct h265sw_hrd *hrd,
	const struct h265sw_hrd *previous)
{
	/* The presence flags, which decide what the sub-layers hold. */
	hrd->nal_hrd_present = previous->nal_hrd_present;
	hrd->vcl_hrd_present = previous->vcl_hrd_present;
	hrd->sub_pic_hrd_params_present = previous->sub_pic_hrd_params_present;

	/* The sub-picture parameters and the scales. */
	hrd->tick_divisor_minus2 = previous->tick_divisor_minus2;
	hrd->du_cpb_removal_delay_increment_length_minus1 = previous->du_cpb_removal_delay_increment_length_minus1;
	hrd->sub_pic_cpb_params_in_pic_timing_sei = previous->sub_pic_cpb_params_in_pic_timing_sei;
	hrd->dpb_output_delay_du_length_minus1 = previous->dpb_output_delay_du_length_minus1;
	hrd->bit_rate_scale = previous->bit_rate_scale;
	hrd->cpb_size_scale = previous->cpb_size_scale;
	hrd->cpb_size_du_scale = previous->cpb_size_du_scale;

	/* The three delay lengths. */
	hrd->initial_cpb_removal_delay_length_minus1 = previous->initial_cpb_removal_delay_length_minus1;
	hrd->au_cpb_removal_delay_length_minus1 = previous->au_cpb_removal_delay_length_minus1;
	hrd->dpb_output_delay_length_minus1 = previous->dpb_output_delay_length_minus1;
}

/*
 * Derives sar_width and sar_height from aspect_ratio_idc (Table E.1).
 *
 * An absent, unspecified (0) or reserved (17..254) index, and an explicit
 * ratio with a zero term, leave both at 0: the ratio is unspecified.
 */
static void
vui_sar_derive(
	struct h265sw_vui *vui)
{
	/* An absent index is unspecified. */
	vui->sar_width = 0U;
	vui->sar_height = 0U;
	if (vui->aspect_ratio_present == 0)
		return;

	/* The explicit ratio is used as signalled, unless a term is 0. */
	if (vui->aspect_ratio_idc == H265SW_EXTENDED_SAR) {
		if (vui->signalled_sar_width == 0U)
			return;
		if (vui->signalled_sar_height == 0U)
			return;
		vui->sar_width = vui->signalled_sar_width;
		vui->sar_height = vui->signalled_sar_height;
		return;
	}

	/* Index 0 is unspecified and 17..254 are reserved, read as 0. */
	if (vui->aspect_ratio_idc == 0U)
		return;
	if (vui->aspect_ratio_idc > VUI_ASPECT_RATIO_LAST)
		return;

	/* The fixed ratio of Table E.1. */
	vui->sar_width = vui_aspect_ratio[vui->aspect_ratio_idc][0];
	vui->sar_height = vui_aspect_ratio[vui->aspect_ratio_idc][1];
}
