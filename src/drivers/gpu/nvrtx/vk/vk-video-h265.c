/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The executor's H.265 arithmetic, which touches no hardware (a host test
 * runs it): the picture description of a decode from its parameter sets,
 * the scaling lists and tiles it decodes with, its references by surface
 * (the DPB slot is the surface), and the bits of its first slice segment
 * header that the decoder is told to skip.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <uapi/errno.h>

#include "drivers/gpu/nvrtx/vk/nvrtx-video.h"

/* The coefficient of a flat list and of a default DC value. */
#define H265_FLAT			16U

/* The NAL unit types: the first of the IRAP pictures, the last of the IRAP range, and the two IDR pictures. */
#define H265_NAL_BLA_W_LP		16U
#define H265_NAL_IRAP_LAST		23U
#define H265_NAL_IDR_W_RADL		19U
#define H265_NAL_IDR_N_LP		20U

/* The header bytes the skip is counted from at most, after the emulation prevention is taken out. */
#define H265_HEADER_BYTES		512U

/* The longest exp-Golomb code read (leading zeros). */
#define H265_GOLOMB_ZEROS		31U

/* A reference order difference's range in the picture structure. */
#define H265_ORDER_MIN			(-128)
#define H265_ORDER_MAX			127

/*
 * The bits of a slice segment header as the parse reads them: the bytes
 * (emulation prevention taken out), how many, the next bit, and whether a
 * read went past the end.
 */
struct h265_bits {
	const uint8_t *bytes;
	size_t length;
	size_t at;
	bool error;
};

static void h265_raster(uint32_t side, uint8_t *positions);
static void h265_list(const uint8_t *coded, const uint8_t *positions, uint32_t count, uint8_t *raster);
static uint32_t h265_ctbs(uint32_t samples, uint32_t log2_ctb);
static void h265_spacing(uint32_t total, uint32_t parts, bool uniform, const uint32_t *minus1, uint32_t *sizes);
static int h265_reference_find(const struct nvrtx_video_h265_reference *references, uint32_t count, uint32_t slot);
static uint32_t h265_set_fill(uint8_t *list, uint32_t at, const uint8_t *set, uint32_t entries);
static uint32_t h265_set_count(const uint8_t *set);
static int32_t h265_order(int32_t difference);
static size_t h265_unescape(const uint8_t *bytes, size_t length, uint8_t *out, size_t room);
static uint32_t h265_read(struct h265_bits *bits, uint32_t count);
static uint32_t h265_golomb(struct h265_bits *bits);
static uint32_t h265_ceil_log2(uint32_t value);

/*
 * The default 8x8 scaling lists (used for the 8x8, 16x16 and 32x32
 * sizes), in the coded (up-right diagonal) order.
 *
 * Values referenced: Recommendation ITU-T H.265 (Table 7-6, the default
 * values of ScalingList[ 1..3 ][ matrixId ][ i ]: matrixId 0 to 2 intra,
 * 3 to 5 inter).
 */
static const uint8_t h265_default_intra[64] = {
	16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 17, 16, 17, 16, 17, 18,
	17, 18, 18, 17, 18, 21, 19, 20, 21, 20, 19, 21, 24, 22, 22, 24,
	24, 22, 22, 24, 25, 25, 27, 30, 27, 25, 25, 29, 31, 35, 35, 31,
	29, 36, 41, 44, 41, 36, 47, 54, 54, 47, 65, 70, 65, 88, 88, 115
};

/* The default inter list, read the same way. */
static const uint8_t h265_default_inter[64] = {
	16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 17, 17, 17, 17, 17, 18,
	18, 18, 18, 18, 18, 20, 20, 20, 20, 20, 20, 20, 24, 24, 24, 24,
	24, 24, 24, 24, 25, 25, 25, 25, 25, 25, 25, 28, 28, 28, 28, 28,
	28, 33, 33, 33, 33, 33, 41, 41, 41, 41, 54, 54, 54, 71, 71, 91
};

/*
 * Fills a picture's description from its sets and its own information:
 * the sizes as the decoder counts them, the tools, the picture parameter
 * set's fields, the picture's kind, and the counts the decoder would parse
 * the slice header's reference sets with.  The references, the skip and
 * the buffers are the caller's.
 *
 * Procedure referenced: averne's FFmpeg nvtegra
 * libavcodec/nvtegra_hevc.c (nvtegra_hevc_prepare_frame_setup: the sizes
 * as log2 values not minus their bases, the reference counts not minus 1,
 * init_qp with its 26, the deblocking offsets twice their halves, no tile
 * counts without tiles).
 */
