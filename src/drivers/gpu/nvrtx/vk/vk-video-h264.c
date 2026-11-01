/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The executor's H.264 arithmetic, which touches no hardware (a host test
 * runs it): the scaling matrices a picture decodes with, the decoder's
 * reference entries of the DPB slots, the picture description of a decode
 * and the NV12 layout of a video image (H.264 and H.265 alike).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <uapi/errno.h>

#include "drivers/gpu/nvrtx/vk/nvrtx-video.h"

/* The scaling lists' places among the twelve: the 4x4 lists 0 to 5, the luma 8x8 lists 6 (intra) and 7 (inter). */
#define H264_LISTS_4X4			6U
#define H264_LIST_8X8_INTRA		6U
#define H264_LIST_8X8_INTER		7U

/* The flat matrix's coefficient. */
#define H264_FLAT			16U

/*
 * The video image's planes: rows of whole GOBs (64 bytes), the planes one
 * after the other, in blocks two GOBs (16 rows) high.  The luma plane's
 * height is whole 64-row CTBs and the chroma plane's half of that, so a
 * decoder that writes an H.265 picture's last CTB row whole stays in its
 * plane.
 *
 * Values referenced: Mesa main 921d028a5fff2beb4ec90451913b5fcc05fb1e6a
 * src/nouveau/nil/tiling.rs (the GOB of 64 bytes by 8 rows) and
 * src/nouveau/nil/image.rs (video images in blocks two GOBs high), and
 * Recommendation ITU-T H.265 (7.4.3.2.1, CtbLog2SizeY at most 6).
 */
#define H264_GOB_BYTES			64U
#define H264_LUMA_ROWS			64U
#define H264_CHROMA_ROWS		32U

static void h264_list4(const struct nvrtx_video_scaling *scaling, uint32_t list, const uint8_t *fallback, uint8_t *scan);
static void h264_list8(const struct nvrtx_video_scaling *scaling, uint32_t list, const uint8_t *fallback, uint8_t *scan);
static void h264_raster4(const uint8_t *scan, uint8_t *raster);
static void h264_raster8(const uint8_t *scan, uint8_t *raster);
static uint32_t h264_align(uint32_t value, uint32_t boundary);

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
 * Resolves the scaling matrices a picture decodes with, in raster order:
 * the picture set's lists when it has them, else the sequence set's,
 * else flat.  A list a set does not carry falls back by the standard's
 * rules: in a sequence set (rule A) to the default list or the list
 * before; in a picture set (rule B) to the sequence-level list or the
 * list before.
 *
 * Procedure referenced: Recommendation ITU-T H.264 (7.4.2.1.1 and
 * 7.4.2.2, Table 7-2, the fall-back rules A and B).
 */
