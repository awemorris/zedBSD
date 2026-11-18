/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The H.264 part of the video engine's decode messages (included by
 * vcn-format.c, whose writers it uses): the H.264 body of a decode
 * message, the scaling lists a picture decodes with, and the sizes of an
 * H.264 session's hidden DPB and decoder context.
 *
 * The firmware keeps the reference pictures in a DPB of its own and knows
 * each by a tag: the picture's DPB slot, or AMDRDNA_VCN_H264_LOOSE_TAG for
 * a picture that has none.  The message names the picture's tag and each
 * reference's tag with its frame number and order counts.
 */

/*
 * The H.264 body's fields, in bytes from its start: the profile and the
 * level; the sequence and picture flags; the chroma format, the bit
 * depths, the frame number's and the order count's sizes and the order
 * count's type, the reference frames; the initial quantizers and the
 * chroma offsets; the slice groups and the default active references;
 * the scaling lists (six 4x4, two 8x8, raster order); the picture's frame
 * number; the references' frame numbers; the picture's two order counts;
 * the references' two order counts each; the picture's tag, the count of
 * references and their tags; the missing references' bits and the
 * references' field bits.
 *
 * Values referenced: Mesa 25.0.7 src/amd/common/ac_vcn_dec.h (the H.264
 * body of a decode message).
 * derived: the offsets of the body's fields in their declared order with
 * their natural alignment; the body is 1124 bytes.
 */
#define H264_BODY_BYTES			1124U
#define H264_PROFILE_AT			0U
#define H264_LEVEL_AT			4U
#define H264_SPS_FLAGS_AT		8U
#define H264_PPS_FLAGS_AT		12U
#define H264_CHROMA_FORMAT_AT		16U
#define H264_LUMA_DEPTH_AT		17U
#define H264_CHROMA_DEPTH_AT		18U
#define H264_FRAME_NUM_BITS_AT		19U
#define H264_POC_TYPE_AT		20U
#define H264_POC_BITS_AT		21U
#define H264_REFERENCE_FRAMES_AT	22U
#define H264_QP_AT			24U
#define H264_QS_AT			25U
#define H264_CHROMA_QP_AT		26U
#define H264_SECOND_CHROMA_QP_AT	27U
#define H264_SLICE_GROUPS_AT		28U
#define H264_L0_ACTIVE_AT		30U
#define H264_L1_ACTIVE_AT		31U
#define H264_SCALING_4X4_AT		36U
#define H264_SCALING_8X8_AT		132U
#define H264_FRAME_NUM_AT		260U
#define H264_FRAME_NUMS_AT		264U
#define H264_ORDER_AT			328U
#define H264_ORDERS_AT			336U
#define H264_TAG_AT			464U
#define H264_REFERENCE_COUNT_AT		468U
#define H264_TAGS_AT			472U
#define H264_MISSING_AT			1116U
#define H264_FIELDS_AT			1120U

/*
 * The firmware's profiles (Baseline, Main, High) and the standard's
 * profile_idc of each, and of Extended, whose streams that keep to the
 * Baseline's or the Main profile's constraints (constraint_set0_flag or
 * constraint_set1_flag) are decoded as those.
 *
 * Values referenced: Mesa 25.0.7 src/amd/common/ac_vcn_dec.h
 * (RDECODE_H264_PROFILE_BASELINE, _MAIN, _HIGH); Recommendation ITU-T
 * H.264 (A.2, profile_idc 66, 77, 88 and 100, and the meaning of
 * constraint_set0_flag and constraint_set1_flag).
 */
#define H264_PROFILE_BASELINE		0U
#define H264_PROFILE_MAIN		1U
#define H264_PROFILE_HIGH		2U
#define H264_IDC_BASELINE		66U
#define H264_IDC_MAIN			77U
#define H264_IDC_EXTENDED		88U
#define H264_IDC_HIGH			100U

/*
 * The sequence flags of the body (direct 8x8 inference, adaptive
 * frame/field, frame macroblocks only, order counts always zero, and the
 * extension support the drivers raise for a DPB the firmware keeps) and
 * the picture flags (8x8 transform, redundant picture counts,
 * constrained intra, deblocking control, the weighted bi-prediction's
 * two bits, weighted prediction, the bottom field's order count,
 * CABAC).
 *
 * Values referenced: Mesa 25.0.7 src/amd/vulkan/radv_video.c
 * (get_h264_msg: the bits of sps_info_flags and pps_info_flags) and
 * src/amd/common/ac_vcn_dec.h
 * (RDECODE_SPS_INFO_H264_EXTENSION_SUPPORT_FLAG_SHIFT).
 */
#define H264_SPS_DIRECT_8X8		(1U << 0)
#define H264_SPS_MBAFF			(1U << 1)
#define H264_SPS_FRAME_MBS_ONLY		(1U << 2)
#define H264_SPS_DELTA_POC_ZERO		(1U << 3)
#define H264_SPS_EXTENSION		(1U << 7)
#define H264_PPS_TRANSFORM_8X8		(1U << 0)
#define H264_PPS_REDUNDANT		(1U << 1)
#define H264_PPS_CONSTRAINED		(1U << 2)
#define H264_PPS_DEBLOCKING		(1U << 3)
#define H264_PPS_BIPRED_SHIFT		4U
#define H264_PPS_BIPRED_MASK		0x3U
#define H264_PPS_WEIGHTED		(1U << 6)
#define H264_PPS_BOTTOM_POC		(1U << 7)
#define H264_PPS_ENTROPY		(1U << 8)

