/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The HCP command builder of an H.265 decode (see vk-video-hcp.h).
 *
 * A picture is: the video pipeline's caches invalidated, the HEVC power
 * well held awake and the pipeline initialized, the pipeline set to an
 * HEVC decode, the output and reference surfaces, the buffers, the
 * bitstream, the twenty quantizer matrices, the picture state and the
 * tiles; then for each slice segment its state, its reference lists, its
 * weights and its BSD object; then the flushes.
 */

#include "vk-video-hcp.h"

#include "../intel/genxml-video.h"

#include <stdint.h>

/* The smallest CTB, which gives the most CTBs a side when the buffers are sized. */
#define I915_HCP_MIN_CTB	16U

/* What the line buffers are sized with: the session's largest picture in whole 64-sample CTBs. */
#define I915_HCP_MAX_CTB	64U

static void i915_hcp_address(struct i915_gfx_batch *batch, uint64_t address);
static void i915_hcp_buffer(struct i915_gfx_batch *batch, uint64_t address, uint32_t mocs);
static void i915_hcp_zeros(struct i915_gfx_batch *batch, unsigned count);
static void i915_hcp_head(struct i915_gfx_batch *batch);
static void i915_hcp_surfaces(struct i915_gfx_batch *batch, const struct i915_video_hcp_decode *decode);
static void i915_hcp_buffers(struct i915_gfx_batch *batch, const struct i915_video_hcp_decode *decode);
static void i915_hcp_bitstream(struct i915_gfx_batch *batch, const struct i915_video_hcp_decode *decode);
static void i915_hcp_matrices(struct i915_gfx_batch *batch, const struct i915_video_hcp_decode *decode);
static void i915_hcp_matrix(struct i915_gfx_batch *batch, uint32_t size, uint32_t prediction, uint32_t color, uint32_t dc, const uint8_t *values, uint32_t count);
static void i915_hcp_picture(struct i915_gfx_batch *batch, const struct i915_video_hcp_decode *decode);
static void i915_hcp_tiles(struct i915_gfx_batch *batch, const struct i915_video_hcp_decode *decode);
static void i915_hcp_positions(struct i915_gfx_batch *batch, const uint32_t *bounds, uint32_t count, uint32_t dwords);
static void i915_hcp_high_bits(struct i915_gfx_batch *batch, const uint32_t *bounds, uint32_t count);
static void i915_hcp_slice(struct i915_gfx_batch *batch, const struct i915_video_hcp_decode *decode, uint32_t index, uint32_t width_ctbs);
static void i915_hcp_lists(struct i915_gfx_batch *batch, const struct i915_video_hcp_decode *decode, const struct i915_video_h265_slice *slice);
static void i915_hcp_weights(struct i915_gfx_batch *batch, const struct i915_video_h265_slice *slice);
static uint32_t i915_hcp_signed(int32_t value, uint32_t mask);
static uint64_t i915_hcp_align(uint64_t value, uint64_t alignment);

/*
 * Reports the bytes of one of a session's nine line buffers for its
 * largest picture: the deblocking filter's line, tile line and tile column
 * buffers, the metadata's and the sample adaptive offset's.  The CTB
 * counts are taken at the smallest CTB (the most of them a picture can
 * have), and each size is doubled and made whole pages.
 *
 * Values referenced: Mesa 25.0.7 src/intel/vulkan/anv_video.c
 * (get_h265_video_mem_size: the deblocking, metadata and SAO buffers'
 * sizes for an 8-bit picture).
 */
uint64_t
drv_i915_video_hcp_line_bytes(
	uint32_t index,
	uint32_t width,
	uint32_t height)
{
	uint64_t width_ctbs;
	uint64_t height_ctbs;
	uint64_t units;

	/* The picture in CTBs at the smallest CTB. */
	width_ctbs = (width + I915_HCP_MIN_CTB - 1U) / I915_HCP_MIN_CTB;
	height_ctbs = (height + I915_HCP_MIN_CTB - 1U) / I915_HCP_MIN_CTB;

	/* Each buffer's size in 64-byte units. */
	switch (index) {
	case 0U:
	case 1U:
		units = i915_hcp_align(width, 32U) >> 3;
		break;
	case 2U:
		units = i915_hcp_align(height + 6U * height_ctbs, 32U) >> 3;
		break;
	case 3U:
		units = (((uint64_t)(width + 15U) >> 4) * 188U + width_ctbs * 9U + 1023U) >> 9;
		break;
	case 4U:
		units = (((uint64_t)(width + 15U) >> 4) * 172U + width_ctbs * 9U + 1023U) >> 9;
		break;
	case 5U:
		units = (((uint64_t)(height + 15U) >> 4) * 176U + height_ctbs * 89U + 1023U) >> 9;
		break;
	case 6U:
		units = i915_hcp_align((width >> 1) + width_ctbs * 3U, 16U) >> 3;
		break;
	case 7U:
		units = i915_hcp_align((width >> 1) + width_ctbs * 6U, 16U) >> 3;
		break;
	default:
		units = i915_hcp_align((height >> 1) + height_ctbs * 6U, 16U) >> 3;
		break;
	}

	/* Doubled, in whole pages. */
	return i915_hcp_align((units << 6) * 2U, 4096U);
}

/*
 * Reports the bytes of one picture's motion vectors: 64 bytes for each 32
 * by 32 block of the picture in whole 64-sample CTBs, an even count of
 * them, in whole pages.
 *
 * Values referenced: Mesa 25.0.7 src/intel/vulkan/anv_video.c
 * (anv_video_get_image_mv_size, the H.265 decode case).
 */
