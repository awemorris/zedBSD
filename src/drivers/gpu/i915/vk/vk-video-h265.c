/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The H.265 part of the executor's video decode (see vk-video-h265.h): the
 * wire's H.265 records, the checks of the decoder's limits, the slice
 * segment headers and the tiles and scaling matrices of a picture.
 */

#include "vk-video-h265.h"
#include "vk-codec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The fixed arrays of the StdVideoH265 structures the wire carries (vulkan_video_codec_h265std.h). */
#define H265_SUB_LAYERS			7U
#define H265_CPB_COUNT			32U
#define H265_SHORT_TERM_SETS		64U
#define H265_LONG_TERM_PICS		32U
#define H265_SET_PICTURES		16U
#define H265_PALETTE_ENTRIES		384U
#define H265_CHROMA_OFFSETS		6U

/* The NAL unit types: the last decodable non-IRAP picture, the IRAP range, and the two IDR pictures. */
#define H265_NAL_LAST_PICTURE		9U
#define H265_NAL_BLA_W_LP		16U
#define H265_NAL_IRAP_DECODABLE_LAST	21U
#define H265_NAL_IRAP_LAST		23U
#define H265_NAL_IDR_W_RADL		19U
#define H265_NAL_IDR_N_LP		20U

/* The longest exp-Golomb code read (leading zeros). */
#define H265_GOLOMB_ZEROS		31U

/* Bounds on header counts that keep every loop short: long-term pictures, entry points, extension bytes. */
#define H265_LONG_TERM_MAX		32U
#define H265_ENTRY_POINTS_MAX		4096U
#define H265_EXTENSION_MAX		256U
#define H265_SHORT_TERM_BITS_MAX	4096U

/* The longest temporary reference list (8.3.4): the larger of 15 and the 24 entries of the three sets. */
#define H265_TEMP_LIST			24U

/* The coefficient of a flat list and the default DC value. */
#define H265_FLAT			16U

/*
 * The default 8x8 scaling lists (used for the 8x8, 16x16 and 32x32
 * sizes), in the coded (up-right diagonal) order; the default 4x4 lists
 * are flat.
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

/* The default inter list, in the same order. */
static const uint8_t h265_default_inter[64] = {
	16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 17, 17, 17, 17, 17, 18,
	18, 18, 18, 18, 18, 20, 20, 20, 20, 20, 20, 20, 24, 24, 24, 24,
	24, 24, 24, 24, 25, 25, 25, 25, 25, 25, 25, 28, 28, 28, 28, 28,
	28, 33, 33, 33, 33, 33, 41, 41, 41, 41, 54, 54, 54, 71, 71, 91
};

/*
 * The bits of a slice segment header as the parse reads them: the
 * segment's bytes (emulation prevention bytes in place), how many, the
 * next byte, the byte being read and its bits left, the zero bytes just
 * read (for the emulation prevention), the emulation prevention bytes
 * passed, and whether a read went past the end.
 */
struct h265_bits {
	const uint8_t *bytes;
	size_t length;
	size_t at;
	uint32_t current;
	uint32_t left;
	uint32_t zeros;
	uint32_t removed;
	bool error;
};

static void wire_words(struct i915_wire_reader *reader, uint32_t *words, uint32_t expected);
static void wire_bytes(struct i915_wire_reader *reader, void *bytes, uint32_t expected);
static void wire_skip(struct i915_wire_reader *reader, uint32_t count);
static void wire_dpb(struct i915_wire_reader *reader);
static void wire_hrd(struct i915_wire_reader *reader, uint32_t sub_layers);
static void wire_sub_layers(struct i915_wire_reader *reader, uint32_t sub_layers);
static void wire_scaling(struct i915_wire_reader *reader, struct i915_video_h265_scaling *scaling);
static void bits_start(struct h265_bits *bits, const uint8_t *bytes, size_t length, size_t at);
static uint32_t bits_byte(struct h265_bits *bits);
static uint32_t bits_read(struct h265_bits *bits, uint32_t count);
static uint32_t bits_golomb(struct h265_bits *bits);
static int32_t bits_signed(struct h265_bits *bits);
static uint64_t bits_remaining(const struct h265_bits *bits);
static uint32_t ceil_log2(uint32_t value);
static int32_t shift_floor(int32_t value, uint32_t shift);
static int32_t clip(int32_t value, int32_t low, int32_t high);
static uint32_t set_count(const uint8_t *set);
static const char *read_references(struct h265_bits *bits, const struct i915_video_h265_sps *sps, const struct i915_video_h265_picture *picture);
static const char *read_long_term(struct h265_bits *bits, const struct i915_video_h265_sps *sps);
static const char *build_list(const struct i915_video_h265_picture *picture, const struct i915_video_h265_slot *slots, uint32_t list,
			      uint32_t count, bool modified, const uint8_t *entries, uint32_t total, uint8_t *out);
static const char *read_predicted(struct h265_bits *bits, const struct i915_video_h265_pps *pps, const struct i915_video_h265_picture *picture,
				  const struct i915_video_h265_slot *slots, struct i915_video_h265_slice *slice);
static const char *read_weights(struct h265_bits *bits, struct i915_video_h265_slice *slice);
static const char *read_list_weights(struct h265_bits *bits, uint32_t list, struct i915_video_h265_slice *slice);
static const char *read_tail(struct h265_bits *bits, const struct i915_video_h265_pps *pps, struct i915_video_h265_slice *slice);
static void copy_slice(const struct i915_video_h265_slice *from, struct i915_video_h265_slice *to);
static void spacing(uint32_t total, uint32_t parts, bool uniform, const uint32_t *minus1, uint32_t *bounds);
static void raster(uint32_t side, const uint8_t *coded, uint8_t *out);

/*
 * Reads one H.265 video parameter set: [flags][id][max sub-layers minus 1]
 * [reserved x2][timing x3][reserved][present]{DPB}[present]{HRD}[present]
 * {PTL}.  Reports its id.
 */
uint32_t
drv_i915_video_h265_read_vps(
	struct i915_wire_reader *reader)
{
	uint64_t present;
	uint32_t id;
	uint32_t sub_layers;

	/* The flags, the id and the sub-layers. */
	(void)drv_i915_wire_read_u32(reader);
	id = drv_i915_wire_read_u32(reader);
	sub_layers = drv_i915_wire_read_u32(reader) + 1U;
	if (sub_layers > H265_SUB_LAYERS)
		reader->error = 1;

	/* The reserved words and the timing. */
	wire_skip(reader, 6U);

	/* The buffering limits, the HRD and the profile, tier and level, which the decode does not use. */
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U)
		wire_dpb(reader);
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U && reader->error == 0)
		wire_hrd(reader, sub_layers);
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U)
		wire_skip(reader, 3U);

	/* Succeeded: the set's id. */
	return id;
}

/*
 * Reads one H.265 sequence parameter set: its fields in declared order,
 * then the structures it points to.  The decode keeps its sizes, tools,
 * reference set counts, profile and scaling lists.
 */
void
drv_i915_video_h265_read_sps(
	struct i915_wire_reader *reader,
	struct i915_video_h265_sps *sps)
{
	uint64_t present;
	uint64_t count;
	uint64_t item;
	uint32_t sub_layers;

	/* The flags, the sampling and the size. */
	sps->flags = drv_i915_wire_read_u32(reader);
	sps->chroma_format_idc = drv_i915_wire_read_u32(reader);
	sps->width = drv_i915_wire_read_u32(reader);
	sps->height = drv_i915_wire_read_u32(reader);

	/* The key, the sub-layers and the depths. */
	sps->vps_id = drv_i915_wire_read_u32(reader);
	sub_layers = drv_i915_wire_read_u32(reader) + 1U;
	if (sub_layers > H265_SUB_LAYERS)
		reader->error = 1;
	sps->id = drv_i915_wire_read_u32(reader);
	sps->bit_depth_luma_minus8 = drv_i915_wire_read_u32(reader);
	sps->bit_depth_chroma_minus8 = drv_i915_wire_read_u32(reader);