/*
 * A reference list entry: no reference, and the bit of a long-term one;
 * the two field bits of a reference in the field word.
 *
 * Values referenced: Mesa 25.0.7 src/amd/vulkan/radv_video.c
 * (get_h264_msg: ref_frame_list filled with 0xff, the 0x80 of a long-term
 * reference, the two bits of used_for_reference_flags for each).
 */
#define H264_NO_REFERENCE		0xffU
#define H264_LONG_TERM			0x80U
#define H264_FIELD_TOP			0x1U
#define H264_FIELD_BOTTOM		0x2U
#define H264_FIELD_BOTH			0x3U

/*
 * The scaling table the inverse transform reads: the six 4x4 lists from
 * byte 0 and the two 8x8 lists from byte 96, in raster order.
 *
 * Values referenced: Mesa 25.0.7 src/amd/vulkan/radv_video.c
 * (get_h264_msg: the copies into it_ptr).
 */
#define H264_TABLE_4X4_AT		0U
#define H264_TABLE_8X8_AT		96U

/* The scaling lists' places among the twelve: the 4x4 lists 0 to 5, the luma 8x8 lists 6 (intra) and 7 (inter). */
#define H264_LISTS_4X4			6U
#define H264_LIST_8X8_INTRA		6U
#define H264_LIST_8X8_INTER		7U

/* The flat matrix's coefficient. */
#define H264_FLAT			16U

/*
 * The levels the firmware is told (ten times the level number), by the
 * level's StdVideoH264LevelIdc (1.0 to 6.2 in order).
 *
 * Values referenced: Mesa 25.0.7 src/amd/vulkan/radv_video.c
 * (h264_levels); Recommendation ITU-T H.264 (Table A-1).
 */
#define H264_LEVELS			19U

/*
 * The hidden DPB of an H.264 session: each picture its luma and chroma
 * samples with the sides on 32 samples, on 1 KiB; as many pictures as
 * the larger of the session's slots and one more, and the pictures the
 * largest level's DPB holds at the session's size and one more, at most
 * 17.  The level is 5.1, the highest the executor reports, whose DPB is
 * 184320 macroblocks.
 *
 * Values referenced: Mesa 25.0.7
 * src/gallium/drivers/radeonsi/radeon_vcn_dec.c (calc_dpb_size: the
 * picture's size and the count for H.264, NUM_H264_REFS 17);
 * Recommendation ITU-T H.264 (Table A-1, MaxDpbMbs of level 5.1).
 */
#define H264_MB				16U
#define H264_DPB_PICTURE_ALIGN		1024U
#define H264_DPB_PICTURES_MAX		17U
#define H264_DPB_MBS			184320U

/*
 * The decoder context of an H.264 session: 192 bytes a macroblock of the
 * largest picture (its height in macroblocks made even), on 256 bytes,
 * for each slot and one more, in whole pages.
 *
 * Values referenced: Mesa 25.0.7 src/amd/vulkan/radv_video.c
 * (calc_ctx_size_h264_perf, and the memory requirement on 4096 bytes in
 * radv_GetVideoSessionMemoryRequirementsKHR).
 */
#define H264_CONTEXT_MB_BYTES		192U
#define H264_CONTEXT_ALIGN		256U
#define H264_CONTEXT_PAGE		4096U

/*
 * The slice of a frame a gap in frame_num implies: an Annex B start code,
 * the NAL unit header of a reference picture's slice (nal_ref_idc 1,
 * nal_unit_type 1), the slice type P, one active reference, the
 * deblocking filter turned off (disable_deblocking_filter_idc 1), and the
 * bitstream's bytes made whole for the engine, which reads it in units of
 * 128 bytes.
 *
 * Values referenced: Recommendation ITU-T H.264 (7.3.1 and Table 7-1,
 * the NAL unit header; 7.3.3, Table 7-6 and 7.4.3, the slice header;
 * 7.3.4, the slice data of CAVLC; B.1, the start code).  Mesa 25.0.7
 * src/amd/vulkan/radv_video.c (the bitstream's size on 128 bytes).
 */
#define H264_GAP_NAL_HEADER		0x21U
#define H264_GAP_SLICE_P		0U
#define H264_GAP_DEBLOCKING_OFF		1U
#define H264_GAP_BITSTREAM_ALIGN	128U
#define H264_GAP_HEADER_BYTES		4U

/* A slice writer: the bytes written, how many, the room, the bits not yet a byte, how many, and whether the room ran out. */
struct h264_bits {
	uint8_t *bytes;
	size_t at;
	size_t room;
	uint32_t pending;
	uint32_t pending_bits;
	uint32_t zeros;
	bool full;
};

static void h264_body_sets(uint8_t *body, const struct amdrdna_vcn_h264_sps *sps, const struct amdrdna_vcn_h264_pps *pps);
static void h264_body_references(uint8_t *body, const struct amdrdna_vcn_h264_decode *h264);
static void h264_list4(const struct amdrdna_vcn_h264_scaling *scaling, uint32_t list, const uint8_t *fallback, uint8_t *scan);
static void h264_list8(const struct amdrdna_vcn_h264_scaling *scaling, uint32_t list, const uint8_t *fallback, uint8_t *scan);
static void h264_raster4(const uint8_t *scan, uint8_t *raster);
static void h264_raster8(const uint8_t *scan, uint8_t *raster);
static uint32_t h264_align(uint32_t value, uint32_t boundary);
static void h264_bits_put(struct h264_bits *bits, uint32_t value, uint32_t count);
static void h264_bits_ue(struct h264_bits *bits, uint32_t value);
static void h264_bits_byte(struct h264_bits *bits, uint8_t byte);

