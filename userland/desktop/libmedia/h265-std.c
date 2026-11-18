/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Vulkan Video StdVideoH265 structures of the H.265 decode path.
 */

#include "h265-std.h"

#include <errno.h>
#include <string.h>

/* The number of levels of Table A.8 that the Vulkan level enumeration names. */
#define STD_LEVEL_COUNT 13U

/* The most entries of a short-term set in the explicit form (both lists together). */
#define STD_RPS_MAX_ENTRIES 16U

/* The largest value of a delta_poc_s0_minus1 or delta_poc_s1_minus1 element: it is a 16-bit field. */
#define STD_DELTA_POC_MINUS1_MAX 65535

/*
 * The levels the Vulkan level enumeration names, as general_level_idc.
 *
 * general_level_idc is 30 times the level number (7.4.4, A.4.1), and the
 * levels are in increasing order, so the index of a level here is also
 * its StdVideoH265LevelIdc value (1.0 is 0, 2.0 is 1, 2.1 is 2, 3.0 is 3,
 * 3.1 is 4, 4.0 is 5, 4.1 is 6, 5.0 is 7, 5.1 is 8, 5.2 is 9, 6.0 is 10,
 * 6.1 is 11 and 6.2 is 12).  Values referenced: ITU-T H.265 (V11)
 * (01/2026) Table A.8's levels (printed as Table A.9 in this edition) 1
 * to 6.2: 1, 2, 2.1, 3, 3.1, 4, 4.1, 5, 5.1, 5.2, 6, 6.1, 6.2.
 * It is read only.
 */
static const unsigned std_level_idc[STD_LEVEL_COUNT] = {
	30U, 60U, 63U, 90U, 93U, 120U, 123U, 150U, 153U, 156U, 180U, 183U, 186U
};

/*
 * The Vulkan level of each entry of std_level_idc, in the same order.
 * It is read only.
 */
static const StdVideoH265LevelIdc std_level_enum[STD_LEVEL_COUNT] = {
	STD_VIDEO_H265_LEVEL_IDC_1_0,
	STD_VIDEO_H265_LEVEL_IDC_2_0,
	STD_VIDEO_H265_LEVEL_IDC_2_1,
	STD_VIDEO_H265_LEVEL_IDC_3_0,
	STD_VIDEO_H265_LEVEL_IDC_3_1,
	STD_VIDEO_H265_LEVEL_IDC_4_0,
	STD_VIDEO_H265_LEVEL_IDC_4_1,
	STD_VIDEO_H265_LEVEL_IDC_5_0,
	STD_VIDEO_H265_LEVEL_IDC_5_1,
	STD_VIDEO_H265_LEVEL_IDC_5_2,
	STD_VIDEO_H265_LEVEL_IDC_6_0,
	STD_VIDEO_H265_LEVEL_IDC_6_1,
	STD_VIDEO_H265_LEVEL_IDC_6_2
};

/* Static function declarations. */
static unsigned std_bit(int value);
static uint32_t std_flag_mask(const uint8_t *flags, unsigned count);
static int std_profile(const struct h265sw_ptl *ptl, StdVideoH265ProfileIdc *profile);
static StdVideoH265LevelIdc std_level(unsigned level_idc);
static void std_dec_pic_buf_mgr(const unsigned *max_dec_pic_buffering_minus1, const unsigned *max_num_reorder_pics, const uint32_t *max_latency_increase_plus1, StdVideoH265DecPicBufMgr *out);
static void std_sub_layer_hrd(const struct h265sw_sub_layer_hrd *in, StdVideoH265SubLayerHrdParameters *out);
static void std_hrd(const struct h265sw_hrd *hrd, StdVideoH265HrdParameters *out, StdVideoH265SubLayerHrdParameters *nal, StdVideoH265SubLayerHrdParameters *vcl);
static void std_scaling_lists(const struct h265sw_scaling_list *in, StdVideoH265ScalingLists *out);
static void std_vui(const struct h265sw_vui *vui, StdVideoH265SequenceParameterSetVui *out);
static int std_short_term_set(const struct h265sw_st_rps *rps, const struct h265sw_st_rps_syntax *syntax, StdVideoH265ShortTermRefPicSet *out);
static void std_sps_flags(const struct h265sw_sps *sps, StdVideoH265SpsFlags *flags);
static void std_sps_sizes(const struct h265sw_sps *sps, StdVideoH265SequenceParameterSet *out);
static void std_pps_flags(const struct h265sw_pps *pps, StdVideoH265PpsFlags *flags);
static int std_curr_list(unsigned count, const int *slots, uint8_t *out);

/*
 * Makes the StdVideo form of a profile, tier and level.
 *
 * The profile follows the rule in h265-std.h: the signalled idc when the
 * Vulkan enumeration names it, else the lowest of Main, Main 10 and Main
 * Still Picture that the stream says it is compatible with.  A level
 * between two defined ones maps to the next one above it, so a value
 * that Table A.8 does not define never stops the decode.
 */
int
h265_std_ptl(
	const struct h265sw_ptl *ptl,
	StdVideoH265ProfileTierLevel *out)
{
	StdVideoH265ProfileIdc profile;
	int error;

	/* Chooses the profile; a stream with none of the supported ones is refused. */
	error = std_profile(ptl, &profile);
	if (error != 0)
		return error;

	/* Clears the output so every field the profile does not name is 0. */
	memset(out, 0, sizeof(*out));

	/* Copies the tier and the four source flags as they were signalled. */
	out->flags.general_tier_flag = std_bit((int)ptl->tier);
	out->flags.general_progressive_source_flag = std_bit(ptl->progressive);
	out->flags.general_interlaced_source_flag = std_bit(ptl->interlaced);
	out->flags.general_non_packed_constraint_flag = std_bit(ptl->non_packed);
	out->flags.general_frame_only_constraint_flag = std_bit(ptl->frame_only);

	/* Stores the chosen profile and the level that general_level_idc names. */
	out->general_profile_idc = profile;
	out->general_level_idc = std_level(ptl->level_idc);

	/* Succeeded: *out holds the profile, tier and level. */
	return 0;
}

/*
 * Makes the StdVideo form of a VPS.
 *
 * The DecPicBufMgr structure holds the seven sub-layers the parse kept,
 * and the HRD structure is present only when the VPS carried one.
 */
int
h265_std_vps(
	const struct h265sw_vps *vps,
	struct h265_std_vps *out)
{
	StdVideoH265ProfileTierLevel ptl;
	int error;

