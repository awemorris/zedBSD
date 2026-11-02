/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The H.264 sequence and picture parameter sets written back from their
 * StdVideo fields (see vpu-codec.h), for a decoder core that parses them
 * from the bitstream itself.
 *
 * Procedure referenced: ITU-T H.264 (08/2021) 7.3.2.1.1 (the sequence
 * parameter set syntax), 7.3.2.1.1.1 (the scaling list syntax), 7.3.2.2
 * (the picture parameter set syntax), 7.4.1 (emulation prevention), E.1.1
 * (the VUI syntax) and A.3.1 Table A-1 (MaxDpbMbs); zedBSD
 * src/drivers/platform/rpi4/rpi4-h264.c (drv_rpi4_h264_write_sps,
 * drv_rpi4_h264_write_pps: the same field order and the same choices).
 */

#include <stddef.h>
#include <stdint.h>

#include <kern/kcrt.h>
#include <uapi/errno.h>

#include "drivers/vpu/vpu-bits.h"
#include "drivers/vpu/vpu-codec.h"

/* The most RBSP bytes a set takes here (far more than any real one). */
#define RBSP_MAX			2048U

/* The value the no-reorder VUI writes for both log2_max_mv_length fields. */
#define MAX_MV_LENGTH			15U

/* The most StdVideoH264LevelIdc enumerant (6.2). */
#define LEVEL_LAST			18U

static int check_sps(const struct vpu_h264_sps *sps);
static int check_pps(const struct vpu_h264_pps *pps);
static void write_scaling(struct vpu_bits *writer, const struct vpu_h264_scaling *scaling, uint32_t lists);
static void write_vui_no_reorder(struct vpu_bits *writer, uint32_t buffering);
static int high_profile(uint32_t profile);
static uint32_t level_number(uint32_t level);
static uint32_t dpb_frames(uint32_t level_idc, uint32_t mbs, uint32_t references);

/*
 * Writes an SPS NAL unit from its fields, field by field in 7.3.2.1.1's
 * order; scaling lists are coded as present, default or explicit with
 * every delta written.
 */