/*
 * The levels the firmware is told, by StdVideoH264LevelIdc.
 *
 * Values referenced: Mesa 25.0.7 src/amd/vulkan/radv_video.c
 * (h264_levels).
 */
static const uint8_t h264_levels[H264_LEVELS] = {
	10, 11, 12, 13, 20, 21, 22, 30, 31, 32, 40, 41, 42, 50, 51, 52, 60, 61, 62
};

/*
 * The 4x4 and 8x8 frame zig-zag scans: the raster position (row times
 * the side plus column) of the coefficient at each scan index.
 *
 * Values referenced: Recommendation ITU-T H.264 (Table 8-12 and Table
 * 8-13, the frame zig-zag scans).
 */
static const uint8_t h264_zigzag4[16] = {
	0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15
};

/* The 8x8 scan, read the same way. */
static const uint8_t h264_zigzag8[64] = {
	0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
	12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
	35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
	58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63
};

/*
 * The default scaling lists, in scan order.
 *
 * Values referenced: Recommendation ITU-T H.264 (Table 7-3,
 * Default_4x4_Intra and Default_4x4_Inter; Table 7-4, Default_8x8_Intra
 * and Default_8x8_Inter).
 */
static const uint8_t h264_default4_intra[16] = {
	6, 13, 13, 20, 20, 20, 28, 28, 28, 28, 32, 32, 32, 37, 37, 42
};

/* The default 4x4 list for the inter lists. */
static const uint8_t h264_default4_inter[16] = {
	10, 14, 14, 20, 20, 20, 24, 24, 24, 24, 27, 27, 27, 30, 30, 34
};

/* The default 8x8 list for the intra list. */
static const uint8_t h264_default8_intra[64] = {
	6, 10, 10, 13, 11, 13, 16, 16, 16, 16, 18, 18, 18, 18, 18, 23,
	23, 23, 23, 23, 23, 25, 25, 25, 25, 25, 25, 25, 27, 27, 27, 27,
	27, 27, 27, 27, 29, 29, 29, 29, 29, 29, 29, 31, 31, 31, 31, 31,
	31, 33, 33, 33, 33, 33, 36, 36, 36, 36, 38, 38, 38, 40, 40, 42
};

/* The default 8x8 list for the inter list. */
static const uint8_t h264_default8_inter[64] = {
	9, 13, 13, 15, 13, 15, 17, 17, 17, 17, 19, 19, 19, 19, 19, 21,
	21, 21, 21, 21, 21, 22, 22, 22, 22, 22, 22, 22, 24, 24, 24, 24,
	24, 24, 24, 24, 25, 25, 25, 25, 25, 25, 25, 27, 27, 27, 27, 27,
	27, 28, 28, 28, 28, 28, 30, 30, 30, 30, 32, 32, 32, 33, 33, 35
};

/*
 * Writes an H.264 decode message: the header and the decode body from
 * frame, then the H.264 body from h264; and the scaling table the
 * inverse transform reads (scaling, scaling_bytes long; the rest of it
 * zeroed).
 *
 * Returns 0, or EINVAL for a buffer too small, a profile the firmware
 * does not take, a level outside the standard's, or more references or
 * a larger tag than the message holds.
 */
int
amdrdna_vcn_message_decode_h264(
	uint8_t *message,
	size_t bytes,
	const struct amdrdna_vcn_frame *frame,
	const struct amdrdna_vcn_h264_decode *h264,
	uint8_t *scaling,
	size_t scaling_bytes)
{
	uint8_t list4[6][16];
	uint8_t list8[2][64];
	uint8_t *body;
	uint32_t profile;
	uint32_t list;
	uint32_t index;
	size_t at;
	int error;

	/* The profile the firmware decodes the stream as. */
	error = amdrdna_vcn_h264_profile(h264->sps, &profile);
	if (error != 0)
		return error;

	/* A level of the standard, the references and the tags the body holds, a table that holds the lists. */
	if (h264->sps->level_idc >= H264_LEVELS)
		return EINVAL;
	if (h264->reference_count > AMDRDNA_VCN_H264_REFERENCES || h264->tag > AMDRDNA_VCN_H264_LOOSE_TAG)
		return EINVAL;
	for (index = 0U; index < h264->reference_count; index++) {
		if (h264->references[index].tag >= AMDRDNA_VCN_H264_LOOSE_TAG)
			return EINVAL;
	}
	if (scaling_bytes < H264_TABLE_8X8_AT + sizeof(list8))
		return EINVAL;

	/* The header and the decode body. */
	error = format_message_decode(message, bytes, frame, AMDRDNA_VCN_STREAM_H264, FORMAT_INDEX_H264, H264_BODY_BYTES);
	if (error != 0)
		return error;

	/* The profile and the level. */
	body = message + FORMAT_CODEC_AT;
	format_put32(body, H264_PROFILE_AT, profile);
	format_put32(body, H264_LEVEL_AT, h264_levels[h264->sps->level_idc]);

	/* What the sets say. */
	h264_body_sets(body, h264->sps, h264->pps);

	/* The scaling lists, in the body and in the table, raster order. */
	amdrdna_vcn_h264_scaling(h264->sps, h264->pps, list4, list8);
	for (at = 0U; at < scaling_bytes; at++)
		scaling[at] = 0U;
	for (list = 0U; list < 6U; list++) {
		for (index = 0U; index < 16U; index++) {
			body[H264_SCALING_4X4_AT + list * 16U + index] = list4[list][index];
			scaling[H264_TABLE_4X4_AT + list * 16U + index] = list4[list][index];
		}
	}
	for (list = 0U; list < 2U; list++) {
		for (index = 0U; index < 64U; index++) {
			body[H264_SCALING_8X8_AT + list * 64U + index] = list8[list][index];
			scaling[H264_TABLE_8X8_AT + list * 64U + index] = list8[list][index];
		}
	}

	/* The picture: its frame number and its two order counts. */
	format_put32(body, H264_FRAME_NUM_AT, h264->picture.frame_num);
	format_put32(body, H264_ORDER_AT, (uint32_t)h264->picture.poc[0]);
	format_put32(body, H264_ORDER_AT + 4U, (uint32_t)h264->picture.poc[1]);

	/* The picture's tag and its references. */
	h264_body_references(body, h264);

	/* Succeeded: the message and the table are written. */
	return 0;
}