	/* Makes the profile, tier and level first, so a refused profile leaves out as it was. */
	error = h265_std_ptl(&vps->ptl, &ptl);
	if (error != 0)
		return error;

	/* Clears the whole output, so every reserved field and unused array entry is 0. */
	memset(out, 0, sizeof(*out));

	/* Stores the profile, tier and level in the structure the VPS points at. */
	out->ptl = ptl;

	/* Copies the four flags of the VPS. */
	out->vps.flags.vps_temporal_id_nesting_flag = std_bit(vps->temporal_id_nesting);
	out->vps.flags.vps_sub_layer_ordering_info_present_flag = std_bit(vps->sub_layer_ordering_info_present);
	out->vps.flags.vps_timing_info_present_flag = std_bit(vps->timing_present);
	out->vps.flags.vps_poc_proportional_to_timing_flag = std_bit(vps->poc_proportional_to_timing);

	/* Copies the identifiers and the timing information. */
	out->vps.vps_video_parameter_set_id = (uint8_t)vps->id;
	out->vps.vps_max_sub_layers_minus1 = (uint8_t)vps->max_sub_layers_minus1;
	out->vps.vps_num_units_in_tick = vps->num_units_in_tick;
	out->vps.vps_time_scale = vps->time_scale;
	out->vps.vps_num_ticks_poc_diff_one_minus1 = vps->num_ticks_poc_diff_one_minus1;

	/* Copies the buffering limits of the seven sub-layers. */
	std_dec_pic_buf_mgr(
		vps->max_dec_pic_buffering_minus1,
		vps->max_num_reorder_pics,
		vps->max_latency_increase_plus1,
		&out->dpb);

	/* Points the VPS at the profile and the buffering limits it owns. */
	out->vps.pProfileTierLevel = &out->ptl;
	out->vps.pDecPicBufMgr = &out->dpb;

	/* Makes the HRD parameters and points the VPS at them only when it carried some. */
	if (vps->num_hrd_parameters > 0U) {
		std_hrd(&vps->hrd, &out->hrd, out->hrd_nal, out->hrd_vcl);
		out->vps.pHrdParameters = &out->hrd;
	}

	/* Succeeded: *out holds the VPS and the structures it points at. */
	return 0;
}

/*
 * Makes the StdVideo form of an SPS.
 *
 * The profile is made first, so a refused profile leaves the output as it
 * was.  A short-term set that does not fit stops the copy part way, and
 * the output is not usable after that refusal.
 */
int
h265_std_sps(
	const struct h265sw_sps *sps,
	struct h265_std_sps *out)
{
	StdVideoH265ProfileTierLevel ptl;
	unsigned lt_count;
	unsigned i;
	int error;

	/* Makes the profile, tier and level first, so a refused profile leaves out as it was. */
	error = h265_std_ptl(&sps->ptl, &ptl);
	if (error != 0)
		return error;

	/* Refuses a count of short-term sets that the arrays cannot hold. */
	if (sps->num_st_rps > H265SW_MAX_ST_RPS - 1U)
		return EINVAL;

	/* Clears the whole output, so every reserved field and unused array entry is 0. */
	memset(out, 0, sizeof(*out));

	/* Stores the profile, tier and level in the structure the SPS points at. */
	out->ptl = ptl;

	/* Copies the flags, the sizes, the identifiers and the block size fields. */
	std_sps_flags(sps, &out->sps.flags);
	std_sps_sizes(sps, &out->sps);

	/* Copies the buffering limits of the seven sub-layers. */
	std_dec_pic_buf_mgr(
		sps->max_dec_pic_buffering_minus1,
		sps->max_num_reorder_pics,
		sps->max_latency_increase_plus1,
		&out->dpb);

	/* Points the SPS at the profile and the buffering limits it owns. */
	out->sps.pProfileTierLevel = &out->ptl;
	out->sps.pDecPicBufMgr = &out->dpb;

	/* Makes the scaling lists in use (the signalled or the default ones) and points the SPS at them. */
	if (sps->scaling_list_enabled != 0) {
		std_scaling_lists(&sps->scaling, &out->scaling);
		out->sps.pScalingLists = &out->scaling;
	}

	/* Makes each short-term set of the SPS in both forms. */
	for (i = 0U; i < sps->num_st_rps; i++) {
		error = std_short_term_set(&sps->st_rps[i], &sps->st_syntax[i], &out->st_rps[i]);
		if (error != 0)
			return error;
	}

	/* Points the SPS at the array of short-term sets when it has any. */
	if (sps->num_st_rps > 0U)
		out->sps.pShortTermRefPicSet = out->st_rps;

	/* Makes the long-term pictures of the SPS and points the SPS at them. */
	if (sps->long_term_present != 0) {
		lt_count = sps->num_lt_sps;
		if (lt_count > H265SW_MAX_LT_SPS)
			lt_count = H265SW_MAX_LT_SPS;

		/* Copies each long-term entry's order count lsb, and its used flag as a bit of the mask. */
		for (i = 0U; i < lt_count; i++) {
			out->lt.lt_ref_pic_poc_lsb_sps[i] = sps->lt_poc_lsb[i];

			/* Sets the bit of an entry that the current picture may use as a reference. */
			if (sps->lt_used[i] != 0U)
				out->lt.used_by_curr_pic_lt_sps_flag |= (uint32_t)1U << i;
		}

		out->sps.pLongTermRefPicsSps = &out->lt;
	}

	/* Makes the video usability information and points the SPS at it. */
	if (sps->vui_present != 0) {
		std_vui(&sps->vui, &out->vui);
		out->sps.pSequenceParameterSetVui = &out->vui;

		/* Makes the HRD parameters of the usability information and points it at them. */
		if (sps->vui.hrd_present != 0) {
			std_hrd(&sps->vui.hrd, &out->hrd, out->hrd_nal, out->hrd_vcl);
			out->vui.pHrdParameters = &out->hrd;
		}
	}

	/* Succeeded: *out holds the SPS and the structures it points at. */
	return 0;
}

/*
 * Makes the StdVideo form of a PPS.
 *
 * The PPS names its SPS twice, by id and through the SPS's VPS id; both
 * come from the SPS the caller gives, which must be the one the PPS names.
 */
int
h265_std_pps(
	const struct h265sw_pps *pps,
	const struct h265sw_sps *sps,
	struct h265_std_pps *out)
{
	unsigned i;

	/* Refuses a missing SPS: the PPS's key needs its VPS id. */
	if (sps == NULL)
		return EINVAL;

	/* Refuses an SPS that is not the one the PPS names. */
	if (pps->sps_id != sps->id)
		return EINVAL;