uint64_t
drv_i915_video_hcp_motion_bytes(
	uint32_t width,
	uint32_t height)
{
	uint64_t blocks;

	/* The 32x32 blocks of the picture in whole CTBs, an even count. */
	blocks = (i915_hcp_align(width, I915_HCP_MAX_CTB) / 32U) * (i915_hcp_align(height, I915_HCP_MAX_CTB) / 32U);
	blocks = i915_hcp_align(blocks, 2U);

	/* 64 bytes each, in whole pages. */
	return i915_hcp_align(blocks * 64U, 4096U);
}

/*
 * Writes the HCP commands that decode one picture into a batch.
 *
 * The batch keeps counting past its room, so a caller sees an overflow in
 * the batch rather than a picture cut off.
 *
 * Procedure referenced: Mesa 25.0.7 src/intel/vulkan/genX_cmd_video.c
 * (anv_h265_decode_video: the order of the commands of a picture and of
 * its slices).
 */
void
drv_i915_video_hcp_build(
	struct i915_gfx_batch *batch,
	const struct i915_video_hcp_decode *decode)
{
	struct i915_video_h265_geometry geometry;
	uint32_t index;

	/* The flush, the power well, the pipeline's initialization and its mode. */
	i915_hcp_head(batch);

	/* The output and reference surfaces. */
	i915_hcp_surfaces(batch, decode);

	/* The output, the line buffers, the references and the motion vectors. */
	i915_hcp_buffers(batch, decode);

	/* The bitstream. */
	i915_hcp_bitstream(batch, decode);

	/* The quantizer matrices, the picture state and the tiles. */
	i915_hcp_matrices(batch, decode);
	i915_hcp_picture(batch, decode);
	if ((decode->pps->flags & I915_VIDEO_H265_PPS_TILES) != 0U)
		i915_hcp_tiles(batch, decode);

	/* Each slice segment. */
	drv_i915_video_h265_geometry(decode->sps, decode->pps, &geometry);
	for (index = 0U; index < decode->slice_count; index++)
		i915_hcp_slice(batch, decode, index, geometry.width_ctbs);

	/* The writes flushed to memory, the pipeline done, then the closing flush before the batch ends. */
	drv_i915_batch_emit(batch, GEN12_VIDEO_HEADER(GEN12_VIDEO_VD_CONTROL_STATE, GEN12_VIDEO_VD_CONTROL_STATE_DWORDS));
	drv_i915_batch_emit(batch, 0U);
	drv_i915_batch_emit(batch, GEN12_VIDEO_VD_MEMORY_IMPLICIT_FLUSH);
	drv_i915_batch_emit(batch, GEN12_VIDEO_HEADER(GEN12_VIDEO_VD_PIPELINE_FLUSH, GEN12_VIDEO_VD_PIPELINE_FLUSH_DWORDS));
	drv_i915_batch_emit(batch, GEN12_VIDEO_VD_HEVC_DONE | GEN12_VIDEO_VD_PARSER_DONE | GEN12_VIDEO_VD_HEVC_COMMAND_FLUSH);
	drv_i915_batch_emit(batch, GEN12_VIDEO_MI_FLUSH_DW);
	i915_hcp_zeros(batch, GEN12_VIDEO_MI_FLUSH_DW_DWORDS - 1U);
}

/* Writes a 64-bit GPU address as its low and its high dword. */
static void
i915_hcp_address(
	struct i915_gfx_batch *batch,
	uint64_t address)
{
	/* The low dword, then the high one. */
	drv_i915_batch_emit(batch, (uint32_t)address);
	drv_i915_batch_emit(batch, (uint32_t)(address >> 32));
}

/* Writes a buffer: its address and its memory attributes (the MOCS, nothing else). */
static void
i915_hcp_buffer(
	struct i915_gfx_batch *batch,
	uint64_t address,
	uint32_t mocs)
{
	/* The address, then the attributes dword. */
	i915_hcp_address(batch, address);
	drv_i915_batch_emit(batch, mocs & GEN12_VIDEO_ATTRIBUTES_MOCS_MASK);
}

/* Writes a run of zero dwords: fields the decode does not use. */
static void
i915_hcp_zeros(
	struct i915_gfx_batch *batch,
	unsigned count)
{
	unsigned index;

	/* One zero for each. */
	for (index = 0U; index < count; index++)
		drv_i915_batch_emit(batch, 0U);
}

/*
 * Writes the start of a decode: the video pipeline's cache invalidated, the
 * HEVC power well held awake, the pipeline initialized and set to an HEVC
 * decode, between waits for it to be idle.
 *
 * Procedure referenced: Mesa 25.0.7 src/intel/vulkan/genX_cmd_video.c
 * (anv_h265_decode_video, its Gen12 start).
 */
static void
i915_hcp_head(
	struct i915_gfx_batch *batch)
{
	/* Invalidates the video pipeline's caches. */
	drv_i915_batch_emit(batch, GEN12_VIDEO_MI_FLUSH_DW | GEN12_VIDEO_MI_FLUSH_DW_INVALIDATE);
	i915_hcp_zeros(batch, GEN12_VIDEO_MI_FLUSH_DW_DWORDS - 1U);

	/* Holds the HEVC power well awake for the decode. */
	drv_i915_batch_emit(batch, GEN12_VIDEO_MI_FORCE_WAKEUP);
	drv_i915_batch_emit(batch, GEN12_VIDEO_FORCE_WAKEUP_HEVC |
	    (GEN12_VIDEO_FORCE_WAKEUP_MASK << GEN12_VIDEO_FORCE_WAKEUP_MASK_SHIFT));

	/* Initializes the pipeline and waits for it. */
	drv_i915_batch_emit(batch, GEN12_VIDEO_HEADER(GEN12_VIDEO_VD_CONTROL_STATE, GEN12_VIDEO_VD_CONTROL_STATE_DWORDS));
	drv_i915_batch_emit(batch, GEN12_VIDEO_VD_PIPELINE_INITIALIZATION);
	drv_i915_batch_emit(batch, 0U);
	drv_i915_batch_emit(batch, GEN12_VIDEO_MFX_WAIT | GEN12_VIDEO_MFX_WAIT_SYNC);