	/* The order count's size and the block sizes. */
	sps->log2_max_poc_lsb_minus4 = drv_i915_wire_read_u32(reader);
	sps->log2_min_cb_minus3 = drv_i915_wire_read_u32(reader);
	sps->log2_diff_cb = drv_i915_wire_read_u32(reader);
	sps->log2_min_tb_minus2 = drv_i915_wire_read_u32(reader);
	sps->log2_diff_tb = drv_i915_wire_read_u32(reader);
	sps->depth_inter = drv_i915_wire_read_u32(reader);
	sps->depth_intra = drv_i915_wire_read_u32(reader);

	/* The reference picture sets the sequence signals. */
	sps->short_term_sets = drv_i915_wire_read_u32(reader);
	sps->long_term_sps = drv_i915_wire_read_u32(reader);

	/* The PCM depths and sizes, two reserved words. */
	sps->pcm_depth_luma_minus1 = drv_i915_wire_read_u32(reader);
	sps->pcm_depth_chroma_minus1 = drv_i915_wire_read_u32(reader);
	sps->log2_min_pcm_minus3 = drv_i915_wire_read_u32(reader);
	sps->log2_diff_pcm = drv_i915_wire_read_u32(reader);
	wire_skip(reader, 2U);

	/* The screen content sizes and the conformance window, which the decode does not use. */
	wire_skip(reader, 8U);

	/* The profile, tier and level: [flags][profile][level]. */
	sps->profile_idc = 0U;
	sps->level_idc = 0U;
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U) {
		(void)drv_i915_wire_read_u32(reader);
		sps->profile_idc = drv_i915_wire_read_u32(reader);
		sps->level_idc = drv_i915_wire_read_u32(reader);
	}

	/* The buffering limits. */
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U)
		wire_dpb(reader);

	/* The scaling lists in use. */
	sps->has_scaling = false;
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U) {
		sps->has_scaling = true;
		wire_scaling(reader, &sps->scaling);
	}

	/* The short-term sets: as many as the sequence counts; the slice headers give the decode what it needs of them. */
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U) {
		count = drv_i915_wire_read_u64(reader);
		if (count != sps->short_term_sets || count > H265_SHORT_TERM_SETS)
			reader->error = 1;
		for (item = 0U; item < count && reader->error == 0; item++) {
			wire_skip(reader, 12U);
			wire_words(reader, NULL, H265_SET_PICTURES);
			wire_words(reader, NULL, H265_SET_PICTURES);
		}
	}

	/* The long-term pictures, the VUI and the palette's initializers. */
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U) {
		wire_skip(reader, 1U);
		wire_words(reader, NULL, H265_LONG_TERM_PICS);
	}
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U) {
		wire_skip(reader, 25U);
		present = drv_i915_wire_read_u64(reader);
		if (present != 0U && reader->error == 0)
			wire_hrd(reader, sub_layers);
	}
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U)
		wire_words(reader, NULL, H265_PALETTE_ENTRIES);
}

/*
 * Reads one H.265 picture parameter set: its fields in declared order,
 * then the structures it points to.
 */
void
drv_i915_video_h265_read_pps(
	struct i915_wire_reader *reader,
	struct i915_video_h265_pps *pps)
{
	uint8_t offsets[H265_CHROMA_OFFSETS];
	uint64_t present;

	/* The flags and the key. */
	pps->flags = drv_i915_wire_read_u32(reader);
	pps->id = drv_i915_wire_read_u32(reader);
	pps->sps_id = drv_i915_wire_read_u32(reader);
	pps->vps_id = drv_i915_wire_read_u32(reader);

	/* The slice header's extra bits and the default reference counts. */
	pps->extra_bits = drv_i915_wire_read_u32(reader);
	pps->l0_minus1 = drv_i915_wire_read_u32(reader);
	pps->l1_minus1 = drv_i915_wire_read_u32(reader);

	/* The quantizer, the deblocking offsets and the merge level, the signed ones widened with their sign. */
	pps->init_qp_minus26 = (int32_t)drv_i915_wire_read_u32(reader);
	pps->cu_qp_delta_depth = drv_i915_wire_read_u32(reader);
	pps->cb_qp_offset = (int32_t)drv_i915_wire_read_u32(reader);
	pps->cr_qp_offset = (int32_t)drv_i915_wire_read_u32(reader);
	pps->beta_div2 = (int32_t)drv_i915_wire_read_u32(reader);
	pps->tc_div2 = (int32_t)drv_i915_wire_read_u32(reader);
	pps->log2_merge_minus2 = drv_i915_wire_read_u32(reader);

	/* The range extension's fields and chroma offset lists, which Main does not use. */
	wire_skip(reader, 3U);
	wire_bytes(reader, offsets, H265_CHROMA_OFFSETS);
	wire_bytes(reader, offsets, H265_CHROMA_OFFSETS);
	wire_skip(reader, 2U);

	/* The screen content fields, which Main does not use. */
	wire_skip(reader, 6U);

	/* The tiles: their counts, two reserved words, each column's width and row's height minus 1, a reserved word. */
	pps->tile_columns_minus1 = drv_i915_wire_read_u32(reader);
	pps->tile_rows_minus1 = drv_i915_wire_read_u32(reader);
	wire_skip(reader, 2U);
	wire_words(reader, pps->column_width_minus1, I915_VIDEO_H265_COLUMN_WIDTHS);
	wire_words(reader, pps->row_height_minus1, I915_VIDEO_H265_ROW_HEIGHTS);
	wire_skip(reader, 1U);

	/* The picture's own scaling lists. */
	pps->has_scaling = false;
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U) {
		pps->has_scaling = true;
		wire_scaling(reader, &pps->scaling);
	}

	/* The palette's initializers, which Main does not use. */
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U)
		wire_words(reader, NULL, H265_PALETTE_ENTRIES);
}

/*
 * Reads the H.265 information of the picture a decode writes: [flags]
 * [vps][sps][pps][NumDeltaPocsOfRefRpsIdx][PicOrderCntVal]
 * [NumBitsForSTRefPicSetInSlice][reserved] and the three sets of eight
 * slots.
 */
void
drv_i915_video_h265_read_picture(
	struct i915_wire_reader *reader,
	struct i915_video_h265_picture *picture)
{
	/* The flags and the sets the picture names. */
	picture->flags = drv_i915_wire_read_u32(reader);
	picture->vps_id = drv_i915_wire_read_u32(reader);
	picture->sps_id = drv_i915_wire_read_u32(reader);
	picture->pps_id = drv_i915_wire_read_u32(reader);

	/* The header's own short-term set's size, and the order count. */
	picture->delta_pocs = drv_i915_wire_read_u32(reader);
	picture->poc = (int32_t)drv_i915_wire_read_u32(reader);
	picture->short_term_bits = drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u32(reader);

	/* The slots of the three current sets. */
	wire_bytes(reader, picture->before, I915_VIDEO_H265_SET_ENTRIES);
	wire_bytes(reader, picture->after, I915_VIDEO_H265_SET_ENTRIES);
	wire_bytes(reader, picture->long_term, I915_VIDEO_H265_SET_ENTRIES);
}

/*
 * Checks a sequence and picture parameter set pair against the decoder:
 * the profile, the format and the extent first, then the values H.265
 * bounds (7.4.3.2.1, 7.4.3.3.1) that the commands' fields hold.
 */