	/* Refuses tile counts that the size arrays cannot hold. */
	if (pps->tile_columns > H265SW_MAX_TILE_COLS)
		return EINVAL;
	if (pps->tile_rows > H265SW_MAX_TILE_ROWS)
		return EINVAL;

	/* Clears the whole output, so every field of an unread extension is 0. */
	memset(out, 0, sizeof(*out));

	/* Copies the flags of the PPS and its range extension flag. */
	std_pps_flags(pps, &out->pps.flags);

	/* Copies the three identifiers that make the PPS's key. */
	out->pps.pps_pic_parameter_set_id = (uint8_t)pps->id;
	out->pps.pps_seq_parameter_set_id = (uint8_t)pps->sps_id;
	out->pps.sps_video_parameter_set_id = (uint8_t)sps->vps_id;

	/* Copies the slice header defaults in the minus forms of the syntax. */
	out->pps.num_extra_slice_header_bits = (uint8_t)pps->num_extra_slice_header_bits;
	out->pps.num_ref_idx_l0_default_active_minus1 = (uint8_t)(pps->num_ref_idx_default[0] - 1U);
	out->pps.num_ref_idx_l1_default_active_minus1 = (uint8_t)(pps->num_ref_idx_default[1] - 1U);
	out->pps.init_qp_minus26 = (int8_t)(pps->init_qp - 26);

	/* Copies the quantization, deblocking and merge level fields. */
	out->pps.diff_cu_qp_delta_depth = (uint8_t)pps->diff_cu_qp_delta_depth;
	out->pps.pps_cb_qp_offset = (int8_t)pps->cb_qp_offset;
	out->pps.pps_cr_qp_offset = (int8_t)pps->cr_qp_offset;
	out->pps.pps_beta_offset_div2 = (int8_t)pps->beta_offset_div2;
	out->pps.pps_tc_offset_div2 = (int8_t)pps->tc_offset_div2;
	out->pps.log2_parallel_merge_level_minus2 = (uint8_t)(pps->log2_parallel_merge_level - 2U);

	/* Copies the tile structure; the sizes are signalled only for non-uniform spacing. */
	if (pps->tiles != 0) {
		if (pps->tile_columns > 0U)
			out->pps.num_tile_columns_minus1 = (uint8_t)(pps->tile_columns - 1U);
		if (pps->tile_rows > 0U)
			out->pps.num_tile_rows_minus1 = (uint8_t)(pps->tile_rows - 1U);

		/* Copies each signalled column width and row height in the minus form of the syntax. */
		if (pps->uniform_spacing == 0) {
			for (i = 0U; i + 1U < pps->tile_columns; i++)
				out->pps.column_width_minus1[i] = (uint16_t)(pps->column_width[i] - 1U);
			for (i = 0U; i + 1U < pps->tile_rows; i++)
				out->pps.row_height_minus1[i] = (uint16_t)(pps->row_height[i] - 1U);
		}
	}

	/* Makes the PPS's own scaling lists and points the PPS at them. */
	if (pps->scaling_list_present != 0) {
		std_scaling_lists(&pps->scaling, &out->scaling);
		out->pps.pScalingLists = &out->scaling;
	}

	/* Succeeded: *out holds the PPS and the scaling lists it points at. */
	return 0;
}

/*
 * Makes the picture information of one decode.
 *
 * The three Curr lists are checked before anything is written, so a
 * refusal leaves *info as it was.
 */
int
h265_std_picture(
	const struct h265_picture *picture,
	const struct h265_dpb_plan *plan,
	StdVideoDecodeH265PictureInfo *info)
{
	StdVideoDecodeH265PictureInfo made;
	int error;

	/* Refuses a Curr set that the Vulkan lists cannot hold. */
	if (plan->num_st_before > STD_VIDEO_DECODE_H265_REF_PIC_SET_LIST_SIZE)
		return ENOTSUP;
	if (plan->num_st_after > STD_VIDEO_DECODE_H265_REF_PIC_SET_LIST_SIZE)
		return ENOTSUP;
	if (plan->num_lt_curr > STD_VIDEO_DECODE_H265_REF_PIC_SET_LIST_SIZE)
		return ENOTSUP;

	/* Clears the picture information so the reserved fields are 0. */
	memset(&made, 0, sizeof(made));

	/*
	 * Copies the flags.  Every decoded picture is marked as used for
	 * short-term reference, so IsReference is always 1.
	 */
	made.flags.IrapPicFlag = std_bit(picture->irap);
	made.flags.IdrPicFlag = std_bit(picture->idr);
	made.flags.IsReference = 1U;
	made.flags.short_term_ref_pic_set_sps_flag = std_bit(picture->header.st_rps_sps);

	/* Copies the three identifiers of the parameter sets and the order count. */
	made.sps_video_parameter_set_id = (uint8_t)picture->vps_id;
	made.pps_seq_parameter_set_id = (uint8_t)picture->sps_id;
	made.pps_pic_parameter_set_id = (uint8_t)picture->pps_id;
	made.PicOrderCntVal = picture->poc;

	/*
	 * A set carried in the slice header gives its size in bits, and a
	 * predicted one also the number of entries of the set it refers to,
	 * which a hardware decoder needs to parse the header; both are 0
	 * for a set of the SPS.
	 */
	if (picture->header.st_rps_sps == 0) {
		made.NumBitsForSTRefPicSetInSlice = (uint16_t)picture->header.st_syntax.bits;

		/* Gives the size of the set the slice header's set is predicted from. */
		if (picture->header.st_syntax.inter_rps_pred != 0)
			made.NumDeltaPocsOfRefRpsIdx = (uint8_t)picture->header.st_syntax.ref_num_delta_pocs;
	}

	/* Makes the three Curr lists from the plan's slots. */
	error = std_curr_list(plan->num_st_before, plan->st_before, made.RefPicSetStCurrBefore);
	if (error != 0)
		return error;

	error = std_curr_list(plan->num_st_after, plan->st_after, made.RefPicSetStCurrAfter);
	if (error != 0)
		return error;

	error = std_curr_list(plan->num_lt_curr, plan->lt_curr, made.RefPicSetLtCurr);
	if (error != 0)
		return error;

	/* Publishes the finished picture information. */
	*info = made;

	/* Succeeded: *info describes the picture and its reference lists. */
	return 0;
}

/*
 * Makes the reference information of the plan's references.
 *
 * A count above the plan's arrays is cut to them, so no read leaves the
 * plan.
 */