void
nvrtx_video_scaling_resolve(
	const struct nvrtx_video_sps *sps,
	const struct nvrtx_video_pps *pps,
	uint8_t scaling_4x4[6][16],
	uint8_t scaling_8x8[2][64])
{
	uint8_t sequence4[6][16];
	uint8_t sequence8[2][64];
	uint8_t picture4[6][16];
	uint8_t picture8[2][64];
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
	if ((sps->flags & NVRTX_VIDEO_SPS_SCALING) != 0U && sps->has_scaling)
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

	/* The picture's lists: the picture set's by rule B when it has a matrix, else the sequence level. */
	picture_lists = false;
	if ((pps->flags & NVRTX_VIDEO_PPS_SCALING) != 0U && pps->has_scaling)
		picture_lists = true;
	if (picture_lists) {
		h264_list4(&pps->scaling, 0U, sequence4[0], picture4[0]);
		h264_list4(&pps->scaling, 1U, picture4[0], picture4[1]);
		h264_list4(&pps->scaling, 2U, picture4[1], picture4[2]);
		h264_list4(&pps->scaling, 3U, sequence4[3], picture4[3]);
		h264_list4(&pps->scaling, 4U, picture4[3], picture4[4]);
		h264_list4(&pps->scaling, 5U, picture4[4], picture4[5]);
		h264_list8(&pps->scaling, H264_LIST_8X8_INTRA, sequence8[0], picture8[0]);
		h264_list8(&pps->scaling, H264_LIST_8X8_INTER, sequence8[1], picture8[1]);
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

	/* The decoder takes them in raster order. */
	for (list = 0U; list < 6U; list++)
		h264_raster4(picture4[list], scaling_4x4[list]);
	for (list = 0U; list < 2U; list++)
		h264_raster8(picture8[list], scaling_8x8[list]);
}

/* Clears every slot's reference entry: a session's start or reset. */
void
nvrtx_video_dpb_reset(
	struct nvrtx_video_dpb *dpb)
{
	uint32_t slot;

	/* No slot has an entry. */
	for (slot = 0U; slot < NVRTX_VIDEO_SLOTS; slot++)
		dpb->entry[slot] = -1;
}

/*
 * Keeps the entries of the slots a coding scope binds (active[n] for slot
 * n) and clears the others': a slot the scope does not bind holds no
 * reference any more.
 *
 * Procedure referenced: Mesa main 921d028a5fff2beb4ec90451913b5fcc05fb1e6a
 * src/nouveau/vulkan/rust/video/decode/h264.rs
 * (Decoder::remove_invalid_slots).
 */
void
nvrtx_video_dpb_keep(
	struct nvrtx_video_dpb *dpb,
	const bool *active)
{
	uint32_t slot;

	/* Each slot that is not bound loses its entry. */
	for (slot = 0U; slot < NVRTX_VIDEO_SLOTS; slot++) {
		if (!active[slot])
			dpb->entry[slot] = -1;
	}
}

/* Clears one slot's entry: a new picture is decoded into the slot. */
void
nvrtx_video_dpb_forget(
	struct nvrtx_video_dpb *dpb,
	uint32_t slot)
{
	/* A slot of the session's. */
	if (slot >= NVRTX_VIDEO_SLOTS)
		return;

	/* The slot holds no reference entry. */
	dpb->entry[slot] = -1;
}

/*
 * Reports the reference entry of a slot's picture, giving it the lowest
 * entry no other slot has when it has none yet.  Returns ENOSPC when every
 * entry is taken, EINVAL for a slot outside the session's.
 */
int
nvrtx_video_dpb_entry(
	struct nvrtx_video_dpb *dpb,
	uint32_t slot,
	uint32_t *entry)
{
	uint32_t taken;
	uint32_t index;
	uint32_t other;

	/* A slot of the session's. */
	if (slot >= NVRTX_VIDEO_SLOTS)
		return EINVAL;

	/* A slot that has its entry keeps it. */
	if (dpb->entry[slot] >= 0) {
		*entry = (uint32_t)dpb->entry[slot];
		return 0;
	}

	/* The entries the other slots have. */
	taken = 0U;
	for (other = 0U; other < NVRTX_VIDEO_SLOTS; other++) {
		if (dpb->entry[other] >= 0)
			taken |= 1U << (uint32_t)dpb->entry[other];
	}

	/* The lowest free one. */
	for (index = 0U; index < NVRTX_NVDEC_REFERENCES; index++) {
		if ((taken & (1U << index)) != 0U)
			continue;

		/* Succeeded: the slot's picture has the entry from now on. */
		dpb->entry[slot] = (int32_t)index;
		*entry = index;
		return 0;
	}

	/* Every entry is another slot's. */
	return ENOSPC;
}

/*
 * Reports a surface for a picture decoded without a slot: the lowest one
 * that is neither a slot holding a reference nor one the decode refers
 * to (referenced lists count surfaces).  There always is one: seventeen
 * surfaces, at most sixteen references.
 */
uint32_t
nvrtx_video_free_surface(
	const struct nvrtx_video_dpb *dpb,
	const uint32_t *referenced,
	uint32_t count)
{
	uint32_t surface;
	uint32_t index;
	bool used;

	/* Prefers a surface no slot holds and no reference is. */
	for (surface = 0U; surface < NVRTX_NVDEC_SURFACES; surface++) {
		used = false;
		if (surface < NVRTX_VIDEO_SLOTS && dpb->entry[surface] >= 0)
			used = true;

		/* A surface a reference list names is in use. */
		for (index = 0U; index < count; index++) {
			if (referenced[index] == surface)
				used = true;
		}

		/* Takes the first surface nothing uses. */
		if (!used)
			return surface;
	}

	/* Else one the decode does not refer to. */
	for (surface = 0U; surface < NVRTX_NVDEC_SURFACES; surface++) {
		used = false;

		/* A surface a reference list names is in use. */
		for (index = 0U; index < count; index++) {
			if (referenced[index] == surface)
				used = true;
		}

		/* Takes the first surface the decode does not refer to. */
		if (!used)
			return surface;
	}

	/* Unreachable with at most sixteen references: the last surface. */
	return NVRTX_NVDEC_SURFACES - 1U;
}

/*
 * Fills the parts of a picture description that come from the parameter
 * sets and the picture: the sequence's and the picture set's fields, the
 * picture's reference flag, frame number and order counts, the scaling
 * matrices, and the work buffers' sizes for the sequence's picture.  The
 * caller adds the surfaces, the references, the bitstream and the pitch.
 *
 * Procedure referenced: Mesa main 921d028a5fff2beb4ec90451913b5fcc05fb1e6a
 * src/nouveau/vulkan/rust/video/decode/h264.rs (Decoder::decode, which
 * field comes from which set; an order count of INT32_MAX takes the
 * other's).
 */
void
nvrtx_video_describe(
	const struct nvrtx_video_sps *sps,
	const struct nvrtx_video_pps *pps,
	const struct nvrtx_video_picture *picture,
	struct nvrtx_nvdec_h264_picture *description)
{
	struct nvrtx_nvdec_buffers buffers;
	uint32_t height;

	/* The sequence's fields; a sequence of field pictures is twice its map units tall. */
	description->log2_max_poc_lsb_minus4 = sps->log2_max_pic_order_cnt_lsb_minus4;
	description->delta_poc_always_zero = 0U;
	if ((sps->flags & NVRTX_VIDEO_SPS_DELTA_POC_ZERO) != 0U)
		description->delta_poc_always_zero = 1U;
	description->frame_mbs_only = 0U;
	if ((sps->flags & NVRTX_VIDEO_SPS_FRAME_MBS_ONLY) != 0U)
		description->frame_mbs_only = 1U;
	description->width_mbs = sps->pic_width_in_mbs_minus1 + 1U;
	height = sps->pic_height_in_map_units_minus1 + 1U;
	if (description->frame_mbs_only == 0U)
		height *= 2U;
	description->height_mbs = height;
	description->direct_8x8_inference = 0U;
	if ((sps->flags & NVRTX_VIDEO_SPS_DIRECT_8X8) != 0U)
		description->direct_8x8_inference = 1U;
	description->log2_max_frame_num_minus4 = sps->log2_max_frame_num_minus4;
	description->chroma_format_idc = sps->chroma_format_idc;
	description->poc_type = sps->pic_order_cnt_type;
	description->transform_bypass = 0U;
	if ((sps->flags & NVRTX_VIDEO_SPS_BYPASS) != 0U)
		description->transform_bypass = 1U;

	/* The picture set's fields. */
	description->entropy_coding = 0U;
	if ((pps->flags & NVRTX_VIDEO_PPS_ENTROPY) != 0U)
		description->entropy_coding = 1U;
	description->bottom_field_poc_present = 0U;
	if ((pps->flags & NVRTX_VIDEO_PPS_BOTTOM_POC) != 0U)
		description->bottom_field_poc_present = 1U;
	description->ref_l0_minus1 = pps->num_ref_idx_l0_default_active_minus1;
	description->ref_l1_minus1 = pps->num_ref_idx_l1_default_active_minus1;
	description->deblocking_control_present = 0U;
	if ((pps->flags & NVRTX_VIDEO_PPS_DEBLOCKING) != 0U)
		description->deblocking_control_present = 1U;
	description->redundant_pic_cnt_present = 0U;
	if ((pps->flags & NVRTX_VIDEO_PPS_REDUNDANT) != 0U)
		description->redundant_pic_cnt_present = 1U;
	description->transform_8x8 = 0U;
	if ((pps->flags & NVRTX_VIDEO_PPS_TRANSFORM_8X8) != 0U)
		description->transform_8x8 = 1U;
	description->weighted_pred = 0U;
	if ((pps->flags & NVRTX_VIDEO_PPS_WEIGHTED) != 0U)
		description->weighted_pred = 1U;
	description->weighted_bipred_idc = pps->weighted_bipred_idc;
	description->constrained_intra_pred = 0U;
	if ((pps->flags & NVRTX_VIDEO_PPS_CONSTRAINED) != 0U)
		description->constrained_intra_pred = 1U;
	description->pic_init_qp_minus26 = pps->pic_init_qp_minus26;
	description->chroma_qp_offset = pps->chroma_qp_index_offset;
	description->second_chroma_qp_offset = pps->second_chroma_qp_index_offset;

	/* The picture: whether it is a reference, its frame number and its order counts. */
	description->is_reference = 0U;
	if ((picture->flags & NVRTX_VIDEO_PICTURE_REFERENCE) != 0U)
		description->is_reference = 1U;
	description->frame_num = picture->frame_num;
	description->field_order[0] = nvrtx_video_order(picture->poc[0], picture->poc[1]);
	description->field_order[1] = nvrtx_video_order(picture->poc[1], picture->poc[0]);

	/* The scaling matrices. */
	nvrtx_video_scaling_resolve(sps, pps, description->scaling_4x4, description->scaling_8x8);

	/* The work buffers' sizes the sequence's picture needs. */
	nvrtx_nvdec_buffer_sizes(description->width_mbs, sps->pic_height_in_map_units_minus1 + 1U, sps->max_num_ref_frames, &buffers);
	description->mbhist_bytes = buffers.mbhist;
	description->history_bytes = buffers.history;
}

/* Reports a picture order count, or the other one when it is not given (INT32_MAX). */
int32_t
nvrtx_video_order(
	int32_t own,
	int32_t other)
{
	/* A count not given takes the other's. */
	if (own == INT32_MAX)
		return other;

	/* Succeeded: the count itself. */
	return own;
}

/*
 * Lays out an NV12 video image: both planes' rows the width's bytes in
 * whole GOBs, the luma plane's height and the chroma plane's half height
 * each in whole blocks of two GOBs, the chroma plane after the luma plane.
 */
void
nvrtx_video_layout(
	uint32_t width,
	uint32_t height,
	struct nvrtx_video_layout *layout)
{
	uint64_t luma_bytes;
	uint64_t chroma_bytes;

	/* The rows: the width's luma bytes, which are also the half width's chroma pairs. */
	layout->pitch = h264_align(width, H264_GOB_BYTES);

	/* The planes' rows in whole CTB rows (whole blocks too). */
	layout->plane_rows[0] = h264_align(height, H264_LUMA_ROWS);
	layout->plane_rows[1] = h264_align((height + 1U) / 2U, H264_CHROMA_ROWS);

	/* The luma plane first, the chroma plane after it. */
	luma_bytes = (uint64_t)layout->pitch * layout->plane_rows[0];
	chroma_bytes = (uint64_t)layout->pitch * layout->plane_rows[1];
	layout->plane_offset[0] = 0U;
	layout->plane_offset[1] = luma_bytes;
	layout->bytes = luma_bytes + chroma_bytes;
}

/*
 * Takes one 4x4 list of a set in scan order: the set's own, the default
 * matrix when the set says so, or the fall-back list when the set does
 * not carry it.
 */
static void
h264_list4(
	const struct nvrtx_video_scaling *scaling,
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
	const struct nvrtx_video_scaling *scaling,
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