void
nvrtx_video_h265_describe(
	const struct nvrtx_video_h265_sps *sps,
	const struct nvrtx_video_h265_pps *pps,
	const struct nvrtx_video_h265_picture *picture,
	struct nvrtx_nvdec_hevc_picture *description)
{
	uint32_t tiles;
	uint32_t pcm;

	/* The picture's size, sampling and depths. */
	description->width = sps->width;
	description->height = sps->height;
	description->chroma_format_idc = sps->chroma_format_idc;
	description->bit_depth_luma = sps->bit_depth_luma_minus8 + 8U;
	description->bit_depth_chroma = sps->bit_depth_chroma_minus8 + 8U;

	/* The coding and transform block sizes, as log2 values. */
	description->log2_min_cb = sps->log2_min_cb_minus3 + 3U;
	description->log2_max_cb = description->log2_min_cb + sps->log2_diff_cb;
	description->log2_min_tb = sps->log2_min_tb_minus2 + 2U;
	description->log2_max_tb = description->log2_min_tb + sps->log2_diff_tb;
	description->depth_inter = sps->depth_inter;
	description->depth_intra = sps->depth_intra;

	/* The sequence's tools. */
	description->scaling_enabled = (sps->flags & NVRTX_VIDEO_H265_SPS_SCALING) != 0U;
	description->amp = (sps->flags & NVRTX_VIDEO_H265_SPS_AMP) != 0U;
	description->sao = (sps->flags & NVRTX_VIDEO_H265_SPS_SAO) != 0U;
	description->temporal_mvp = (sps->flags & NVRTX_VIDEO_H265_SPS_TEMPORAL_MVP) != 0U;
	description->strong_intra_smoothing = (sps->flags & NVRTX_VIDEO_H265_SPS_STRONG_INTRA) != 0U;

	/* The PCM samples' depths and block sizes, all zero without PCM. */
	pcm = (sps->flags & NVRTX_VIDEO_H265_SPS_PCM) != 0U;
	description->pcm = pcm;
	if (pcm != 0U) {
		description->pcm_depth_luma = sps->pcm_depth_luma_minus1 + 1U;
		description->pcm_depth_chroma = sps->pcm_depth_chroma_minus1 + 1U;
		description->log2_min_pcm = sps->log2_min_pcm_minus3 + 3U;
		description->log2_max_pcm = description->log2_min_pcm + sps->log2_diff_pcm;
		description->pcm_loop_filter_disabled = (sps->flags & NVRTX_VIDEO_H265_SPS_PCM_FILTER_OFF) != 0U;
	}

	/* The picture parameter set: the slice header's shape. */
	description->dependent_slices = (pps->flags & NVRTX_VIDEO_H265_PPS_DEPENDENT) != 0U;
	description->output_flag_present = (pps->flags & NVRTX_VIDEO_H265_PPS_OUTPUT_FLAG) != 0U;
	description->extra_header_bits = pps->extra_bits;
	description->sign_data_hiding = (pps->flags & NVRTX_VIDEO_H265_PPS_SIGN_HIDING) != 0U;
	description->cabac_init_present = (pps->flags & NVRTX_VIDEO_H265_PPS_CABAC_INIT) != 0U;

	/* The default reference counts, the quantizer and its deltas. */
	description->ref_l0_default = pps->l0_minus1 + 1U;
	description->ref_l1_default = pps->l1_minus1 + 1U;
	description->init_qp = (uint32_t)(pps->init_qp_minus26 + 26 + 6 * (int32_t)sps->bit_depth_luma_minus8);
	description->constrained_intra = (pps->flags & NVRTX_VIDEO_H265_PPS_CONSTRAINED) != 0U;
	description->transform_skip = (pps->flags & NVRTX_VIDEO_H265_PPS_TRANSFORM_SKIP) != 0U;
	description->cu_qp_delta = (pps->flags & NVRTX_VIDEO_H265_PPS_CU_QP_DELTA) != 0U;
	description->cu_qp_delta_depth = pps->cu_qp_delta_depth;

	/* The chroma quantizer offsets, and the deblocking offsets twice the halves the syntax codes. */
	description->cb_qp_offset = pps->cb_qp_offset;
	description->cr_qp_offset = pps->cr_qp_offset;
	description->beta_offset = pps->beta_div2 * 2;
	description->tc_offset = pps->tc_div2 * 2;