/*
 * Reports the firmware's profile a sequence decodes as: Baseline, Main or
 * High by its profile_idc, and an Extended sequence that keeps to the
 * Main or the Baseline profile's constraints as that one.  Returns 0, or
 * EINVAL for a profile the firmware does not take.
 */
int
amdrdna_vcn_h264_profile(
	const struct amdrdna_vcn_h264_sps *sps,
	uint32_t *profile)
{
	/* By profile_idc, and the constraint flags of an Extended sequence. */
	if (sps->profile_idc == H264_IDC_BASELINE) {
		*profile = H264_PROFILE_BASELINE;
	} else if (sps->profile_idc == H264_IDC_MAIN) {
		*profile = H264_PROFILE_MAIN;
	} else if (sps->profile_idc == H264_IDC_HIGH) {
		*profile = H264_PROFILE_HIGH;
	} else if (sps->profile_idc == H264_IDC_EXTENDED && (sps->flags & AMDRDNA_VCN_SPS_CONSTRAINT_SET1) != 0U) {
		*profile = H264_PROFILE_MAIN;
	} else if (sps->profile_idc == H264_IDC_EXTENDED && (sps->flags & AMDRDNA_VCN_SPS_CONSTRAINT_SET0) != 0U) {
		*profile = H264_PROFILE_BASELINE;
	} else {
		return EINVAL;
	}

	/* Succeeded: the profile. */
	return 0;
}

/* Reports the level the firmware is told (ten times the level number) for a StdVideoH264LevelIdc; 0 for none. */
uint32_t
amdrdna_vcn_h264_level(
	uint32_t level_idc)
{
	/* A level outside the standard's has none. */
	if (level_idc >= H264_LEVELS)
		return 0U;

	/* Succeeded: the level. */
	return h264_levels[level_idc];
}

/*
 * Reports the bytes of the hidden DPB of an H.264 session whose largest
 * picture is width by height samples and which has slots DPB slots.
 */
uint64_t
amdrdna_vcn_h264_dpb_bytes(
	uint32_t width,
	uint32_t height,
	uint32_t slots)
{
	uint64_t picture;
	uint32_t width_mbs;
	uint32_t height_mbs;
	uint32_t frame_mbs;
	uint32_t pictures;
	uint32_t lean;

	/* The sides in whole macroblocks, the height's count made even. */
	width = h264_align(width, H264_MB);
	height = h264_align(height, H264_MB);
	width_mbs = width / H264_MB;
	height_mbs = h264_align(height / H264_MB, 2U);
	frame_mbs = width_mbs * height_mbs;
	if (frame_mbs == 0U)
		return 0U;

	/* One picture: its samples on 32, half again for the chroma, on 1 KiB. */
	picture = (uint64_t)h264_align(width, FORMAT_DPB_ALIGN) * h264_align(height, FORMAT_DPB_ALIGN);
	picture += picture / 2U;
	picture = (picture + H264_DPB_PICTURE_ALIGN - 1U) & ~((uint64_t)H264_DPB_PICTURE_ALIGN - 1U);

	/* The pictures: the level's DPB and one more (at most 17), or the slots and one more. */
	lean = H264_DPB_MBS / frame_mbs + 1U;
	if (lean > H264_DPB_PICTURES_MAX)
		lean = H264_DPB_PICTURES_MAX;
	pictures = slots + 1U;
	if (lean > pictures)
		pictures = lean;

	/* Succeeded: the DPB's bytes. */
	return picture * pictures;
}

/*
 * Reports the bytes of the decoder context of an H.264 session whose
 * largest picture is width by height samples and which has slots DPB
 * slots.
 */
uint64_t
amdrdna_vcn_h264_context_bytes(
	uint32_t width,
	uint32_t height,
	uint32_t slots)
{
	uint64_t bytes;
	uint32_t width_mbs;
	uint32_t height_mbs;

	/* The sides in whole macroblocks, the height's count made even. */
	width_mbs = h264_align(width, H264_MB) / H264_MB;
	height_mbs = h264_align(h264_align(height, H264_MB) / H264_MB, 2U);

	/* Each slot and one more: 192 bytes a macroblock on 256 bytes; in whole pages. */
	bytes = (uint64_t)(slots + 1U) * h264_align(width_mbs * height_mbs * H264_CONTEXT_MB_BYTES, H264_CONTEXT_ALIGN);
	bytes = (bytes + H264_CONTEXT_PAGE - 1U) & ~((uint64_t)H264_CONTEXT_PAGE - 1U);

	/* Succeeded: the context's bytes. */
	return bytes;
}

/*
 * Resolves the scaling matrices a picture decodes with, in raster order:
 * the picture set's lists when it has them, else the sequence set's,
 * else flat.  A list a set does not carry falls back by the standard's
 * rules: in a sequence set by rule A (the default list, or the list
 * before); in a picture set by rule A when the sequence set has no
 * matrix, else by rule B (the sequence-level list, or the list before).
 *
 * Procedure referenced: Recommendation ITU-T H.264 (7.4.2.1.1 and
 * 7.4.2.2, Table 7-2, the fall-back rules A and B).
 */