	/* HEVC, decode, the legacy pipe mode: every field zero. */
	drv_i915_batch_emit(batch, GEN12_VIDEO_HEADER(GEN12_VIDEO_HCP_PIPE_MODE_SELECT, GEN12_VIDEO_HCP_PIPE_MODE_SELECT_DWORDS));
	i915_hcp_zeros(batch, GEN12_VIDEO_HCP_PIPE_MODE_SELECT_DWORDS - 1U);

	/* Waits for the new mode to settle. */
	drv_i915_batch_emit(batch, GEN12_VIDEO_MFX_WAIT | GEN12_VIDEO_MFX_WAIT_SYNC);
}

/*
 * Writes the output picture's surface and the references' (Gen12 takes
 * both): NV12 in Y tiles at the output's pitch, the CbCr plane below the
 * Y plane at its row.  The references are read through the same surface,
 * which is why they must be laid out as the output is.
 */
static void
i915_hcp_surfaces(
	struct i915_gfx_batch *batch,
	const struct i915_video_hcp_decode *decode)
{
	uint32_t surface;
	uint32_t identity;

	/* The decoded picture's surface, then the references'. */
	for (surface = 0U; surface < 2U; surface++) {
		identity = GEN12_VIDEO_HCP_SURFACE_DECODED;
		if (surface != 0U)
			identity = GEN12_VIDEO_HCP_SURFACE_REFERENCE;
		drv_i915_batch_emit(batch, GEN12_VIDEO_HEADER(GEN12_VIDEO_HCP_SURFACE_STATE, GEN12_VIDEO_HCP_SURFACE_STATE_DWORDS));
		drv_i915_batch_emit(batch, (decode->pitch - 1U) | (identity << GEN12_VIDEO_HCP_SURFACE_ID_SHIFT));
		drv_i915_batch_emit(batch, decode->chroma_rows | (GEN12_VIDEO_HCP_SURFACE_PLANAR_420_8 << GEN12_VIDEO_HCP_SURFACE_FORMAT_SHIFT));
		drv_i915_batch_emit(batch, GEN12_VIDEO_HCP_SURFACE_ALPHA);
		drv_i915_batch_emit(batch, 0U);
	}
}

/*
 * Writes the pipeline's buffers: the output, the nine line buffers, the
 * motion vectors the picture writes, the eight reference pictures, and
 * the eight references' motion vectors (an unused place repeats a used
 * one).  The buffers a decode does not use have address zero and still
 * carry the MOCS.
 *
 * Procedure referenced: Mesa 25.0.7 src/intel/vulkan/genX_cmd_video.c
 * (anv_h265_decode_video, HCP_PIPE_BUF_ADDR_STATE).
 */
static void
i915_hcp_buffers(
	struct i915_gfx_batch *batch,
	const struct i915_video_hcp_decode *decode)
{
	uint64_t fallback_picture;
	uint64_t fallback_motion;
	unsigned index;

	/* The header and the decoded picture. */
	drv_i915_batch_emit(batch, GEN12_VIDEO_HEADER(GEN12_VIDEO_HCP_PIPE_BUF_ADDR_STATE, GEN12_VIDEO_HCP_PIPE_BUF_ADDR_STATE_DWORDS));
	i915_hcp_buffer(batch, decode->destination, decode->mocs);

	/* The deblocking, metadata and SAO line buffers, then the motion vectors written (dword 31). */
	for (index = 0U; index < I915_VIDEO_HCP_LINE_BUFFERS; index++)
		i915_hcp_buffer(batch, decode->line_buffers[index], decode->mocs);
	i915_hcp_buffer(batch, decode->motion_write, decode->mocs);

	/* Three reserved dwords before the references (dword 37). */
	i915_hcp_zeros(batch, GEN12_VIDEO_HCP_BUF_REFERENCES - GEN12_VIDEO_HCP_BUF_CURRENT_MV - 3U);

	/* The reference pictures; an unused place is the first reference's, or the output's. */
	fallback_picture = decode->destination;
	fallback_motion = decode->motion_write;
	if (decode->reference_count != 0U) {
		fallback_picture = decode->references[0].picture;
		fallback_motion = decode->references[0].motion;
	}
	for (index = 0U; index < GEN12_VIDEO_HCP_REFERENCES; index++) {
		if (index < decode->reference_count)
			i915_hcp_address(batch, decode->references[index].picture);
		else
			i915_hcp_address(batch, fallback_picture);
	}
	drv_i915_batch_emit(batch, decode->mocs & GEN12_VIDEO_ATTRIBUTES_MOCS_MASK);

	/* The encoder's source and stream-out, the status and the ILDB stream-out buffers the decode does not use. */
	for (index = 0U; index < 4U; index++)
		i915_hcp_buffer(batch, 0U, decode->mocs);

	/* The references' motion vectors (dword 66). */
	for (index = 0U; index < GEN12_VIDEO_HCP_REFERENCES; index++) {
		if (index < decode->reference_count)
			i915_hcp_address(batch, decode->references[index].motion);
		else
			i915_hcp_address(batch, fallback_motion);
	}
	drv_i915_batch_emit(batch, decode->mocs & GEN12_VIDEO_ATTRIBUTES_MOCS_MASK);

	/* The VP9, stream-out, statistics, row store and scalability buffers the decode does not use (dwords 83 to 118). */
	for (index = 0U; index < 12U; index++)
		i915_hcp_buffer(batch, 0U, decode->mocs);

	/* The scalability CABAC buffer's upper bound (dwords 119 and 120). */
	i915_hcp_address(batch, 0U);
}