const char *
drv_i915_video_h265_check(
	const struct i915_video_h265_sps *sps,
	const struct i915_video_h265_pps *pps)
{
	uint32_t log2_ctb;
	uint32_t log2_min_tb;
	uint32_t log2_max_tb;

	/* Main's tools: 4:2:0 in one plane, eight bits both, no range or screen content tools (whatever profile the stream names). */
	if (sps->chroma_format_idc != 1U || (sps->flags & I915_VIDEO_H265_SPS_SEPARATE_PLANES) != 0U)
		return "H.265 chroma format not 4:2:0";
	if (sps->bit_depth_luma_minus8 != 0U || sps->bit_depth_chroma_minus8 != 0U)
		return "H.265 bit depth not 8";
	if ((sps->flags & (I915_VIDEO_H265_SPS_RANGE_EXTENSION | I915_VIDEO_H265_SPS_RANGE_TOOLS | I915_VIDEO_H265_SPS_SCC_EXTENSION)) != 0U)
		return "H.265 range or screen content extension";
	if ((pps->flags & (I915_VIDEO_H265_PPS_CROSS_COMPONENT | I915_VIDEO_H265_PPS_CHROMA_QP_LIST | I915_VIDEO_H265_PPS_SCC_TOOLS)) != 0U)
		return "H.265 range or screen content tools";

	/* The extent the decoder takes. */
	if (sps->width < I915_VIDEO_H265_MIN_SIDE || sps->height < I915_VIDEO_H265_MIN_SIDE ||
	    sps->width > I915_VIDEO_H265_MAX_SIDE || sps->height > I915_VIDEO_H265_MAX_SIDE)
		return "H.265 picture size outside the decoder";

	/* Block sizes within H.265: CTBs of 16 to 64, transforms of 4 to 32 within the CTB. */
	if (sps->log2_min_cb_minus3 > 3U || sps->log2_diff_cb > 3U)
		return "H.265 coding block sizes out of range";
	log2_ctb = sps->log2_min_cb_minus3 + 3U + sps->log2_diff_cb;
	if (log2_ctb < 4U || log2_ctb > 6U)
		return "H.265 CTB size out of range";
	if (sps->log2_min_tb_minus2 > 3U || sps->log2_diff_tb > 3U)
		return "H.265 transform sizes out of range";
	log2_min_tb = sps->log2_min_tb_minus2 + 2U;
	log2_max_tb = log2_min_tb + sps->log2_diff_tb;
	if (log2_max_tb > 5U || log2_max_tb > log2_ctb || log2_min_tb >= sps->log2_min_cb_minus3 + 3U)
		return "H.265 transform sizes out of range";
	if (sps->depth_inter > log2_ctb - log2_min_tb || sps->depth_intra > log2_ctb - log2_min_tb)
		return "H.265 transform depth out of range";
	if ((sps->width % (1U << (sps->log2_min_cb_minus3 + 3U))) != 0U || (sps->height % (1U << (sps->log2_min_cb_minus3 + 3U))) != 0U)
		return "H.265 picture size not whole coding blocks";

	/* The order count's size, the reference set counts and the picture set's own ranges. */
	if (sps->log2_max_poc_lsb_minus4 > 12U || sps->short_term_sets > 64U || sps->long_term_sps > 32U)
		return "H.265 sequence reference values out of range";
	if (pps->l0_minus1 > 14U || pps->l1_minus1 > 14U || pps->extra_bits > 7U)
		return "H.265 picture reference values out of range";
	if (pps->cu_qp_delta_depth > sps->log2_diff_cb || pps->cu_qp_delta_depth > 3U || pps->log2_merge_minus2 > log2_ctb - 2U)
		return "H.265 picture depth values out of range";
	if (pps->init_qp_minus26 < -26 || pps->init_qp_minus26 > 25)
		return "H.265 initial QP out of range";
	if (pps->cb_qp_offset < -12 || pps->cb_qp_offset > 12 || pps->cr_qp_offset < -12 || pps->cr_qp_offset > 12)
		return "H.265 chroma QP offset out of range";
	if (pps->beta_div2 < -6 || pps->beta_div2 > 6 || pps->tc_div2 < -6 || pps->tc_div2 > 6)
		return "H.265 deblocking offset out of range";

	/* PCM's sizes, when it is on, within the CTB. */
	if ((sps->flags & I915_VIDEO_H265_SPS_PCM) != 0U) {
		if (sps->pcm_depth_luma_minus1 > 7U || sps->pcm_depth_chroma_minus1 > 7U)
			return "H.265 PCM depth out of range";
		if (sps->log2_min_pcm_minus3 > 2U || sps->log2_min_pcm_minus3 + 3U + sps->log2_diff_pcm > 5U)
			return "H.265 PCM sizes out of range";
	}

	/* Tiles within the decoder's tile state. */
	if ((pps->flags & I915_VIDEO_H265_PPS_TILES) != 0U &&
	    (pps->tile_columns_minus1 >= I915_VIDEO_H265_TILE_COLUMNS || pps->tile_rows_minus1 >= I915_VIDEO_H265_TILE_ROWS))
		return "H.265 tiles past the decoder's";

	/* Succeeded: the decoder takes the pair. */
	return NULL;
}

/*
 * Reads one slice segment's header.
 *
 * Procedure referenced: Recommendation ITU-T H.265 (7.3.1.1 and 7.3.1.2,
 * the NAL unit and its emulation prevention; 7.3.6.1 to 7.3.6.3 and
 * 7.4.7.1 to 7.4.7.3, the slice segment header with the reference list
 * modification and the weights; 7.3.2.11, byte_alignment()).
 */
const char *
drv_i915_video_h265_slice_read(
	const uint8_t *bytes,
	size_t length,
	const struct i915_video_h265_sps *sps,
	const struct i915_video_h265_pps *pps,
	const struct i915_video_h265_picture *picture,
	const struct i915_video_h265_slot *slots,
	const struct i915_video_h265_slice *independent,
	struct i915_video_h265_slice *slice)
{
	struct h265_bits bits;
	struct i915_video_h265_geometry geometry;
	const char *reason;
	size_t start;
	uint32_t header;
	uint32_t nal_type;
	uint32_t layer;
	uint32_t temporal;
	uint32_t pps_id;
	uint32_t ctbs;
	uint32_t index;
	uint32_t value;
	bool first;
	bool dependent;

	/* Clears the result. */
	copy_slice(NULL, slice);

	/* A start code prefix in front of the NAL unit is skipped (three bytes, or four with a zero before them). */
	start = 0;
	if (length >= 4U && bytes[0] == 0U && bytes[1] == 0U && bytes[2] == 0U && bytes[3] == 1U) {
		start = 4U;
	} else if (length >= 3U && bytes[0] == 0U && bytes[1] == 0U && bytes[2] == 1U) {
		start = 3U;
	} else {
		return "slice segment without a start code";
	}

	/* The two-byte NAL unit header: a decodable picture's type of the base layer. */
	if (length < start + 3U)
		return "slice segment too short";
	header = ((uint32_t)bytes[start] << 8) | bytes[start + 1U];
	nal_type = (header >> 9) & 63U;
	layer = (header >> 3) & 63U;
	temporal = header & 7U;
	if ((header & 0x8000U) != 0U || layer != 0U || temporal == 0U)
		return "slice segment NAL unit header not of the base layer";
	if (nal_type > H265_NAL_LAST_PICTURE && (nal_type < H265_NAL_BLA_W_LP || nal_type > H265_NAL_IRAP_DECODABLE_LAST))
		return "slice segment NAL unit type not a picture's";
	slice->nal_type = nal_type;
	slice->nal_offset = start;

	/* The payload's bits, after the header. */
	bits_start(&bits, bytes, length, start + 2U);

	/* The first fields of every segment: whether it starts the picture, the IRAP output flag and the PPS. */
	first = bits_read(&bits, 1U) != 0U;
	if (nal_type >= H265_NAL_BLA_W_LP && nal_type <= H265_NAL_IRAP_LAST)
		(void)bits_read(&bits, 1U);
	pps_id = bits_golomb(&bits);
	if (bits.error || pps_id != pps->id)
		return "slice segment of another picture parameter set";

	/* A later segment's dependence and address, in CTBs of the picture (Ceil( Log2( PicSizeInCtbsY ) ) bits). */
	drv_i915_video_h265_geometry(sps, pps, &geometry);
	ctbs = geometry.width_ctbs * geometry.height_ctbs;
	dependent = false;
	slice->address = 0;
	if (!first) {
		if ((pps->flags & I915_VIDEO_H265_PPS_DEPENDENT) != 0U)
			dependent = bits_read(&bits, 1U) != 0U;
		slice->address = bits_read(&bits, ceil_log2(ctbs));
		if (bits.error || slice->address == 0U || slice->address >= ctbs)
			return "slice segment address out of range";
	}

	/* A dependent segment takes the independent one's fields, then reads only its tail. */
	if (dependent) {
		if (independent == NULL)
			return "dependent slice segment without its slice";
		value = slice->address;
		copy_slice(independent, slice);
		slice->nal_type = nal_type;
		slice->nal_offset = start;
		slice->address = value;
		slice->first_in_picture = false;
		slice->dependent = true;
		return read_tail(&bits, pps, slice);
	}
	slice->first_in_picture = first;
	slice->dependent = false;
	slice->slice_address = slice->address;