int
vpu_h264_write_sps(
	const struct vpu_h264_sps *sps,
	int no_reorder,
	uint8_t *output,
	size_t capacity,
	size_t *written)
{
	uint8_t rbsp[RBSP_MAX];
	struct vpu_bits writer;
	uint32_t constraints;
	uint32_t lists;
	uint32_t index;
	uint32_t frame_rows;
	uint32_t buffering;
	uint32_t mbs;
	int high;
	int error;

	/* Refuses fields H.264 cannot code. */
	error = check_sps(sps);
	if (error != 0)
		return error;

	/* The RBSP is written into a zeroed scratch first. */
	vpu_bits_start(&writer, rbsp, sizeof(rbsp));

	/* constraint_set0..5 from their flags, most significant first, and two reserved zeros. */
	constraints = 0;
	for (index = 0; index < 6U; index++) {
		if ((sps->flags & (VPU_H264_SPS_CONSTRAINT_SET0 << index)) != 0)
			constraints |= 0x80U >> index;
	}

	/* The profile, the constraint flags, the level and the id. */
	vpu_bits_put(&writer, sps->profile_idc, 8);
	vpu_bits_put(&writer, constraints, 8);
	vpu_bits_put(&writer, level_number(sps->level), 8);
	vpu_bits_ue(&writer, sps->id);

	/* The high profiles' chroma format, bit depths and scaling matrix. */
	high = high_profile(sps->profile_idc);
	if (high) {
		vpu_bits_ue(&writer, sps->chroma_format_idc);
		if (sps->chroma_format_idc == 3U)
			vpu_bits_flag(&writer, sps->flags, VPU_H264_SPS_SEPARATE_PLANES);
		vpu_bits_ue(&writer, sps->bit_depth_luma_minus8);
		vpu_bits_ue(&writer, sps->bit_depth_chroma_minus8);
		vpu_bits_flag(&writer, sps->flags, VPU_H264_SPS_QPPRIME_BYPASS);
		vpu_bits_flag(&writer, sps->flags, VPU_H264_SPS_SCALING);
		if ((sps->flags & VPU_H264_SPS_SCALING) != 0) {
			lists = 8U;
			if (sps->chroma_format_idc == 3U)
				lists = 12U;
			write_scaling(&writer, &sps->scaling, lists);
		}
	}

	/* The frame number and the picture order count syntax. */
	vpu_bits_ue(&writer, sps->log2_max_frame_num_minus4);
	vpu_bits_ue(&writer, sps->pic_order_cnt_type);
	if (sps->pic_order_cnt_type == 0) {
		vpu_bits_ue(&writer, sps->log2_max_pic_order_cnt_lsb_minus4);
	} else if (sps->pic_order_cnt_type == 1U) {
		vpu_bits_flag(&writer, sps->flags, VPU_H264_SPS_DELTA_POC_ZERO);
		vpu_bits_se(&writer, sps->offset_for_non_ref_pic);
		vpu_bits_se(&writer, sps->offset_for_top_to_bottom_field);
		vpu_bits_ue(&writer, sps->offset_count);

		/* Each offset of the cycle. */
		for (index = 0; index < sps->offset_count; index++)
			vpu_bits_se(&writer, sps->offsets[index]);
	}

	/* The references, the size, the field flags. */
	vpu_bits_ue(&writer, sps->max_num_ref_frames);
	vpu_bits_flag(&writer, sps->flags, VPU_H264_SPS_GAPS_ALLOWED);
	vpu_bits_ue(&writer, sps->pic_width_in_mbs_minus1);
	vpu_bits_ue(&writer, sps->pic_height_in_map_units_minus1);
	vpu_bits_flag(&writer, sps->flags, VPU_H264_SPS_FRAME_MBS_ONLY);
	if ((sps->flags & VPU_H264_SPS_FRAME_MBS_ONLY) == 0)
		vpu_bits_flag(&writer, sps->flags, VPU_H264_SPS_MB_ADAPTIVE);
	vpu_bits_flag(&writer, sps->flags, VPU_H264_SPS_DIRECT_8X8);

	/* The cropping, each offset when it is there. */
	vpu_bits_flag(&writer, sps->flags, VPU_H264_SPS_CROPPING);
	if ((sps->flags & VPU_H264_SPS_CROPPING) != 0) {
		for (index = 0; index < 4U; index++)
			vpu_bits_ue(&writer, sps->crop[index]);
	}

	/* A map unit is two rows of macroblocks in a stream of fields. */
	frame_rows = 2U;
	if ((sps->flags & VPU_H264_SPS_FRAME_MBS_ONLY) != 0)
		frame_rows = 1U;

	/* The VUI asking for no reordering, or none. */
	if (no_reorder) {
		mbs = (sps->pic_width_in_mbs_minus1 + 1U) * (sps->pic_height_in_map_units_minus1 + 1U) * frame_rows;
		buffering = dpb_frames(level_number(sps->level), mbs, sps->max_num_ref_frames);
		write_vui_no_reorder(&writer, buffering);
	} else {
		vpu_bits_put(&writer, 0, 1);
	}

	/* Ends the RBSP. */
	vpu_bits_trailing(&writer);
	if (writer.error != 0)
		return ENOSPC;

	/* The start code, the header (nal_ref_idc 3, type 7) and the escaped body. */
	error = vpu_bits_nal(0x67U, 1, rbsp, writer.position / 8U, output, capacity, written);
	if (error != 0)
		return error;

	/* Succeeded: output holds the unit. */
	return 0;
}

/*
 * Writes a PPS NAL unit from its fields (7.3.2.2), with one slice group.
 * The optional tail (the 8x8 transform, the scaling lists and the second
 * chroma offset) is written only when one of them differs from what its
 * absence means.
 */