/*
 * Writes the bitstream: the indirect bitstream object starts at the page of
 * the decode's range and is bounded by the end of the buffer's bound range,
 * so a slice can be read only inside the buffer.  The other indirect
 * objects are the encoder's.
 */
static void
i915_hcp_bitstream(
	struct i915_gfx_batch *batch,
	const struct i915_video_hcp_decode *decode)
{
	unsigned index;

	/* The bitstream object: its address, its attributes and its upper bound. */
	drv_i915_batch_emit(batch, GEN12_VIDEO_HEADER(GEN12_VIDEO_HCP_IND_OBJ_BASE_ADDR_STATE, GEN12_VIDEO_HCP_IND_OBJ_BASE_ADDR_STATE_DWORDS));
	i915_hcp_buffer(batch, decode->bitstream_base, decode->mocs);
	i915_hcp_address(batch, decode->bitstream_end);

	/* The CU object, the PAK-BSE object with its bound, and the five VP9 PAK objects. */
	i915_hcp_buffer(batch, 0U, decode->mocs);
	i915_hcp_buffer(batch, 0U, decode->mocs);
	i915_hcp_address(batch, 0U);
	for (index = 0U; index < 5U; index++)
		i915_hcp_buffer(batch, 0U, decode->mocs);
}

/*
 * Writes the twenty quantizer matrices: for each size (4x4, 8x8, 16x16,
 * 32x32), prediction (intra, inter) and colour (Y, Cb, Cr; only Y for
 * 32x32), the matrix in raster order and the DC value of the two larger
 * sizes.
 *
 * Procedure referenced: Mesa 25.0.7 src/intel/vulkan/genX_cmd_video.c
 * (scaling_list and anv_h265_decode_video: the order of the matrices and
 * the raster order of their entries).
 */
static void
i915_hcp_matrices(
	struct i915_gfx_batch *batch,
	const struct i915_video_hcp_decode *decode)
{
	const struct i915_video_h265_matrices *matrices;
	uint32_t prediction;
	uint32_t color;
	uint32_t list;

	/* Each size, prediction and colour. */
	matrices = &decode->matrices;
	for (prediction = 0U; prediction < 2U; prediction++) {
		for (color = 0U; color < 3U; color++) {
			list = 3U * prediction + color;
			i915_hcp_matrix(batch, 0U, prediction, color, 0U, matrices->list4[list], 16U);
		}
	}
	for (prediction = 0U; prediction < 2U; prediction++) {
		for (color = 0U; color < 3U; color++) {
			list = 3U * prediction + color;
			i915_hcp_matrix(batch, 1U, prediction, color, 0U, matrices->list8[list], 64U);
		}
	}
	for (prediction = 0U; prediction < 2U; prediction++) {
		for (color = 0U; color < 3U; color++) {
			list = 3U * prediction + color;
			i915_hcp_matrix(batch, 2U, prediction, color, matrices->dc16[list], matrices->list16[list], 64U);
		}
	}
	for (prediction = 0U; prediction < 2U; prediction++)
		i915_hcp_matrix(batch, 3U, prediction, 0U, matrices->dc32[prediction], matrices->list32[prediction], 64U);
}

/* Writes one quantizer matrix: its kind and DC value, then its entries four a dword (the rest zero). */
static void
i915_hcp_matrix(
	struct i915_gfx_batch *batch,
	uint32_t size,
	uint32_t prediction,
	uint32_t color,
	uint32_t dc,
	const uint8_t *values,
	uint32_t count)
{
	uint32_t word;
	uint32_t index;
	uint32_t byte;

	/* The header and the matrix's kind. */
	drv_i915_batch_emit(batch, GEN12_VIDEO_HEADER(GEN12_VIDEO_HCP_QM_STATE, GEN12_VIDEO_HCP_QM_STATE_DWORDS));
	drv_i915_batch_emit(batch, prediction |
	    (size << GEN12_VIDEO_HCP_QM_SIZE_SHIFT) |
	    (color << GEN12_VIDEO_HCP_QM_COLOR_SHIFT) |
	    (dc << GEN12_VIDEO_HCP_QM_DC_SHIFT));

	/* The 64 bytes, the low byte of a dword first. */
	for (index = 0U; index < GEN12_VIDEO_HCP_QM_BYTES; index += 4U) {
		word = 0U;
		for (byte = 0U; byte < 4U; byte++) {
			if (index + byte < count)
				word |= (uint32_t)values[index + byte] << (8U * byte);
		}
		drv_i915_batch_emit(batch, word);
	}
}

/*
 * Writes the picture state from the sets: the picture in minimum coding
 * blocks, the block sizes, the tools, the chroma offsets and the depths.
 * The fields of the range extension and of the encoder stay zero.
 *
 * Procedure referenced: Mesa 25.0.7 src/intel/vulkan/genX_cmd_video.c
 * (anv_h265_decode_video, HCP_PIC_STATE).
 */