	/* The predictions and the bypass. */
	description->slice_chroma_qp_offsets = (pps->flags & NVRTX_VIDEO_H265_PPS_SLICE_QP) != 0U;
	description->weighted_pred = (pps->flags & NVRTX_VIDEO_H265_PPS_WEIGHTED) != 0U;
	description->weighted_bipred = (pps->flags & NVRTX_VIDEO_H265_PPS_BIPRED) != 0U;
	description->transquant_bypass = (pps->flags & NVRTX_VIDEO_H265_PPS_BYPASS) != 0U;
	description->entropy_sync = (pps->flags & NVRTX_VIDEO_H265_PPS_ENTROPY_SYNC) != 0U;

	/* The tiles: their counts and the filter across them only with tiles. */
	tiles = (pps->flags & NVRTX_VIDEO_H265_PPS_TILES) != 0U;
	description->tiles = tiles;
	if (tiles != 0U) {
		description->tile_columns = pps->tile_columns_minus1 + 1U;
		description->tile_rows = pps->tile_rows_minus1 + 1U;
		description->filter_across_tiles = (pps->flags & NVRTX_VIDEO_H265_PPS_ACROSS_TILES) != 0U;
	}

	/* The loop filters, the lists' modification, the merge level and the header's extension. */
	description->filter_across_slices = (pps->flags & NVRTX_VIDEO_H265_PPS_ACROSS_SLICES) != 0U;
	description->deblocking_control = (pps->flags & NVRTX_VIDEO_H265_PPS_DEBLOCKING_CONTROL) != 0U;
	description->deblocking_override = (pps->flags & NVRTX_VIDEO_H265_PPS_DEBLOCKING_OVERRIDE) != 0U;
	description->deblocking_disabled = (pps->flags & NVRTX_VIDEO_H265_PPS_DEBLOCKING_OFF) != 0U;
	description->lists_modification = (pps->flags & NVRTX_VIDEO_H265_PPS_LISTS_MODIFICATION) != 0U;
	description->log2_parallel_merge = pps->log2_merge_minus2 + 2U;
	description->header_extension = (pps->flags & NVRTX_VIDEO_H265_PPS_HEADER_EXTENSION) != 0U;

	/* The picture's kind. */
	description->idr = (picture->flags & NVRTX_VIDEO_H265_PICTURE_IDR) != 0U;
	description->irap = (picture->flags & NVRTX_VIDEO_H265_PICTURE_IRAP) != 0U;

	/* The counts the decoder parses the slice header's reference sets with, when it is told to. */
	description->separate_planes = (sps->flags & NVRTX_VIDEO_H265_SPS_SEPARATE_PLANES) != 0U;
	description->log2_max_poc_lsb_minus4 = sps->log2_max_poc_lsb_minus4;
	description->short_term_sets = sps->short_term_sets;
	description->long_term_sps = sps->long_term_sps;
	description->long_term_present = (sps->flags & NVRTX_VIDEO_H265_SPS_LONG_TERM) != 0U;
	description->delta_pocs = picture->delta_pocs;
	description->short_term_bits = picture->short_term_bits;
}

/*
 * Resolves the scaling lists a picture decodes with, in the decoder's
 * raster order: its picture parameter set's own, else its sequence's
 * own, else the defaults.  Called only when the sequence enables scaling
 * lists.
 *
 * Procedure referenced: Recommendation ITU-T H.265 (7.4.3.2.1 and
 * 7.4.3.3.1, which set of lists applies; 7.4.5, the defaults of Table
 * 7-5 and Table 7-6 and the DC value 16; 6.5.3, the up-right diagonal
 * scan the lists are coded in).
 */
void
nvrtx_video_h265_scaling(
	const struct nvrtx_video_h265_sps *sps,
	const struct nvrtx_video_h265_pps *pps,
	struct nvrtx_nvdec_hevc_scaling *scaling)
{
	const struct nvrtx_video_h265_scaling *source;
	const uint8_t *fallback;
	uint8_t positions4[16];
	uint8_t positions8[64];
	uint8_t flat[16];
	uint32_t list;
	uint32_t index;

	/* The raster position of each coded coefficient of the two sides. */
	h265_raster(4U, positions4);
	h265_raster(8U, positions8);