void
h265_std_references(
	const struct h265_dpb_plan *plan,
	StdVideoDecodeH265ReferenceInfo *info)
{
	unsigned count;
	unsigned i;

	/* Cuts the count to what the plan's arrays hold. */
	count = plan->reference_count;
	if (count > H265_DPB_PICTURES)
		count = H265_DPB_PICTURES;

	/* Makes the information of each reference in the plan's order. */
	for (i = 0U; i < count; i++) {
		memset(&info[i], 0, sizeof(info[i]));

		info[i].PicOrderCntVal = plan->reference_poc[i];
		info[i].flags.used_for_long_term_reference = std_bit((int)plan->reference_long[i]);
	}

	/* Finished: every reference has its information. */
	return;
}

/*
 * Makes the reference information of the picture being decoded.
 *
 * A new picture is a short-term reference, so both flags are 0.
 */
void
h265_std_setup(
	int32_t poc,
	StdVideoDecodeH265ReferenceInfo *info)
{
	/* Clears both flags and stores the order count. */
	memset(info, 0, sizeof(*info));
	info->PicOrderCntVal = poc;

	/* Finished: *info describes the setup slot. */
	return;
}

/*
 * Gives a flag in the form a one-bit field takes: 1 for any non-zero value.
 */
static unsigned
std_bit(
	int value)
{
	/* A set flag is 1 whatever non-zero value the parse kept it as. */
	if (value != 0)
		return 1U;

	/* A clear flag is 0. */
	return 0U;
}

/*
 * Makes the bit mask of a per-sub-layer flag array: bit i is flags[ i ].
 */
static uint32_t
std_flag_mask(
	const uint8_t *flags,
	unsigned count)
{
	uint32_t mask;
	unsigned i;

	/* Sets the bit of every sub-layer whose flag is set. */
	mask = 0U;
	for (i = 0U; i < count; i++) {
		/* A set flag of sub-layer i is bit i of the mask. */
		if (flags[i] != 0U)
			mask |= (uint32_t)1U << i;
	}

	/* Reports the mask. */
	return mask;
}

/*
 * Chooses the StdVideo profile of a profile, tier and level.
 *
 * Returns 0 and *profile; ENOTSUP when the signalled profile is not one
 * the Vulkan enumeration names and no compatibility flag of Main, Main 10
 * or Main Still Picture is set.
 */
static int
std_profile(
	const struct h265sw_ptl *ptl,
	StdVideoH265ProfileIdc *profile)
{
	/* Main, Main 10 and Main Still Picture, the profiles a compatibility flag j of 1, 2 and 3 names. */
	static const StdVideoH265ProfileIdc compatible[3] = {
		STD_VIDEO_H265_PROFILE_IDC_MAIN,
		STD_VIDEO_H265_PROFILE_IDC_MAIN_10,
		STD_VIDEO_H265_PROFILE_IDC_MAIN_STILL_PICTURE
	};
	unsigned j;

	/* Takes a profile that the Vulkan enumeration names as it was signalled. */
	switch (ptl->profile_idc) {
	case 1U:
		*profile = STD_VIDEO_H265_PROFILE_IDC_MAIN;
		return 0;
	case 2U:
		*profile = STD_VIDEO_H265_PROFILE_IDC_MAIN_10;
		return 0;
	case 3U:
		*profile = STD_VIDEO_H265_PROFILE_IDC_MAIN_STILL_PICTURE;
		return 0;
	case 4U:
		*profile = STD_VIDEO_H265_PROFILE_IDC_FORMAT_RANGE_EXTENSIONS;
		return 0;
	case 9U:
		*profile = STD_VIDEO_H265_PROFILE_IDC_SCC_EXTENSIONS;
		return 0;
	default:
		break;
	}

	/* Takes the lowest of Main, Main 10 and Main Still Picture that the stream is compatible with (7.4.4). */
	for (j = 1U; j <= 3U; j++) {
		/* The stream says it conforms to the profile whose idc is j. */
		if (((ptl->compatibility >> j) & 1U) != 0U) {
			*profile = compatible[j - 1U];
			return 0;
		}
	}

	/* The stream names no profile that the driver can use. */
	return ENOTSUP;
}

/*
 * Maps general_level_idc to the next defined level at or above it.
 *
 * A value above the highest defined level, such as the unconstrained
 * level 8.5 (255), maps to the highest, 6.2.
 */
static StdVideoH265LevelIdc
std_level(
	unsigned level_idc)
{
	unsigned i;

	/* Takes the first defined level that is not below the value. */
	for (i = 0U; i < STD_LEVEL_COUNT; i++) {
		/* The value is at or below this level, so this is the next one above it. */
		if (level_idc <= std_level_idc[i])
			return std_level_enum[i];
	}

	/* The value is above every defined level: the highest one stands for it. */
	return std_level_enum[STD_LEVEL_COUNT - 1U];
}

/*
 * Copies the buffering limits of the seven sub-layers (the parse keeps the
 * entries above the highest sub-layer 0).
 */
static void
std_dec_pic_buf_mgr(
	const unsigned *max_dec_pic_buffering_minus1,
	const unsigned *max_num_reorder_pics,
	const uint32_t *max_latency_increase_plus1,
	StdVideoH265DecPicBufMgr *out)
{
	unsigned i;

	/* Copies the three limits of each sub-layer. */
	for (i = 0U; i < STD_VIDEO_H265_SUBLAYERS_LIST_SIZE; i++) {
		out->max_dec_pic_buffering_minus1[i] = (uint8_t)max_dec_pic_buffering_minus1[i];
		out->max_num_reorder_pics[i] = (uint8_t)max_num_reorder_pics[i];
		out->max_latency_increase_plus1[i] = max_latency_increase_plus1[i];
	}

	/* Finished: all seven sub-layers are copied. */
	return;
}

/*
 * Copies the CPB parameters of one sub-layer and one HRD kind (NAL or VCL).
 */
static void
std_sub_layer_hrd(
	const struct h265sw_sub_layer_hrd *in,
	StdVideoH265SubLayerHrdParameters *out)
{
	unsigned j;

	/* Copies the four values of each of the CPB specifications. */
	for (j = 0U; j < STD_VIDEO_H265_CPB_CNT_LIST_SIZE; j++) {
		out->bit_rate_value_minus1[j] = in->bit_rate_value_minus1[j];
		out->cpb_size_value_minus1[j] = in->cpb_size_value_minus1[j];
		out->cpb_size_du_value_minus1[j] = in->cpb_size_du_value_minus1[j];
		out->bit_rate_du_value_minus1[j] = in->bit_rate_du_value_minus1[j];
	}

	/* Copies the constant-bit-rate flags, bit j being the flag of specification j. */
	out->cbr_flag = in->cbr_flags;

	/* Finished: the sub-layer's parameters are copied. */
	return;
}