int
vpu_h264_write_pps(
	const struct vpu_h264_pps *pps,
	const struct vpu_h264_sps *sps,
	uint8_t *output,
	size_t capacity,
	size_t *written)
{
	uint8_t rbsp[RBSP_MAX];
	struct vpu_bits writer;
	uint32_t lists;
	int tail;
	int error;

	/* Refuses fields H.264 cannot code. */
	error = check_pps(pps);
	if (error != 0)
		return error;

	/* The RBSP is written into a zeroed scratch first. */
	vpu_bits_start(&writer, rbsp, sizeof(rbsp));

	/* The ids, the entropy coder, the field order flag and one slice group. */
	vpu_bits_ue(&writer, pps->pps_id);
	vpu_bits_ue(&writer, pps->sps_id);
	vpu_bits_flag(&writer, pps->flags, VPU_H264_PPS_CABAC);
	vpu_bits_flag(&writer, pps->flags, VPU_H264_PPS_BOTTOM_FIELD_POC);
	vpu_bits_ue(&writer, 0);

	/* The default references, the weighted prediction and the quantizers. */
	vpu_bits_ue(&writer, pps->num_ref_idx_l0_default_active_minus1);
	vpu_bits_ue(&writer, pps->num_ref_idx_l1_default_active_minus1);
	vpu_bits_flag(&writer, pps->flags, VPU_H264_PPS_WEIGHTED_PRED);
	vpu_bits_put(&writer, pps->weighted_bipred_idc, 2);
	vpu_bits_se(&writer, pps->pic_init_qp_minus26);
	vpu_bits_se(&writer, pps->pic_init_qs_minus26);
	vpu_bits_se(&writer, pps->chroma_qp_index_offset);
	vpu_bits_flag(&writer, pps->flags, VPU_H264_PPS_DEBLOCKING_CONTROL);
	vpu_bits_flag(&writer, pps->flags, VPU_H264_PPS_CONSTRAINED_INTRA);
	vpu_bits_flag(&writer, pps->flags, VPU_H264_PPS_REDUNDANT_PIC_CNT);

	/* The optional tail is there when the transform, the lists or the second offset say something. */
	tail = 0;
	if ((pps->flags & (VPU_H264_PPS_TRANSFORM_8X8 | VPU_H264_PPS_SCALING)) != 0)
		tail = 1;
	if (pps->second_chroma_qp_index_offset != pps->chroma_qp_index_offset)
		tail = 1;

	/* The tail: the transform, the lists (6, two more with the 8x8 transform, four more at 4:4:4) and the offset. */
	if (tail) {
		vpu_bits_flag(&writer, pps->flags, VPU_H264_PPS_TRANSFORM_8X8);
		vpu_bits_flag(&writer, pps->flags, VPU_H264_PPS_SCALING);
		if ((pps->flags & VPU_H264_PPS_SCALING) != 0) {
			lists = 6U;
			if ((pps->flags & VPU_H264_PPS_TRANSFORM_8X8) != 0) {
				lists += 2U;
				if (sps != NULL && sps->chroma_format_idc == 3U)
					lists += 4U;
			}

			write_scaling(&writer, &pps->scaling, lists);
		}

		vpu_bits_se(&writer, pps->second_chroma_qp_index_offset);
	}

	/* Ends the RBSP. */
	vpu_bits_trailing(&writer);
	if (writer.error != 0)
		return ENOSPC;

	/* The start code, the header (nal_ref_idc 3, type 8) and the escaped body. */
	error = vpu_bits_nal(0x68U, 1, rbsp, writer.position / 8U, output, capacity, written);
	if (error != 0)
		return error;

	/* Succeeded: output holds the unit. */
	return 0;
}