	/* The picture's lists when it sends its own, else the sequence's, else none (the defaults). */
	source = NULL;
	if ((sps->flags & NVRTX_VIDEO_H265_SPS_SCALING_DATA) != 0U && sps->has_scaling)
		source = &sps->scaling;
	if ((pps->flags & NVRTX_VIDEO_H265_PPS_SCALING_DATA) != 0U && pps->has_scaling)
		source = &pps->scaling;

	/* The defaults: flat 4x4 lists, Table 7-6 for the larger ones, DC values of 16. */
	if (source == NULL) {
		for (index = 0U; index < 16U; index++)
			flat[index] = H265_FLAT;
		for (list = 0U; list < 6U; list++) {
			/* The intra lists are the first three of each size, the inter lists the last three. */
			fallback = h265_default_inter;
			if (list < 3U)
				fallback = h265_default_intra;
			h265_list(flat, positions4, 16U, scaling->list4[list]);
			h265_list(fallback, positions8, 64U, scaling->list8[list]);
			h265_list(fallback, positions8, 64U, scaling->list16[list]);
			scaling->dc16[list] = H265_FLAT;
		}
		h265_list(h265_default_intra, positions8, 64U, scaling->list32[0]);
		h265_list(h265_default_inter, positions8, 64U, scaling->list32[1]);
		scaling->dc32[0] = H265_FLAT;
		scaling->dc32[1] = H265_FLAT;
		return;
	}

	/* The set's 4x4, 8x8 and 16x16 lists and the 16x16 DC values. */
	for (list = 0U; list < 6U; list++) {
		h265_list(source->list4[list], positions4, 16U, scaling->list4[list]);
		h265_list(source->list8[list], positions8, 64U, scaling->list8[list]);
		h265_list(source->list16[list], positions8, 64U, scaling->list16[list]);
		scaling->dc16[list] = source->dc16[list];
	}

	/* The set's two 32x32 lists and their DC values. */
	for (list = 0U; list < 2U; list++) {
		h265_list(source->list32[list], positions8, 64U, scaling->list32[list]);
		scaling->dc32[list] = source->dc32[list];
	}
}

/*
 * Reports a picture's tiles: their counts and each column's width and
 * row's height in CTBs (one tile of the whole picture without tiles).
 *
 * Procedure referenced: Recommendation ITU-T H.265 (6.5.1, colWidth and
 * rowHeight, uniform and explicit).
 */
void
nvrtx_video_h265_tiles(
	const struct nvrtx_video_h265_sps *sps,
	const struct nvrtx_video_h265_pps *pps,
	struct nvrtx_nvdec_hevc_tiles *tiles)
{
	uint32_t log2_ctb;
	uint32_t width_ctbs;
	uint32_t height_ctbs;
	bool uniform;

	/* The CTB's size and the picture in CTBs. */
	log2_ctb = sps->log2_min_cb_minus3 + 3U + sps->log2_diff_cb;
	width_ctbs = h265_ctbs(sps->width, log2_ctb);
	height_ctbs = h265_ctbs(sps->height, log2_ctb);
	tiles->log2_ctb = log2_ctb;

	/* Without tiles: the one tile of the picture. */
	if ((pps->flags & NVRTX_VIDEO_H265_PPS_TILES) == 0U) {
		tiles->columns = 1U;
		tiles->rows = 1U;
		tiles->width[0] = width_ctbs;
		tiles->height[0] = height_ctbs;
		return;
	}

	/* The counts, within the decoder's. */
	tiles->columns = pps->tile_columns_minus1 + 1U;
	tiles->rows = pps->tile_rows_minus1 + 1U;
	if (tiles->columns > NVRTX_NVDEC_HEVC_TILE_COLUMNS)
		tiles->columns = NVRTX_NVDEC_HEVC_TILE_COLUMNS;
	if (tiles->rows > NVRTX_NVDEC_HEVC_TILE_ROWS)
		tiles->rows = NVRTX_NVDEC_HEVC_TILE_ROWS;

	/* Each column's width and each row's height, uniform or as sent. */
	uniform = (pps->flags & NVRTX_VIDEO_H265_PPS_UNIFORM) != 0U;
	h265_spacing(width_ctbs, tiles->columns, uniform, pps->column_width_minus1, tiles->width);
	h265_spacing(height_ctbs, tiles->rows, uniform, pps->row_height_minus1, tiles->height);
}