void
amdrdna_vcn_h264_scaling(
	const struct amdrdna_vcn_h264_sps *sps,
	const struct amdrdna_vcn_h264_pps *pps,
	uint8_t scaling_4x4[6][16],
	uint8_t scaling_8x8[2][64])
{
	uint8_t sequence4[6][16];
	uint8_t sequence8[2][64];
	uint8_t picture4[6][16];
	uint8_t picture8[2][64];
	const uint8_t *fallback4_intra;
	const uint8_t *fallback4_inter;
	const uint8_t *fallback8_intra;
	const uint8_t *fallback8_inter;
	uint32_t list;
	uint32_t index;
	bool sequence_lists;
	bool picture_lists;

	/* Flat until a set says otherwise. */
	for (list = 0U; list < 6U; list++) {
		for (index = 0U; index < 16U; index++)
			sequence4[list][index] = H264_FLAT;
	}

	/* The 8x8 lists are flat likewise. */
	for (list = 0U; list < 2U; list++) {
		for (index = 0U; index < 64U; index++)
			sequence8[list][index] = H264_FLAT;
	}

	/* The sequence-level lists: the sequence set's by rule A, when it has a matrix. */
	sequence_lists = false;
	if ((sps->flags & AMDRDNA_VCN_SPS_SCALING) != 0U && sps->has_scaling)
		sequence_lists = true;
	if (sequence_lists) {
		h264_list4(&sps->scaling, 0U, h264_default4_intra, sequence4[0]);
		h264_list4(&sps->scaling, 1U, sequence4[0], sequence4[1]);
		h264_list4(&sps->scaling, 2U, sequence4[1], sequence4[2]);
		h264_list4(&sps->scaling, 3U, h264_default4_inter, sequence4[3]);
		h264_list4(&sps->scaling, 4U, sequence4[3], sequence4[4]);
		h264_list4(&sps->scaling, 5U, sequence4[4], sequence4[5]);
		h264_list8(&sps->scaling, H264_LIST_8X8_INTRA, h264_default8_intra, sequence8[0]);
		h264_list8(&sps->scaling, H264_LIST_8X8_INTER, h264_default8_inter, sequence8[1]);
	}

	/* The first lists of a picture set fall back to the sequence level's (rule B), or to the defaults without a sequence matrix (rule A). */
	fallback4_intra = sequence4[0];
	fallback4_inter = sequence4[3];
	fallback8_intra = sequence8[0];
	fallback8_inter = sequence8[1];
	if (!sequence_lists) {
		fallback4_intra = h264_default4_intra;
		fallback4_inter = h264_default4_inter;
		fallback8_intra = h264_default8_intra;
		fallback8_inter = h264_default8_inter;
	}

	/* The picture's lists: the picture set's when it has a matrix, else the sequence level's. */
	picture_lists = false;
	if ((pps->flags & AMDRDNA_VCN_PPS_SCALING) != 0U && pps->has_scaling)
		picture_lists = true;
	if (picture_lists) {
		h264_list4(&pps->scaling, 0U, fallback4_intra, picture4[0]);
		h264_list4(&pps->scaling, 1U, picture4[0], picture4[1]);
		h264_list4(&pps->scaling, 2U, picture4[1], picture4[2]);
		h264_list4(&pps->scaling, 3U, fallback4_inter, picture4[3]);
		h264_list4(&pps->scaling, 4U, picture4[3], picture4[4]);
		h264_list4(&pps->scaling, 5U, picture4[4], picture4[5]);
		h264_list8(&pps->scaling, H264_LIST_8X8_INTRA, fallback8_intra, picture8[0]);
		h264_list8(&pps->scaling, H264_LIST_8X8_INTER, fallback8_inter, picture8[1]);
	} else {
		/* No picture set matrix: the picture takes the sequence level's lists. */
		for (list = 0U; list < 6U; list++) {
			for (index = 0U; index < 16U; index++)
				picture4[list][index] = sequence4[list][index];
		}

		/* The 8x8 lists likewise. */
		for (list = 0U; list < 2U; list++) {
			for (index = 0U; index < 64U; index++)
				picture8[list][index] = sequence8[list][index];
		}
	}

	/* The firmware takes them in raster order. */
	for (list = 0U; list < 6U; list++)
		h264_raster4(picture4[list], scaling_4x4[list]);
	for (list = 0U; list < 2U; list++)
		h264_raster8(picture8[list], scaling_8x8[list]);
}

/*
 * Writes the slice of a frame a gap in frame_num implies (8.2.5.2) into
 * bytes (room long): a reference P picture of the sequence's size whose
 * every macroblock is skipped, numbered frame_num, with the low bits of
 * the order count poc for a type 0 sequence, as the picture set pps' (made
 * by amdrdna_vcn_h264_gap_pps) reads it; the bytes after it to the
 * engine's unit are zeroed.  The engine keeps the reference marking
 * itself from the slices it decodes, and decoding this slice puts the
 * frame into its DPB with the sliding window, as the standard's decoder
 * puts in a frame the gap implies.
 *
 * Returns the bytes to hand the engine, or 0 when room cannot hold them.
 */