static void
i915_hcp_picture(
	struct i915_gfx_batch *batch,
	const struct i915_video_hcp_decode *decode)
{
	const struct i915_video_h265_sps *sps;
	const struct i915_video_h265_pps *pps;
	uint32_t min_cb;
	uint32_t log2_ctb;
	uint32_t log2_min_tb;
	uint32_t log2_min_pcm;
	uint32_t word;

	sps = decode->sps;
	pps = decode->pps;
	min_cb = sps->log2_min_cb_minus3 + 3U;
	log2_ctb = min_cb + sps->log2_diff_cb;
	log2_min_tb = sps->log2_min_tb_minus2 + 2U;
	log2_min_pcm = sps->log2_min_pcm_minus3 + 3U;

	/* The header and the picture in minimum coding blocks, less one each way. */
	drv_i915_batch_emit(batch, GEN12_VIDEO_HEADER(GEN12_VIDEO_HCP_PIC_STATE, GEN12_VIDEO_HCP_PIC_STATE_DWORDS));
	drv_i915_batch_emit(batch, ((sps->width >> min_cb) - 1U) | (((sps->height >> min_cb) - 1U) << GEN12_VIDEO_HCP_PIC_HEIGHT_SHIFT));

	/* The coding, transform and PCM block sizes, and 4:2:0. */
	word = (min_cb - 3U) |
	    ((log2_ctb - 3U) << GEN12_VIDEO_HCP_PIC_LCU_SHIFT) |
	    ((log2_min_tb - 2U) << GEN12_VIDEO_HCP_PIC_MIN_TU_SHIFT) |
	    ((log2_min_tb + sps->log2_diff_tb - 2U) << GEN12_VIDEO_HCP_PIC_MAX_TU_SHIFT) |
	    sps->chroma_format_idc << GEN12_VIDEO_HCP_PIC_CHROMA_SHIFT;
	if ((sps->flags & I915_VIDEO_H265_SPS_PCM) != 0U) {
		word |= (log2_min_pcm - 3U) << GEN12_VIDEO_HCP_PIC_MIN_PCM_SHIFT;
		word |= (log2_min_pcm + sps->log2_diff_pcm - 3U) << GEN12_VIDEO_HCP_PIC_MAX_PCM_SHIFT;
	}
	drv_i915_batch_emit(batch, word);

	/*
	 * Neither picture is told to be intra; the range extension's flags are
	 * off, and Log2MaxTransformSkipSize is 2, as H.265 infers it without
	 * the extension (7.4.3.3.2: log2_max_transform_skip_block_size_minus2
	 * is 0).
	 *
	 * Procedure referenced: Mesa 26.3.0-rc3 src/intel/vulkan/genX_cmd_video.c
	 * (anv_h265_decode_video, HCP_PIC_STATE Log2MaxTransformSkipSize).
	 */
	drv_i915_batch_emit(batch, 2U << GEN12_VIDEO_HCP_PIC_SKIP_SIZE_SHIFT);

	/* The tools. */
	word = (pps->cu_qp_delta_depth << GEN12_VIDEO_HCP_PIC_DQP_DEPTH_SHIFT) |
	    (pps->log2_merge_minus2 << GEN12_VIDEO_HCP_PIC_MERGE_LEVEL_SHIFT) |
	    GEN12_VIDEO_HCP_PIC_TOP_FIELD;
	if ((sps->flags & I915_VIDEO_H265_SPS_SAO) != 0U)
		word |= GEN12_VIDEO_HCP_PIC_SAO;
	if ((sps->flags & I915_VIDEO_H265_SPS_PCM) != 0U)
		word |= GEN12_VIDEO_HCP_PIC_PCM;
	if ((pps->flags & I915_VIDEO_H265_PPS_CU_QP_DELTA) != 0U)
		word |= GEN12_VIDEO_HCP_PIC_CU_QP_DELTA;
	if ((sps->flags & I915_VIDEO_H265_SPS_PCM_FILTER_OFF) != 0U)
		word |= GEN12_VIDEO_HCP_PIC_PCM_FILTER_OFF;
	if ((pps->flags & I915_VIDEO_H265_PPS_CONSTRAINED) != 0U)
		word |= GEN12_VIDEO_HCP_PIC_CONSTRAINED_INTRA;
	if ((pps->flags & I915_VIDEO_H265_PPS_SIGN_HIDING) != 0U)
		word |= GEN12_VIDEO_HCP_PIC_SIGN_HIDING;
	if ((pps->flags & I915_VIDEO_H265_PPS_ACROSS_TILES) != 0U)
		word |= GEN12_VIDEO_HCP_PIC_FILTER_TILES;
	if ((pps->flags & I915_VIDEO_H265_PPS_ENTROPY_SYNC) != 0U)
		word |= GEN12_VIDEO_HCP_PIC_ENTROPY_SYNC;
	if ((pps->flags & I915_VIDEO_H265_PPS_TILES) != 0U)
		word |= GEN12_VIDEO_HCP_PIC_TILES;
	if ((pps->flags & I915_VIDEO_H265_PPS_BIPRED) != 0U)
		word |= GEN12_VIDEO_HCP_PIC_WEIGHTED_BIPRED;
	if ((pps->flags & I915_VIDEO_H265_PPS_WEIGHTED) != 0U)
		word |= GEN12_VIDEO_HCP_PIC_WEIGHTED_PRED;
	if ((pps->flags & I915_VIDEO_H265_PPS_TRANSFORM_SKIP) != 0U)
		word |= GEN12_VIDEO_HCP_PIC_TRANSFORM_SKIP;
	if ((sps->flags & I915_VIDEO_H265_SPS_AMP) != 0U)
		word |= GEN12_VIDEO_HCP_PIC_AMP;
	if ((pps->flags & I915_VIDEO_H265_PPS_BYPASS) != 0U)
		word |= GEN12_VIDEO_HCP_PIC_TRANSQUANT_BYPASS;
	if ((sps->flags & I915_VIDEO_H265_SPS_STRONG_INTRA) != 0U)
		word |= GEN12_VIDEO_HCP_PIC_STRONG_INTRA;
	drv_i915_batch_emit(batch, word);

	/* The chroma QP offsets, the transform depths, the PCM depths and the bit depths. */
	word = i915_hcp_signed(pps->cb_qp_offset, GEN12_VIDEO_HCP_QP_OFFSET_MASK) |
	    (i915_hcp_signed(pps->cr_qp_offset, GEN12_VIDEO_HCP_QP_OFFSET_MASK) << GEN12_VIDEO_HCP_PIC_CR_OFFSET_SHIFT) |
	    (sps->depth_intra << GEN12_VIDEO_HCP_PIC_INTRA_DEPTH_SHIFT) |
	    (sps->depth_inter << GEN12_VIDEO_HCP_PIC_INTER_DEPTH_SHIFT) |
	    (sps->bit_depth_chroma_minus8 << GEN12_VIDEO_HCP_PIC_DEPTH_CHROMA_SHIFT) |
	    (sps->bit_depth_luma_minus8 << GEN12_VIDEO_HCP_PIC_DEPTH_LUMA_SHIFT);
	if ((sps->flags & I915_VIDEO_H265_SPS_PCM) != 0U) {
		word |= (sps->pcm_depth_chroma_minus1 & 0xfU) << GEN12_VIDEO_HCP_PIC_PCM_CHROMA_SHIFT;
		word |= (sps->pcm_depth_luma_minus1 & 0xfU) << GEN12_VIDEO_HCP_PIC_PCM_LUMA_SHIFT;
	}
	drv_i915_batch_emit(batch, word);

	/* The encoder's fields and the chroma offset lists of the range extension. */
	i915_hcp_zeros(batch, GEN12_VIDEO_HCP_PIC_STATE_DWORDS - 6U);
}

