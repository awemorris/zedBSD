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

#include "drivers/vpu/vpu-codec.h"

/* The most RBSP bytes a set takes here (far more than any real one). */
#define RBSP_MAX			2048U

/* The value the no-reorder VUI writes for both log2_max_mv_length fields. */
#define MAX_MV_LENGTH			15U

/* The most StdVideoH264LevelIdc enumerant (6.2). */
#define LEVEL_LAST			18U

/*
 * Writes bits of an RBSP, most significant first, into a zeroed buffer.
 *
 * error is set, and stays set, when a write would run past the capacity.
 */
struct bit_writer {
	uint8_t *data;
	size_t capacity_bits;
	size_t position;
	int error;
};

static int check_sps(const struct vpu_h264_sps *sps);
static int check_pps(const struct vpu_h264_pps *pps);
static void write_bits(struct bit_writer *writer, uint32_t value, unsigned count);
static void write_flag(struct bit_writer *writer, uint32_t flags, uint32_t flag);
static void write_ue(struct bit_writer *writer, uint32_t value);
static void write_se(struct bit_writer *writer, int32_t value);
static void write_scaling(struct bit_writer *writer, const struct vpu_h264_scaling *scaling, uint32_t lists);
static void write_vui_no_reorder(struct bit_writer *writer, uint32_t buffering);
static void write_trailing(struct bit_writer *writer);
static int high_profile(uint32_t profile);
static uint32_t level_number(uint32_t level);
static uint32_t dpb_frames(uint32_t level_idc, uint32_t mbs, uint32_t references);
static int pack_nal(uint8_t header, const uint8_t *rbsp, size_t bytes, uint8_t *output, size_t capacity, size_t *written);

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
	struct bit_writer writer;
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
	kern_memset(rbsp, 0, sizeof(rbsp));
	writer.data = rbsp;
	writer.capacity_bits = sizeof(rbsp) * 8U;
	writer.position = 0;
	writer.error = 0;

	/* constraint_set0..5 from their flags, most significant first, and two reserved zeros. */
	constraints = 0;
	for (index = 0; index < 6U; index++) {
		if ((sps->flags & (VPU_H264_SPS_CONSTRAINT_SET0 << index)) != 0)
			constraints |= 0x80U >> index;
	}

	/* The profile, the constraint flags, the level and the id. */
	write_bits(&writer, sps->profile_idc, 8);
	write_bits(&writer, constraints, 8);
	write_bits(&writer, level_number(sps->level), 8);
	write_ue(&writer, sps->id);

	/* The high profiles' chroma format, bit depths and scaling matrix. */
	high = high_profile(sps->profile_idc);
	if (high) {
		write_ue(&writer, sps->chroma_format_idc);
		if (sps->chroma_format_idc == 3U)
			write_flag(&writer, sps->flags, VPU_H264_SPS_SEPARATE_PLANES);
		write_ue(&writer, sps->bit_depth_luma_minus8);
		write_ue(&writer, sps->bit_depth_chroma_minus8);
		write_flag(&writer, sps->flags, VPU_H264_SPS_QPPRIME_BYPASS);
		write_flag(&writer, sps->flags, VPU_H264_SPS_SCALING);
		if ((sps->flags & VPU_H264_SPS_SCALING) != 0) {
			lists = 8U;
			if (sps->chroma_format_idc == 3U)
				lists = 12U;
			write_scaling(&writer, &sps->scaling, lists);
		}
	}

	/* The frame number and the picture order count syntax. */
	write_ue(&writer, sps->log2_max_frame_num_minus4);
	write_ue(&writer, sps->pic_order_cnt_type);
	if (sps->pic_order_cnt_type == 0) {
		write_ue(&writer, sps->log2_max_pic_order_cnt_lsb_minus4);
	} else if (sps->pic_order_cnt_type == 1U) {
		write_flag(&writer, sps->flags, VPU_H264_SPS_DELTA_POC_ZERO);
		write_se(&writer, sps->offset_for_non_ref_pic);
		write_se(&writer, sps->offset_for_top_to_bottom_field);
		write_ue(&writer, sps->offset_count);

		/* Each offset of the cycle. */
		for (index = 0; index < sps->offset_count; index++)
			write_se(&writer, sps->offsets[index]);
	}

	/* The references, the size, the field flags. */
	write_ue(&writer, sps->max_num_ref_frames);
	write_flag(&writer, sps->flags, VPU_H264_SPS_GAPS_ALLOWED);
	write_ue(&writer, sps->pic_width_in_mbs_minus1);
	write_ue(&writer, sps->pic_height_in_map_units_minus1);
	write_flag(&writer, sps->flags, VPU_H264_SPS_FRAME_MBS_ONLY);
	if ((sps->flags & VPU_H264_SPS_FRAME_MBS_ONLY) == 0)
		write_flag(&writer, sps->flags, VPU_H264_SPS_MB_ADAPTIVE);
	write_flag(&writer, sps->flags, VPU_H264_SPS_DIRECT_8X8);

	/* The cropping, each offset when it is there. */
	write_flag(&writer, sps->flags, VPU_H264_SPS_CROPPING);
	if ((sps->flags & VPU_H264_SPS_CROPPING) != 0) {
		for (index = 0; index < 4U; index++)
			write_ue(&writer, sps->crop[index]);
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
		write_bits(&writer, 0, 1);
	}

	/* Ends the RBSP. */
	write_trailing(&writer);
	if (writer.error != 0)
		return ENOSPC;

	/* The start code, the header (nal_ref_idc 3, type 7) and the escaped body. */
	error = pack_nal(0x67U, rbsp, writer.position / 8U, output, capacity, written);
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
	struct bit_writer writer;
	uint32_t lists;
	int tail;
	int error;

	/* Refuses fields H.264 cannot code. */
	error = check_pps(pps);
	if (error != 0)
		return error;

	/* The RBSP is written into a zeroed scratch first. */
	kern_memset(rbsp, 0, sizeof(rbsp));
	writer.data = rbsp;
	writer.capacity_bits = sizeof(rbsp) * 8U;
	writer.position = 0;
	writer.error = 0;

	/* The ids, the entropy coder, the field order flag and one slice group. */
	write_ue(&writer, pps->pps_id);
	write_ue(&writer, pps->sps_id);
	write_flag(&writer, pps->flags, VPU_H264_PPS_CABAC);
	write_flag(&writer, pps->flags, VPU_H264_PPS_BOTTOM_FIELD_POC);
	write_ue(&writer, 0);

	/* The default references, the weighted prediction and the quantizers. */
	write_ue(&writer, pps->num_ref_idx_l0_default_active_minus1);
	write_ue(&writer, pps->num_ref_idx_l1_default_active_minus1);
	write_flag(&writer, pps->flags, VPU_H264_PPS_WEIGHTED_PRED);
	write_bits(&writer, pps->weighted_bipred_idc, 2);
	write_se(&writer, pps->pic_init_qp_minus26);
	write_se(&writer, pps->pic_init_qs_minus26);
	write_se(&writer, pps->chroma_qp_index_offset);
	write_flag(&writer, pps->flags, VPU_H264_PPS_DEBLOCKING_CONTROL);
	write_flag(&writer, pps->flags, VPU_H264_PPS_CONSTRAINED_INTRA);
	write_flag(&writer, pps->flags, VPU_H264_PPS_REDUNDANT_PIC_CNT);

	/* The optional tail is there when the transform, the lists or the second offset say something. */
	tail = 0;
	if ((pps->flags & (VPU_H264_PPS_TRANSFORM_8X8 | VPU_H264_PPS_SCALING)) != 0)
		tail = 1;
	if (pps->second_chroma_qp_index_offset != pps->chroma_qp_index_offset)
		tail = 1;

	/* The tail: the transform, the lists (6, two more with the 8x8 transform, four more at 4:4:4) and the offset. */
	if (tail) {
		write_flag(&writer, pps->flags, VPU_H264_PPS_TRANSFORM_8X8);
		write_flag(&writer, pps->flags, VPU_H264_PPS_SCALING);
		if ((pps->flags & VPU_H264_PPS_SCALING) != 0) {
			lists = 6U;
			if ((pps->flags & VPU_H264_PPS_TRANSFORM_8X8) != 0) {
				lists += 2U;
				if (sps != NULL && sps->chroma_format_idc == 3U)
					lists += 4U;
			}

			write_scaling(&writer, &pps->scaling, lists);
		}

		write_se(&writer, pps->second_chroma_qp_index_offset);
	}

	/* Ends the RBSP. */
	write_trailing(&writer);
	if (writer.error != 0)
		return ENOSPC;

	/* The start code, the header (nal_ref_idc 3, type 8) and the escaped body. */
	error = pack_nal(0x68U, rbsp, writer.position / 8U, output, capacity, written);
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