/*
 * Fills a picture's references into its description by surface (the
 * slot): each reference's order count difference (cut to the field's
 * range) and long-term bit, the initial reference lists from the three
 * current sets, and how many references the sets name.  Every surface
 * without a reference takes the difference of the substitute: the first
 * reference of the sets, or the picture itself (difference 0) without
 * one.  *substitute is the substitute's surface (the picture's own,
 * description->surface, without one).
 *
 * Returns EINVAL when a set names a slot the decode does not reference.
 *
 * Procedure referenced: averne's FFmpeg nvtegra libavcodec/nvtegra_hevc.c
 * (nvtegra_hevc_prepare_frame_setup: the scratch reference, the order
 * differences and long-term bits by index, FILL_REFLIST) and
 * Recommendation ITU-T H.265 (8.3.4, RefPicListTemp0 and RefPicListTemp1
 * repeated to the list's length).
 */
int
nvrtx_video_h265_references(
	const struct nvrtx_video_h265_picture *picture,
	const struct nvrtx_video_h265_reference *references,
	uint32_t count,
	struct nvrtx_nvdec_hevc_picture *description,
	uint32_t *substitute)
{
	const uint8_t *sets[3];
	int32_t substitute_difference;
	uint32_t total;
	uint32_t set;
	uint32_t entry;
	uint32_t surface;
	uint32_t at;
	int found;

	/* The three current sets, in the order the substitute is looked for. */
	sets[0] = picture->before;
	sets[1] = picture->after;
	sets[2] = picture->long_term;

	/* Every slot a set names must be a reference of the decode. */
	for (set = 0U; set < 3U; set++) {
		for (entry = 0U; entry < NVRTX_VIDEO_H265_SET_ENTRIES; entry++) {
			if (sets[set][entry] == NVRTX_VIDEO_H265_NO_SLOT)
				break;
			found = h265_reference_find(references, count, sets[set][entry]);
			if (found < 0)
				return EINVAL;
		}
	}

	/* The substitute: the first reference of the sets, else the picture itself. */
	*substitute = description->surface;
	substitute_difference = 0;
	for (set = 0U; set < 3U; set++) {
		if (sets[set][0] == NVRTX_VIDEO_H265_NO_SLOT)
			continue;
		found = h265_reference_find(references, count, sets[set][0]);
		*substitute = sets[set][0];
		substitute_difference = h265_order(picture->poc - references[found].poc);
		break;
	}

	/* Each surface's difference: its reference's, or the substitute's. */
	description->long_term_mask = 0U;
	for (surface = 0U; surface < NVRTX_NVDEC_HEVC_SURFACES; surface++) {
		description->order_difference[surface] = substitute_difference;
		found = h265_reference_find(references, count, surface);
		if (found < 0 || surface == description->surface)
			continue;

		/* A reference's own difference, and its long-term bit (bit 15 for surface 0). */
		description->order_difference[surface] = h265_order(picture->poc - references[found].poc);
		if ((references[found].flags & NVRTX_VIDEO_H265_REFERENCE_LONG_TERM) != 0U)
			description->long_term_mask |= 1U << (15U - surface);
	}

	/* The picture's own surface has no difference. */
	if (description->surface < NVRTX_NVDEC_HEVC_SURFACES)
		description->order_difference[description->surface] = 0;

	/* How many references the current sets name (NumPicTotalCurr). */
	total = h265_set_count(picture->before) + h265_set_count(picture->after) + h265_set_count(picture->long_term);
	description->reference_count = total;

	/* Without references the lists stay empty. */
	for (at = 0U; at < 16U; at++) {
		description->list0[at] = 0U;
		description->list1[at] = 0U;
	}
	if (total == 0U)
		return 0;

	/* List 0: before, after, long term, repeated to sixteen entries. */
	at = 0U;
	while (at < 16U) {
		at = h265_set_fill(description->list0, at, picture->before, NVRTX_VIDEO_H265_SET_ENTRIES);
		at = h265_set_fill(description->list0, at, picture->after, NVRTX_VIDEO_H265_SET_ENTRIES);
		at = h265_set_fill(description->list0, at, picture->long_term, NVRTX_VIDEO_H265_SET_ENTRIES);
	}

	/* List 1: after, before, long term, repeated the same way. */
	at = 0U;
	while (at < 16U) {
		at = h265_set_fill(description->list1, at, picture->after, NVRTX_VIDEO_H265_SET_ENTRIES);
		at = h265_set_fill(description->list1, at, picture->before, NVRTX_VIDEO_H265_SET_ENTRIES);
		at = h265_set_fill(description->list1, at, picture->long_term, NVRTX_VIDEO_H265_SET_ENTRIES);
	}

	/* Succeeded: the references are in the description. */
	return 0;
}