/*
 * Writes the tiles: their counts, then each tile column's and row's first
 * CTB, the low eight bits and the two high bits apart.
 *
 * Procedure referenced: Mesa 25.0.7 src/intel/vulkan/genX_cmd_video.c
 * (anv_h265_decode_video, HCP_TILE_STATE: each tile's first CTB).
 */
static void
i915_hcp_tiles(
	struct i915_gfx_batch *batch,
	const struct i915_video_hcp_decode *decode)
{
	struct i915_video_h265_geometry geometry;

	/* The tile boundaries. */
	drv_i915_video_h265_geometry(decode->sps, decode->pps, &geometry);

	/* The header and the counts, less one. */
	drv_i915_batch_emit(batch, GEN12_VIDEO_HEADER(GEN12_VIDEO_HCP_TILE_STATE, GEN12_VIDEO_HCP_TILE_STATE_DWORDS));
	drv_i915_batch_emit(batch, (geometry.rows - 1U) | ((geometry.columns - 1U) << GEN12_VIDEO_HCP_TILE_COLUMNS_SHIFT));

	/* The columns' and the rows' first CTBs, then their high bits. */
	i915_hcp_positions(batch, geometry.column_bounds, geometry.columns, GEN12_VIDEO_HCP_TILE_COLUMN_DWORDS);
	i915_hcp_positions(batch, geometry.row_bounds, geometry.rows, GEN12_VIDEO_HCP_TILE_ROW_DWORDS);
	i915_hcp_high_bits(batch, geometry.column_bounds, geometry.columns);
	i915_hcp_high_bits(batch, geometry.row_bounds, geometry.rows);
}

/* Writes the low eight bits of each tile's first CTB, four a dword over dwords dwords. */
static void
i915_hcp_positions(
	struct i915_gfx_batch *batch,
	const uint32_t *bounds,
	uint32_t count,
	uint32_t dwords)
{
	uint32_t word;
	uint32_t index;
	uint32_t byte;

	/* Each dword's four positions. */
	for (index = 0U; index < dwords; index++) {
		word = 0U;
		for (byte = 0U; byte < 4U; byte++) {
			if (4U * index + byte < count)
				word |= (bounds[4U * index + byte] & 0xffU) << (8U * byte);
		}
		drv_i915_batch_emit(batch, word);
	}
}

/* Writes the two high bits of each tile's first CTB, two bits an entry from the low bit of two dwords. */
static void
i915_hcp_high_bits(
	struct i915_gfx_batch *batch,
	const uint32_t *bounds,
	uint32_t count)
{
	uint64_t bits;
	uint32_t index;

	/* Each entry's bits 9:8. */
	bits = 0U;
	for (index = 0U; index < count; index++)
		bits |= (uint64_t)((bounds[index] >> 8) & 3U) << (2U * index);
	drv_i915_batch_emit(batch, (uint32_t)bits);
	drv_i915_batch_emit(batch, (uint32_t)(bits >> 32));
}

/*
 * Writes one slice segment: its state (its place and the next one's, its
 * type, QP, filters, the collocated picture and the header's length), its
 * reference lists and weights for a predicted slice, and its BSD object
 * (its NAL unit after the start code).
 *
 * Procedure referenced: Mesa 25.0.7 src/intel/vulkan/genX_cmd_video.c
 * (anv_h265_decode_video, HCP_SLICE_STATE and HCP_BSD_OBJECT).
 */