/*
 * Makes the StdVideo form of one HRD parameters structure.
 *
 * nal and vcl are the arrays of the seven sub-layers' parameters that
 * out points at when the matching present flag is 1; out is cleared
 * first.
 */
static void
std_hrd(
	const struct h265sw_hrd *hrd,
	StdVideoH265HrdParameters *out,
	StdVideoH265SubLayerHrdParameters *nal,
	StdVideoH265SubLayerHrdParameters *vcl)
{
	unsigned i;

	/* Clears the structure so the reserved fields are 0. */
	memset(out, 0, sizeof(*out));

	/* Copies the common flags. */
	out->flags.nal_hrd_parameters_present_flag = std_bit(hrd->nal_hrd_present);
	out->flags.vcl_hrd_parameters_present_flag = std_bit(hrd->vcl_hrd_present);
	out->flags.sub_pic_hrd_params_present_flag = std_bit(hrd->sub_pic_hrd_params_present);
	out->flags.sub_pic_cpb_params_in_pic_timing_sei_flag = std_bit(hrd->sub_pic_cpb_params_in_pic_timing_sei);

	/* Copies the per-sub-layer flags as masks in which bit i is the flag of sub-layer i. */
	out->flags.fixed_pic_rate_general_flag = std_flag_mask(hrd->fixed_pic_rate_general, STD_VIDEO_H265_SUBLAYERS_LIST_SIZE);
	out->flags.fixed_pic_rate_within_cvs_flag = std_flag_mask(hrd->fixed_pic_rate_within_cvs, STD_VIDEO_H265_SUBLAYERS_LIST_SIZE);
	out->flags.low_delay_hrd_flag = std_flag_mask(hrd->low_delay_hrd, STD_VIDEO_H265_SUBLAYERS_LIST_SIZE);

	/* Copies the common fields. */
	out->tick_divisor_minus2 = (uint8_t)hrd->tick_divisor_minus2;
	out->du_cpb_removal_delay_increment_length_minus1 = (uint8_t)hrd->du_cpb_removal_delay_increment_length_minus1;
	out->dpb_output_delay_du_length_minus1 = (uint8_t)hrd->dpb_output_delay_du_length_minus1;
	out->bit_rate_scale = (uint8_t)hrd->bit_rate_scale;
	out->cpb_size_scale = (uint8_t)hrd->cpb_size_scale;
	out->cpb_size_du_scale = (uint8_t)hrd->cpb_size_du_scale;
	out->initial_cpb_removal_delay_length_minus1 = (uint8_t)hrd->initial_cpb_removal_delay_length_minus1;
	out->au_cpb_removal_delay_length_minus1 = (uint8_t)hrd->au_cpb_removal_delay_length_minus1;
	out->dpb_output_delay_length_minus1 = (uint8_t)hrd->dpb_output_delay_length_minus1;

	/* Copies the CPB count and the duration of each sub-layer. */
	for (i = 0U; i < STD_VIDEO_H265_SUBLAYERS_LIST_SIZE; i++) {
		out->cpb_cnt_minus1[i] = hrd->cpb_cnt_minus1[i];
		out->elemental_duration_in_tc_minus1[i] = hrd->elemental_duration_in_tc_minus1[i];
	}

	/* Makes the NAL HRD's sub-layer parameters and points at them when the NAL HRD is present. */
	if (hrd->nal_hrd_present != 0) {
		for (i = 0U; i < STD_VIDEO_H265_SUBLAYERS_LIST_SIZE; i++)
			std_sub_layer_hrd(&hrd->nal[i], &nal[i]);

		out->pSubLayerHrdParametersNal = nal;
	}

	/* Makes the VCL HRD's sub-layer parameters and points at them when the VCL HRD is present. */
	if (hrd->vcl_hrd_present != 0) {
		for (i = 0U; i < STD_VIDEO_H265_SUBLAYERS_LIST_SIZE; i++)
			std_sub_layer_hrd(&hrd->vcl[i], &vcl[i]);

		out->pSubLayerHrdParametersVcl = vcl;
	}

	/* Finished: the HRD parameters are copied. */
	return;
}

/*
 * Makes the StdVideo scaling lists from the parse's lists.
 *
 * The parse keeps ScalingList[ sizeId ][ matrixId ][ i ] in the order the
 * syntax codes it, the order of the StdVideo arrays.  The 32x32 arrays
 * hold the two matrices the syntax codes (matrixId 0 and 3), and so do
 * their DC arrays.
 */
static void
std_scaling_lists(
	const struct h265sw_scaling_list *in,
	StdVideoH265ScalingLists *out)
{
	unsigned i;

	/* Clears the lists so every entry past a list's length is 0. */
	memset(out, 0, sizeof(*out));

	/* Copies the 4x4 lists (sizeId 0, 16 entries). */
	for (i = 0U; i < STD_VIDEO_H265_SCALING_LIST_4X4_NUM_LISTS; i++)
		memcpy(out->ScalingList4x4[i], in->list[0][i], STD_VIDEO_H265_SCALING_LIST_4X4_NUM_ELEMENTS);

	/* Copies the 8x8 lists (sizeId 1, 64 entries). */
	for (i = 0U; i < STD_VIDEO_H265_SCALING_LIST_8X8_NUM_LISTS; i++)
		memcpy(out->ScalingList8x8[i], in->list[1][i], STD_VIDEO_H265_SCALING_LIST_8X8_NUM_ELEMENTS);

	/* Copies the 16x16 lists (sizeId 2) and their DC values. */
	for (i = 0U; i < STD_VIDEO_H265_SCALING_LIST_16X16_NUM_LISTS; i++) {
		memcpy(out->ScalingList16x16[i], in->list[2][i], STD_VIDEO_H265_SCALING_LIST_16X16_NUM_ELEMENTS);
		out->ScalingListDCCoef16x16[i] = in->dc[0][i];
	}

	/* Copies the two 32x32 lists (sizeId 3, matrixId 0 and 3) and their DC values. */
	for (i = 0U; i < STD_VIDEO_H265_SCALING_LIST_32X32_NUM_LISTS; i++) {
		memcpy(out->ScalingList32x32[i], in->list[3][i * 3U], STD_VIDEO_H265_SCALING_LIST_32X32_NUM_ELEMENTS);
		out->ScalingListDCCoef32x32[i] = in->dc[1][i * 3U];
	}

	/* Finished: the lists are copied. */
	return;
}

