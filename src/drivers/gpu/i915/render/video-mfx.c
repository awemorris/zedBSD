/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The MFX command builder of an H.264 decode (see video-mfx.h).
 *
 * One picture is one run of commands on the video engine: a cache flush and
 * the power well, the pipeline mode (AVC, VLD decode, short format), the
 * output surface, the buffers, the bitstream, the DPB, the picture state,
 * the quantizer matrices and the direct mode state, then one BSD object a
 * slice, which the hardware decodes from the slice header on.  Everything
 * indexed by a reference -- the reference picture addresses, the DPB
 * entries, the picture IDs, the motion vector buffers and the order counts
 * -- takes the reference's place in the decode's pReferenceSlots[] (the
 * same order anv uses).  An entry no reference uses names the output's own
 * picture and motion vectors rather than address zero, which nothing maps
 * (design D21).
 */

#include "video-mfx.h"
#include "batch.h"

#include "../intel/genxml-video.h"

#include <stddef.h>
#include <stdint.h>

static void i915_mfx_address(struct i915_gfx_batch *batch, uint64_t address);
static void i915_mfx_buffer(struct i915_gfx_batch *batch, uint64_t address, uint32_t mocs);
static void i915_mfx_zeros(struct i915_gfx_batch *batch, unsigned count);
static void i915_mfx_head(struct i915_gfx_batch *batch);
static void i915_mfx_surface(struct i915_gfx_batch *batch, const struct i915_video_mfx_decode *decode);
static void i915_mfx_buffers(struct i915_gfx_batch *batch, const struct i915_video_mfx_decode *decode);
static void i915_mfx_bitstream(struct i915_gfx_batch *batch, const struct i915_video_mfx_decode *decode);
static void i915_mfx_dpb(struct i915_gfx_batch *batch, const struct i915_video_mfx_decode *decode);
static void i915_mfx_picture(struct i915_gfx_batch *batch, const struct i915_video_mfx_decode *decode);
static void i915_mfx_matrices(struct i915_gfx_batch *batch, const struct i915_video_mfx_decode *decode);
static void i915_mfx_matrix(struct i915_gfx_batch *batch, uint32_t kind, const uint8_t *forward);
static void i915_mfx_direct(struct i915_gfx_batch *batch, const struct i915_video_mfx_decode *decode);
static void i915_mfx_slices(struct i915_gfx_batch *batch, const struct i915_video_mfx_decode *decode);
static void i915_mfx_derive(const struct i915_video_scaling *scaling, int fall_back_b, const struct i915_video_matrices *sequence, struct i915_video_matrices *matrices);
static void i915_mfx_copy(uint8_t *destination, const uint8_t *source, unsigned count);

/*
 * Derives the scaling matrices a picture decodes with from its sets.
 *
 * H.264 7.4.2.1.1 and 7.4.2.2 with Table 7-2: without a matrix in either
 * set every list is Flat_16; a sequence matrix gives the sequence's lists,
 * a list it does not send falling back by rule A; a picture matrix replaces
 * them, a list it does not send falling back by rule A when the sequence
 * has no matrix and by rule B (the sequence's list) when it has one.  A
 * list sent with useDefaultScalingMatrixFlag is the default list.  Only the
 * two 8x8 lists of 4:2:0 are derived.
 */
void
drv_i915_video_mfx_matrices(
	const struct i915_video_sps *sps,
	const struct i915_video_pps *pps,
	struct i915_video_matrices *matrices)
{
	struct i915_video_matrices sequence;
	struct i915_video_scaling none;
	const struct i915_video_scaling *lists;
	unsigned list;
	unsigned entry;
	int sequence_matrix;

	/* A set that says it has a matrix but sent no lists sends none of them. */
	none.present_mask = 0U;
	none.default_mask = 0U;

	/* Without a sequence matrix every sequence list is Flat_16. */
	sequence_matrix = 0;
	if ((sps->flags & I915_VIDEO_SPS_SCALING_MATRIX) != 0U)
		sequence_matrix = 1;
	for (list = 0U; list < 6U; list++) {
		for (entry = 0U; entry < 16U; entry++)
			sequence.list4[list][entry] = 16U;
	}
	for (list = 0U; list < 2U; list++) {
		for (entry = 0U; entry < 64U; entry++)
			sequence.list8[list][entry] = 16U;
	}

	/* The sequence's own lists, falling back by rule A. */
	if (sequence_matrix) {
		lists = &none;
		if (sps->has_scaling)
			lists = &sps->scaling;
		i915_mfx_derive(lists, 0, NULL, &sequence);
	}

	/* Without a picture matrix the picture decodes with the sequence's lists. */
	if ((pps->flags & I915_VIDEO_PPS_SCALING_MATRIX) == 0U) {
		*matrices = sequence;
		return;
	}