/* Reports the first surface no reference of the decode holds (there is one: fifteen references at most). */
uint32_t
nvrtx_video_h265_free_surface(
	const struct nvrtx_video_h265_reference *references,
	uint32_t count)
{
	uint32_t surface;
	int found;

	/* The first surface without a reference. */
	for (surface = 0U; surface < NVRTX_NVDEC_HEVC_SURFACES; surface++) {
		found = h265_reference_find(references, count, surface);
		if (found < 0)
			return surface;
	}

	/* Every surface is a reference's (more than the limit): the last one. */
	return NVRTX_NVDEC_HEVC_SURFACES - 1U;
}

/*
 * Counts the bits of a picture's first slice segment header that the
 * decoder skips: from after slice_type to the end of the long-term
 * reference pictures (before slice_temporal_mvp_enabled_flag), or for an
 * IDR picture to the end of colour_plane_id.  bytes are the slice
 * segment's, from its start code (or its NAL unit header without one).
 * The slice header's own short-term set is passed over by the picture's
 * count of its bits.
 *
 * Returns EINVAL for a header that is not a first slice segment of the
 * picture's parameter set, or that ends too soon.
 *
 * Procedure referenced: Recommendation ITU-T H.265 (7.3.6.1
 * slice_segment_header, 7.3.1.2 nal_unit_header, 7.4.2.4.2 emulation
 * prevention) and averne's FFmpeg nvtegra libavcodec/hevc/hevcdec.c
 * (hls_slice_header: nvidia_skip_length, where it starts and ends).
 */
int
nvrtx_video_h265_skip_bits(
	const uint8_t *bytes,
	size_t length,
	const struct nvrtx_video_h265_sps *sps,
	const struct nvrtx_video_h265_pps *pps,
	const struct nvrtx_video_h265_picture *picture,
	uint32_t *bits)
{
	struct h265_bits reader;
	uint8_t header[H265_HEADER_BYTES];
	uint32_t nal_type;
	uint32_t pps_id;
	uint32_t index;
	uint32_t start;
	uint32_t long_term_sps;
	uint32_t long_term_pics;
	uint32_t poc_bits;
	bool sps_set;
	size_t at;

	/* Passes over the start code: zero bytes, then the one. */
	at = 0U;
	while (at < length && bytes[at] == 0U)
		at++;
	if (at >= 2U && at < length && bytes[at] == 1U) {
		at++;
	} else {
		at = 0U;
	}

	/* The two bytes of the NAL unit header: the type is bits 1 to 6 of the first. */
	if (length < at + 2U)
		return EINVAL;
	nal_type = (bytes[at] >> 1) & 0x3fU;
	at += 2U;

	/* The header's bits without the emulation prevention bytes. */
	reader.bytes = header;
	reader.length = h265_unescape(bytes + at, length - at, header, sizeof(header));
	reader.at = 0U;
	reader.error = false;

	/* The first slice segment of a picture, of the picture's parameter set. */
	if (h265_read(&reader, 1U) != 1U)
		return EINVAL;
	if (nal_type >= H265_NAL_BLA_W_LP && nal_type <= H265_NAL_IRAP_LAST)
		(void)h265_read(&reader, 1U);
	pps_id = h265_golomb(&reader);
	if (reader.error || pps_id != pps->id)
		return EINVAL;

	/* The reserved flags and the slice type. */
	for (index = 0U; index < pps->extra_bits; index++)
		(void)h265_read(&reader, 1U);
	(void)h265_golomb(&reader);

	/* The skip starts here. */
	start = (uint32_t)reader.at;

	/* The output flag and the colour plane. */
	if ((pps->flags & NVRTX_VIDEO_H265_PPS_OUTPUT_FLAG) != 0U)
		(void)h265_read(&reader, 1U);
	if ((sps->flags & NVRTX_VIDEO_H265_SPS_SEPARATE_PLANES) != 0U)
		(void)h265_read(&reader, 2U);

	/* An IDR picture has no reference picture sets in its header. */
	if (nal_type == H265_NAL_IDR_W_RADL || nal_type == H265_NAL_IDR_N_LP) {
		if (reader.error)
			return EINVAL;
		*bits = (uint32_t)reader.at - start;
		return 0;
	}

	/* The order count's low bits and the short-term set: the sequence's by its index, or the header's own. */
	poc_bits = sps->log2_max_poc_lsb_minus4 + 4U;
	(void)h265_read(&reader, poc_bits);
	sps_set = h265_read(&reader, 1U) != 0U;
	if (!sps_set) {
		reader.at += picture->short_term_bits;
	} else if (sps->short_term_sets > 1U) {
		(void)h265_read(&reader, h265_ceil_log2(sps->short_term_sets));
	}

	/* The long-term pictures: from the sequence's by index, or with their own order counts. */
	if ((sps->flags & NVRTX_VIDEO_H265_SPS_LONG_TERM) != 0U) {
		long_term_sps = 0U;
		if (sps->long_term_sps > 0U)
			long_term_sps = h265_golomb(&reader);
		long_term_pics = h265_golomb(&reader);
		if (long_term_sps > 32U || long_term_pics > 32U)
			return EINVAL;

		/* Each one: its index or its order count and use, then its order count's high part if sent. */
		for (index = 0U; index < long_term_sps + long_term_pics && !reader.error; index++) {
			if (index < long_term_sps) {
				if (sps->long_term_sps > 1U)
					(void)h265_read(&reader, h265_ceil_log2(sps->long_term_sps));
			} else {
				(void)h265_read(&reader, poc_bits);
				(void)h265_read(&reader, 1U);
			}
			if (h265_read(&reader, 1U) != 0U)
				(void)h265_golomb(&reader);
		}
	}

	/* A header that ends too soon. */
	if (reader.error || reader.at > reader.length * 8U)
		return EINVAL;

	/* Succeeded: the bits from after the slice type to here. */
	*bits = (uint32_t)reader.at - start;
	return 0;
}