size_t
amdrdna_vcn_h264_gap_slice(
	uint8_t *bytes,
	size_t room,
	const struct amdrdna_vcn_h264_sps *sps,
	const struct amdrdna_vcn_h264_pps *pps,
	uint32_t frame_num,
	int32_t poc)
{
	struct h264_bits bits;
	uint32_t lsb_bits;
	uint32_t height_mbs;
	uint32_t mbs;
	size_t length;
	size_t at;

	/* The start code and the NAL unit header, written as they are. */
	if (room < H264_GAP_HEADER_BYTES)
		return 0U;
	bytes[0] = 0U;
	bytes[1] = 0U;
	bytes[2] = 1U;
	bytes[3] = (uint8_t)H264_GAP_NAL_HEADER;

	/* The payload after them, its emulation prevention counted from the header on. */
	bits.bytes = bytes;
	bits.at = H264_GAP_HEADER_BYTES;
	bits.room = room;
	bits.pending = 0U;
	bits.pending_bits = 0U;
	bits.zeros = 0U;
	bits.full = false;

	/* The slice's first macroblock, its type, its picture set and its frame number. */
	h264_bits_ue(&bits, 0U);
	h264_bits_ue(&bits, H264_GAP_SLICE_P);
	h264_bits_ue(&bits, pps->pps_id);
	h264_bits_put(&bits, frame_num, sps->log2_max_frame_num_minus4 + 4U);

	/* A sequence of fields as well as frames says this is a frame. */
	if ((sps->flags & AMDRDNA_VCN_SPS_FRAME_MBS_ONLY) == 0U)
		h264_bits_put(&bits, 0U, 1U);

	/* The order count: the low bits of type 0, and no difference from the expected count of type 1. */
	lsb_bits = sps->log2_max_pic_order_cnt_lsb_minus4 + 4U;
	if (sps->pic_order_cnt_type == 0U) {
		h264_bits_put(&bits, (uint32_t)poc & ((1U << lsb_bits) - 1U), lsb_bits);
		if ((pps->flags & AMDRDNA_VCN_PPS_BOTTOM_POC) != 0U)
			h264_bits_ue(&bits, 0U);
	} else if (sps->pic_order_cnt_type == 1U && (sps->flags & AMDRDNA_VCN_SPS_DELTA_POC_ZERO) == 0U) {
		h264_bits_ue(&bits, 0U);
		if ((pps->flags & AMDRDNA_VCN_PPS_BOTTOM_POC) != 0U)
			h264_bits_ue(&bits, 0U);
	}

	/* No redundant picture. */
	if ((pps->flags & AMDRDNA_VCN_PPS_REDUNDANT) != 0U)
		h264_bits_ue(&bits, 0U);

	/* One active reference, the list as initialized, the sliding window, the picture set's quantizer. */
	h264_bits_put(&bits, 1U, 1U);
	h264_bits_ue(&bits, 0U);
	h264_bits_put(&bits, 0U, 1U);
	h264_bits_put(&bits, 0U, 1U);
	h264_bits_ue(&bits, 0U);

	/* The deblocking filter off, where the picture set lets the slice say. */
	if ((pps->flags & AMDRDNA_VCN_PPS_DEBLOCKING) != 0U)
		h264_bits_ue(&bits, H264_GAP_DEBLOCKING_OFF);

	/* Every macroblock of the frame skipped, then the slice's trailing bits. */
	height_mbs = sps->pic_height_in_map_units_minus1 + 1U;
	if ((sps->flags & AMDRDNA_VCN_SPS_FRAME_MBS_ONLY) == 0U)
		height_mbs *= 2U;
	mbs = (sps->pic_width_in_mbs_minus1 + 1U) * height_mbs;
	h264_bits_ue(&bits, mbs);
	h264_bits_put(&bits, 1U, 1U);
	if (bits.pending_bits != 0U)
		h264_bits_put(&bits, 0U, 8U - bits.pending_bits);

	/* The bytes the engine reads: the slice, zeros to its unit. */
	length = (bits.at + H264_GAP_BITSTREAM_ALIGN - 1U) & ~((size_t)H264_GAP_BITSTREAM_ALIGN - 1U);
	if (bits.full || length > room)
		return 0U;
	for (at = bits.at; at < length; at++)
		bytes[at] = 0U;

	/* Succeeded: the slice and its padding. */
	return length;
}

/*
 * Makes the picture set a frame a gap implies is decoded with (gap) from
 * the stream's (pps): its numbers and the flags its slice header follows,
 * with CAVLC, without weighted prediction and without the 8x8 transform,
 * which the skipped macroblocks of amdrdna_vcn_h264_gap_slice() leave
 * unused.
 */
void
amdrdna_vcn_h264_gap_pps(
	const struct amdrdna_vcn_h264_pps *pps,
	struct amdrdna_vcn_h264_pps *gap)
{
	/* The stream's set without the tools the slice does not use. */
	*gap = *pps;
	gap->flags &= ~(AMDRDNA_VCN_PPS_ENTROPY | AMDRDNA_VCN_PPS_WEIGHTED | AMDRDNA_VCN_PPS_TRANSFORM_8X8);
	gap->weighted_bipred_idc = 0U;
}