/*
 * Makes the StdVideo form of the video usability information, without its
 * HRD pointer (the caller sets it).
 *
 * sar_width and sar_height are the signalled values, which are 0 unless
 * aspect_ratio_idc is EXTENDED_SAR, not the derived ratio.
 */
static void
std_vui(
	const struct h265sw_vui *vui,
	StdVideoH265SequenceParameterSetVui *out)
{
	/* Clears the structure so the reserved fields are 0. */
	memset(out, 0, sizeof(*out));

	/* Copies the flags in the order of the syntax. */
	out->flags.aspect_ratio_info_present_flag = std_bit(vui->aspect_ratio_present);
	out->flags.overscan_info_present_flag = std_bit(vui->overscan_present);
	out->flags.overscan_appropriate_flag = std_bit(vui->overscan_appropriate);
	out->flags.video_signal_type_present_flag = std_bit(vui->signal_present);
	out->flags.video_full_range_flag = std_bit(vui->full_range);
	out->flags.colour_description_present_flag = std_bit(vui->colour_present);
	out->flags.chroma_loc_info_present_flag = std_bit(vui->chroma_loc_present);
	out->flags.neutral_chroma_indication_flag = std_bit(vui->neutral_chroma_indication);
	out->flags.field_seq_flag = std_bit(vui->field_seq);
	out->flags.frame_field_info_present_flag = std_bit(vui->frame_field_info_present);
	out->flags.default_display_window_flag = std_bit(vui->default_display_window);
	out->flags.vui_timing_info_present_flag = std_bit(vui->timing_present);
	out->flags.vui_poc_proportional_to_timing_flag = std_bit(vui->poc_proportional_to_timing);
	out->flags.vui_hrd_parameters_present_flag = std_bit(vui->hrd_present);
	out->flags.bitstream_restriction_flag = std_bit(vui->bitstream_restriction);
	out->flags.tiles_fixed_structure_flag = std_bit(vui->tiles_fixed_structure);
	out->flags.motion_vectors_over_pic_boundaries_flag = std_bit(vui->motion_vectors_over_pic_boundaries);
	out->flags.restricted_ref_pic_lists_flag = std_bit(vui->restricted_ref_pic_lists);

	/* Copies the sample aspect ratio as signalled. */
	out->aspect_ratio_idc = (StdVideoH265AspectRatioIdc)vui->aspect_ratio_idc;
	out->sar_width = (uint16_t)vui->signalled_sar_width;
	out->sar_height = (uint16_t)vui->signalled_sar_height;

	/* Copies the video format, the colour description and the chroma sample locations. */
	out->video_format = (uint8_t)vui->video_format;
	out->colour_primaries = (uint8_t)vui->colour_primaries;
	out->transfer_characteristics = (uint8_t)vui->transfer_characteristics;
	out->matrix_coeffs = (uint8_t)vui->matrix_coeffs;
	out->chroma_sample_loc_type_top_field = (uint8_t)vui->chroma_sample_loc_type_top_field;
	out->chroma_sample_loc_type_bottom_field = (uint8_t)vui->chroma_sample_loc_type_bottom_field;

	/* Copies the default display window offsets. */
	out->def_disp_win_left_offset = (uint16_t)vui->def_disp_win_left_offset;
	out->def_disp_win_right_offset = (uint16_t)vui->def_disp_win_right_offset;
	out->def_disp_win_top_offset = (uint16_t)vui->def_disp_win_top_offset;
	out->def_disp_win_bottom_offset = (uint16_t)vui->def_disp_win_bottom_offset;

	/* Copies the timing information. */
	out->vui_num_units_in_tick = vui->num_units_in_tick;
	out->vui_time_scale = vui->time_scale;
	out->vui_num_ticks_poc_diff_one_minus1 = vui->num_ticks_poc_diff_one_minus1;

	/* Copies the bitstream restriction fields. */
	out->min_spatial_segmentation_idc = (uint16_t)vui->min_spatial_segmentation_idc;
	out->max_bytes_per_pic_denom = (uint8_t)vui->max_bytes_per_pic_denom;
	out->max_bits_per_min_cu_denom = (uint8_t)vui->max_bits_per_min_cu_denom;
	out->log2_max_mv_length_horizontal = (uint8_t)vui->log2_max_mv_length_horizontal;
	out->log2_max_mv_length_vertical = (uint8_t)vui->log2_max_mv_length_vertical;

	/* Finished: the usability information is copied. */
	return;
}

/*
 * Makes the StdVideo form of one short-term reference picture set, with
 * the prediction syntax and the explicit form both filled in.
 *
 * A set coded explicitly carries its syntax as signalled.  A predicted
 * set carries the prediction syntax and the derived set written as an
 * explicit one: delta_poc_s0_minus1[ i ] is the step from DeltaPocS0[
 * i - 1 ] (0 for i = 0) down to DeltaPocS0[ i ], minus 1, and
 * delta_poc_s1_minus1[ i ] the step up from DeltaPocS1[ i - 1 ] to
 * DeltaPocS1[ i ], minus 1 (7-67 to 7-70 solved for the syntax elements);
 * each used flag is bit i of its mask.
 *
 * Returns 0; EINVAL when a count does not fit the arrays or a step is not
 * in 1..65536, the range of a minus1 element plus 1.
 */