/* Writes up to 32 bits, most significant first. */
static void
write_bits(
	struct bit_writer *writer,
	uint32_t value,
	unsigned count)
{
	unsigned index;
	uint32_t bit;

	/* A write past the capacity sets the error and writes nothing. */
	if (writer->error != 0 || writer->position + count > writer->capacity_bits) {
		writer->error = 1;
		return;
	}

	/* One bit at a time into the zeroed buffer. */
	for (index = 0; index < count; index++) {
		bit = (value >> (count - 1U - index)) & 1U;
		if (bit != 0)
			writer->data[writer->position / 8U] |= (uint8_t)(0x80U >> (writer->position % 8U));
		writer->position++;
	}
}

/* Writes one flag bit: whether flags has flag set. */
static void
write_flag(
	struct bit_writer *writer,
	uint32_t flags,
	uint32_t flag)
{
	uint32_t bit;

	/* The flag as one bit. */
	bit = 0;
	if ((flags & flag) != 0)
		bit = 1;
	write_bits(writer, bit, 1);
}

/* Writes an unsigned Exp-Golomb code. */
static void
write_ue(
	struct bit_writer *writer,
	uint32_t value)
{
	uint32_t code;
	unsigned length;

	/* value + 1 in binary, after as many zeros as it has bits less one. */
	code = value + 1U;
	length = 0;
	while ((code >> length) > 1U)
		length++;
	write_bits(writer, 0, length);
	write_bits(writer, code, length + 1U);
}