/* Writes what a picture's two sets say into the H.264 body: the flags, the formats, the sizes and the quantizers. */
static void
h264_body_sets(
	uint8_t *body,
	const struct amdrdna_vcn_h264_sps *sps,
	const struct amdrdna_vcn_h264_pps *pps)
{
	uint32_t flags;

	/* The sequence flags, with the extension support of a DPB the firmware keeps. */
	flags = H264_SPS_EXTENSION;
	if ((sps->flags & AMDRDNA_VCN_SPS_DIRECT_8X8) != 0U)
		flags |= H264_SPS_DIRECT_8X8;
	if ((sps->flags & AMDRDNA_VCN_SPS_MBAFF) != 0U)
		flags |= H264_SPS_MBAFF;
	if ((sps->flags & AMDRDNA_VCN_SPS_FRAME_MBS_ONLY) != 0U)
		flags |= H264_SPS_FRAME_MBS_ONLY;
	if ((sps->flags & AMDRDNA_VCN_SPS_DELTA_POC_ZERO) != 0U)
		flags |= H264_SPS_DELTA_POC_ZERO;
	format_put32(body, H264_SPS_FLAGS_AT, flags);

	/* The picture flags, with the weighted bi-prediction's mode. */
	flags = (pps->weighted_bipred_idc & H264_PPS_BIPRED_MASK) << H264_PPS_BIPRED_SHIFT;
	if ((pps->flags & AMDRDNA_VCN_PPS_TRANSFORM_8X8) != 0U)
		flags |= H264_PPS_TRANSFORM_8X8;
	if ((pps->flags & AMDRDNA_VCN_PPS_REDUNDANT) != 0U)
		flags |= H264_PPS_REDUNDANT;
	if ((pps->flags & AMDRDNA_VCN_PPS_CONSTRAINED) != 0U)
		flags |= H264_PPS_CONSTRAINED;
	if ((pps->flags & AMDRDNA_VCN_PPS_DEBLOCKING) != 0U)
		flags |= H264_PPS_DEBLOCKING;
	if ((pps->flags & AMDRDNA_VCN_PPS_WEIGHTED) != 0U)
		flags |= H264_PPS_WEIGHTED;
	if ((pps->flags & AMDRDNA_VCN_PPS_BOTTOM_POC) != 0U)
		flags |= H264_PPS_BOTTOM_POC;
	if ((pps->flags & AMDRDNA_VCN_PPS_ENTROPY) != 0U)
		flags |= H264_PPS_ENTROPY;
	format_put32(body, H264_PPS_FLAGS_AT, flags);

	/* The sequence's formats and sizes, a byte each. */
	body[H264_CHROMA_FORMAT_AT] = (uint8_t)sps->chroma_format_idc;
	body[H264_LUMA_DEPTH_AT] = (uint8_t)sps->bit_depth_luma_minus8;
	body[H264_CHROMA_DEPTH_AT] = (uint8_t)sps->bit_depth_chroma_minus8;
	body[H264_FRAME_NUM_BITS_AT] = (uint8_t)sps->log2_max_frame_num_minus4;
	body[H264_POC_TYPE_AT] = (uint8_t)sps->pic_order_cnt_type;
	body[H264_POC_BITS_AT] = (uint8_t)sps->log2_max_pic_order_cnt_lsb_minus4;
	body[H264_REFERENCE_FRAMES_AT] = (uint8_t)sps->max_num_ref_frames;

	/* The picture set's quantizers, signed bytes. */
	body[H264_QP_AT] = (uint8_t)pps->pic_init_qp_minus26;
	body[H264_QS_AT] = (uint8_t)pps->pic_init_qs_minus26;
	body[H264_CHROMA_QP_AT] = (uint8_t)pps->chroma_qp_index_offset;
	body[H264_SECOND_CHROMA_QP_AT] = (uint8_t)pps->second_chroma_qp_index_offset;

	/* One slice group, and the default counts of active references. */
	body[H264_SLICE_GROUPS_AT] = 0U;
	body[H264_L0_ACTIVE_AT] = (uint8_t)pps->num_ref_idx_l0_default_active_minus1;
	body[H264_L1_ACTIVE_AT] = (uint8_t)pps->num_ref_idx_l1_default_active_minus1;
}

/*
 * Writes the picture's tag and its references into the H.264 body: each
 * reference's tag (with the long-term bit), frame number, two order
 * counts, its field bits (both fields for a frame), and whether it is
 * missing; the entries past the references name none.
 */
static void
h264_body_references(
	uint8_t *body,
	const struct amdrdna_vcn_h264_decode *h264)
{
	const struct amdrdna_vcn_h264_reference *reference;
	uint32_t fields;
	uint32_t missing;
	uint32_t field_bits;
	uint32_t index;
	uint8_t tag;

	/* No entry names a reference until one does. */
	for (index = 0U; index < AMDRDNA_VCN_H264_REFERENCES; index++)
		body[H264_TAGS_AT + index] = H264_NO_REFERENCE;

	/* Each reference: its tag, its frame number, its order counts and its fields. */
	fields = 0U;
	missing = 0U;
	for (index = 0U; index < h264->reference_count; index++) {
		/* The tag, marked when the reference is long-term. */
		reference = &h264->references[index];
		tag = (uint8_t)reference->tag;
		if ((reference->flags & AMDRDNA_VCN_REFERENCE_LONG_TERM) != 0U)
			tag |= H264_LONG_TERM;
		body[H264_TAGS_AT + index] = tag;

		/* The frame number (or long-term index) and the two order counts. */
		format_put32(body, H264_FRAME_NUMS_AT + index * 4U, reference->frame_num);
		format_put32(body, H264_ORDERS_AT + index * 8U, (uint32_t)reference->poc[0]);
		format_put32(body, H264_ORDERS_AT + index * 8U + 4U, (uint32_t)reference->poc[1]);

		/* The fields it is a reference by: the ones named, or both for a frame. */
		field_bits = 0U;
		if ((reference->flags & AMDRDNA_VCN_REFERENCE_TOP) != 0U)
			field_bits |= H264_FIELD_TOP;
		if ((reference->flags & AMDRDNA_VCN_REFERENCE_BOTTOM) != 0U)
			field_bits |= H264_FIELD_BOTTOM;
		if (field_bits == 0U)
			field_bits = H264_FIELD_BOTH;
		fields |= field_bits << (2U * index);

		/* A reference the stream lost (a gap in the frame numbers). */
		if ((reference->flags & AMDRDNA_VCN_REFERENCE_MISSING) != 0U)
			missing |= 1U << index;
	}

	/* The picture's tag, the count of references, the missing ones and the fields. */
	format_put32(body, H264_TAG_AT, h264->tag);
	format_put32(body, H264_REFERENCE_COUNT_AT, h264->reference_count);
	format_put16(body, H264_MISSING_AT, missing);
	format_put32(body, H264_FIELDS_AT, fields);
}