	/* The reserved flags, the type (an IRAP picture is intra) and the output flag. */
	for (index = 0; index < pps->extra_bits; index++)
		(void)bits_read(&bits, 1U);
	slice->slice_type = bits_golomb(&bits);
	if (bits.error || slice->slice_type > I915_VIDEO_H265_SLICE_I)
		return "slice type out of range";
	if (nal_type >= H265_NAL_BLA_W_LP && slice->slice_type != I915_VIDEO_H265_SLICE_I)
		return "IRAP picture with a predicted slice";
	if ((pps->flags & I915_VIDEO_H265_PPS_OUTPUT_FLAG) != 0U)
		(void)bits_read(&bits, 1U);

	/* A picture other than IDR: its order count, its sets, and the temporal motion vector flag. */
	slice->temporal_mvp = false;
	if (nal_type != H265_NAL_IDR_W_RADL && nal_type != H265_NAL_IDR_N_LP) {
		reason = read_references(&bits, sps, picture);
		if (reason != NULL)
			return reason;
		if ((sps->flags & I915_VIDEO_H265_SPS_TEMPORAL_MVP) != 0U)
			slice->temporal_mvp = bits_read(&bits, 1U) != 0U;
	}

	/* The sample adaptive offset of luma and chroma (ChromaArrayType 1). */
	slice->sao_luma = false;
	slice->sao_chroma = false;
	if ((sps->flags & I915_VIDEO_H265_SPS_SAO) != 0U) {
		slice->sao_luma = bits_read(&bits, 1U) != 0U;
		slice->sao_chroma = bits_read(&bits, 1U) != 0U;
	}

	/* A predicted slice's references, lists, collocated picture, weights and merge candidates. */
	slice->references[0] = 0;
	slice->references[1] = 0;
	slice->collocated_from_l0 = true;
	slice->max_merge_candidates = 5U;
	if (slice->slice_type != I915_VIDEO_H265_SLICE_I) {
		reason = read_predicted(&bits, pps, picture, slots, slice);
		if (reason != NULL)
			return reason;
	}

	/* The quantizer: SliceQpY, within -QpBdOffsetY (0 at eight bits) and 51 (7.4.7.1). */
	slice->qp = 26 + pps->init_qp_minus26 + bits_signed(&bits);
	if (bits.error || slice->qp < 0 || slice->qp > 51)
		return "slice QP out of range";

	/* The slice's chroma quantizer offsets, -12 to 12 alone and with the picture's. */
	slice->cb_qp_offset = 0;
	slice->cr_qp_offset = 0;
	if ((pps->flags & I915_VIDEO_H265_PPS_SLICE_QP) != 0U) {
		slice->cb_qp_offset = bits_signed(&bits);
		slice->cr_qp_offset = bits_signed(&bits);
		if (bits.error || slice->cb_qp_offset < -12 || slice->cb_qp_offset > 12 || slice->cr_qp_offset < -12 || slice->cr_qp_offset > 12)
			return "slice chroma QP offset out of range";
		if (pps->cb_qp_offset + slice->cb_qp_offset < -12 || pps->cb_qp_offset + slice->cb_qp_offset > 12 ||
		    pps->cr_qp_offset + slice->cr_qp_offset < -12 || pps->cr_qp_offset + slice->cr_qp_offset > 12)
			return "slice chroma QP offset out of range";
	}

	/* The deblocking: the picture's, unless the slice overrides it. */
	slice->deblocking_disabled = (pps->flags & I915_VIDEO_H265_PPS_DEBLOCKING_OFF) != 0U;
	slice->beta_div2 = pps->beta_div2;
	slice->tc_div2 = pps->tc_div2;
	value = 0;
	if ((pps->flags & I915_VIDEO_H265_PPS_DEBLOCKING_OVERRIDE) != 0U)
		value = bits_read(&bits, 1U);
	if (value != 0U) {
		slice->deblocking_disabled = bits_read(&bits, 1U) != 0U;
		if (!slice->deblocking_disabled) {
			slice->beta_div2 = bits_signed(&bits);
			slice->tc_div2 = bits_signed(&bits);
			if (bits.error || slice->beta_div2 < -6 || slice->beta_div2 > 6 || slice->tc_div2 < -6 || slice->tc_div2 > 6)
				return "slice deblocking offset out of range";
		}
	}

	/* The loop filter across slices: read only when a filter runs, else the picture's. */
	slice->loop_filter_across_slices = (pps->flags & I915_VIDEO_H265_PPS_ACROSS_SLICES) != 0U;
	if (slice->loop_filter_across_slices &&
	    (slice->sao_luma || slice->sao_chroma || !slice->deblocking_disabled))
		slice->loop_filter_across_slices = bits_read(&bits, 1U) != 0U;
	if (bits.error)
		return "slice segment header past its bytes";

	/* The entry points, the extension and the alignment. */
	return read_tail(&bits, pps, slice);
}

/*
 * Makes a picture's CTB geometry: the CTB size, the picture in CTBs and
 * the tile boundaries (6.5.1, uniform or as sent).
 */
void
drv_i915_video_h265_geometry(
	const struct i915_video_h265_sps *sps,
	const struct i915_video_h265_pps *pps,
	struct i915_video_h265_geometry *geometry)
{
	uint32_t ctb;
	bool uniform;

	/* The CTB and the picture in whole CTBs. */
	geometry->log2_ctb = sps->log2_min_cb_minus3 + 3U + sps->log2_diff_cb;
	ctb = 1U << geometry->log2_ctb;
	geometry->width_ctbs = (sps->width + ctb - 1U) / ctb;
	geometry->height_ctbs = (sps->height + ctb - 1U) / ctb;

	/* Without tiles, one tile of the picture. */
	geometry->columns = 1U;
	geometry->rows = 1U;
	uniform = true;
	if ((pps->flags & I915_VIDEO_H265_PPS_TILES) != 0U) {
		geometry->columns = pps->tile_columns_minus1 + 1U;
		geometry->rows = pps->tile_rows_minus1 + 1U;
		uniform = (pps->flags & I915_VIDEO_H265_PPS_UNIFORM) != 0U;
	}

	/* A tile is never narrower than one CTB, and never more than the decoder's: more tiles are cut down. */
	if (geometry->columns > geometry->width_ctbs)
		geometry->columns = geometry->width_ctbs;
	if (geometry->rows > geometry->height_ctbs)
		geometry->rows = geometry->height_ctbs;
	if (geometry->columns > I915_VIDEO_H265_TILE_COLUMNS)
		geometry->columns = I915_VIDEO_H265_TILE_COLUMNS;
	if (geometry->rows > I915_VIDEO_H265_TILE_ROWS)
		geometry->rows = I915_VIDEO_H265_TILE_ROWS;

	/* The boundaries. */
	spacing(geometry->width_ctbs, geometry->columns, uniform, pps->column_width_minus1, geometry->column_bounds);
	spacing(geometry->height_ctbs, geometry->rows, uniform, pps->row_height_minus1, geometry->row_bounds);
}

/*
 * Reports a CTB's place in the tile scan: the CTBs of the tiles before its
 * own (tiles in raster order), then its place in its tile in raster order.
 *
 * Procedure referenced: Recommendation ITU-T H.265 (6.5.1, CtbAddrRsToTs).
 */
uint32_t
drv_i915_video_h265_tile_scan(
	const struct i915_video_h265_geometry *geometry,
	uint32_t address)
{
	uint32_t x;
	uint32_t y;
	uint32_t column;
	uint32_t row;
	uint32_t place;
	uint32_t index;

	/* The CTB's column and row, and its tile's. */
	x = address % geometry->width_ctbs;
	y = address / geometry->width_ctbs;
	column = 0;
	while (column + 1U < geometry->columns && x >= geometry->column_bounds[column + 1U])
		column++;
	row = 0;
	while (row + 1U < geometry->rows && y >= geometry->row_bounds[row + 1U])
		row++;

	/* The rows of tiles above, whole; the tiles before it in its row of tiles. */
	place = geometry->row_bounds[row] * geometry->width_ctbs;
	for (index = 0; index < column; index++)
		place += (geometry->column_bounds[index + 1U] - geometry->column_bounds[index]) * (geometry->row_bounds[row + 1U] - geometry->row_bounds[row]);

	/* Its place in its tile. */
	place += (y - geometry->row_bounds[row]) * (geometry->column_bounds[column + 1U] - geometry->column_bounds[column]) +
	    (x - geometry->column_bounds[column]);
	return place;
}