static void
i915_hcp_slice(
	struct i915_gfx_batch *batch,
	const struct i915_video_hcp_decode *decode,
	uint32_t index,
	uint32_t width_ctbs)
{
	const struct i915_video_h265_slice *slice;
	const struct i915_video_h265_slice *next;
	uint32_t word;
	uint32_t list;
	uint32_t entry;
	uint32_t slot;
	uint32_t collocated;
	int32_t qp;
	bool low_delay;

	slice = &decode->slices[index];
	next = NULL;
	if (index + 1U < decode->slice_count)
		next = &decode->slices[index + 1U];

	/* The header, the segment's first CTB and the next segment's (0 after the last). */
	drv_i915_batch_emit(batch, GEN12_VIDEO_HEADER(GEN12_VIDEO_HCP_SLICE_STATE, GEN12_VIDEO_HCP_SLICE_STATE_DWORDS));
	drv_i915_batch_emit(batch, (slice->address % width_ctbs) | ((slice->address / width_ctbs) << GEN12_VIDEO_HCP_SLICE_Y_SHIFT));
	if (next != NULL)
		drv_i915_batch_emit(batch, (next->address % width_ctbs) | ((next->address / width_ctbs) << GEN12_VIDEO_HCP_SLICE_Y_SHIFT));
	else
		drv_i915_batch_emit(batch, 0U);

	/* The type, the last segment, the dependence, the temporal MVP, the QP and the chroma offsets. */
	qp = slice->qp;
	word = slice->slice_type | ((uint32_t)(qp < 0 ? -qp : qp) << GEN12_VIDEO_HCP_SLICE_QP_SHIFT) |
	    (i915_hcp_signed(slice->cb_qp_offset, GEN12_VIDEO_HCP_QP_OFFSET_MASK) << GEN12_VIDEO_HCP_SLICE_CB_SHIFT) |
	    (i915_hcp_signed(slice->cr_qp_offset, GEN12_VIDEO_HCP_QP_OFFSET_MASK) << GEN12_VIDEO_HCP_SLICE_CR_SHIFT);
	if (qp < 0)
		word |= GEN12_VIDEO_HCP_SLICE_QP_SIGN;
	if (next == NULL)
		word |= GEN12_VIDEO_HCP_SLICE_LAST;
	if (slice->dependent)
		word |= GEN12_VIDEO_HCP_SLICE_DEPENDENT;
	if (slice->temporal_mvp)
		word |= GEN12_VIDEO_HCP_SLICE_TEMPORAL_MVP;
	drv_i915_batch_emit(batch, word);

	/*
	 * NoBackwardPredFlag (8.5.3.2.2): no picture of the lists follows the
	 * current one in order.
	 */
	low_delay = slice->slice_type != I915_VIDEO_H265_SLICE_I;
	for (list = 0U; list < 2U; list++) {
		for (entry = 0U; entry < slice->references[list]; entry++) {
			slot = slice->list[list][entry];
			if (slot < I915_VIDEO_H265_SLOTS && decode->slots[slot].poc > decode->poc)
				low_delay = false;
		}
	}

	/* The collocated picture's place. */
	collocated = 0U;
	if (slice->slice_type != I915_VIDEO_H265_SLICE_I && slice->temporal_mvp) {
		list = slice->collocated_from_l0 ? 0U : 1U;
		if (slice->collocated_ref_idx < slice->references[list]) {
			slot = slice->list[list][slice->collocated_ref_idx];
			if (slot < I915_VIDEO_H265_SLOTS && decode->place[slot] != I915_VIDEO_H265_NO_SLOT)
				collocated = decode->place[slot];
		}
	}

	/* The filters, the motion vector difference, the weights' denominators, CABAC and the merge candidates. */
	word = (i915_hcp_signed(slice->tc_div2, GEN12_VIDEO_HCP_SLICE_OFFSET_MASK) << GEN12_VIDEO_HCP_SLICE_TC_SHIFT) |
	    (i915_hcp_signed(slice->beta_div2, GEN12_VIDEO_HCP_SLICE_OFFSET_MASK) << GEN12_VIDEO_HCP_SLICE_BETA_SHIFT) |
	    ((slice->max_merge_candidates - 1U) << GEN12_VIDEO_HCP_SLICE_MERGE_SHIFT) |
	    (collocated << GEN12_VIDEO_HCP_SLICE_COLLOCATED_SHIFT);
	if (slice->deblocking_disabled)
		word |= GEN12_VIDEO_HCP_SLICE_DEBLOCKING_OFF;
	if (slice->loop_filter_across_slices)
		word |= GEN12_VIDEO_HCP_SLICE_FILTER_SLICES;
	if (slice->sao_chroma)
		word |= GEN12_VIDEO_HCP_SLICE_SAO_CHROMA;
	if (slice->sao_luma)
		word |= GEN12_VIDEO_HCP_SLICE_SAO_LUMA;
	if (slice->mvd_l1_zero)
		word |= GEN12_VIDEO_HCP_SLICE_MVD_L1_ZERO;
	if (low_delay)
		word |= GEN12_VIDEO_HCP_SLICE_LOW_DELAY;
	if (slice->collocated_from_l0)
		word |= GEN12_VIDEO_HCP_SLICE_COLLOCATED_L0;
	if (slice->weighted) {
		word |= slice->weights.chroma_log2_denom << GEN12_VIDEO_HCP_SLICE_CHROMA_DENOM_SHIFT;
		word |= slice->weights.luma_log2_denom << GEN12_VIDEO_HCP_SLICE_LUMA_DENOM_SHIFT;
	}
	if (slice->cabac_init)
		word |= GEN12_VIDEO_HCP_SLICE_CABAC_INIT;
	drv_i915_batch_emit(batch, word);

	/* The header's length; the encoder's dwords. */
	drv_i915_batch_emit(batch, (uint32_t)slice->header_bytes & GEN12_VIDEO_HCP_SLICE_HEADER_MASK);
	i915_hcp_zeros(batch, 5U);

	/* The first CTB of the slice the segment belongs to; the screen content's offsets. */
	drv_i915_batch_emit(batch, (slice->slice_address % width_ctbs) | ((slice->slice_address / width_ctbs) << GEN12_VIDEO_HCP_SLICE_Y_SHIFT));
	drv_i915_batch_emit(batch, 0U);

	/* A predicted slice's lists and weights. */
	if (slice->slice_type != I915_VIDEO_H265_SLICE_I) {
		i915_hcp_lists(batch, decode, slice);
		if (slice->weighted)
			i915_hcp_weights(batch, slice);
	}

	/* The BSD object: the NAL unit from its header to the segment's end. */
	drv_i915_batch_emit(batch, GEN12_VIDEO_HEADER(GEN12_VIDEO_HCP_BSD_OBJECT, GEN12_VIDEO_HCP_BSD_OBJECT_DWORDS));
	drv_i915_batch_emit(batch, decode->slice_ends[index] - decode->slice_starts[index]);
	drv_i915_batch_emit(batch, (decode->skew + decode->slice_starts[index]) & GEN12_VIDEO_HCP_BSD_START_MASK);
}