	/* The picture's own lists: rule B over a sequence matrix, rule A without one. */
	lists = &none;
	if (pps->has_scaling)
		lists = &pps->scaling;
	if (sequence_matrix)
		i915_mfx_derive(lists, 1, &sequence, matrices);
	else
		i915_mfx_derive(lists, 0, NULL, matrices);
}

/*
 * Checks the values of a picture's sequence and picture sets against what
 * the decoder takes (design §6.6, items 2 and 4, and the frame size of 3):
 * returns NULL, or why the picture is skipped.
 */
const char *
drv_i915_video_mfx_check_sets(
	const struct i915_video_sps *sps,
	const struct i915_video_pps *pps)
{
	uint32_t width;
	uint32_t height;

	/* 8-bit 4:2:0 frames only. */
	if (sps->chroma_format_idc != 1U)
		return "chroma format is not 4:2:0";
	if (sps->bit_depth_luma_minus8 != 0U || sps->bit_depth_chroma_minus8 != 0U)
		return "bit depth is not 8";
	if ((sps->flags & I915_VIDEO_SPS_FRAME_MBS_ONLY) == 0U)
		return "not frames only";

	/* The numbering the decoder can follow. */
	if (sps->pic_order_cnt_type > 2U)
		return "picture order count type past 2";
	if (sps->log2_max_frame_num_minus4 > 12U || sps->log2_max_pic_order_cnt_lsb_minus4 > 12U)
		return "frame or order count width past 16 bits";

	/* At most 36864 macroblocks (level 5.1's frame size, the 16-bit Frame Size field). */
	width = sps->pic_width_in_mbs_minus1 + 1U;
	height = sps->pic_height_in_map_units_minus1 + 1U;
	if (width > I915_VIDEO_MFX_MAX_SIDE_MBS || height > I915_VIDEO_MFX_MAX_SIDE_MBS)
		return "picture wider or taller than 4096";
	if (width * height > I915_VIDEO_MFX_MAX_FRAME_MBS)
		return "picture larger than 36864 macroblocks";

	/* The picture set's ranges. */
	if (pps->num_ref_idx_l0_default_active_minus1 > 31U || pps->num_ref_idx_l1_default_active_minus1 > 31U)
		return "reference index count past 32";
	if (pps->weighted_bipred_idc > 2U)
		return "weighted bi-prediction mode past 2";
	if (pps->pic_init_qp_minus26 < -26 || pps->pic_init_qp_minus26 > 25)
		return "initial quantizer out of range";
	if (pps->chroma_qp_index_offset < -12 || pps->chroma_qp_index_offset > 12 ||
	    pps->second_chroma_qp_index_offset < -12 || pps->second_chroma_qp_index_offset > 12)
		return "chroma quantizer offset out of range";

	/* Succeeded: the decoder takes the values. */
	return NULL;
}

/*
 * Writes the MFX commands that decode one picture into a batch.
 *
 * The batch keeps counting past its room, so a caller sees an overflow in
 * the batch rather than a picture cut off.
 */
void
drv_i915_video_mfx_build(
	struct i915_gfx_batch *batch,
	const struct i915_video_mfx_decode *decode)
{
	/* The flush, the power well and the pipeline's mode. */
	i915_mfx_head(batch);

	/* The output picture's surface. */
	i915_mfx_surface(batch, decode);

	/* The output, the row stores and the reference pictures. */
	i915_mfx_buffers(batch, decode);

	/* The bitstream and the two row stores of the bitstream decoder. */
	i915_mfx_bitstream(batch, decode);

	/* The DPB entries and the picture IDs of the references. */
	i915_mfx_dpb(batch, decode);

	/* The picture state from the sets and the picture. */
	i915_mfx_picture(batch, decode);

	/* The quantizer matrices. */
	i915_mfx_matrices(batch, decode);

	/* The motion vector buffers and the order counts. */
	i915_mfx_direct(batch, decode);

	/* One BSD object a slice. */
	i915_mfx_slices(batch, decode);

	/* Flushes what the decode wrote before the batch ends. */
	drv_i915_batch_emit(batch, GEN12_VIDEO_MI_FLUSH_DW);
	i915_mfx_zeros(batch, GEN12_VIDEO_MI_FLUSH_DW_DWORDS - 1U);
}

/* Writes a 64-bit GPU address as its low and its high dword. */
static void
i915_mfx_address(
	struct i915_gfx_batch *batch,
	uint64_t address)
{
	/* The low dword, then the high one. */
	drv_i915_batch_emit(batch, (uint32_t)address);
	drv_i915_batch_emit(batch, (uint32_t)(address >> 32));
}

/* Writes a buffer: its address and its memory attributes (the MOCS, nothing else). */
static void
i915_mfx_buffer(
	struct i915_gfx_batch *batch,
	uint64_t address,
	uint32_t mocs)
{
	/* The address, then the attributes dword. */
	i915_mfx_address(batch, address);
	drv_i915_batch_emit(batch, mocs & GEN12_VIDEO_ATTRIBUTES_MOCS_MASK);
}