/*
 * Makes the scaling matrices of a picture in raster order (6.5.3's
 * up-right diagonal scan undone).
 *
 * Procedure referenced: Recommendation ITU-T H.265 (7.3.4 and 7.4.5, the
 * scaling lists and their defaults; 6.5.3, the up-right diagonal scan).
 */
void
drv_i915_video_h265_matrices(
	const struct i915_video_h265_sps *sps,
	const struct i915_video_h265_pps *pps,
	struct i915_video_h265_matrices *matrices)
{
	const struct i915_video_h265_scaling *source;
	const uint8_t *fallback;
	uint32_t matrix;
	uint32_t index;

	/* Without scaling lists every list is flat. */
	if ((sps->flags & I915_VIDEO_H265_SPS_SCALING) == 0U) {
		for (matrix = 0; matrix < 6U; matrix++) {
			for (index = 0; index < 16U; index++)
				matrices->list4[matrix][index] = H265_FLAT;
			for (index = 0; index < 64U; index++) {
				matrices->list8[matrix][index] = H265_FLAT;
				matrices->list16[matrix][index] = H265_FLAT;
			}
			matrices->dc16[matrix] = H265_FLAT;
		}
		for (matrix = 0; matrix < 2U; matrix++) {
			for (index = 0; index < 64U; index++)
				matrices->list32[matrix][index] = H265_FLAT;
			matrices->dc32[matrix] = H265_FLAT;
		}
		return;
	}

	/* The picture's lists when it sends them, else the sequence's, else the defaults. */
	source = NULL;
	if ((sps->flags & I915_VIDEO_H265_SPS_SCALING_DATA) != 0U && sps->has_scaling)
		source = &sps->scaling;
	if ((pps->flags & I915_VIDEO_H265_PPS_SCALING_DATA) != 0U && pps->has_scaling)
		source = &pps->scaling;

	/* The defaults: flat 4x4, Table 7-6 for the larger, DC values of 16. */
	if (source == NULL) {
		for (matrix = 0; matrix < 6U; matrix++) {
			fallback = h265_default_inter;
			if (matrix < 3U)
				fallback = h265_default_intra;
			for (index = 0; index < 16U; index++)
				matrices->list4[matrix][index] = H265_FLAT;
			raster(8U, fallback, matrices->list8[matrix]);
			raster(8U, fallback, matrices->list16[matrix]);
			matrices->dc16[matrix] = H265_FLAT;
		}
		raster(8U, h265_default_intra, matrices->list32[0]);
		raster(8U, h265_default_inter, matrices->list32[1]);
		matrices->dc32[0] = H265_FLAT;
		matrices->dc32[1] = H265_FLAT;
		return;
	}

	/* The set's lists of each size, with the DC values. */
	for (matrix = 0; matrix < 6U; matrix++) {
		raster(4U, source->list4[matrix], matrices->list4[matrix]);
		raster(8U, source->list8[matrix], matrices->list8[matrix]);
		raster(8U, source->list16[matrix], matrices->list16[matrix]);
		matrices->dc16[matrix] = source->dc16[matrix];
	}
	for (matrix = 0; matrix < 2U; matrix++) {
		raster(8U, source->list32[matrix], matrices->list32[matrix]);
		matrices->dc32[matrix] = source->dc32[matrix];
	}
}

/* Reads an array of words that must have expected elements: [count][words]; into words when not NULL. */
static void
wire_words(
	struct i915_wire_reader *reader,
	uint32_t *words,
	uint32_t expected)
{
	uint64_t count;
	uint32_t index;
	uint32_t value;

	/* The count must be the array's. */
	count = drv_i915_wire_read_u64(reader);
	if (count != expected) {
		reader->error = 1;
		return;
	}

	/* Each word. */
	for (index = 0U; index < expected && reader->error == 0; index++) {
		value = drv_i915_wire_read_u32(reader);
		if (words != NULL)
			words[index] = value;
	}
}

/* Reads a byte array that must have expected bytes: [count][bytes]. */
static void
wire_bytes(
	struct i915_wire_reader *reader,
	void *bytes,
	uint32_t expected)
{
	uint64_t count;

	/* The count must be the array's. */
	count = drv_i915_wire_read_u64(reader);
	if (count != expected) {
		reader->error = 1;
		return;
	}

	/* The bytes. */
	i915_vkc_read_bytes(reader, bytes, expected);
}

/* Reads and drops count words the decode does not use. */
static void
wire_skip(
	struct i915_wire_reader *reader,
	uint32_t count)
{
	uint32_t index;

	/* Each word. */
	for (index = 0U; index < count && reader->error == 0; index++)
		(void)drv_i915_wire_read_u32(reader);
}

/* Reads H.265 buffering limits: [7]{latency}[7 bytes]{buffering}[7 bytes]{reordering}. */
static void
wire_dpb(
	struct i915_wire_reader *reader)
{
	uint8_t bytes[H265_SUB_LAYERS];

	/* The latency words, then the two byte arrays. */
	wire_words(reader, NULL, H265_SUB_LAYERS);
	wire_bytes(reader, bytes, H265_SUB_LAYERS);
	wire_bytes(reader, bytes, H265_SUB_LAYERS);
}

/*
 * Reads H.265 HRD parameters: the flags and lengths, the buffer counts,
 * the durations, the reserved words, then the NAL and VCL sub-layer
 * arrays.
 */
static void
wire_hrd(
	struct i915_wire_reader *reader,
	uint32_t sub_layers)
{
	uint8_t counts[H265_SUB_LAYERS];
	uint64_t present;

	/* The flags and the masks, the lengths and the scales. */
	wire_skip(reader, 13U);

	/* The buffer counts, the durations and the reserved words. */
	wire_bytes(reader, counts, H265_SUB_LAYERS);
	wire_words(reader, NULL, H265_SUB_LAYERS);
	wire_words(reader, NULL, 3U);

	/* The NAL and the VCL sub-layers. */
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U)
		wire_sub_layers(reader, sub_layers);
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U)
		wire_sub_layers(reader, sub_layers);
}

/* Reads an array of H.265 sub-layer HRD parameters: [count]{four arrays of 32 words and the CBR mask}. */
static void
wire_sub_layers(
	struct i915_wire_reader *reader,
	uint32_t sub_layers)
{
	uint64_t count;
	uint64_t item;

	/* One entry for each sub-layer. */
	count = drv_i915_wire_read_u64(reader);
	if (count != sub_layers)
		reader->error = 1;

	/* Each entry's rates and sizes, then its CBR mask. */
	for (item = 0U; item < count && reader->error == 0; item++) {
		wire_words(reader, NULL, H265_CPB_COUNT);
		wire_words(reader, NULL, H265_CPB_COUNT);
		wire_words(reader, NULL, H265_CPB_COUNT);
		wire_words(reader, NULL, H265_CPB_COUNT);
		wire_skip(reader, 1U);
	}
}

/* Reads H.265 scaling lists: the four sizes' lists and the two DC arrays, each a byte array. */
static void
wire_scaling(
	struct i915_wire_reader *reader,
	struct i915_video_h265_scaling *scaling)
{
	/* The lists, list after list, then the DC values. */
	wire_bytes(reader, scaling->list4, sizeof(scaling->list4));
	wire_bytes(reader, scaling->list8, sizeof(scaling->list8));
	wire_bytes(reader, scaling->list16, sizeof(scaling->list16));
	wire_bytes(reader, scaling->list32, sizeof(scaling->list32));
	wire_bytes(reader, scaling->dc16, sizeof(scaling->dc16));
	wire_bytes(reader, scaling->dc32, sizeof(scaling->dc32));
}