/*
 * Takes one 4x4 list of a set in scan order: the fall-back when the set
 * does not carry it, the default matrix when the set says so, else the
 * set's own.
 */
static void
h264_list4(
	const struct amdrdna_vcn_h264_scaling *scaling,
	uint32_t list,
	const uint8_t *fallback,
	uint8_t *scan)
{
	const uint8_t *source;
	uint32_t index;

	/* The fall-back when the list is not there, the default when it says so, else its own. */
	source = fallback;
	if ((scaling->present_mask & (1U << list)) != 0U) {
		source = scaling->list4[list];
		if ((scaling->default_mask & (1U << list)) != 0U) {
			source = h264_default4_inter;
			if (list < 3U)
				source = h264_default4_intra;
		}
	}

	/* The sixteen coefficients. */
	for (index = 0U; index < 16U; index++)
		scan[index] = source[index];
}

/* Takes one 8x8 list of a set in scan order (list 6 intra, 7 inter), as h264_list4() does. */
static void
h264_list8(
	const struct amdrdna_vcn_h264_scaling *scaling,
	uint32_t list,
	const uint8_t *fallback,
	uint8_t *scan)
{
	const uint8_t *source;
	uint32_t index;

	/* The fall-back when the list is not there, the default when it says so, else its own. */
	source = fallback;
	if ((scaling->present_mask & (1U << list)) != 0U) {
		source = scaling->list8[list - H264_LISTS_4X4];
		if ((scaling->default_mask & (1U << list)) != 0U) {
			source = h264_default8_inter;
			if (list == H264_LIST_8X8_INTRA)
				source = h264_default8_intra;
		}
	}

	/* The sixty-four coefficients. */
	for (index = 0U; index < 64U; index++)
		scan[index] = source[index];
}

/* Moves a 4x4 list from scan order to raster order. */
static void
h264_raster4(
	const uint8_t *scan,
	uint8_t *raster)
{
	uint32_t index;

	/* Each coefficient to the raster place of its scan index. */
	for (index = 0U; index < 16U; index++)
		raster[h264_zigzag4[index]] = scan[index];
}

/* Moves an 8x8 list from scan order to raster order. */
static void
h264_raster8(
	const uint8_t *scan,
	uint8_t *raster)
{
	uint32_t index;

	/* Each coefficient to the raster place of its scan index. */
	for (index = 0U; index < 64U; index++)
		raster[h264_zigzag8[index]] = scan[index];
}

/* Rounds a value up to a boundary that is a power of two. */
static uint32_t
h264_align(
	uint32_t value,
	uint32_t boundary)
{
	/* Succeeded: the rounded value. */
	return (value + boundary - 1U) & ~(boundary - 1U);
}

/* Appends count bits of value (most significant first) to a slice. */
static void
h264_bits_put(
	struct h264_bits *bits,
	uint32_t value,
	uint32_t count)
{
	uint32_t index;

	/* Each bit, a byte out whenever eight are pending. */
	for (index = count; index > 0U; index--) {
		bits->pending = (bits->pending << 1) | ((value >> (index - 1U)) & 1U);
		bits->pending_bits++;
		if (bits->pending_bits == 8U) {
			h264_bits_byte(bits, (uint8_t)bits->pending);
			bits->pending = 0U;
			bits->pending_bits = 0U;
		}
	}
}

/* Appends an unsigned Exp-Golomb code (9.1) to a slice. */
static void
h264_bits_ue(
	struct h264_bits *bits,
	uint32_t value)
{
	uint64_t code;
	uint32_t length;

	/* The code is value + 1 in its own bits, after as many zeros less one. */
	code = (uint64_t)value + 1U;
	length = 0U;
	while ((code >> length) > 1U)
		length++;

	/* The zeros, then the code (at most 33 bits, in two parts). */
	h264_bits_put(bits, 0U, length);
	h264_bits_put(bits, (uint32_t)(code >> 16), length + 1U > 16U ? length + 1U - 16U : 0U);
	h264_bits_put(bits, (uint32_t)code & 0xffffU, length + 1U > 16U ? 16U : length + 1U);
}

/*
 * Appends one byte of the payload to a slice, an emulation prevention byte
 * before it where two zero bytes come before a byte of 3 or less (7.4.1).
 */
static void
h264_bits_byte(
	struct h264_bits *bits,
	uint8_t byte)
{
	/* The emulation prevention byte. */
	if (bits->zeros >= 2U && byte <= 3U) {
		if (bits->at >= bits->room) {
			bits->full = true;
			return;
		}
		bits->bytes[bits->at] = 3U;
		bits->at++;
		bits->zeros = 0U;
	}

	/* The byte, where the room holds it. */
	if (bits->at >= bits->room) {
		bits->full = true;
		return;
	}
	bits->bytes[bits->at] = byte;
	bits->at++;
	bits->zeros = byte == 0U ? bits->zeros + 1U : 0U;
}