/*
 * Fills the raster position (row times side plus column) of each coded
 * coefficient of a side by side block, in the up-right diagonal scan.
 *
 * Procedure referenced: Recommendation ITU-T H.265 (6.5.3, up-right
 * diagonal scan order array initialization).
 */
static void
h265_raster(
	uint32_t side,
	uint8_t *positions)
{
	uint32_t index;
	int32_t x;
	int32_t y;
	int32_t diagonal;

	/* Each anti-diagonal from its bottom left up to its top right. */
	index = 0U;
	for (diagonal = 0; diagonal < (int32_t)(2U * side - 1U); diagonal++) {
		x = 0;
		y = diagonal;
		while (y >= 0) {
			if (x < (int32_t)side && y < (int32_t)side) {
				positions[index] = (uint8_t)((uint32_t)y * side + (uint32_t)x);
				index++;
			}
			y--;
			x++;
		}
	}
}

/* Puts a list's coded coefficients in their raster places. */
static void
h265_list(
	const uint8_t *coded,
	const uint8_t *positions,
	uint32_t count,
	uint8_t *raster)
{
	uint32_t index;

	/* Each coefficient where the scan puts it. */
	for (index = 0U; index < count; index++)
		raster[positions[index]] = coded[index];
}

/* Reports how many CTBs of a size cover a number of samples. */
static uint32_t
h265_ctbs(
	uint32_t samples,
	uint32_t log2_ctb)
{
	uint32_t ctb;

	/* Whole CTBs. */
	ctb = 1U << log2_ctb;
	return (samples + ctb - 1U) / ctb;
}

/*
 * Divides total CTBs into parts: evenly, or as sent (each part but the
 * last its size minus 1, the last what is left).
 */
static void
h265_spacing(
	uint32_t total,
	uint32_t parts,
	bool uniform,
	const uint32_t *minus1,
	uint32_t *sizes)
{
	uint32_t index;
	uint32_t used;

	/* Evenly: each part's end at its share of the total. */
	if (uniform) {
		for (index = 0U; index < parts; index++)
			sizes[index] = ((index + 1U) * total) / parts - (index * total) / parts;
		return;
	}

	/* As sent, the last part what the others leave. */
	used = 0U;
	for (index = 0U; index + 1U < parts; index++) {
		sizes[index] = minus1[index] + 1U;
		used += sizes[index];
	}
	sizes[parts - 1U] = 0U;
	if (total > used)
		sizes[parts - 1U] = total - used;
}