/* Writes a run of zero dwords: fields the decode does not use. */
static void
i915_mfx_zeros(
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
 * MFX power well held awake, and the pipeline set to an AVC VLD decode in
 * the short format with the deblocked output, between two waits for the
 * pipeline to be idle (anv's Gen12 sequence).
 */
static void
i915_mfx_head(
	struct i915_gfx_batch *batch)
{
	/* Invalidates the video pipeline's caches. */
	drv_i915_batch_emit(batch, GEN12_VIDEO_MI_FLUSH_DW | GEN12_VIDEO_MI_FLUSH_DW_INVALIDATE);
	i915_mfx_zeros(batch, GEN12_VIDEO_MI_FLUSH_DW_DWORDS - 1U);

	/* Holds the MFX power well awake for the decode. */
	drv_i915_batch_emit(batch, GEN12_VIDEO_MI_FORCE_WAKEUP);
	drv_i915_batch_emit(batch, GEN12_VIDEO_FORCE_WAKEUP_MFX |
	    (GEN12_VIDEO_FORCE_WAKEUP_MASK << GEN12_VIDEO_FORCE_WAKEUP_MASK_SHIFT));

	/* Waits for the pipeline before its mode changes. */
	drv_i915_batch_emit(batch, GEN12_VIDEO_MFX_WAIT | GEN12_VIDEO_MFX_WAIT_SYNC);

	/* AVC, decode, VLD, the short format, the deblocked picture written out. */
	drv_i915_batch_emit(batch, GEN12_VIDEO_HEADER(GEN12_VIDEO_MFX_PIPE_MODE_SELECT, GEN12_VIDEO_MFX_PIPE_MODE_SELECT_DWORDS));
	drv_i915_batch_emit(batch, GEN12_VIDEO_MODE_STANDARD_AVC | GEN12_VIDEO_MODE_POST_DEBLOCKING);
	i915_mfx_zeros(batch, GEN12_VIDEO_MFX_PIPE_MODE_SELECT_DWORDS - 2U);

	/* Waits for the new mode to settle. */
	drv_i915_batch_emit(batch, GEN12_VIDEO_MFX_WAIT | GEN12_VIDEO_MFX_WAIT_SYNC);
}

/*
 * Writes the output picture's surface: NV12 in Y tiles, the CbCr plane
 * interleaved below the Y plane at its row.  The references are read
 * through the same surface, which is why they must be laid out as the
 * output is.
 */
static void
i915_mfx_surface(
	struct i915_gfx_batch *batch,
	const struct i915_video_mfx_decode *decode)
{
	uint32_t extent;
	uint32_t layout;

	/* The extent, less one each way. */
	extent = ((decode->width - 1U) << GEN12_VIDEO_SURFACE_WIDTH_SHIFT) |
	    ((decode->height - 1U) << GEN12_VIDEO_SURFACE_HEIGHT_SHIFT);

	/* Y-tiled, the pitch less one, the chroma interleaved, PLANAR_420_8. */
	layout = GEN12_VIDEO_SURFACE_WALK_YMAJOR |
	    GEN12_VIDEO_SURFACE_TILED |
	    ((decode->pitch - 1U) << GEN12_VIDEO_SURFACE_PITCH_SHIFT) |
	    GEN12_VIDEO_SURFACE_INTERLEAVE_CHROMA |
	    (GEN12_VIDEO_SURFACE_PLANAR_420_8 << GEN12_VIDEO_SURFACE_FORMAT_SHIFT);

	/* The surface of ID 0; both chroma offsets are the CbCr plane's row (anv writes both). */
	drv_i915_batch_emit(batch, GEN12_VIDEO_HEADER(GEN12_VIDEO_MFX_SURFACE_STATE, GEN12_VIDEO_MFX_SURFACE_STATE_DWORDS));
	drv_i915_batch_emit(batch, 0U);
	drv_i915_batch_emit(batch, extent);
	drv_i915_batch_emit(batch, layout);
	drv_i915_batch_emit(batch, decode->chroma_rows);
	drv_i915_batch_emit(batch, decode->chroma_rows);
}

/*
 * Writes the pipeline's buffers: the deblocked output, the intra and the
 * deblocking filter row stores, and the sixteen reference pictures (an
 * unused one names the output).  The buffers a decode does not use have
 * address zero and still carry the MOCS, as anv writes them.
 */
static void
i915_mfx_buffers(
	struct i915_gfx_batch *batch,
	const struct i915_video_mfx_decode *decode)
{
	uint64_t address;
	unsigned index;

	/* The header; the output before deblocking, the deblocked output, the encoder's source and stream-out. */
	drv_i915_batch_emit(batch, GEN12_VIDEO_HEADER(GEN12_VIDEO_MFX_PIPE_BUF_ADDR_STATE, GEN12_VIDEO_MFX_PIPE_BUF_ADDR_STATE_DWORDS));
	i915_mfx_buffer(batch, 0U, decode->mocs);
	i915_mfx_buffer(batch, decode->destination, decode->mocs);
	i915_mfx_buffer(batch, 0U, decode->mocs);
	i915_mfx_buffer(batch, 0U, decode->mocs);

	/* The intra and the deblocking filter row stores (the session's first two bindings). */
	i915_mfx_buffer(batch, decode->row_stores[0], decode->mocs);
	i915_mfx_buffer(batch, decode->row_stores[1], decode->mocs);

	/* The reference pictures in the decode's order, then the output for every unused entry. */
	for (index = 0U; index < I915_VIDEO_MFX_REFERENCES; index++) {
		/* A reference's picture, or the output's own. */
		address = decode->destination;
		if (index < decode->reference_count)
			address = decode->references[index].picture;
		i915_mfx_address(batch, address);
	}
	drv_i915_batch_emit(batch, decode->mocs & GEN12_VIDEO_ATTRIBUTES_MOCS_MASK);

	/* The status and the stream-out buffers the decode does not write. */
	i915_mfx_buffer(batch, 0U, decode->mocs);
	i915_mfx_buffer(batch, 0U, decode->mocs);
	i915_mfx_buffer(batch, 0U, decode->mocs);

	/* No reference picture is compressed. */
	drv_i915_batch_emit(batch, 0U);

	/* No scaled reference surface. */
	i915_mfx_buffer(batch, 0U, decode->mocs);
}

/*
 * Writes the bitstream: the indirect bitstream object starts at the page of
 * the decode's range and is bounded by the end of the buffer's bound range,
 * so a slice can be read only inside the buffer.  The other indirect
 * objects are the encoder's.  Then the BSD/MPC and the MPR row stores (the
 * session's third and fourth bindings).
 */
static void
i915_mfx_bitstream(
	struct i915_gfx_batch *batch,
	const struct i915_video_mfx_decode *decode)
{
	unsigned index;

	/* The bitstream object: its address, its attributes and its upper bound. */
	drv_i915_batch_emit(batch, GEN12_VIDEO_HEADER(GEN12_VIDEO_MFX_IND_OBJ_BASE_ADDR_STATE, GEN12_VIDEO_MFX_IND_OBJ_BASE_ADDR_STATE_DWORDS));
	i915_mfx_buffer(batch, decode->bitstream_base, decode->mocs);
	i915_mfx_address(batch, decode->bitstream_end);

	/* The motion vector, coefficient, deblocking and PAK objects the decode does not use. */
	for (index = 1U; index < GEN12_VIDEO_IND_OBJECTS; index++) {
		i915_mfx_buffer(batch, 0U, decode->mocs);
		i915_mfx_address(batch, 0U);
	}

	/* The bitstream decoder's row stores; no bitplane buffer (VC-1's). */
	drv_i915_batch_emit(batch, GEN12_VIDEO_HEADER(GEN12_VIDEO_MFX_BSP_BUF_BASE_ADDR_STATE, GEN12_VIDEO_MFX_BSP_BUF_BASE_ADDR_STATE_DWORDS));
	i915_mfx_buffer(batch, decode->row_stores[2], decode->mocs);
	i915_mfx_buffer(batch, decode->row_stores[3], decode->mocs);
	i915_mfx_buffer(batch, 0U, decode->mocs);
}

/*
 * Writes the DPB entries and the picture IDs of the references: each
 * reference's non-existing and long-term marks, its use as a frame or a
 * field, its FrameNum, and its DPB slot as its picture ID; an unused
 * picture ID is 0xffff.
 */
static void
i915_mfx_dpb(
	struct i915_gfx_batch *batch,
	const struct i915_video_mfx_decode *decode)
{
	const struct i915_video_mfx_reference *reference;
	uint32_t marks;
	uint32_t uses;
	uint32_t use;
	uint32_t numbers[I915_VIDEO_MFX_REFERENCES];
	uint32_t ids[I915_VIDEO_MFX_REFERENCES];
	unsigned index;

	/* Gathers each reference's marks, use, number and ID; the rest stay unused. */
	marks = 0U;
	uses = 0U;
	for (index = 0U; index < I915_VIDEO_MFX_REFERENCES; index++) {
		numbers[index] = 0U;
		ids[index] = GEN12_VIDEO_PICID_UNUSED;
	}
	for (index = 0U; index < decode->reference_count; index++) {
		reference = &decode->references[index];

		/* A picture the decoder has to infer, and a long-term one. */
		if ((reference->flags & I915_VIDEO_REFERENCE_NON_EXISTING) != 0U)
			marks |= 1U << index;
		if ((reference->flags & I915_VIDEO_REFERENCE_LONG_TERM) != 0U)
			marks |= 1U << (GEN12_VIDEO_DPB_LONG_TERM_SHIFT + index);

		/* Used as the field or fields it names, or as a frame when it names neither. */
		use = 0U;
		if ((reference->flags & I915_VIDEO_REFERENCE_TOP_FIELD) != 0U)
			use |= 1U;
		if ((reference->flags & I915_VIDEO_REFERENCE_BOTTOM_FIELD) != 0U)
			use |= 2U;
		if (use == 0U)
			use = GEN12_VIDEO_DPB_FRAME;
		uses |= use << (2U * index);

		/* Its FrameNum (or LongTermFrameIdx), and its DPB slot as its picture ID. */
		numbers[index] = reference->frame_num & 0xffffU;
		ids[index] = (uint32_t)reference->slot_index & 0xffffU;
	}

	/* MFD_AVC_DPB_STATE: the marks, the uses, the numbers two to a dword, no MVC views. */
	drv_i915_batch_emit(batch, GEN12_VIDEO_HEADER(GEN12_VIDEO_MFD_AVC_DPB_STATE, GEN12_VIDEO_MFD_AVC_DPB_STATE_DWORDS));
	drv_i915_batch_emit(batch, marks);
	drv_i915_batch_emit(batch, uses);
	for (index = 0U; index < I915_VIDEO_MFX_REFERENCES; index += 2U)
		drv_i915_batch_emit(batch, numbers[index] | (numbers[index + 1U] << 16));
	i915_mfx_zeros(batch, GEN12_VIDEO_MFD_AVC_DPB_STATE_DWORDS - GEN12_VIDEO_DPB_FRAME_NUMBERS - I915_VIDEO_MFX_REFERENCES / 2U);

	/* MFD_AVC_PICID_STATE: the 16-bit IDs, two to a dword. */
	drv_i915_batch_emit(batch, GEN12_VIDEO_HEADER(GEN12_VIDEO_MFD_AVC_PICID_STATE, GEN12_VIDEO_MFD_AVC_PICID_STATE_DWORDS));
	drv_i915_batch_emit(batch, 0U);
	for (index = 0U; index < I915_VIDEO_MFX_REFERENCES; index += 2U)
		drv_i915_batch_emit(batch, ids[index] | (ids[index + 1U] << 16));
}

/*
 * Writes the picture state: the picture's extent and size in macroblocks,
 * and the fields of its sets and of its picture information the slices
 * need (anv's mapping).  Only frame pictures of sequences of frames only
 * reach the builder (design §6.6, item 2), so the picture is a frame and
 * MBAFF is off.
 */
static void
i915_mfx_picture(
	struct i915_gfx_batch *batch,
	const struct i915_video_mfx_decode *decode)
{
	const struct i915_video_sps *sps;
	const struct i915_video_pps *pps;
	uint32_t width;
	uint32_t height;
	uint32_t weights;
	uint32_t coding;
	uint32_t references;
	uint32_t order;

	/* The picture's sets. */
	sps = decode->sps;
	pps = decode->pps;

	/* The extent in macroblocks: a sequence of frames only (the checks before) has a map unit a macroblock. */
	width = sps->pic_width_in_mbs_minus1 + 1U;
	height = sps->pic_height_in_map_units_minus1 + 1U;

	/* A frame picture; the weighted prediction and the two chroma QP offsets (5-bit signed). */
	weights = (pps->weighted_bipred_idc << GEN12_VIDEO_IMG_WEIGHTED_BIPRED_SHIFT) |
	    (((uint32_t)pps->chroma_qp_index_offset & GEN12_VIDEO_IMG_QP_OFFSET_MASK) << GEN12_VIDEO_IMG_CHROMA_OFFSET_SHIFT) |
	    (((uint32_t)pps->second_chroma_qp_index_offset & GEN12_VIDEO_IMG_QP_OFFSET_MASK) << GEN12_VIDEO_IMG_SECOND_CHROMA_SHIFT);
	if ((pps->flags & I915_VIDEO_PPS_WEIGHTED_PRED) != 0U)
		weights |= GEN12_VIDEO_IMG_WEIGHTED_PRED;

	/* The coding tools the sets turn on, and whether the picture is not a reference. */
	coding = sps->chroma_format_idc << GEN12_VIDEO_IMG_CHROMA_FORMAT_SHIFT;
	if ((sps->flags & I915_VIDEO_SPS_FRAME_MBS_ONLY) != 0U)
		coding |= GEN12_VIDEO_IMG_FRAME_MB_ONLY;
	if ((pps->flags & I915_VIDEO_PPS_TRANSFORM_8X8) != 0U)
		coding |= GEN12_VIDEO_IMG_TRANSFORM_8X8;
	if ((sps->flags & I915_VIDEO_SPS_DIRECT_8X8_INFERENCE) != 0U)
		coding |= GEN12_VIDEO_IMG_DIRECT_8X8;
	if ((pps->flags & I915_VIDEO_PPS_CONSTRAINED_INTRA) != 0U)
		coding |= GEN12_VIDEO_IMG_CONSTRAINED_INTRA;
	if ((decode->picture_flags & I915_VIDEO_PICTURE_IS_REFERENCE) == 0U)
		coding |= GEN12_VIDEO_IMG_NON_REFERENCE;
	if ((pps->flags & I915_VIDEO_PPS_CABAC) != 0U)
		coding |= GEN12_VIDEO_IMG_CABAC;

	/* The initial QP (8-bit signed), the default active references of each list, and the references. */
	references = ((uint32_t)pps->pic_init_qp_minus26 & GEN12_VIDEO_IMG_QP_MASK) |
	    ((pps->num_ref_idx_l0_default_active_minus1 + 1U) << GEN12_VIDEO_IMG_L0_SHIFT) |
	    ((pps->num_ref_idx_l1_default_active_minus1 + 1U) << GEN12_VIDEO_IMG_L1_SHIFT) |
	    (decode->reference_count << GEN12_VIDEO_IMG_REFERENCES_SHIFT);

	/* The order count and the slice header's syntax the sets decide. */
	order = (sps->pic_order_cnt_type << GEN12_VIDEO_IMG_POC_TYPE_SHIFT) |
	    (sps->log2_max_frame_num_minus4 << GEN12_VIDEO_IMG_LOG2_FRAME_NUM_SHIFT) |
	    (sps->log2_max_pic_order_cnt_lsb_minus4 << GEN12_VIDEO_IMG_LOG2_POC_LSB_SHIFT);
	if ((pps->flags & I915_VIDEO_PPS_BOTTOM_FIELD_POC) != 0U)
		order |= GEN12_VIDEO_IMG_PIC_ORDER_PRESENT;
	if ((sps->flags & I915_VIDEO_SPS_DELTA_ALWAYS_ZERO) != 0U)
		order |= GEN12_VIDEO_IMG_DELTA_ALWAYS_ZERO;
	if ((pps->flags & I915_VIDEO_PPS_REDUNDANT_PIC_COUNT) != 0U)
		order |= GEN12_VIDEO_IMG_REDUNDANT_PIC_COUNT;
	if ((pps->flags & I915_VIDEO_PPS_DEBLOCKING_CONTROL) != 0U)
		order |= GEN12_VIDEO_IMG_DEBLOCKING_CONTROL;

	/* MFX_AVC_IMG_STATE: dwords 1 to 4, the encoder's 5 to 12, 13 to 15, the rest zero. */
	drv_i915_batch_emit(batch, GEN12_VIDEO_HEADER(GEN12_VIDEO_MFX_AVC_IMG_STATE, GEN12_VIDEO_MFX_AVC_IMG_STATE_DWORDS));
	drv_i915_batch_emit(batch, (width * height) & 0xffffU);
	drv_i915_batch_emit(batch, (width - 1U) | ((height - 1U) << GEN12_VIDEO_IMG_HEIGHT_SHIFT));
	drv_i915_batch_emit(batch, weights);
	drv_i915_batch_emit(batch, coding);
	drv_i915_batch_emit(batch, GEN12_VIDEO_IMG_TRELLIS_CHROMA_DISABLE);
	i915_mfx_zeros(batch, 7U);
	drv_i915_batch_emit(batch, references);
	drv_i915_batch_emit(batch, order);
	drv_i915_batch_emit(batch, (decode->frame_num & 0xffffU) << GEN12_VIDEO_IMG_FRAME_NUM_SHIFT);
	i915_mfx_zeros(batch, GEN12_VIDEO_MFX_AVC_IMG_STATE_DWORDS - 16U);
}

/*
 * Writes the quantizer matrices: the 4x4 intra and inter ones always, the
 * 8x8 ones when the picture uses the 8x8 transform.  The lists are in scan
 * order and the hardware takes raster order (design B2).
 */
static void
i915_mfx_matrices(
	struct i915_gfx_batch *batch,
	const struct i915_video_mfx_decode *decode)
{
	struct i915_video_matrices matrices;
	uint8_t forward[GEN12_VIDEO_QM_BYTES];
	unsigned kind;
	unsigned list;
	unsigned scan;

	/* The matrices of the picture's sets. */
	drv_i915_video_mfx_matrices(decode->sps, decode->pps, &matrices);

	/* The two 4x4 matrices: the Y, Cb and Cr lists of the intra ones, then of the inter ones. */
	for (kind = 0U; kind < 2U; kind++) {
		for (scan = 0U; scan < GEN12_VIDEO_QM_BYTES; scan++)
			forward[scan] = 0U;
		for (list = 0U; list < 3U; list++) {
			for (scan = 0U; scan < 16U; scan++)
				forward[list * 16U + drv_i915_video_zigzag4[scan]] = matrices.list4[kind * 3U + list][scan];
		}
		i915_mfx_matrix(batch, GEN12_VIDEO_QM_4X4_INTRA + kind, forward);
	}

	/* Without the 8x8 transform there is nothing more. */
	if ((decode->pps->flags & I915_VIDEO_PPS_TRANSFORM_8X8) == 0U)
		return;

	/* The two 8x8 matrices of luma: intra, then inter. */
	for (kind = 0U; kind < 2U; kind++) {
		for (scan = 0U; scan < 64U; scan++)
			forward[drv_i915_video_zigzag8[scan]] = matrices.list8[kind][scan];
		i915_mfx_matrix(batch, GEN12_VIDEO_QM_8X8_INTRA + kind, forward);
	}
}

/* Writes one MFX_QM_STATE: the matrix's kind and its 64 bytes, four to a dword from the low byte. */
static void
i915_mfx_matrix(
	struct i915_gfx_batch *batch,
	uint32_t kind,
	const uint8_t *forward)
{
	unsigned index;

	/* The header and the kind. */
	drv_i915_batch_emit(batch, GEN12_VIDEO_HEADER(GEN12_VIDEO_MFX_QM_STATE, GEN12_VIDEO_MFX_QM_STATE_DWORDS));
	drv_i915_batch_emit(batch, kind);

	/* The bytes. */
	for (index = 0U; index < GEN12_VIDEO_QM_BYTES; index += 4U) {
		drv_i915_batch_emit(batch,
				    (uint32_t)forward[index] |
				    ((uint32_t)forward[index + 1U] << 8) |
				    ((uint32_t)forward[index + 2U] << 16) |
				    ((uint32_t)forward[index + 3U] << 24));
	}
}

/*
 * Writes the direct mode state: each reference's slot's motion vectors (an
 * unused entry names the buffer this picture writes), the buffer this
 * picture writes, and the top and bottom order counts of the references
 * and of this picture.
 */
static void
i915_mfx_direct(
	struct i915_gfx_batch *batch,
	const struct i915_video_mfx_decode *decode)
{
	uint64_t address;
	uint32_t top;
	uint32_t bottom;
	unsigned index;

	/* The header and the references' motion vectors, or the written buffer for an unused entry. */
	drv_i915_batch_emit(batch, GEN12_VIDEO_HEADER(GEN12_VIDEO_MFX_AVC_DIRECTMODE_STATE, GEN12_VIDEO_MFX_AVC_DIRECTMODE_STATE_DWORDS));
	for (index = 0U; index < I915_VIDEO_MFX_REFERENCES; index++) {
		/* A reference's buffer, or this picture's. */
		address = decode->motion_write;
		if (index < decode->reference_count)
			address = decode->references[index].motion;
		i915_mfx_address(batch, address);
	}
	drv_i915_batch_emit(batch, decode->mocs & GEN12_VIDEO_ATTRIBUTES_MOCS_MASK);

	/* The buffer this picture writes. */
	i915_mfx_buffer(batch, decode->motion_write, decode->mocs);

	/* The references' order counts, top then bottom, zero for an unused entry. */
	for (index = 0U; index < I915_VIDEO_MFX_REFERENCES; index++) {
		/* A reference's counts. */
		top = 0U;
		bottom = 0U;
		if (index < decode->reference_count) {
			top = (uint32_t)decode->references[index].poc[0];
			bottom = (uint32_t)decode->references[index].poc[1];
		}
		drv_i915_batch_emit(batch, top);
		drv_i915_batch_emit(batch, bottom);
	}

	/* This picture's counts. */
	drv_i915_batch_emit(batch, (uint32_t)decode->poc[0]);
	drv_i915_batch_emit(batch, (uint32_t)decode->poc[1]);
}

/*
 * Writes the slices: before each slice's BSD object but the last, the next
 * slice's address, which the decoder reads ahead; a slice starts at its NAL
 * unit header (after its start code), at an offset from the bitstream
 * object's page, and runs to the next slice's start code or the range's
 * end.  The concealment settings are anv's.
 */
static void
i915_mfx_slices(
	struct i915_gfx_batch *batch,
	const struct i915_video_mfx_decode *decode)
{
	uint32_t last;
	uint32_t next;
	unsigned slice;

	/* One slice after the other. */
	for (slice = 0U; slice < decode->slice_count; slice++) {
		/* The next slice's address, ahead of this one. */
		if (slice + 1U < decode->slice_count) {
			next = slice + 1U;
			drv_i915_batch_emit(batch, GEN12_VIDEO_HEADER(GEN12_VIDEO_MFD_AVC_SLICEADDR, GEN12_VIDEO_MFD_AVC_SLICEADDR_DWORDS));
			drv_i915_batch_emit(batch, decode->slice_ends[next] - decode->slice_starts[next]);
			drv_i915_batch_emit(batch, (decode->skew + decode->slice_starts[next]) & GEN12_VIDEO_BSD_START_MASK);
			drv_i915_batch_emit(batch, 0U);
		}

		/* Whether this is the picture's last slice. */
		last = 0U;
		if (slice + 1U == decode->slice_count)
			last = GEN12_VIDEO_BSD_LAST_SLICE;

		/* The slice's BSD object: its length, its start and the inline data. */
		drv_i915_batch_emit(batch, GEN12_VIDEO_HEADER(GEN12_VIDEO_MFD_AVC_BSD_OBJECT, GEN12_VIDEO_MFD_AVC_BSD_OBJECT_DWORDS));
		drv_i915_batch_emit(batch, decode->slice_ends[slice] - decode->slice_starts[slice]);
		drv_i915_batch_emit(batch, (decode->skew + decode->slice_starts[slice]) & GEN12_VIDEO_BSD_START_MASK);
		drv_i915_batch_emit(batch, 0U);
		drv_i915_batch_emit(batch, last | GEN12_VIDEO_BSD_FIX_PREV_MB_SKIPPED);
		drv_i915_batch_emit(batch, GEN12_VIDEO_BSD_INTRA_ERROR_CONTROL |
		    GEN12_VIDEO_BSD_INTRA_CONCEALMENT |
		    GEN12_VIDEO_BSD_I_SLICE_CONCEALMENT);
		drv_i915_batch_emit(batch, 0U);
	}
}

/*
 * Derives the eight lists of one set: a list sent is the set's own, or the
 * default one when the set says to use it; a list not sent falls back by
 * rule A (the default list for the first intra and the first inter list,
 * the list before it otherwise) or, with `fall_back_b`, by rule B (the
 * sequence's list for the first ones).
 */
static void
i915_mfx_derive(
	const struct i915_video_scaling *scaling,
	int fall_back_b,
	const struct i915_video_matrices *sequence,
	struct i915_video_matrices *matrices)
{
	const uint8_t *source;
	uint32_t bit;
	unsigned list;

	/* The six 4x4 lists: 0 to 2 intra, 3 to 5 inter. */
	for (list = 0U; list < 6U; list++) {
		bit = 1U << list;

		/* Picks where the list comes from. */
		if ((scaling->present_mask & bit) != 0U && (scaling->default_mask & bit) != 0U) {
			/* Sent, asking for the default list. */
			source = drv_i915_video_default4_intra;
			if (list >= 3U)
				source = drv_i915_video_default4_inter;
		} else if ((scaling->present_mask & bit) != 0U) {
			/* Sent. */
			source = &scaling->list4[list * 16U];
		} else if (list == 0U || list == 3U) {
			/* The first of its kind, not sent: rule B's sequence list, or rule A's default. */
			if (fall_back_b) {
				source = sequence->list4[list];
			} else if (list == 0U) {
				source = drv_i915_video_default4_intra;
			} else {
				source = drv_i915_video_default4_inter;
			}
		} else {
			/* Another one not sent: the list before it. */
			source = matrices->list4[list - 1U];
		}
		i915_mfx_copy(matrices->list4[list], source, 16U);
	}

	/* The two 8x8 lists of 4:2:0: intra (list 6), then inter (list 7). */
	for (list = 0U; list < 2U; list++) {
		bit = 1U << (6U + list);

		/* Picks where the list comes from. */
		if ((scaling->present_mask & bit) != 0U && (scaling->default_mask & bit) != 0U) {
			/* Sent, asking for the default list. */
			source = drv_i915_video_default8_intra;
			if (list == 1U)
				source = drv_i915_video_default8_inter;
		} else if ((scaling->present_mask & bit) != 0U) {
			/* Sent. */
			source = &scaling->list8[list * 64U];
		} else if (fall_back_b) {
			/* Not sent, over a sequence matrix: the sequence's list. */
			source = sequence->list8[list];
		} else if (list == 0U) {
			/* Not sent: the default intra list. */
			source = drv_i915_video_default8_intra;
		} else {
			/* Not sent: the default inter list. */
			source = drv_i915_video_default8_inter;
		}
		i915_mfx_copy(matrices->list8[list], source, 64U);
	}
}

/* Copies a list's bytes. */
static void
i915_mfx_copy(
	uint8_t *destination,
	const uint8_t *source,
	unsigned count)
{
	unsigned index;

	/* Byte by byte: the lists are short. */
	for (index = 0U; index < count; index++)
		destination[index] = source[index];
}