/* Writes a signed Exp-Golomb code. */
static void
write_se(
	struct bit_writer *writer,
	int32_t value)
{
	/* Positive values take the odd codes, the others the even ones. */
	if (value > 0) {
		write_ue(writer, (uint32_t)value * 2U - 1U);
	} else {
		write_ue(writer, (uint32_t)(-(int64_t)value) * 2U);
	}
}

/*
 * Writes the scaling lists of a set: a present flag per list, and for a
 * present one either the default marker or every delta.
 */
static void
write_scaling(
	struct bit_writer *writer,
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
			write_bits(writer, 0, 1);
			continue;
		}

		/* The list follows. */
		write_bits(writer, 1, 1);

		/* A default list is the delta that makes the first scale zero. */
		if ((scaling->default_mask & (1U << index)) != 0) {
			write_se(writer, -8);
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
			write_se(writer, delta);
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
	struct bit_writer *writer,
	uint32_t buffering)
{
	unsigned index;

	/* VUI present; no aspect, overscan, signal, chroma location, timing, HRD or picture structure. */
	write_bits(writer, 1, 1);
	for (index = 0; index < 8U; index++)
		write_bits(writer, 0, 1);

	/* The restriction: no size limits, the widest motion, no reordering. */
	write_bits(writer, 1, 1);
	write_bits(writer, 1, 1);
	write_ue(writer, 0);
	write_ue(writer, 0);
	write_ue(writer, MAX_MV_LENGTH);
	write_ue(writer, MAX_MV_LENGTH);
	write_ue(writer, 0);
	write_ue(writer, buffering);
}

/* Writes rbsp_trailing_bits: a one, then zeros to the byte boundary. */
static void
write_trailing(
	struct bit_writer *writer)
{
	/* The stop bit and the alignment. */
	write_bits(writer, 1, 1);
	while ((writer->position % 8U) != 0 && writer->error == 0)
		write_bits(writer, 0, 1);
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

/*
 * Writes a start code, a NAL header byte and an RBSP with emulation
 * prevention: a 0x03 goes in after any two zeros that precede a byte of
 * three or less.
 */
static int
pack_nal(
	uint8_t header,
	const uint8_t *rbsp,
	size_t bytes,
	uint8_t *output,
	size_t capacity,
	size_t *written)
{
	size_t index;
	size_t zeros;
	size_t out;

	/* The four-byte start code and the header. */
	if (capacity < 5U)
		return ENOSPC;
	output[0] = 0;
	output[1] = 0;
	output[2] = 0;
	output[3] = 1;
	output[4] = header;
	out = 5U;

	/* The body, escaped. */
	zeros = 0;
	for (index = 0; index < bytes; index++) {
		if (zeros >= 2U && rbsp[index] <= 3U) {
			if (out >= capacity)
				return ENOSPC;
			output[out] = 3;
			out++;
			zeros = 0;
		}

		/* Refuses an output that does not fit. */
		if (out >= capacity)
			return ENOSPC;
		output[out] = rbsp[index];
		out++;
		if (rbsp[index] == 0) {
			zeros++;
		} else {
			zeros = 0;
		}
	}

	/* Succeeded: output holds the unit. */
	*written = out;
	return 0;
}