static int
std_short_term_set(
	const struct h265sw_st_rps *rps,
	const struct h265sw_st_rps_syntax *syntax,
	StdVideoH265ShortTermRefPicSet *out)
{
	int64_t previous;
	int64_t step;
	unsigned i;

	/* Clears the set so the reserved fields and the unused entries are 0. */
	memset(out, 0, sizeof(*out));

	/* Copies the prediction syntax; its fields are 0 for a set coded explicitly. */
	out->flags.inter_ref_pic_set_prediction_flag = std_bit(syntax->inter_rps_pred);
	out->flags.delta_rps_sign = std_bit(syntax->delta_rps_sign);
	out->delta_idx_minus1 = syntax->delta_idx_minus1;
	out->use_delta_flag = (uint16_t)syntax->use_delta_flags;
	out->abs_delta_rps_minus1 = (uint16_t)syntax->abs_delta_rps_minus1;
	out->used_by_curr_pic_flag = (uint16_t)syntax->used_by_curr_pic_flags;

	/* Copies a set coded explicitly as it was signalled; both forms are then the same. */
	if (syntax->inter_rps_pred == 0) {
		if (syntax->num_negative_pics > STD_RPS_MAX_ENTRIES)
			return EINVAL;
		if (syntax->num_positive_pics > STD_RPS_MAX_ENTRIES)
			return EINVAL;

		out->num_negative_pics = (uint8_t)syntax->num_negative_pics;
		out->num_positive_pics = (uint8_t)syntax->num_positive_pics;
		out->used_by_curr_pic_s0_flag = (uint16_t)syntax->used_by_curr_pic_s0_flags;
		out->used_by_curr_pic_s1_flag = (uint16_t)syntax->used_by_curr_pic_s1_flags;

		for (i = 0U; i < syntax->num_negative_pics; i++)
			out->delta_poc_s0_minus1[i] = syntax->delta_poc_s0_minus1[i];
		for (i = 0U; i < syntax->num_positive_pics; i++)
			out->delta_poc_s1_minus1[i] = syntax->delta_poc_s1_minus1[i];

		/* Succeeded: the explicit set is copied. */
		return 0;
	}

	/* Refuses a derived set with more entries than the arrays hold. */
	if (rps->num_negative > STD_RPS_MAX_ENTRIES)
		return EINVAL;
	if (rps->num_positive > STD_RPS_MAX_ENTRIES)
		return EINVAL;
	if (rps->num_negative + rps->num_positive > STD_RPS_MAX_ENTRIES)
		return EINVAL;

	/* Writes the derived set's counts. */
	out->num_negative_pics = (uint8_t)rps->num_negative;
	out->num_positive_pics = (uint8_t)rps->num_positive;

	/* Writes the negative pictures: the step down from the one before, minus 1, and the used flag. */
	previous = 0;
	for (i = 0U; i < rps->num_negative; i++) {
		step = previous - (int64_t)rps->delta_poc[i] - 1;
		if (step < 0 || step > STD_DELTA_POC_MINUS1_MAX)
			return EINVAL;

		out->delta_poc_s0_minus1[i] = (uint16_t)step;

		/* Sets the bit of a picture that the current picture uses as a reference. */
		if (rps->used[i] != 0U)
			out->used_by_curr_pic_s0_flag |= (uint16_t)((uint16_t)1U << i);

		previous = (int64_t)rps->delta_poc[i];
	}

	/* Writes the positive pictures: the step up from the one before, minus 1, and the used flag. */
	previous = 0;
	for (i = 0U; i < rps->num_positive; i++) {
		step = (int64_t)rps->delta_poc[rps->num_negative + i] - previous - 1;
		if (step < 0 || step > STD_DELTA_POC_MINUS1_MAX)
			return EINVAL;

		out->delta_poc_s1_minus1[i] = (uint16_t)step;

		/* Sets the bit of a picture that the current picture uses as a reference. */
		if (rps->used[rps->num_negative + i] != 0U)
			out->used_by_curr_pic_s1_flag |= (uint16_t)((uint16_t)1U << i);

		previous = (int64_t)rps->delta_poc[rps->num_negative + i];
	}

	/* Succeeded: the predicted set is copied with its explicit form. */
	return 0;
}

/*
 * Copies the flags of an SPS, including the nine range extension flags.
 *
 * Bit n of range_extension is the n-th flag of sps_range_extension() in
 * the order of the syntax (7.3.2.2.2).  The screen content flags are 0:
 * the parse refuses the streams that use them.
 */
static void
std_sps_flags(
	const struct h265sw_sps *sps,
	StdVideoH265SpsFlags *flags)
{
	/* Copies the flags of the base syntax. */
	flags->sps_temporal_id_nesting_flag = std_bit(sps->temporal_id_nesting);
	flags->separate_colour_plane_flag = std_bit(sps->separate_colour_plane);
	flags->conformance_window_flag = std_bit(sps->conformance_window);
	flags->sps_sub_layer_ordering_info_present_flag = std_bit(sps->sub_layer_ordering_info_present);
	flags->scaling_list_enabled_flag = std_bit(sps->scaling_list_enabled);
	flags->sps_scaling_list_data_present_flag = std_bit(sps->scaling_list_data_present);
	flags->amp_enabled_flag = std_bit(sps->amp);
	flags->sample_adaptive_offset_enabled_flag = std_bit(sps->sao);
	flags->pcm_enabled_flag = std_bit(sps->pcm);
	flags->pcm_loop_filter_disabled_flag = std_bit(sps->pcm_loop_filter_disabled);
	flags->long_term_ref_pics_present_flag = std_bit(sps->long_term_present);
	flags->sps_temporal_mvp_enabled_flag = std_bit(sps->temporal_mvp);
	flags->strong_intra_smoothing_enabled_flag = std_bit(sps->strong_intra_smoothing);
	flags->vui_parameters_present_flag = std_bit(sps->vui_present);
	flags->sps_extension_present_flag = std_bit(sps->extension_present);
	flags->sps_range_extension_flag = std_bit(sps->range_extension_present);

	/* Copies the nine flags of the range extension, bit 0 first. */
	flags->transform_skip_rotation_enabled_flag = std_bit((int)((sps->range_extension >> 0) & 1U));
	flags->transform_skip_context_enabled_flag = std_bit((int)((sps->range_extension >> 1) & 1U));
	flags->implicit_rdpcm_enabled_flag = std_bit((int)((sps->range_extension >> 2) & 1U));
	flags->explicit_rdpcm_enabled_flag = std_bit((int)((sps->range_extension >> 3) & 1U));
	flags->extended_precision_processing_flag = std_bit((int)((sps->range_extension >> 4) & 1U));
	flags->intra_smoothing_disabled_flag = std_bit((int)((sps->range_extension >> 5) & 1U));
	flags->high_precision_offsets_enabled_flag = std_bit((int)((sps->range_extension >> 6) & 1U));
	flags->persistent_rice_adaptation_enabled_flag = std_bit((int)((sps->range_extension >> 7) & 1U));
	flags->cabac_bypass_alignment_enabled_flag = std_bit((int)((sps->range_extension >> 8) & 1U));

	/* Finished: the screen content flags stay 0. */
	return;
}

/*
 * Copies the sizes, identifiers and block size fields of an SPS in the
 * minus and difference forms of the syntax, and the PCM and conformance
 * window fields.
 */