/* Checks an SPS's fields against 7.4.2.1.1's ranges and what the writer holds; EINVAL for one outside. */
static int
check_sps(
	const struct vpu_h264_sps *sps)
{
	uint32_t index;

	/* The id, the level, the chroma format, the bit depths. */
	if (sps->offset_count > 255U || sps->id > 31U || sps->level > LEVEL_LAST)
		return EINVAL;
	if (sps->chroma_format_idc > 3U || sps->bit_depth_luma_minus8 > 6U || sps->bit_depth_chroma_minus8 > 6U)
		return EINVAL;

	/* The numbering and the order counts. */
	if (sps->log2_max_frame_num_minus4 > 12U || sps->pic_order_cnt_type > 2U || sps->log2_max_pic_order_cnt_lsb_minus4 > 12U)
		return EINVAL;

	/* The references and the size. */
	if (sps->max_num_ref_frames > 16U || sps->pic_width_in_mbs_minus1 > 1023U || sps->pic_height_in_map_units_minus1 > 1023U)
		return EINVAL;

	/* Each crop offset within what Exp-Golomb holds here. */
	for (index = 0; index < 4U; index++) {
		if (sps->crop[index] > 0xffffU)
			return EINVAL;
	}

	/* The order count offsets within 32 bits. */
	if (sps->offset_for_non_ref_pic == INT32_MIN || sps->offset_for_top_to_bottom_field == INT32_MIN)
		return EINVAL;

	/* Each offset of the cycle within 32 bits. */
	for (index = 0; index < sps->offset_count; index++) {
		if (sps->offsets[index] == INT32_MIN)
			return EINVAL;
	}

	/* Succeeded: the set can be written. */
	return 0;
}

/* Checks a PPS's fields against 7.4.2.2's ranges; EINVAL for one outside. */
static int
check_pps(
	const struct vpu_h264_pps *pps)
{
	/* The ids, the prediction mode and the default references. */
	if (pps->pps_id > 255U || pps->sps_id > 31U || pps->weighted_bipred_idc > 2U)
		return EINVAL;
	if (pps->num_ref_idx_l0_default_active_minus1 > 31U || pps->num_ref_idx_l1_default_active_minus1 > 31U)
		return EINVAL;

	/* The initial quantizers. */
	if (pps->pic_init_qp_minus26 < -64 || pps->pic_init_qp_minus26 > 25)
		return EINVAL;
	if (pps->pic_init_qs_minus26 < -26 || pps->pic_init_qs_minus26 > 25)
		return EINVAL;

	/* The chroma offsets. */
	if (pps->chroma_qp_index_offset < -12 || pps->chroma_qp_index_offset > 12)
		return EINVAL;
	if (pps->second_chroma_qp_index_offset < -12 || pps->second_chroma_qp_index_offset > 12)
		return EINVAL;

	/* Succeeded: the set can be written. */
	return 0;
}

/*
 * Writes the scaling lists of a set: a present flag per list, and for a
 * present one either the default marker or every delta.
 */
static void
write_scaling(
	struct vpu_bits *writer,
	const struct vpu_h264_scaling *scaling,
	uint32_t lists)
{
	const uint8_t *list;
	uint32_t index;
	uint32_t size;
	uint32_t entry;
	int32_t last;
	int32_t delta;

	/* Each list in coding order: the six 4x4 lists, then the 8x8 ones. */
	for (index = 0; index < lists; index++) {
		if ((scaling->present_mask & (1U << index)) == 0) {
			vpu_bits_put(writer, 0, 1);
			continue;
		}

		/* The list follows. */
		vpu_bits_put(writer, 1, 1);

		/* A default list is the delta that makes the first scale zero. */
		if ((scaling->default_mask & (1U << index)) != 0) {
			vpu_bits_se(writer, -8);
			continue;
		}

		/* An explicit list: a 4x4 one of 16 entries, an 8x8 one of 64. */
		if (index < 6U) {
			list = scaling->list4[index];
			size = 16U;
		} else {
			list = scaling->list8[index - 6U];
			size = 64U;
		}

		/* Each entry as its delta from the last, wrapped to -128..127. */
		last = 8;
		for (entry = 0; entry < size; entry++) {
			delta = ((int32_t)list[entry] - last + 256) % 256;
			if (delta > 127)
				delta -= 256;
			vpu_bits_se(writer, delta);
			last = list[entry];
		}
	}
}