/* Finds the reference of a slot among a decode's; -1 for none. */
static int
h265_reference_find(
	const struct nvrtx_video_h265_reference *references,
	uint32_t count,
	uint32_t slot)
{
	uint32_t index;

	/* The first reference of the slot. */
	for (index = 0U; index < count; index++) {
		if (references[index].slot == slot)
			return (int)index;
	}

	/* No reference of the slot. */
	return -1;
}

/* Appends a set's slots to a list from at, up to sixteen entries; reports where the list goes on. */
static uint32_t
h265_set_fill(
	uint8_t *list,
	uint32_t at,
	const uint8_t *set,
	uint32_t entries)
{
	uint32_t entry;

	/* Each slot of the set until its end or the list's. */
	for (entry = 0U; entry < entries && at < 16U; entry++) {
		if (set[entry] == NVRTX_VIDEO_H265_NO_SLOT)
			break;
		list[at] = set[entry];
		at++;
	}

	/* Succeeded: the list's next entry. */
	return at;
}

/* Counts a set's slots up to its end. */
static uint32_t
h265_set_count(
	const uint8_t *set)
{
	uint32_t entry;

	/* The slots before the first empty entry. */
	for (entry = 0U; entry < NVRTX_VIDEO_H265_SET_ENTRIES; entry++) {
		if (set[entry] == NVRTX_VIDEO_H265_NO_SLOT)
			break;
	}

	/* Succeeded: the count. */
	return entry;
}

/* Cuts an order count difference to the picture structure's range. */
static int32_t
h265_order(
	int32_t difference)
{
	/* Below the range. */
	if (difference < H265_ORDER_MIN)
		return H265_ORDER_MIN;

	/* Above the range. */
	if (difference > H265_ORDER_MAX)
		return H265_ORDER_MAX;

	/* Succeeded: within the range. */
	return difference;
}

/*
 * Copies a NAL unit's payload without its emulation prevention bytes (a 3
 * after two zero bytes), up to room bytes; reports how many it copied.
 */
static size_t
h265_unescape(
	const uint8_t *bytes,
	size_t length,
	uint8_t *out,
	size_t room)
{
	size_t at;
	size_t copied;
	uint32_t zeros;

	/* Each byte but the 3 that follows two zeros. */
	copied = 0U;
	zeros = 0U;
	for (at = 0U; at < length && copied < room; at++) {
		if (zeros >= 2U && bytes[at] == 3U) {
			zeros = 0U;
			continue;
		}

		/* Counts the zeros a 3 would follow. */
		if (bytes[at] == 0U) {
			zeros++;
		} else {
			zeros = 0U;
		}
		out[copied] = bytes[at];
		copied++;
	}

	/* Succeeded: the bytes copied. */
	return copied;
}

/* Reads count bits (at most 32), the first the most significant; past the end marks the error and reads 0. */
static uint32_t
h265_read(
	struct h265_bits *bits,
	uint32_t count)
{
	uint32_t value;
	uint32_t index;
	uint32_t bit;

	/* Each bit, most significant first. */
	value = 0U;
	for (index = 0U; index < count; index++) {
		if (bits->at >= bits->length * 8U) {
			bits->error = true;
			return 0U;
		}
		bit = (bits->bytes[bits->at / 8U] >> (7U - (uint32_t)(bits->at % 8U))) & 1U;
		value = (value << 1) | bit;
		bits->at++;
	}

	/* Succeeded: the bits' value. */
	return value;
}

/* Reads an unsigned exp-Golomb code (ue(v)); an over-long one marks the error. */
static uint32_t
h265_golomb(
	struct h265_bits *bits)
{
	uint32_t zeros;
	uint32_t suffix;

	/* The leading zeros up to the one. */
	zeros = 0U;
	while (h265_read(bits, 1U) == 0U) {
		if (bits->error)
			return 0U;
		zeros++;
		if (zeros > H265_GOLOMB_ZEROS) {
			bits->error = true;
			return 0U;
		}
	}

	/* The suffix of as many bits. */
	suffix = h265_read(bits, zeros);

	/* Succeeded: the code's value. */
	return ((1U << zeros) - 1U) + suffix;
}

/* Reports Ceil(Log2(value)) for a value of at least 1. */
static uint32_t
h265_ceil_log2(
	uint32_t value)
{
	uint32_t log2;

	/* The smallest power of two not below the value. */
	log2 = 0U;
	while ((1U << log2) < value)
		log2++;

	/* Succeeded: its exponent. */
	return log2;
}