static void
std_sps_sizes(
	const struct h265sw_sps *sps,
	StdVideoH265SequenceParameterSet *out)
{
	/* Copies the chroma format and the picture size. */
	out->chroma_format_idc = (StdVideoH265ChromaFormatIdc)sps->chroma_format_idc;
	out->pic_width_in_luma_samples = sps->width;
	out->pic_height_in_luma_samples = sps->height;

	/* Copies the identifiers and the bit depths and order count length in the minus forms. */
	out->sps_video_parameter_set_id = (uint8_t)sps->vps_id;
	out->sps_max_sub_layers_minus1 = (uint8_t)sps->max_sub_layers_minus1;
	out->sps_seq_parameter_set_id = (uint8_t)sps->id;
	out->bit_depth_luma_minus8 = (uint8_t)(sps->bit_depth_luma - 8U);
	out->bit_depth_chroma_minus8 = (uint8_t)(sps->bit_depth_chroma - 8U);
	out->log2_max_pic_order_cnt_lsb_minus4 = (uint8_t)(sps->log2_max_poc_lsb - 4U);

	/* Copies the coding block and transform block sizes in the minus and difference forms. */
	out->log2_min_luma_coding_block_size_minus3 = (uint8_t)(sps->min_cb_log2 - 3U);
	out->log2_diff_max_min_luma_coding_block_size = (uint8_t)(sps->ctb_log2 - sps->min_cb_log2);
	out->log2_min_luma_transform_block_size_minus2 = (uint8_t)(sps->min_tb_log2 - 2U);
	out->log2_diff_max_min_luma_transform_block_size = (uint8_t)(sps->max_tb_log2 - sps->min_tb_log2);
	out->max_transform_hierarchy_depth_inter = (uint8_t)sps->max_th_depth_inter;
	out->max_transform_hierarchy_depth_intra = (uint8_t)sps->max_th_depth_intra;

	/* Copies the number of short-term sets and of long-term pictures of the SPS. */
	out->num_short_term_ref_pic_sets = (uint8_t)sps->num_st_rps;
	out->num_long_term_ref_pics_sps = (uint8_t)sps->num_lt_sps;

	/* Copies the PCM sizes in the minus and difference forms; they are 0 when PCM is off. */
	if (sps->pcm != 0) {
		out->pcm_sample_bit_depth_luma_minus1 = (uint8_t)(sps->pcm_bit_depth_luma - 1U);
		out->pcm_sample_bit_depth_chroma_minus1 = (uint8_t)(sps->pcm_bit_depth_chroma - 1U);
		out->log2_min_pcm_luma_coding_block_size_minus3 = (uint8_t)(sps->pcm_min_log2 - 3U);
		out->log2_diff_max_min_pcm_luma_coding_block_size = (uint8_t)(sps->pcm_max_log2 - sps->pcm_min_log2);
	}

	/* Copies the conformance window offsets as signalled: left, right, top, bottom. */
	out->conf_win_left_offset = sps->conf_win_offset[0];
	out->conf_win_right_offset = sps->conf_win_offset[1];
	out->conf_win_top_offset = sps->conf_win_offset[2];
	out->conf_win_bottom_offset = sps->conf_win_offset[3];

	/* Finished: the sizes are copied. */
	return;
}

/*
 * Copies the flags of a PPS.  The flags of the extensions whose tools the
 * parse refuses stay 0, except pps_range_extension_flag.
 */
static void
std_pps_flags(
	const struct h265sw_pps *pps,
	StdVideoH265PpsFlags *flags)
{
	/* Copies the flags of the base syntax. */
	flags->dependent_slice_segments_enabled_flag = std_bit(pps->dependent_slices);
	flags->output_flag_present_flag = std_bit(pps->output_flag_present);
	flags->sign_data_hiding_enabled_flag = std_bit(pps->sign_data_hiding);
	flags->cabac_init_present_flag = std_bit(pps->cabac_init_present);
	flags->constrained_intra_pred_flag = std_bit(pps->constrained_intra);
	flags->transform_skip_enabled_flag = std_bit(pps->transform_skip);
	flags->cu_qp_delta_enabled_flag = std_bit(pps->cu_qp_delta);
	flags->pps_slice_chroma_qp_offsets_present_flag = std_bit(pps->slice_chroma_qp_offsets_present);
	flags->weighted_pred_flag = std_bit(pps->weighted_pred);
	flags->weighted_bipred_flag = std_bit(pps->weighted_bipred);
	flags->transquant_bypass_enabled_flag = std_bit(pps->transquant_bypass);
	flags->tiles_enabled_flag = std_bit(pps->tiles);
	flags->entropy_coding_sync_enabled_flag = std_bit(pps->entropy_sync);
	flags->uniform_spacing_flag = std_bit(pps->uniform_spacing);
	flags->loop_filter_across_tiles_enabled_flag = std_bit(pps->loop_filter_across_tiles);
	flags->pps_loop_filter_across_slices_enabled_flag = std_bit(pps->loop_filter_across_slices);
	flags->deblocking_filter_control_present_flag = std_bit(pps->deblocking_control_present);
	flags->deblocking_filter_override_enabled_flag = std_bit(pps->deblocking_override_enabled);
	flags->pps_deblocking_filter_disabled_flag = std_bit(pps->deblocking_disabled);
	flags->pps_scaling_list_data_present_flag = std_bit(pps->scaling_list_present);
	flags->lists_modification_present_flag = std_bit(pps->lists_modification_present);
	flags->slice_segment_header_extension_present_flag = std_bit(pps->slice_header_extension_present);
	flags->pps_extension_present_flag = std_bit(pps->extension_present);
	flags->pps_range_extension_flag = std_bit(pps->range_extension_present);

	/* Finished: the extension tool flags stay 0. */
	return;
}

/*
 * Makes one Curr list of the picture information: the slots of the plan's
 * list, STD_VIDEO_H265_NO_REFERENCE_PICTURE for a missing reference and
 * for every entry past the count.
 *
 * Returns 0; EINVAL for a slot that does not fit a uint8_t entry below
 * the "no reference" value.
 */
static int
std_curr_list(
	unsigned count,
	const int *slots,
	uint8_t *out)
{
	unsigned i;

	/* Marks every entry as no reference picture first. */
	for (i = 0U; i < STD_VIDEO_DECODE_H265_REF_PIC_SET_LIST_SIZE; i++)
		out[i] = STD_VIDEO_H265_NO_REFERENCE_PICTURE;

	/* Stores the slot of each listed reference; a missing one stays "no reference". */
	for (i = 0U; i < count && i < STD_VIDEO_DECODE_H265_REF_PIC_SET_LIST_SIZE; i++) {
		/* A missing reference picture (or any negative slot) keeps the "no reference" entry. */
		if (slots[i] < 0)
			continue;

		/* Refuses a slot that cannot be told apart from the "no reference" entry. */
		if (slots[i] >= STD_VIDEO_H265_NO_REFERENCE_PICTURE)
			return EINVAL;

		out[i] = (uint8_t)slots[i];
	}

	/* Succeeded: the list is made. */
	return 0;
}