/*
 * Writes vui_parameters_present_flag and VUI whose only content is the
 * bitstream restriction asking for no reordering, with buffering as
 * max_dec_frame_buffering.
 */
static void
write_vui_no_reorder(
	struct vpu_bits *writer,
	uint32_t buffering)
{
	unsigned index;

	/* VUI present; no aspect, overscan, signal, chroma location, timing, HRD or picture structure. */
	vpu_bits_put(writer, 1, 1);
	for (index = 0; index < 8U; index++)
		vpu_bits_put(writer, 0, 1);

	/* The restriction: no size limits, the widest motion, no reordering. */
	vpu_bits_put(writer, 1, 1);
	vpu_bits_put(writer, 1, 1);
	vpu_bits_ue(writer, 0);
	vpu_bits_ue(writer, 0);
	vpu_bits_ue(writer, MAX_MV_LENGTH);
	vpu_bits_ue(writer, MAX_MV_LENGTH);
	vpu_bits_ue(writer, 0);
	vpu_bits_ue(writer, buffering);
}

/* Reports whether a profile carries the chroma format and scaling matrix fields. */
static int
high_profile(
	uint32_t profile)
{
	/* The profiles 7.3.2.1.1 lists. */
	switch (profile) {
	case 100:
	case 110:
	case 122:
	case 244:
	case 44:
	case 83:
	case 86:
	case 118:
	case 128:
	case 138:
	case 139:
	case 134:
	case 135:
		return 1;
	default:
		break;
	}

	/* Another profile has none of them. */
	return 0;
}

/* Converts a StdVideoH264LevelIdc enumerant (checked to be at most 6.2) to level_idc. */
static uint32_t
level_number(
	uint32_t level)
{
	/* The enumerant counts the levels 1.0 to 6.2 in order. */
	static const uint8_t numbers[LEVEL_LAST + 1U] = {
		10, 11, 12, 13, 20, 21, 22, 30, 31, 32, 40, 41, 42, 50, 51, 52, 60, 61, 62
	};

	/* Succeeded: the level's number. */
	return numbers[level];
}

/*
 * Reports max_dec_frame_buffering for the no-reorder VUI: the level's
 * MaxDpbFrames for the picture's size (Table A-1's MaxDpbMbs over the
 * picture's macroblocks, at most 16), and never less than the reference
 * frames or one.
 */
static uint32_t
dpb_frames(
	uint32_t level_idc,
	uint32_t mbs,
	uint32_t references)
{
	uint32_t largest;
	uint32_t frames;

	/* MaxDpbMbs of the level (level 1b, coded as 11 or 9, is taken as 1.1). */
	if (level_idc <= 10U) {
		largest = 396U;
	} else if (level_idc <= 11U) {
		largest = 900U;
	} else if (level_idc <= 20U) {
		largest = 2376U;
	} else if (level_idc <= 21U) {
		largest = 4752U;
	} else if (level_idc <= 30U) {
		largest = 8100U;
	} else if (level_idc <= 31U) {
		largest = 18000U;
	} else if (level_idc <= 32U) {
		largest = 20480U;
	} else if (level_idc <= 41U) {
		largest = 32768U;
	} else if (level_idc <= 42U) {
		largest = 34816U;
	} else if (level_idc <= 50U) {
		largest = 110400U;
	} else if (level_idc <= 52U) {
		largest = 184320U;
	} else {
		largest = 696320U;
	}

	/* The frames of the picture's size, bounded by 16 and by the references. */
	frames = 16U;
	if (mbs != 0 && largest / mbs < frames)
		frames = largest / mbs;
	if (frames < references)
		frames = references;
	if (frames == 0)
		frames = 1U;

	/* Succeeded: the buffering. */
	return frames;
}