/* Starts reading a NAL unit's payload at byte at. */
static void
bits_start(
	struct h265_bits *bits,
	const uint8_t *bytes,
	size_t length,
	size_t at)
{
	/* No byte read yet, and no zeros before the payload's first. */
	bits->bytes = bytes;
	bits->length = length;
	bits->at = at;
	bits->current = 0;
	bits->left = 0;
	bits->zeros = 0;
	bits->removed = 0;
	bits->error = false;
}

/* Reads the next payload byte, past an emulation prevention byte; 0 and the error past the end. */
static uint32_t
bits_byte(
	struct h265_bits *bits)
{
	uint32_t byte;

	/* Past the end: the error, once and for all. */
	if (bits->at >= bits->length) {
		bits->error = true;
		return 0;
	}

	/* A three after two zeros is not data (7.4.2). */
	byte = bits->bytes[bits->at];
	if (bits->zeros >= 2U && byte == 3U) {
		bits->at++;
		bits->zeros = 0;
		bits->removed++;
		if (bits->at >= bits->length) {
			bits->error = true;
			return 0;
		}
		byte = bits->bytes[bits->at];
	}

	/* The byte, and the zeros it continues or ends. */
	bits->at++;
	if (byte == 0U)
		bits->zeros++;
	else
		bits->zeros = 0;

	/* Succeeded: the byte. */
	return byte;
}

/* Reads count bits (at most 32), most significant first; 0 after an error. */
static uint32_t
bits_read(
	struct h265_bits *bits,
	uint32_t count)
{
	uint32_t value;
	uint32_t index;

	/* Each bit, taking a new byte when the last one is used up. */
	value = 0;
	for (index = 0; index < count && !bits->error; index++) {
		if (bits->left == 0U) {
			bits->current = bits_byte(bits);
			bits->left = 8U;
		}
		bits->left--;
		value = (value << 1) | ((bits->current >> bits->left) & 1U);
	}

	/* A failed read reads as 0. */
	if (bits->error)
		return 0;

	/* Succeeded: the bits. */
	return value;
}

/* Reads an unsigned exp-Golomb code (9.2); a code longer than 32 bits is an error. */
static uint32_t
bits_golomb(
	struct h265_bits *bits)
{
	uint32_t zeros;
	uint32_t suffix;

	/* The leading zeros, up to the limit. */
	zeros = 0;
	while (bits_read(bits, 1U) == 0U && !bits->error) {
		zeros++;
		if (zeros > H265_GOLOMB_ZEROS) {
			bits->error = true;
			return 0;
		}
	}

	/* The suffix of as many bits. */
	suffix = bits_read(bits, zeros);
	if (bits->error)
		return 0;

	/* Succeeded: 2^zeros - 1 plus the suffix. */
	return (uint32_t)(((uint64_t)1U << zeros) - 1U + suffix);
}

/* Reads a signed exp-Golomb code (9.2.2). */
static int32_t
bits_signed(
	struct h265_bits *bits)
{
	uint32_t code;

	/* Odd codes are positive, even ones negative. */
	code = bits_golomb(bits);
	if ((code & 1U) != 0U)
		return (int32_t)((code >> 1) + 1U);

	/* Succeeded: the negative (or zero) value. */
	return -(int32_t)(code >> 1);
}

/* Reports the bits left in the segment, counting emulation prevention bytes as data (an upper bound). */
static uint64_t
bits_remaining(
	const struct h265_bits *bits)
{
	/* The bytes not read, and the bits of the current byte. */
	return (uint64_t)(bits->length - bits->at) * 8U + bits->left;
}

/* Reports Ceil( Log2( value ) ): the bits that number values from 0 (0 for one value or none). */
static uint32_t
ceil_log2(
	uint32_t value)
{
	uint32_t bits;

	/* The smallest power of two at or above the value. */
	bits = 0;
	while (bits < 32U && ((uint64_t)1U << bits) < value)
		bits++;

	/* Succeeded: its exponent. */
	return bits;
}

/* Shifts a signed value right, rounding toward minus infinity as H.265's >> does. */
static int32_t
shift_floor(
	int32_t value,
	uint32_t shift)
{
	/* A negative value: the negated quotient rounded up. */
	if (value < 0)
		return -(int32_t)((uint32_t)(-(int64_t)value + ((int64_t)1 << shift) - 1) >> shift);

	/* Succeeded: the plain shift. */
	return (int32_t)((uint32_t)value >> shift);
}

/* Clips a value to a range. */
static int32_t
clip(
	int32_t value,
	int32_t low,
	int32_t high)
{
	/* Below and above the range. */
	if (value < low)
		return low;
	if (value > high)
		return high;

	/* Succeeded: within it. */
	return value;
}

/*
 * Counts the entries of a reference picture set: up to its last entry
 * that names a slot, so an entry of no slot before it (a missing picture,
 * which H.265 still counts in NumPicTotalCurr) keeps its place.
 */
static uint32_t
set_count(
	const uint8_t *set)
{
	uint32_t count;
	uint32_t index;

	/* One past the last entry that names a slot. */
	count = 0;
	for (index = 0; index < I915_VIDEO_H265_SET_ENTRIES; index++) {
		if (set[index] != I915_VIDEO_H265_NO_SLOT)
			count = index + 1U;
	}

	/* Succeeded: the set's size. */
	return count;
}

/*
 * Reads a slice's reference fields up to the long-term pictures: the
 * order count bits, the short-term set (its bits skipped as Vulkan counts
 * them, or its index) and the long-term pictures.
 */
static const char *
read_references(
	struct h265_bits *bits,
	const struct i915_video_h265_sps *sps,
	const struct i915_video_h265_picture *picture)
{
	uint32_t index;
	uint32_t remaining;
	uint32_t chunk;
	bool from_sps;
	const char *reason;

	/* slice_pic_order_cnt_lsb, then short_term_ref_pic_set_sps_flag. */
	(void)bits_read(bits, sps->log2_max_poc_lsb_minus4 + 4U);
	from_sps = bits_read(bits, 1U) != 0U;

	/* A set in the header is skipped by its size; one of the sequence's is named by index. */
	if (!from_sps) {
		if (picture->short_term_bits > H265_SHORT_TERM_BITS_MAX || picture->short_term_bits > bits_remaining(bits))
			return "slice short-term set past its bytes";
		remaining = picture->short_term_bits;
		while (remaining != 0U && !bits->error) {
			chunk = remaining;
			if (chunk > 32U)
				chunk = 32U;
			(void)bits_read(bits, chunk);
			remaining -= chunk;
		}
	} else {
		if (sps->short_term_sets == 0U)
			return "slice names a short-term set the sequence has none of";
		index = 0;
		if (sps->short_term_sets > 1U)
			index = bits_read(bits, ceil_log2(sps->short_term_sets));
		if (index >= sps->short_term_sets)
			return "slice short-term set index out of range";
	}

	/* The long-term pictures. */
	if ((sps->flags & I915_VIDEO_H265_SPS_LONG_TERM) != 0U) {
		reason = read_long_term(bits, sps);
		if (reason != NULL)
			return reason;
	}
	if (bits->error)
		return "slice segment header past its bytes";

	/* Succeeded: the fields are read past. */
	return NULL;
}

/* Reads past a slice's long-term pictures (7.3.6.1), bounding their count. */
static const char *
read_long_term(
	struct h265_bits *bits,
	const struct i915_video_h265_sps *sps)
{
	uint32_t from_sps;
	uint32_t pictures;
	uint32_t index;
	uint32_t cycle;

	/* The counts: of the sequence's pictures, then of the header's own. */
	from_sps = 0;
	if (sps->long_term_sps > 0U)
		from_sps = bits_golomb(bits);
	pictures = bits_golomb(bits);
	if (bits->error || from_sps > sps->long_term_sps || pictures > H265_LONG_TERM_MAX || from_sps + pictures > H265_LONG_TERM_MAX)
		return "slice long-term pictures out of range";

	/* Each picture: its index or its order count and use, and its order count's high part. */
	for (index = 0; index < from_sps + pictures && !bits->error; index++) {
		if (index < from_sps) {
			if (sps->long_term_sps > 1U)
				(void)bits_read(bits, ceil_log2(sps->long_term_sps));
		} else {
			(void)bits_read(bits, sps->log2_max_poc_lsb_minus4 + 4U);
			(void)bits_read(bits, 1U);
		}

		/* delta_poc_msb_present_flag and delta_poc_msb_cycle_lt, at most 2^( 32 - log2_max_poc_lsb ). */
		if (bits_read(bits, 1U) != 0U) {
			cycle = bits_golomb(bits);
			if ((uint64_t)cycle > ((uint64_t)1U << (28U - sps->log2_max_poc_lsb_minus4)))
				return "slice long-term order count out of range";
		}
	}
	if (bits->error)
		return "slice segment header past its bytes";

	/* Succeeded: the pictures are read past. */
	return NULL;
}