/*
 * Writes a predicted slice's reference lists: L0, and L1 of a B slice.
 * Each entry is its picture's place, the difference of the order counts
 * clipped to eight bits, whether its weights were sent and whether it is
 * long-term.
 *
 * Procedure referenced: Mesa 25.0.7 src/intel/vulkan/genX_cmd_video.c
 * (anv_h265_decode_video, HCP_REF_IDX_STATE).
 */
static void
i915_hcp_lists(
	struct i915_gfx_batch *batch,
	const struct i915_video_hcp_decode *decode,
	const struct i915_video_h265_slice *slice)
{
	uint32_t lists;
	uint32_t list;
	uint32_t entry;
	uint32_t slot;
	uint32_t word;
	int64_t difference;

	/* One command for each list. */
	lists = 1U;
	if (slice->slice_type == I915_VIDEO_H265_SLICE_B)
		lists = 2U;
	for (list = 0U; list < lists; list++) {
		drv_i915_batch_emit(batch, GEN12_VIDEO_HEADER(GEN12_VIDEO_HCP_REF_IDX_STATE, GEN12_VIDEO_HCP_REF_IDX_STATE_DWORDS));
		drv_i915_batch_emit(batch, list | ((slice->references[list] - 1U) << GEN12_VIDEO_HCP_REF_ACTIVE_SHIFT));
		for (entry = 0U; entry < GEN12_VIDEO_HCP_REF_ENTRIES; entry++) {
			/* An entry past the list's is zero. */
			if (entry >= slice->references[list]) {
				drv_i915_batch_emit(batch, 0U);
				continue;
			}

			/* The entry's picture: its place, the clipped difference of the order counts and its marks. */
			slot = slice->list[list][entry];
			difference = (int64_t)decode->poc - decode->slots[slot].poc;
			if (difference < -128)
				difference = -128;
			if (difference > 127)
				difference = 127;
			word = ((uint32_t)difference & 0xffU) |
			    ((uint32_t)decode->place[slot] << GEN12_VIDEO_HCP_REF_ENTRY_SHIFT) |
			    GEN12_VIDEO_HCP_REF_TOP_FIELD;
			if (slice->weighted && slice->weights.chroma_sent[list][entry])
				word |= GEN12_VIDEO_HCP_REF_CHROMA_WEIGHTED;
			if (slice->weighted && slice->weights.luma_sent[list][entry])
				word |= GEN12_VIDEO_HCP_REF_LUMA_WEIGHTED;
			if (decode->slots[slot].long_term)
				word |= GEN12_VIDEO_HCP_REF_LONG_TERM;
			drv_i915_batch_emit(batch, word);
		}
	}
}

/*
 * Writes a weighted slice's weights: for L0, and L1 of a B slice, each
 * entry's luma weight's difference from the default and its offset, and
 * the same of Cb and Cr (ChromaOffsetLX).
 *
 * Procedure referenced: Mesa 25.0.7 src/intel/vulkan/genX_cmd_video.c
 * (anv_h265_decode_video, HCP_WEIGHTOFFSET_STATE).
 */
static void
i915_hcp_weights(
	struct i915_gfx_batch *batch,
	const struct i915_video_h265_slice *slice)
{
	const struct i915_video_h265_weights *weights;
	uint32_t lists;
	uint32_t list;
	uint32_t entry;
	uint32_t word;
	int32_t luma_default;
	int32_t chroma_default;

	weights = &slice->weights;
	luma_default = 1 << weights->luma_log2_denom;
	chroma_default = 1 << weights->chroma_log2_denom;
	lists = 1U;
	if (slice->slice_type == I915_VIDEO_H265_SLICE_B)
		lists = 2U;
	for (list = 0U; list < lists; list++) {
		/* The header and the list. */
		drv_i915_batch_emit(batch, GEN12_VIDEO_HEADER(GEN12_VIDEO_HCP_WEIGHTOFFSET_STATE, GEN12_VIDEO_HCP_WEIGHTOFFSET_STATE_DWORDS));
		drv_i915_batch_emit(batch, list);

		/* The luma entries. */
		for (entry = 0U; entry < GEN12_VIDEO_HCP_WEIGHT_ENTRIES; entry++) {
			word = 0U;
			if (entry < slice->references[list]) {
				word = ((uint32_t)(weights->luma_weight[list][entry] - luma_default) & 0xffU) |
				    (((uint32_t)weights->luma_offset[list][entry] & 0xffU) << 8);
			}
			drv_i915_batch_emit(batch, word);
		}

		/* The chroma entries. */
		for (entry = 0U; entry < GEN12_VIDEO_HCP_WEIGHT_ENTRIES; entry++) {
			word = 0U;
			if (entry < slice->references[list]) {
				word = ((uint32_t)(weights->chroma_weight[list][entry][0] - chroma_default) & 0xffU) |
				    (((uint32_t)weights->chroma_offset[list][entry][0] & 0xffU) << 8) |
				    (((uint32_t)(weights->chroma_weight[list][entry][1] - chroma_default) & 0xffU) << 16) |
				    (((uint32_t)weights->chroma_offset[list][entry][1] & 0xffU) << 24);
			}
			drv_i915_batch_emit(batch, word);
		}

		/* The offsets' high bytes, zero at eight bits. */
		i915_hcp_zeros(batch, GEN12_VIDEO_HCP_WEIGHT_HIGH_DWORDS);
	}
}

/* A signed value in a field of the mask's width, as two's complement. */
static uint32_t
i915_hcp_signed(
	int32_t value,
	uint32_t mask)
{
	/* The low bits of the two's complement. */
	return (uint32_t)value & mask;
}

/* Rounds a value up to a multiple of a power of two. */
static uint64_t
i915_hcp_align(
	uint64_t value,
	uint64_t alignment)
{
	/* Up to the next multiple. */
	return (value + alignment - 1U) & ~(alignment - 1U);
}