/*
 * Builds a reference list as DPB slots (8.3.4): the temporary list of the
 * three current sets in the list's order, repeated to the larger of its
 * length and the sets' total, then the entries the modification names (or
 * the first ones).  A set's entry of a picture the decode does not have
 * takes the first picture it has (8.3.3 makes one up).
 */
static const char *
build_list(
	const struct i915_video_h265_picture *picture,
	const struct i915_video_h265_slot *slots,
	uint32_t list,
	uint32_t count,
	bool modified,
	const uint8_t *entries,
	uint32_t total,
	uint8_t *out)
{
	uint8_t temporary[H265_TEMP_LIST];
	const uint8_t *first;
	const uint8_t *second;
	uint32_t length;
	uint32_t used;
	uint32_t index;
	uint32_t entry;
	uint8_t substitute;

	/* List 0 takes the pictures before the current first, list 1 those after. */
	first = picture->before;
	second = picture->after;
	if (list != 0U) {
		first = picture->after;
		second = picture->before;
	}

	/* The temporary list, the sets again and again until it is long enough (NumRpsCurrTempListX). */
	length = count;
	if (total > length)
		length = total;
	if (length > H265_TEMP_LIST)
		return "reference list too long";
	used = 0;
	while (used < length) {
		for (index = 0; index < set_count(first) && used < length; index++)
			temporary[used++] = first[index];
		for (index = 0; index < set_count(second) && used < length; index++)
			temporary[used++] = second[index];
		for (index = 0; index < set_count(picture->long_term) && used < length; index++)
			temporary[used++] = picture->long_term[index];
	}

	/* A missing picture's place takes the first picture the decode does reference. */
	substitute = I915_VIDEO_H265_NO_SLOT;
	for (index = 0; index < used && substitute == I915_VIDEO_H265_NO_SLOT; index++) {
		if (temporary[index] < I915_VIDEO_H265_SLOTS && slots[temporary[index]].present)
			substitute = temporary[index];
	}
	if (substitute == I915_VIDEO_H265_NO_SLOT)
		return "predicted slice without a reference picture";
	for (index = 0; index < used; index++) {
		if (temporary[index] >= I915_VIDEO_H265_SLOTS || !slots[temporary[index]].present)
			temporary[index] = substitute;
	}

	/* The list's entries. */
	for (index = 0; index < count; index++) {
		entry = index;
		if (modified)
			entry = entries[index];
		if (entry >= length)
			return "reference list entry out of range";
		out[index] = temporary[entry];
	}

	/* Succeeded: the list names the slots. */
	return NULL;
}

/*
 * Reads a predicted slice's fields from the reference counts to the merge
 * candidates: the counts, the list modification and the lists, the zero
 * L1 differences, the CABAC initialization, the collocated picture, the
 * weights and MaxNumMergeCand.
 */
static const char *
read_predicted(
	struct h265_bits *bits,
	const struct i915_video_h265_pps *pps,
	const struct i915_video_h265_picture *picture,
	const struct i915_video_h265_slot *slots,
	struct i915_video_h265_slice *slice)
{
	uint8_t entries[2][I915_VIDEO_H265_LIST];
	const char *reason;
	bool modified[2];
	bool b_slice;
	uint32_t total;
	uint32_t lists;
	uint32_t list;
	uint32_t index;
	uint32_t count;
	uint32_t candidates;

	/* The active counts: the slice's own when overridden, else the picture's (1 to 15). */
	b_slice = slice->slice_type == I915_VIDEO_H265_SLICE_B;
	lists = 1U;
	if (b_slice)
		lists = 2U;
	slice->references[0] = pps->l0_minus1 + 1U;
	slice->references[1] = 0;
	if (b_slice)
		slice->references[1] = pps->l1_minus1 + 1U;
	if (bits_read(bits, 1U) != 0U) {
		slice->references[0] = bits_golomb(bits) + 1U;
		if (b_slice)
			slice->references[1] = bits_golomb(bits) + 1U;
	}
	if (bits->error || slice->references[0] > I915_VIDEO_H265_LIST || slice->references[1] > I915_VIDEO_H265_LIST)
		return "slice reference count out of range";

	/* NumPicTotalCurr: the three current sets' pictures, of which a predicted slice needs one. */
	total = set_count(picture->before) + set_count(picture->after) + set_count(picture->long_term);
	if (total == 0U)
		return "predicted slice without a current reference set";

	/* The modification: each list's entries, Ceil( Log2( NumPicTotalCurr ) ) bits each and below it. */
	modified[0] = false;
	modified[1] = false;
	if ((pps->flags & I915_VIDEO_H265_PPS_LISTS_MODIFICATION) != 0U && total > 1U) {
		for (list = 0; list < lists; list++) {
			modified[list] = bits_read(bits, 1U) != 0U;
			if (!modified[list])
				continue;
			for (index = 0; index < slice->references[list]; index++) {
				entries[list][index] = (uint8_t)bits_read(bits, ceil_log2(total));
				if (entries[list][index] >= total)
					return "slice list modification out of range";
			}
		}
	}
	if (bits->error)
		return "slice segment header past its bytes";

	/* The lists as slots. */
	for (list = 0; list < lists; list++) {
		reason = build_list(picture, slots, list, slice->references[list], modified[list], entries[list], total, slice->list[list]);
		if (reason != NULL)
			return reason;
	}

	/* The zero L1 differences of a B slice and the CABAC initialization. */
	slice->mvd_l1_zero = false;
	if (b_slice)
		slice->mvd_l1_zero = bits_read(bits, 1U) != 0U;
	slice->cabac_init = false;
	if ((pps->flags & I915_VIDEO_H265_PPS_CABAC_INIT) != 0U)
		slice->cabac_init = bits_read(bits, 1U) != 0U;

	/* The collocated picture: its list (L0 unless a B slice says L1) and its index there. */
	slice->collocated_from_l0 = true;
	slice->collocated_ref_idx = 0;
	if (slice->temporal_mvp) {
		if (b_slice)
			slice->collocated_from_l0 = bits_read(bits, 1U) != 0U;
		count = slice->references[1];
		if (slice->collocated_from_l0)
			count = slice->references[0];
		if (count > 1U) {
			slice->collocated_ref_idx = bits_golomb(bits);
			if (bits->error || slice->collocated_ref_idx >= count)
				return "slice collocated reference out of range";
		}
	}

	/* The weights, when the picture asks for them for this type. */
	slice->weighted = false;
	if (!b_slice && (pps->flags & I915_VIDEO_H265_PPS_WEIGHTED) != 0U)
		slice->weighted = true;
	if (b_slice && (pps->flags & I915_VIDEO_H265_PPS_BIPRED) != 0U)
		slice->weighted = true;
	if (slice->weighted) {
		reason = read_weights(bits, slice);
		if (reason != NULL)
			return reason;
	}

	/* MaxNumMergeCand: 5 - five_minus_max_num_merge_cand, 1 to 5. */
	candidates = bits_golomb(bits);
	if (bits->error || candidates > 4U)
		return "slice merge candidates out of range";
	slice->max_merge_candidates = 5U - candidates;

	/* Succeeded: the predicted slice's fields. */
	return NULL;
}

/*
 * Reads pred_weight_table() (7.3.6.3): the denominators (0 to 7), then
 * each list's weights.  The picture is 4:2:0, so chroma weights are sent.
 */
static const char *
read_weights(
	struct h265_bits *bits,
	struct i915_video_h265_slice *slice)
{
	const char *reason;
	int32_t denominator;

	/* luma_log2_weight_denom and ChromaLog2WeightDenom. */
	slice->weights.luma_log2_denom = bits_golomb(bits);
	if (bits->error || slice->weights.luma_log2_denom > 7U)
		return "slice weight denominator out of range";
	denominator = (int32_t)slice->weights.luma_log2_denom + bits_signed(bits);
	if (bits->error || denominator < 0 || denominator > 7)
		return "slice weight denominator out of range";
	slice->weights.chroma_log2_denom = (uint32_t)denominator;

	/* The lists' weights: L0, then L1 of a B slice. */
	reason = read_list_weights(bits, 0U, slice);
	if (reason == NULL && slice->slice_type == I915_VIDEO_H265_SLICE_B)
		reason = read_list_weights(bits, 1U, slice);

	/* Succeeded, or the first list's reason. */
	return reason;
}

/* Reads one list's weights (7.3.6.3) into the slice. */
static const char *
read_list_weights(
	struct h265_bits *bits,
	uint32_t list,
	struct i915_video_h265_slice *slice)
{
	struct i915_video_h265_weights *weights;
	uint32_t count;
	uint32_t index;
	uint32_t component;
	int32_t delta;
	int32_t offset;
	int32_t weight;

	/* The list's entries and the flags of each. */
	weights = &slice->weights;
	count = slice->references[list];
	for (index = 0; index < count; index++)
		weights->luma_sent[list][index] = bits_read(bits, 1U) != 0U;
	for (index = 0; index < count; index++)
		weights->chroma_sent[list][index] = bits_read(bits, 1U) != 0U;

	/* Each entry's weights and offsets, the defaults where its flag is clear. */
	for (index = 0; index < count && !bits->error; index++) {
		/* Luma: LumaWeightLX and luma_offset_lX, both -128 to 127 at eight bits. */
		weights->luma_weight[list][index] = 1 << weights->luma_log2_denom;
		weights->luma_offset[list][index] = 0;
		if (weights->luma_sent[list][index]) {
			delta = bits_signed(bits);
			offset = bits_signed(bits);
			if (delta < -128 || delta > 127 || offset < -128 || offset > 127)
				return "slice luma weight out of range";
			weights->luma_weight[list][index] += delta;
			weights->luma_offset[list][index] = offset;
		}

		/* Chroma: ChromaWeightLX and ChromaOffsetLX (7-56) of Cb and Cr. */
		for (component = 0; component < 2U; component++) {
			weights->chroma_weight[list][index][component] = 1 << weights->chroma_log2_denom;
			weights->chroma_offset[list][index][component] = 0;
			if (!weights->chroma_sent[list][index])
				continue;
			delta = bits_signed(bits);
			offset = bits_signed(bits);
			if (delta < -128 || delta > 127 || offset < -512 || offset > 511)
				return "slice chroma weight out of range";
			weight = (1 << weights->chroma_log2_denom) + delta;
			weights->chroma_weight[list][index][component] = weight;
			weights->chroma_offset[list][index][component] =
				clip(128 + offset - shift_floor(128 * weight, weights->chroma_log2_denom), -128, 127);
		}
	}
	if (bits->error)
		return "slice segment header past its bytes";

	/* Succeeded: the list's weights. */
	return NULL;
}

/*
 * Reads a segment's tail: its entry points (their offsets read past, the
 * decoder finds them itself), the header extension, the byte alignment,
 * and where its data starts (past an emulation prevention byte there).
 */
static const char *
read_tail(
	struct h265_bits *bits,
	const struct i915_video_h265_pps *pps,
	struct i915_video_h265_slice *slice)
{
	uint32_t length;
	uint32_t index;
	uint64_t needed;

	/* The entry points with tiles or wavefronts: a count, the offsets' length, the offsets. */
	slice->entry_points = 0;
	if ((pps->flags & (I915_VIDEO_H265_PPS_TILES | I915_VIDEO_H265_PPS_ENTROPY_SYNC)) != 0U) {
		slice->entry_points = bits_golomb(bits);
		if (bits->error || slice->entry_points > H265_ENTRY_POINTS_MAX)
			return "slice entry points out of range";
		if (slice->entry_points != 0U) {
			length = bits_golomb(bits) + 1U;
			needed = (uint64_t)length * slice->entry_points;
			if (bits->error || length > 32U || needed > bits_remaining(bits))
				return "slice entry points past its bytes";
			for (index = 0; index < slice->entry_points && !bits->error; index++)
				(void)bits_read(bits, length);
		}
	}

	/* The extension: a length and as many bytes. */
	if ((pps->flags & I915_VIDEO_H265_PPS_HEADER_EXTENSION) != 0U) {
		length = bits_golomb(bits);
		if (bits->error || length > H265_EXTENSION_MAX)
			return "slice header extension too long";
		for (index = 0; index < length && !bits->error; index++)
			(void)bits_read(bits, 8U);
	}

	/* byte_alignment(): a one, then zeros to the byte's end. */
	if (bits_read(bits, 1U) != 1U || bits->error)
		return "slice segment header not byte aligned";
	(void)bits_read(bits, bits->left);
	if (bits->error)
		return "slice segment header past its bytes";

	/* The header's bytes as the decoder counts them, without the emulation prevention bytes in it. */
	slice->header_bytes = bits->at - slice->nal_offset - bits->removed;

	/* The data's first byte, which an emulation prevention byte there does not begin. */
	if (bits->at < bits->length && bits->zeros >= 2U && bits->bytes[bits->at] == 3U)
		bits->at++;
	if (bits->at >= bits->length)
		return "slice segment without data";
	slice->data_offset = bits->at;

	/* Succeeded: the segment's data is located. */
	return NULL;
}

/* Copies a slice's fields (clears them for NULL). */
static void
copy_slice(
	const struct i915_video_h265_slice *from,
	struct i915_video_h265_slice *to)
{
	const uint8_t *source;
	uint8_t *destination;
	size_t index;

	/* Byte by byte: the file uses nothing of the kernel. */
	destination = (uint8_t *)to;
	source = (const uint8_t *)from;
	for (index = 0; index < sizeof(*to); index++) {
		if (from == NULL)
			destination[index] = 0;
		else
			destination[index] = source[index];
	}
}

/*
 * Divides total CTBs into parts and reports the parts + 1 boundaries:
 * evenly (6-3, 6-4), or as sent (each part but the last its size minus 1,
 * cut so every later part keeps a CTB; the last takes the rest).  parts
 * is at most total.
 */
static void
spacing(
	uint32_t total,
	uint32_t parts,
	bool uniform,
	const uint32_t *minus1,
	uint32_t *bounds)
{
	uint32_t index;
	uint64_t size;
	uint64_t room;

	/* The first boundary is the picture's edge. */
	bounds[0] = 0;

	/* Evenly: each part's end at its share of the total. */
	if (uniform) {
		for (index = 0; index < parts; index++)
			bounds[index + 1U] = (uint32_t)(((uint64_t)(index + 1U) * total) / parts);
		return;
	}

	/* As sent, each within what the later parts leave, the last what the others leave. */
	for (index = 0; index + 1U < parts; index++) {
		size = (uint64_t)minus1[index] + 1U;
		room = (uint64_t)total - bounds[index] - (parts - 1U - index);
		if (size > room)
			size = room;
		if (size == 0U)
			size = 1U;
		bounds[index + 1U] = bounds[index] + (uint32_t)size;
	}
	bounds[parts] = total;
}

/*
 * Puts a coded scaling list of side 4 or 8 into raster order: the coded
 * entries walk each anti-diagonal from its bottom left up to its top
 * right (6.5.3).
 */
static void
raster(
	uint32_t side,
	const uint8_t *coded,
	uint8_t *out)
{
	uint32_t count;
	uint32_t index;
	int32_t diagonal;
	int32_t column;
	int32_t row;

	/* Each anti-diagonal, entry by entry into its raster place. */
	count = side * side;
	index = 0;
	for (diagonal = 0; diagonal < (int32_t)(2U * side - 1U); diagonal++) {
		column = 0;
		row = diagonal;
		while (row >= 0) {
			if (column < (int32_t)side && row < (int32_t)side && index < count) {
				out[(uint32_t)row * side + (uint32_t)column] = coded[index];
				index++;
			}
			row--;
			column++;
		}
	}
}
