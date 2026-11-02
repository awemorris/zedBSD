/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Raspberry Pi 5's H.265 decode arithmetic (see vulkan-video-hevc.h):
 * the slice segment headers read from the bitstream, and the decoder's
 * phase 1 command list and phase 2 values built from them.  It touches no
 * hardware and calls nothing of the kernel, so a host test runs it.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/vk/vulkan-video-hevc.h"

/* The NAL unit types: the last decodable non-IRAP picture, the IRAP range, and the two IDR pictures. */
#define HEVC_NAL_LAST_PICTURE		9U
#define HEVC_NAL_BLA_W_LP		16U
#define HEVC_NAL_IRAP_DECODABLE_LAST	21U
#define HEVC_NAL_IRAP_LAST		23U
#define HEVC_NAL_IDR_W_RADL		19U
#define HEVC_NAL_IDR_N_LP		20U

/* The longest exp-Golomb code read (leading zeros). */
#define HEVC_GOLOMB_ZEROS		31U

/* Bounds on header counts that keep every loop short: long-term pictures, entry points, extension bytes. */
#define HEVC_LONG_TERM_MAX		32U
#define HEVC_ENTRY_POINTS_MAX		4096U
#define HEVC_EXTENSION_MAX		256U
#define HEVC_SHORT_TERM_BITS_MAX	4096U

/* The longest temporary reference list (8.3.4): the larger of 15 and the 24 entries of the three sets. */
#define HEVC_TEMP_LIST			24U

/* The coefficient of a flat list and of a default DC value. */
#define HEVC_FLAT			16U

/*
 * The decoder's phase 1 registers that the command list writes, and the
 * windows its tables are written into.
 *
 * Values referenced: Raspberry Pi Linux rpi-6.12.y (7821b6a1)
 * drivers/media/platform/raspberrypi/hevc_dec/hevc_d_hw.h, the "Phase 1
 * Register offsets" (RPI_SPS0 to RPI_CFSTATUS); hevc_d_h265.c write_prob
 * (0x1000), write_scaling_factors (0x2000) and program_slicecmds
 * (0x4000).
 */
#define REG_SPS0			0x00U
#define REG_SPS1			0x04U
#define REG_PPS				0x08U
#define REG_SLICE			0x0cU
#define REG_TILESTART			0x10U
#define REG_TILEEND			0x14U
#define REG_SLICESTART			0x18U
#define REG_MODE			0x1cU
#define REG_QP				0x30U
#define REG_CONTROL			0x34U
#define REG_STATUS			0x38U
#define REG_BFBASE			0x40U
#define REG_BFNUM			0x44U
#define REG_BFCONTROL			0x48U
#define REG_SLICECMDS			0x60U
#define REG_BEGINTILEEND		0x64U
#define REG_TRANSFER			0x68U
#define WINDOW_PROBABILITIES		0x1000U
#define WINDOW_SCALING			0x2000U
#define WINDOW_SLICE_MESSAGES		0x4000U

/*
 * The context transfer words (save the probabilities after writing them,
 * or reload the saved ones), the entry point pause modes, and the
 * bitstream control bits (stop, and remove emulation prevention bytes).
 *
 * Values referenced: hevc_d_h265.c (rpi-6.12.y, 7821b6a1) PROB_BACKUP,
 * PROB_RELOAD, PAUSE_MODE_WPP, PAUSE_MODE_TILE, write_bitstream.
 */
#define TRANSFER_BACKUP			((20U << 12) + (20U << 6))
#define TRANSFER_RELOAD			((20U << 12) + 20U)
#define PAUSE_WAVEFRONT			1U
#define PAUSE_TILE			0xffffU
#define BITSTREAM_STOP			(1U << 7)
#define BITSTREAM_UNESCAPE		(1U << 6)

/* The probability values written (154, rounded up to whole words), and the scaling factor table's bytes. */
#define PROBABILITY_VALUES		154U
#define PROBABILITY_BYTES		156U
#define SCALING_BYTES			4064U

/* The slice messages one segment has at most: the slice word, eight for each reference of both lists, and two. */
#define SLICE_MESSAGES			(2U * BCM2712_HEVC_SLOTS * 8U + 3U)

/*
 * The context variables' initial values, in the decoder's order, for each
 * initialization type (0: I, 1 and 2: P and B as cabac_init_flag picks).
 *
 * Values referenced: hevc_d_h265.c (rpi-6.12.y, 7821b6a1) prob_init (the
 * initValue entries of Recommendation ITU-T H.265 Tables 9-5 to 9-37 in
 * the decoder's order).
 */
static const uint8_t hevc_initial_values[3][PROBABILITY_BYTES] = {
	{
		153, 200, 139, 141, 157, 154, 154, 154, 154, 154, 184, 154, 154,
		154, 184, 63,  154, 154, 154, 154, 154, 154, 154, 154, 154, 154,
		154, 154, 154, 153, 138, 138, 111, 141, 94,  138, 182, 154, 154,
		154, 140, 92,  137, 138, 140, 152, 138, 139, 153, 74,  149, 92,
		139, 107, 122, 152, 140, 179, 166, 182, 140, 227, 122, 197, 110,
		110, 124, 125, 140, 153, 125, 127, 140, 109, 111, 143, 127, 111,
		79,  108, 123, 63,  110, 110, 124, 125, 140, 153, 125, 127, 140,
		109, 111, 143, 127, 111, 79,  108, 123, 63,  91,  171, 134, 141,
		138, 153, 136, 167, 152, 152, 139, 139, 111, 111, 125, 110, 110,
		94,  124, 108, 124, 107, 125, 141, 179, 153, 125, 107, 125, 141,
		179, 153, 125, 107, 125, 141, 179, 153, 125, 140, 139, 182, 182,
		152, 136, 152, 136, 153, 136, 139, 111, 136, 139, 111, 0,   0
	},
	{
		153, 185, 107, 139, 126, 197, 185, 201, 154, 149, 154, 139, 154,
		154, 154, 152, 110, 122, 95,  79,  63,  31,  31,  153, 153, 168,
		140, 198, 79,  124, 138, 94,  153, 111, 149, 107, 167, 154, 154,
		154, 154, 196, 196, 167, 154, 152, 167, 182, 182, 134, 149, 136,
		153, 121, 136, 137, 169, 194, 166, 167, 154, 167, 137, 182, 125,
		110, 94,  110, 95,  79,  125, 111, 110, 78,  110, 111, 111, 95,
		94,  108, 123, 108, 125, 110, 94,  110, 95,  79,  125, 111, 110,
		78,  110, 111, 111, 95,  94,  108, 123, 108, 121, 140, 61,  154,
		107, 167, 91,  122, 107, 167, 139, 139, 155, 154, 139, 153, 139,
		123, 123, 63,  153, 166, 183, 140, 136, 153, 154, 166, 183, 140,
		136, 153, 154, 166, 183, 140, 136, 153, 154, 170, 153, 123, 123,
		107, 121, 107, 121, 167, 151, 183, 140, 151, 183, 140, 0,   0
	},
	{
		153, 160, 107, 139, 126, 197, 185, 201, 154, 134, 154, 139, 154,
		154, 183, 152, 154, 137, 95,  79,  63,  31,  31,  153, 153, 168,
		169, 198, 79,  224, 167, 122, 153, 111, 149, 92,  167, 154, 154,
		154, 154, 196, 167, 167, 154, 152, 167, 182, 182, 134, 149, 136,
		153, 121, 136, 122, 169, 208, 166, 167, 154, 152, 167, 182, 125,
		110, 124, 110, 95,  94,  125, 111, 111, 79,  125, 126, 111, 111,
		79,  108, 123, 93,  125, 110, 124, 110, 95,  94,  125, 111, 111,
		79,  125, 126, 111, 111, 79,  108, 123, 93,  121, 140, 61,  154,
		107, 167, 91,  107, 107, 167, 139, 139, 170, 154, 139, 153, 139,
		123, 123, 63,  124, 166, 183, 140, 136, 153, 154, 166, 183, 140,
		136, 153, 154, 166, 183, 140, 136, 153, 154, 170, 153, 138, 138,
		122, 121, 122, 121, 167, 151, 183, 140, 151, 183, 140, 0,   0
	}
};

/*
 * Where each scaling matrix starts in the decoder's factor table: sizeId 0
 * to 3, matrixId 0 to 5 (sizeId 3 has two).
 *
 * Values referenced: hevc_d_h265.c (rpi-6.12.y, 7821b6a1)
 * populate_scaling_factors, scaling_factor_offsets.
 */
static const uint16_t hevc_scaling_offsets[4][6] = {
	{ 0x0000, 0x0010, 0x0020, 0x0030, 0x0040, 0x0050 },
	{ 0x0060, 0x00a0, 0x00e0, 0x0120, 0x0160, 0x01a0 },
	{ 0x01e0, 0x02e0, 0x03e0, 0x04e0, 0x05e0, 0x06e0 },
	{ 0x07e0, 0x0be0, 0x0000, 0x0000, 0x0000, 0x0000 }
};

/*
 * The default 8x8 scaling lists (used for the 8x8, 16x16 and 32x32
 * sizes), in the coded (up-right diagonal) order.
 *
 * Values referenced: Recommendation ITU-T H.265 (Table 7-6, the default
 * values of ScalingList[ 1..3 ][ matrixId ][ i ]: matrixId 0 to 2 intra,
 * 3 to 5 inter).
 */
static const uint8_t hevc_default_intra[64] = {
	16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 17, 16, 17, 16, 17, 18,
	17, 18, 18, 17, 18, 21, 19, 20, 21, 20, 19, 21, 24, 22, 22, 24,
	24, 22, 22, 24, 25, 25, 27, 30, 27, 25, 25, 29, 31, 35, 35, 31,
	29, 36, 41, 44, 41, 36, 47, 54, 54, 47, 65, 70, 65, 88, 88, 115
};

/* The default inter list, read the same way. */
static const uint8_t hevc_default_inter[64] = {
	16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 17, 17, 17, 17, 17, 18,
	18, 18, 18, 18, 18, 20, 20, 20, 20, 20, 20, 20, 24, 24, 24, 24,
	24, 24, 24, 24, 25, 25, 25, 25, 25, 25, 25, 28, 28, 28, 28, 28,
	28, 33, 33, 33, 33, 33, 41, 41, 41, 41, 54, 54, 54, 71, 71, 91
};

/*
 * The bits of a slice segment header as the parse reads them: the
 * segment's bytes (emulation prevention bytes in place), how many, the
 * next byte, the byte being read and its bits left, the zero bytes just
 * read (for the emulation prevention), and whether a read went past the
 * end or broke a code.
 */
struct hevc_bits {
	const uint8_t *bytes;
	size_t length;
	size_t at;
	uint32_t current;
	uint32_t left;
	uint32_t zeros;
	bool error;
};

/*
 * The picture's CTB geometry: the CTB's size, the picture in CTBs, and the
 * tiles' column and row boundaries in CTBs (columns + 1 and rows + 1 of
 * them, the first 0 and the last the picture's).
 */
struct hevc_geometry {
	uint32_t log2_ctb;
	uint32_t width_ctbs;
	uint32_t height_ctbs;
	uint32_t columns;
	uint32_t rows;
	uint32_t column_bounds[BCM2712_HEVC_TILE_COLUMNS + 1U];
	uint32_t row_bounds[BCM2712_HEVC_TILE_ROWS + 1U];
};

/*
 * The state of building one picture's command list: where it goes, the
 * sets and geometry, the entry point last written (its tile, CTB, QP and
 * slice word, which the next fills repeat), the slice start of the last
 * independent segment, the current segment's messages and derived values
 * (its first CTB in tile scan and its place, the previous segment's last
 * CTB, the QP, the reference counts and merge candidates), and the slot
 * of the collocated picture.
 */
struct hevc_program {
	struct bcm2712_hevc_commands *commands;
	const struct bcm2712_hevc_sps *sps;
	const struct bcm2712_hevc_pps *pps;
	const struct bcm2712_hevc_picture *picture;
	const struct bcm2712_hevc_slot *slots;
	struct hevc_geometry geometry;
	uint32_t entry_tile_x;
	uint32_t entry_tile_y;
	uint32_t entry_x;
	uint32_t entry_y;
	uint32_t entry_qp;
	uint32_t entry_slice;
	uint32_t slice_start;
	uint16_t messages[SLICE_MESSAGES];
	uint32_t message_count;
	uint32_t start_ts;
	uint32_t start_x;
	uint32_t start_y;
	uint32_t previous_x;
	uint32_t previous_y;
	uint32_t slice_qp;
	uint32_t collocated_slot;
	uint8_t scaling[SCALING_BYTES];
};

static void bits_start(struct hevc_bits *bits, const uint8_t *bytes, size_t length, size_t at);
static uint32_t bits_byte(struct hevc_bits *bits);
static uint32_t bits_read(struct hevc_bits *bits, uint32_t count);
static uint32_t bits_golomb(struct hevc_bits *bits);
static int32_t bits_signed(struct hevc_bits *bits);
static uint64_t bits_remaining(const struct hevc_bits *bits);
static uint32_t ceil_log2(uint32_t value);
static int32_t shift_floor(int32_t value, uint32_t shift);
static int32_t clip(int32_t value, int32_t low, int32_t high);
static uint32_t set_count(const uint8_t *set);
static int read_references(struct hevc_bits *bits, const struct bcm2712_hevc_sps *sps, const struct bcm2712_hevc_picture *picture);
static int read_long_term(struct hevc_bits *bits, const struct bcm2712_hevc_sps *sps);
static int build_list(const struct bcm2712_hevc_picture *picture, const struct bcm2712_hevc_slot *slots, uint32_t list, uint32_t count, bool modified, const uint8_t *entries, uint32_t total, uint8_t *out);
static int read_predicted(struct hevc_bits *bits, const struct bcm2712_hevc_pps *pps, const struct bcm2712_hevc_picture *picture, const struct bcm2712_hevc_slot *slots, struct bcm2712_hevc_slice *slice);
static int read_weights(struct hevc_bits *bits, struct bcm2712_hevc_slice *slice);
static int read_list_weights(struct hevc_bits *bits, bool chroma, uint32_t list, struct bcm2712_hevc_slice *slice);
static int read_tail(struct hevc_bits *bits, const struct bcm2712_hevc_pps *pps, struct bcm2712_hevc_slice *slice);
static void copy_slice(const struct bcm2712_hevc_slice *from, struct bcm2712_hevc_slice *to);
static void geometry_make(const struct bcm2712_hevc_sps *sps, const struct bcm2712_hevc_pps *pps, struct hevc_geometry *geometry);
static void spacing(uint32_t total, uint32_t parts, bool uniform, const uint32_t *minus1, uint32_t *bounds);
static uint32_t tile_of(const uint32_t *bounds, uint32_t parts, uint32_t ctb);
static uint32_t raster_to_tile_scan(const struct hevc_geometry *geometry, uint32_t x, uint32_t y);
static void tile_scan_to_raster(const struct hevc_geometry *geometry, uint32_t ts, uint32_t *x, uint32_t *y);
static void emit(struct hevc_program *program, uint32_t reg, uint32_t value);
static uint32_t slice_word(const struct bcm2712_hevc_slice *slice);
static void write_slice_size(struct hevc_program *program, uint32_t word, uint32_t column, uint32_t row);
static void entry_point(struct hevc_program *program, bool tile_end, bool reset_qp, uint32_t pause, uint32_t tile_x, uint32_t tile_y, uint32_t column, uint32_t row, uint32_t qp, uint32_t word);
static void wavefront_pause(struct hevc_program *program, uint32_t row);
static void wavefront_fill(struct hevc_program *program, uint32_t last_row);
static void wavefront_end_previous(struct hevc_program *program);
static void wavefront_segment(struct hevc_program *program, const struct bcm2712_hevc_slice *slice, uint32_t index, bool last);
static void tile_fill(struct hevc_program *program, uint32_t last_x, uint32_t last_y);
static void tile_end_previous(struct hevc_program *program);
static void tile_segment(struct hevc_program *program, const struct bcm2712_hevc_slice *slice, uint32_t index, bool last);
static void slice_messages(struct hevc_program *program, const struct bcm2712_hevc_slice *slice);
static void reference_messages(struct hevc_program *program, const struct bcm2712_hevc_slice *slice, uint32_t list);
static void write_bitstream(struct hevc_program *program, const struct bcm2712_hevc_slice *slice);
static void write_probabilities(struct hevc_program *program, const struct bcm2712_hevc_slice *slice);
static void write_messages(struct hevc_program *program, uint32_t index);
static void write_segment(struct hevc_program *program, const struct bcm2712_hevc_slice *slice);
static void scaling_table(const struct bcm2712_hevc_sps *sps, const struct bcm2712_hevc_pps *pps, uint8_t *table);
static void scaling_expand(uint32_t size, const uint8_t *coded, uint8_t dc, uint8_t *out);
static uint32_t config2_word(const struct bcm2712_hevc_sps *sps, const struct bcm2712_hevc_pps *pps, uint32_t log2_ctb, bool write_motion, bool temporal_mvp);

/*
 * Checks a sequence and picture parameter set pair against the decoder.
 *
 * Values referenced: hevc_d_h265.c (rpi-6.12.y, 7821b6a1) try_ctrl_sps
 * (4:2:0, equal 8- or 10-bit depths, 1 to 4096 samples a side) and
 * try_ctrl_pps (no wavefronts with tiles); hevc_d_video.c
 * HEVC_D_MIN_WIDTH and HEVC_D_MIN_HEIGHT (16).
 */
int
bcm2712_hevc_check(
	const struct bcm2712_hevc_sps *sps,
	const struct bcm2712_hevc_pps *pps)
{
	uint32_t log2_ctb;
	uint32_t log2_min_tb;
	uint32_t log2_max_tb;

	/* Main: 4:2:0 in one plane, eight bits both, no range or screen content tools. */
	if (sps->chroma_format_idc != 1U || (sps->flags & BCM2712_HEVC_SPS_SEPARATE_PLANES) != 0U)
		return ENOTSUP;
	if (sps->bit_depth_luma_minus8 != 0U || sps->bit_depth_chroma_minus8 != 0U)
		return ENOTSUP;
	if ((sps->flags & (BCM2712_HEVC_SPS_RANGE_EXTENSION | BCM2712_HEVC_SPS_SCC_EXTENSION)) != 0U)
		return ENOTSUP;
	if ((pps->flags & (BCM2712_HEVC_PPS_CHROMA_QP_LIST | BCM2712_HEVC_PPS_CURRENT_PICTURE | BCM2712_HEVC_PPS_ACT_OFFSETS)) != 0U)
		return ENOTSUP;

	/* The extent the decoder takes. */
	if (sps->width < BCM2712_HEVC_MIN_SIDE || sps->height < BCM2712_HEVC_MIN_SIDE)
		return ENOTSUP;
	if (sps->width > BCM2712_HEVC_MAX_SIDE || sps->height > BCM2712_HEVC_MAX_SIDE)
		return ENOTSUP;

	/* Block sizes within H.265: CTBs of 16 to 64, transforms of 4 to 32 within the CTB (7.4.3.2.1). */
	if (sps->log2_min_cb_minus3 > 3U || sps->log2_diff_cb > 3U)
		return EINVAL;
	log2_ctb = sps->log2_min_cb_minus3 + 3U + sps->log2_diff_cb;
	if (log2_ctb < 4U || log2_ctb > 6U)
		return EINVAL;
	log2_min_tb = sps->log2_min_tb_minus2 + 2U;
	if (sps->log2_min_tb_minus2 > 3U || sps->log2_diff_tb > 3U)
		return EINVAL;
	log2_max_tb = log2_min_tb + sps->log2_diff_tb;
	if (log2_max_tb > 5U || log2_max_tb > log2_ctb || log2_min_tb >= sps->log2_min_cb_minus3 + 3U)
		return EINVAL;
	if (sps->depth_inter > log2_ctb - log2_min_tb || sps->depth_intra > log2_ctb - log2_min_tb)
		return EINVAL;

	/* The order count's size, the reference set counts and the picture set's own ranges. */
	if (sps->log2_max_poc_lsb_minus4 > 12U || sps->short_term_sets > 64U || sps->long_term_sps > 32U)
		return EINVAL;
	if (pps->l0_minus1 > 14U || pps->l1_minus1 > 14U || pps->extra_bits > 7U)
		return EINVAL;
	if (pps->cu_qp_delta_depth > sps->log2_diff_cb || pps->log2_merge_minus2 > log2_ctb - 2U)
		return EINVAL;
	if (pps->init_qp_minus26 < -26 || pps->init_qp_minus26 > 25)
		return EINVAL;
	if (pps->cb_qp_offset < -12 || pps->cb_qp_offset > 12 || pps->cr_qp_offset < -12 || pps->cr_qp_offset > 12)
		return EINVAL;
	if (pps->beta_div2 < -6 || pps->beta_div2 > 6 || pps->tc_div2 < -6 || pps->tc_div2 > 6)
		return EINVAL;

	/* PCM's sizes, when it is on, within the CTB (7.4.3.2.1). */
	if ((sps->flags & BCM2712_HEVC_SPS_PCM) != 0U) {
		if (sps->pcm_depth_luma_minus1 > 7U || sps->pcm_depth_chroma_minus1 > 7U)
			return EINVAL;
		if (sps->log2_min_pcm_minus3 > 2U || sps->log2_min_pcm_minus3 + 3U + sps->log2_diff_pcm > 5U)
			return EINVAL;
	}

	/* Tiles within their limits, and never with wavefronts. */
	if ((pps->flags & BCM2712_HEVC_PPS_TILES) != 0U) {
		if (pps->tile_columns_minus1 >= BCM2712_HEVC_TILE_COLUMNS || pps->tile_rows_minus1 >= BCM2712_HEVC_TILE_ROWS)
			return EINVAL;
		if ((pps->flags & BCM2712_HEVC_PPS_ENTROPY_SYNC) != 0U &&
		    (pps->tile_columns_minus1 != 0U || pps->tile_rows_minus1 != 0U))
			return ENOTSUP;
	}

	/* Succeeded: the decoder takes the pair. */
	return 0;
}

/*
 * Reads one slice segment's header.
 *
 * Procedure referenced: Recommendation ITU-T H.265 (7.3.1.1 and 7.3.1.2,
 * the NAL unit and its emulation prevention; 7.3.6.1 to 7.3.6.3 and
 * 7.4.7.1 to 7.4.7.3, the slice segment header with the reference list
 * modification and the weights; 7.3.2.11, byte_alignment()).
 */
int
bcm2712_hevc_slice_read(
	const uint8_t *bytes,
	size_t length,
	const struct bcm2712_hevc_sps *sps,
	const struct bcm2712_hevc_pps *pps,
	const struct bcm2712_hevc_picture *picture,
	const struct bcm2712_hevc_slot *slots,
	const struct bcm2712_hevc_slice *independent,
	struct bcm2712_hevc_slice *slice)
{
	struct hevc_bits bits;
	struct hevc_geometry geometry;
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
	int error;

	/* Clears the result. */
	copy_slice(NULL, slice);

	/* A start code prefix in front of the NAL unit is skipped (three bytes, or four with a zero before them). */
	start = 0;
	if (length >= 4U && bytes[0] == 0U && bytes[1] == 0U && bytes[2] == 0U && bytes[3] == 1U) {
		start = 4U;
	} else if (length >= 3U && bytes[0] == 0U && bytes[1] == 0U && bytes[2] == 1U) {
		start = 3U;
	}

	/* The two-byte NAL unit header: a decodable picture's type of the base layer. */
	if (length < start + 3U)
		return EINVAL;
	header = ((uint32_t)bytes[start] << 8) | bytes[start + 1U];
	nal_type = (header >> 9) & 63U;
	layer = (header >> 3) & 63U;
	temporal = header & 7U;
	if ((header & 0x8000U) != 0U || layer != 0U || temporal == 0U)
		return EINVAL;
	if (nal_type > HEVC_NAL_LAST_PICTURE && (nal_type < HEVC_NAL_BLA_W_LP || nal_type > HEVC_NAL_IRAP_DECODABLE_LAST))
		return EINVAL;
	slice->nal_type = nal_type;
	slice->temporal_id_plus1 = temporal;

	/* The payload's bits, after the header. */
	bits_start(&bits, bytes, length, start + 2U);

	/* The first fields of every segment: whether it starts the picture, the IRAP output flag and the PPS. */
	first = bits_read(&bits, 1U) != 0U;
	if (nal_type >= HEVC_NAL_BLA_W_LP && nal_type <= HEVC_NAL_IRAP_LAST)
		(void)bits_read(&bits, 1U);
	pps_id = bits_golomb(&bits);
	if (bits.error || pps_id != pps->id)
		return EINVAL;

	/* A later segment's dependence and address, in CTBs of the picture (Ceil( Log2( PicSizeInCtbsY ) ) bits). */
	geometry_make(sps, pps, &geometry);
	ctbs = geometry.width_ctbs * geometry.height_ctbs;
	dependent = false;
	slice->address = 0;
	if (!first) {
		if ((pps->flags & BCM2712_HEVC_PPS_DEPENDENT) != 0U)
			dependent = bits_read(&bits, 1U) != 0U;
		slice->address = bits_read(&bits, ceil_log2(ctbs));
		if (bits.error || slice->address == 0U || slice->address >= ctbs)
			return EINVAL;
	}

	/* A dependent segment takes the independent one's fields, then reads only its tail. */
	if (dependent) {
		if (independent == NULL)
			return EINVAL;
		value = slice->address;
		copy_slice(independent, slice);
		slice->nal_type = nal_type;
		slice->temporal_id_plus1 = temporal;
		slice->address = value;
		slice->first_in_picture = false;
		slice->dependent = true;
		error = read_tail(&bits, pps, slice);
		if (error != 0)
			return error;

		/* Succeeded: the dependent segment's place and data. */
		return 0;
	}
	slice->first_in_picture = first;
	slice->dependent = false;

	/* The reserved flags, the type (an IRAP picture is intra) and the output flag. */
	for (index = 0; index < pps->extra_bits; index++)
		(void)bits_read(&bits, 1U);
	slice->slice_type = bits_golomb(&bits);
	if (bits.error || slice->slice_type > BCM2712_HEVC_SLICE_I)
		return EINVAL;
	if (nal_type >= HEVC_NAL_BLA_W_LP && slice->slice_type != BCM2712_HEVC_SLICE_I)
		return EINVAL;
	if ((pps->flags & BCM2712_HEVC_PPS_OUTPUT_FLAG) != 0U)
		(void)bits_read(&bits, 1U);

	/* A picture other than IDR: its order count, its sets, and the temporal motion vector flag. */
	slice->temporal_mvp = false;
	if (nal_type != HEVC_NAL_IDR_W_RADL && nal_type != HEVC_NAL_IDR_N_LP) {
		error = read_references(&bits, sps, picture);
		if (error != 0)
			return error;
		if ((sps->flags & BCM2712_HEVC_SPS_TEMPORAL_MVP) != 0U)
			slice->temporal_mvp = bits_read(&bits, 1U) != 0U;
	}

	/* The sample adaptive offset of luma and chroma (ChromaArrayType 1). */
	slice->sao_luma = false;
	slice->sao_chroma = false;
	if ((sps->flags & BCM2712_HEVC_SPS_SAO) != 0U) {
		slice->sao_luma = bits_read(&bits, 1U) != 0U;
		slice->sao_chroma = bits_read(&bits, 1U) != 0U;
	}

	/* A predicted slice's references, lists, collocated picture, weights and merge candidates. */
	slice->references[0] = 0;
	slice->references[1] = 0;
	slice->collocated_from_l0 = true;
	slice->max_merge_candidates = 0;
	if (slice->slice_type != BCM2712_HEVC_SLICE_I) {
		error = read_predicted(&bits, pps, picture, slots, slice);
		if (error != 0)
			return error;
	}

	/* The quantizer: SliceQpY, within -QpBdOffsetY (0 at eight bits) and 51 (7.4.7.1). */
	slice->qp = 26 + pps->init_qp_minus26 + bits_signed(&bits);
	if (bits.error || slice->qp < 0 || slice->qp > 51)
		return EINVAL;

	/* The slice's chroma quantizer offsets, -12 to 12 alone and with the picture's. */
	slice->cb_qp_offset = 0;
	slice->cr_qp_offset = 0;
	if ((pps->flags & BCM2712_HEVC_PPS_SLICE_QP) != 0U) {
		slice->cb_qp_offset = bits_signed(&bits);
		slice->cr_qp_offset = bits_signed(&bits);
		if (bits.error || slice->cb_qp_offset < -12 || slice->cb_qp_offset > 12 || slice->cr_qp_offset < -12 || slice->cr_qp_offset > 12)
			return EINVAL;
		if (pps->cb_qp_offset + slice->cb_qp_offset < -12 || pps->cb_qp_offset + slice->cb_qp_offset > 12)
			return EINVAL;
		if (pps->cr_qp_offset + slice->cr_qp_offset < -12 || pps->cr_qp_offset + slice->cr_qp_offset > 12)
			return EINVAL;
	}

	/* The deblocking: the picture's, unless the slice overrides it. */
	slice->deblocking_disabled = (pps->flags & BCM2712_HEVC_PPS_DEBLOCKING_OFF) != 0U;
	slice->beta_div2 = pps->beta_div2;
	slice->tc_div2 = pps->tc_div2;
	value = 0;
	if ((pps->flags & BCM2712_HEVC_PPS_DEBLOCKING_OVERRIDE) != 0U)
		value = bits_read(&bits, 1U);
	if (value != 0U) {
		slice->deblocking_disabled = bits_read(&bits, 1U) != 0U;
		if (!slice->deblocking_disabled) {
			slice->beta_div2 = bits_signed(&bits);
			slice->tc_div2 = bits_signed(&bits);
			if (bits.error || slice->beta_div2 < -6 || slice->beta_div2 > 6 || slice->tc_div2 < -6 || slice->tc_div2 > 6)
				return EINVAL;
		}
	}

	/* The loop filter across slices: read only when a filter runs, else the picture's. */
	slice->loop_filter_across_slices = (pps->flags & BCM2712_HEVC_PPS_ACROSS_SLICES) != 0U;
	if (slice->loop_filter_across_slices &&
	    (slice->sao_luma || slice->sao_chroma || !slice->deblocking_disabled))
		slice->loop_filter_across_slices = bits_read(&bits, 1U) != 0U;
	if (bits.error)
		return EINVAL;

	/* The entry points, the extension and the alignment. */
	error = read_tail(&bits, pps, slice);
	if (error != 0)
		return error;

	/* Succeeded: the segment's header is read and its data located. */
	return 0;
}

/*
 * Builds a picture's phase 1 command list and phase 2 values.
 *
 * Procedure referenced: hevc_d_h265.c (rpi-6.12.y, 7821b6a1)
 * hevc_d_h265_setup (the per-segment derivations and the frame-end
 * values), decode_slice and wpp_decode_slice with the helpers they call
 * (the order of the writes of each segment, the entry point fills of
 * tiles and wavefront rows, the closing status writes), mk_config2.
 */
int
bcm2712_hevc_program(
	const struct bcm2712_hevc_sps *sps,
	const struct bcm2712_hevc_pps *pps,
	const struct bcm2712_hevc_picture *picture,
	const struct bcm2712_hevc_slot *slots,
	const struct bcm2712_hevc_slice *slices,
	uint32_t count,
	struct bcm2712_hevc_commands *commands,
	struct bcm2712_hevc_phase2 *phase2)
{
	static struct hevc_program program_state;
	struct hevc_program *program;
	const struct bcm2712_hevc_slice *slice;
	uint32_t index;
	uint32_t ts;
	uint32_t previous_ts;
	uint32_t x;
	uint32_t y;
	bool wavefront;
	bool last;

	/* A picture starts at its first CTB with an independent segment, and has segments the list can count. */
	if (count == 0U || count > BCM2712_HEVC_SLICES)
		return EINVAL;
	if (!slices[0].first_in_picture || slices[0].address != 0U || slices[0].dependent)
		return EINVAL;

	/*
	 * The builder's state: too large for a kernel stack, it is one static
	 * area, which the executor's single decode at a time (under the
	 * controller mutex) and the host test use alone.
	 */
	program = &program_state;
	program->commands = commands;
	program->sps = sps;
	program->pps = pps;
	program->picture = picture;
	program->slots = slots;
	program->entry_tile_x = 0;
	program->entry_tile_y = 0;
	program->entry_x = 0;
	program->entry_y = 0;
	program->entry_qp = 0;
	program->entry_slice = 0;
	program->slice_start = 0;
	program->message_count = 0;
	program->collocated_slot = BCM2712_HEVC_NO_SLOT;
	commands->count = 0;
	commands->overflow = false;
	geometry_make(sps, pps, &program->geometry);
	wavefront = (pps->flags & BCM2712_HEVC_PPS_ENTROPY_SYNC) != 0U;

	/* The factor table of the scaling lists, written by the first segment when the sequence uses them. */
	if ((sps->flags & BCM2712_HEVC_SPS_SCALING) != 0U)
		scaling_table(sps, pps, program->scaling);

	/* Each segment in order: its place in tile scan, after the previous one's, then its commands. */
	previous_ts = 0;
	for (index = 0; index < count; index++) {
		/* The segment's first CTB in tile scan and in the picture. */
		slice = &slices[index];
		program->start_x = slice->address % program->geometry.width_ctbs;
		program->start_y = slice->address / program->geometry.width_ctbs;
		ts = raster_to_tile_scan(&program->geometry, program->start_x, program->start_y);
		if (index != 0U && ts <= previous_ts)
			return EINVAL;
		previous_ts = ts;
		program->start_ts = ts;

		/* The previous segment's last CTB, the one before this one's first in tile scan. */
		x = 0;
		y = 0;
		if (ts != 0U)
			tile_scan_to_raster(&program->geometry, ts - 1U, &x, &y);
		program->previous_x = x;
		program->previous_y = y;
		program->slice_qp = (uint32_t)slice->qp;

		/* The segment's commands, by the picture's entry points. */
		last = index + 1U == count;
		if (wavefront) {
			wavefront_segment(program, slice, index, last);
		} else {
			tile_segment(program, slice, index, last);
		}
	}

	/* A list past the capacity is refused whole. */
	if (commands->overflow)
		return ENOSPC;

	/*
	 * The phase 2 values: the configuration with the motion vectors
	 * written when the sequence may use them later, the size, the order
	 * count, the rows, and the collocated picture's slot.
	 */
	phase2->write_motion = (sps->flags & BCM2712_HEVC_SPS_TEMPORAL_MVP) != 0U;
	phase2->config2 = config2_word(sps, pps, program->geometry.log2_ctb, phase2->write_motion, slices[0].temporal_mvp);
	phase2->frame_size = (sps->height << 16) | sps->width;
	phase2->current_poc = (uint32_t)picture->poc;
	phase2->ctb_rows = program->geometry.height_ctbs;
	phase2->collocated_slot = program->collocated_slot;

	/* Succeeded: the list and the values describe the picture. */
	return 0;
}

/*
 * Lays out a picture's SAND output: columns 128 bytes wide, the luma
 * column of the height rounded up to 16 rows (so the chroma column's half
 * is a multiple of eight rows, which the texture unit reads), the chroma
 * column half as high.
 *
 * Values referenced: hevc_d_video.c (rpi-6.12.y, 7821b6a1)
 * hevc_d_prepare_dst_format (NV12MT_COL128: the width in 128-byte
 * columns, a column the plane's height of 128-byte rows, the chroma plane
 * half the luma's); Mesa src/gallium/drivers/v3d/v3dx_tfu.c tfu_sand (a
 * SAND input's column height a multiple of eight).
 */
void
bcm2712_hevc_sand_layout(
	uint32_t width,
	uint32_t height,
	struct bcm2712_hevc_sand *sand)
{
	/* The columns and their heights. */
	sand->column_rows = (height + 15U) & ~15U;
	sand->columns = (width + 127U) / 128U;

	/* Each plane: its columns of 128-byte rows. */
	sand->luma_bytes = (uint64_t)sand->columns * sand->column_rows * 128U;
	sand->chroma_bytes = sand->luma_bytes / 2U;
}

/*
 * Reports a picture's motion vector store: a stride of the width rounded
 * up to 64, for each 16 rows of the height rounded up to 64.
 *
 * Values referenced: hevc_d_h265.c (rpi-6.12.y, 7821b6a1) setup_colmv.
 */
void
bcm2712_hevc_motion_layout(
	uint32_t width,
	uint32_t height,
	uint32_t *stride,
	uint64_t *bytes)
{
	/* The stride, then the rows of 16 lines. */
	*stride = (width + 63U) & ~63U;
	*bytes = (uint64_t)*stride * (((height + 63U) & ~63U) >> 4);
}

/*
 * Reports the starting sizes of phase 1's PU and coefficient lists: a
 * quarter of the picture's samples and all of them, rounded up.
 *
 * Values referenced: hevc_d_h265.c (rpi-6.12.y, 7821b6a1)
 * hevc_d_h265_start (coeff_alloc and pu_alloc).
 */
void
bcm2712_hevc_list_sizes(
	uint32_t width,
	uint32_t height,
	uint64_t *pu_bytes,
	uint64_t *coefficient_bytes)
{
	uint64_t samples;

	/* Rounded as a list that ran out grows. */
	samples = (uint64_t)width * height;
	*pu_bytes = bcm2712_hevc_list_grow(samples / 4U - 1U);
	*coefficient_bytes = bcm2712_hevc_list_grow(samples - 1U);
}

/*
 * Reports the next size of a phase 1 list: the smallest of 3 * 2^n and
 * 4 * 2^n above the size, and no less than 768 bytes.
 *
 * Values referenced: hevc_d_video.c (rpi-6.12.y, 7821b6a1)
 * hevc_d_round_up_size, and hevc_d_h265.c next_size (one more than the
 * size, rounded so).
 */
uint64_t
bcm2712_hevc_list_grow(
	uint64_t bytes)
{
	uint64_t size;
	uint32_t power;

	/* The size to round: one more than the list's. */
	size = bytes + 1U;

	/* The power of two at or below it, no less than 2^8. */
	power = 8U;
	while (power < 62U && ((uint64_t)1U << (power + 1U)) <= size)
		power++;

	/* Three of the power when that covers it, else four. */
	if (size < ((uint64_t)3U << power))
		return (uint64_t)3U << power;

	/* Succeeded: four of the power. */
	return (uint64_t)4U << power;
}

/* Starts reading a NAL unit's payload at byte at. */
static void
bits_start(
	struct hevc_bits *bits,
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
	bits->error = false;
}

/* Reads the next payload byte, past an emulation prevention byte; 0 and the error past the end. */
static uint32_t
bits_byte(
	struct hevc_bits *bits)
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
		if (bits->at >= bits->length) {
			bits->error = true;
			return 0;
		}
		byte = bits->bytes[bits->at];
	}

	/* The byte, and the zeros it continues or ends. */
	bits->at++;
	if (byte == 0U) {
		bits->zeros++;
	} else {
		bits->zeros = 0;
	}

	/* Succeeded: the byte. */
	return byte;
}

/* Reads count bits (at most 32), most significant first; 0 after an error. */
static uint32_t
bits_read(
	struct hevc_bits *bits,
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
	struct hevc_bits *bits)
{
	uint32_t zeros;
	uint32_t suffix;

	/* The leading zeros, up to the limit. */
	zeros = 0;
	while (bits_read(bits, 1U) == 0U && !bits->error) {
		zeros++;
		if (zeros > HEVC_GOLOMB_ZEROS) {
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
	struct hevc_bits *bits)
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
	const struct hevc_bits *bits)
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

/* Counts the entries of a reference picture set up to its first empty one. */
static uint32_t
set_count(
	const uint8_t *set)
{
	uint32_t count;

	/* The slots before the first empty entry. */
	count = 0;
	while (count < BCM2712_HEVC_SET_ENTRIES && set[count] != BCM2712_HEVC_NO_SLOT)
		count++;

	/* Succeeded: the set's size. */
	return count;
}

/*
 * Reads a slice's reference fields up to the long-term pictures: the
 * order count bits, the short-term set (its bits skipped as Vulkan counts
 * them, or its index) and the long-term pictures.
 */
static int
read_references(
	struct hevc_bits *bits,
	const struct bcm2712_hevc_sps *sps,
	const struct bcm2712_hevc_picture *picture)
{
	uint32_t index;
	uint32_t remaining;
	uint32_t chunk;
	bool from_sps;
	int error;

	/* slice_pic_order_cnt_lsb, then short_term_ref_pic_set_sps_flag. */
	(void)bits_read(bits, sps->log2_max_poc_lsb_minus4 + 4U);
	from_sps = bits_read(bits, 1U) != 0U;

	/* A set in the header is skipped by its size; one of the sequence's is named by index. */
	if (!from_sps) {
		if (picture->short_term_bits > HEVC_SHORT_TERM_BITS_MAX || picture->short_term_bits > bits_remaining(bits))
			return EINVAL;
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
			return EINVAL;
		index = 0;
		if (sps->short_term_sets > 1U)
			index = bits_read(bits, ceil_log2(sps->short_term_sets));
		if (index >= sps->short_term_sets)
			return EINVAL;
	}

	/* The long-term pictures. */
	if ((sps->flags & BCM2712_HEVC_SPS_LONG_TERM) != 0U) {
		error = read_long_term(bits, sps);
		if (error != 0)
			return error;
	}
	if (bits->error)
		return EINVAL;

	/* Succeeded: the fields are read past. */
	return 0;
}

/* Reads past a slice's long-term pictures (7.3.6.1), bounding their count. */
static int
read_long_term(
	struct hevc_bits *bits,
	const struct bcm2712_hevc_sps *sps)
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
	if (bits->error || from_sps > sps->long_term_sps || pictures > HEVC_LONG_TERM_MAX || from_sps + pictures > HEVC_LONG_TERM_MAX)
		return EINVAL;

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
				return EINVAL;
		}
	}
	if (bits->error)
		return EINVAL;

	/* Succeeded: the pictures are read past. */
	return 0;
}

/*
 * Builds a reference list as DPB slots (8.3.4): the temporary list of the
 * three current sets in the list's order, repeated to the larger of its
 * length and the sets' total, then the entries the modification names (or
 * the first ones).  Every slot must be one the decode references.
 */
static int
build_list(
	const struct bcm2712_hevc_picture *picture,
	const struct bcm2712_hevc_slot *slots,
	uint32_t list,
	uint32_t count,
	bool modified,
	const uint8_t *entries,
	uint32_t total,
	uint8_t *out)
{
	uint8_t temporary[HEVC_TEMP_LIST + BCM2712_HEVC_SLOTS];
	const uint8_t *first;
	const uint8_t *second;
	uint32_t length;
	uint32_t used;
	uint32_t index;
	uint32_t entry;

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
	used = 0;
	while (used < length) {
		for (index = 0; index < set_count(first) && used < length; index++)
			temporary[used++] = first[index];
		for (index = 0; index < set_count(second) && used < length; index++)
			temporary[used++] = second[index];
		for (index = 0; index < set_count(picture->long_term) && used < length; index++)
			temporary[used++] = picture->long_term[index];
	}

	/* The list's entries, each a slot the decode references. */
	for (index = 0; index < count; index++) {
		entry = index;
		if (modified)
			entry = entries[index];
		if (entry >= length)
			return EINVAL;
		out[index] = temporary[entry];
		if (out[index] >= BCM2712_HEVC_SLOTS || !slots[out[index]].present)
			return EINVAL;
	}

	/* Succeeded: the list names the slots. */
	return 0;
}

/*
 * Reads a predicted slice's fields from the reference counts to the merge
 * candidates: the counts, the list modification and the lists, the zero
 * L1 differences, the CABAC initialization, the collocated picture, the
 * weights and MaxNumMergeCand.
 */
static int
read_predicted(
	struct hevc_bits *bits,
	const struct bcm2712_hevc_pps *pps,
	const struct bcm2712_hevc_picture *picture,
	const struct bcm2712_hevc_slot *slots,
	struct bcm2712_hevc_slice *slice)
{
	uint8_t entries[2][BCM2712_HEVC_SLOTS];
	bool modified[2];
	bool b_slice;
	uint32_t total;
	uint32_t lists;
	uint32_t list;
	uint32_t index;
	uint32_t count;
	uint32_t candidates;
	int error;

	/* The active counts: the slice's own when overridden, else the picture's (1 to 15). */
	b_slice = slice->slice_type == BCM2712_HEVC_SLICE_B;
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
	if (bits->error || slice->references[0] > BCM2712_HEVC_REFERENCES || slice->references[1] > BCM2712_HEVC_REFERENCES)
		return EINVAL;

	/* NumPicTotalCurr: the three current sets' pictures, of which a predicted slice needs one. */
	total = set_count(picture->before) + set_count(picture->after) + set_count(picture->long_term);
	if (total == 0U)
		return EINVAL;

	/* The modification: each list's entries, Ceil( Log2( NumPicTotalCurr ) ) bits each and below it. */
	modified[0] = false;
	modified[1] = false;
	if ((pps->flags & BCM2712_HEVC_PPS_LISTS_MODIFICATION) != 0U && total > 1U) {
		for (list = 0; list < lists; list++) {
			modified[list] = bits_read(bits, 1U) != 0U;
			if (!modified[list])
				continue;
			for (index = 0; index < slice->references[list]; index++) {
				entries[list][index] = (uint8_t)bits_read(bits, ceil_log2(total));
				if (entries[list][index] >= total)
					return EINVAL;
			}
		}
	}
	if (bits->error)
		return EINVAL;

	/* The lists as slots. */
	for (list = 0; list < lists; list++) {
		error = build_list(picture, slots, list, slice->references[list], modified[list], entries[list], total, slice->list[list]);
		if (error != 0)
			return error;
	}

	/* The zero L1 differences of a B slice and the CABAC initialization. */
	slice->mvd_l1_zero = false;
	if (b_slice)
		slice->mvd_l1_zero = bits_read(bits, 1U) != 0U;
	slice->cabac_init = false;
	if ((pps->flags & BCM2712_HEVC_PPS_CABAC_INIT) != 0U)
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
				return EINVAL;
		}
	}

	/* The weights, when the picture asks for them for this type. */
	slice->weighted = false;
	if (!b_slice && (pps->flags & BCM2712_HEVC_PPS_WEIGHTED) != 0U)
		slice->weighted = true;
	if (b_slice && (pps->flags & BCM2712_HEVC_PPS_BIPRED) != 0U)
		slice->weighted = true;
	if (slice->weighted) {
		error = read_weights(bits, slice);
		if (error != 0)
			return error;
	}

	/* MaxNumMergeCand: 5 - five_minus_max_num_merge_cand, 1 to 5. */
	candidates = bits_golomb(bits);
	if (bits->error || candidates > 4U)
		return EINVAL;
	slice->max_merge_candidates = 5U - candidates;

	/* Succeeded: the predicted slice's fields. */
	return 0;
}

/*
 * Reads pred_weight_table() (7.3.6.3): the denominators (0 to 7), then
 * each list's weights.  The picture is 4:2:0, so chroma weights are sent.
 */
static int
read_weights(
	struct hevc_bits *bits,
	struct bcm2712_hevc_slice *slice)
{
	int32_t denominator;
	int error;

	/* luma_log2_weight_denom and ChromaLog2WeightDenom. */
	slice->weights.luma_log2_denom = bits_golomb(bits);
	if (bits->error || slice->weights.luma_log2_denom > 7U)
		return EINVAL;
	denominator = (int32_t)slice->weights.luma_log2_denom + bits_signed(bits);
	if (bits->error || denominator < 0 || denominator > 7)
		return EINVAL;
	slice->weights.chroma_log2_denom = (uint32_t)denominator;

	/* The lists' weights: L0, then L1 of a B slice. */
	error = read_list_weights(bits, true, 0U, slice);
	if (error != 0)
		return error;
	if (slice->slice_type == BCM2712_HEVC_SLICE_B) {
		error = read_list_weights(bits, true, 1U, slice);
		if (error != 0)
			return error;
	}

	/* Succeeded: the weights. */
	return 0;
}

/* Reads one list's weights (7.3.6.3) into the slice, chroma when the picture has it. */
static int
read_list_weights(
	struct hevc_bits *bits,
	bool chroma,
	uint32_t list,
	struct bcm2712_hevc_slice *slice)
{
	struct bcm2712_hevc_weights *weights;
	bool luma_flags[BCM2712_HEVC_SLOTS];
	bool chroma_flags[BCM2712_HEVC_SLOTS];
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
		luma_flags[index] = bits_read(bits, 1U) != 0U;
	for (index = 0; index < count; index++) {
		chroma_flags[index] = false;
		if (chroma)
			chroma_flags[index] = bits_read(bits, 1U) != 0U;
	}

	/* Each entry's weights and offsets, the defaults where its flag is clear. */
	for (index = 0; index < count && !bits->error; index++) {
		/* Luma: LumaWeightLX and luma_offset_lX, both -128 to 127 at eight bits. */
		weights->luma_weight[list][index] = 1 << weights->luma_log2_denom;
		weights->luma_offset[list][index] = 0;
		if (luma_flags[index]) {
			delta = bits_signed(bits);
			offset = bits_signed(bits);
			if (delta < -128 || delta > 127 || offset < -128 || offset > 127)
				return EINVAL;
			weights->luma_weight[list][index] += delta;
			weights->luma_offset[list][index] = offset;
		}

		/* Chroma: ChromaWeightLX and ChromaOffsetLX (7-56) of Cb and Cr. */
		for (component = 0; component < 2U; component++) {
			weights->chroma_weight[list][index][component] = 1 << weights->chroma_log2_denom;
			weights->chroma_offset[list][index][component] = 0;
			if (!chroma_flags[index])
				continue;
			delta = bits_signed(bits);
			offset = bits_signed(bits);
			if (delta < -128 || delta > 127 || offset < -512 || offset > 511)
				return EINVAL;
			weight = (1 << weights->chroma_log2_denom) + delta;
			weights->chroma_weight[list][index][component] = weight;
			weights->chroma_offset[list][index][component] =
				clip(128 + offset - shift_floor(128 * weight, weights->chroma_log2_denom), -128, 127);
		}
	}
	if (bits->error)
		return EINVAL;

	/* Succeeded: the list's weights. */
	return 0;
}

/*
 * Reads a segment's tail: its entry points (their offsets read past, the
 * decoder does not need them), the header extension, the byte alignment,
 * and where its data starts (past an emulation prevention byte there).
 */
static int
read_tail(
	struct hevc_bits *bits,
	const struct bcm2712_hevc_pps *pps,
	struct bcm2712_hevc_slice *slice)
{
	uint32_t length;
	uint32_t index;
	uint64_t needed;

	/* The entry points with tiles or wavefronts: a count, the offsets' length, the offsets. */
	slice->entry_points = 0;
	if ((pps->flags & (BCM2712_HEVC_PPS_TILES | BCM2712_HEVC_PPS_ENTROPY_SYNC)) != 0U) {
		slice->entry_points = bits_golomb(bits);
		if (bits->error || slice->entry_points > HEVC_ENTRY_POINTS_MAX)
			return EINVAL;
		if (slice->entry_points != 0U) {
			length = bits_golomb(bits) + 1U;
			needed = (uint64_t)length * slice->entry_points;
			if (bits->error || length > 32U || needed > bits_remaining(bits))
				return EINVAL;
			for (index = 0; index < slice->entry_points && !bits->error; index++)
				(void)bits_read(bits, length);
		}
	}

	/* The extension: a length and as many bytes. */
	if ((pps->flags & BCM2712_HEVC_PPS_HEADER_EXTENSION) != 0U) {
		length = bits_golomb(bits);
		if (bits->error || length > HEVC_EXTENSION_MAX)
			return EINVAL;
		for (index = 0; index < length && !bits->error; index++)
			(void)bits_read(bits, 8U);
	}

	/* byte_alignment(): a one, then zeros to the byte's end. */
	if (bits_read(bits, 1U) != 1U || bits->error)
		return EINVAL;
	(void)bits_read(bits, bits->left);
	if (bits->error)
		return EINVAL;

	/* The data's first byte, which an emulation prevention byte there does not begin. */
	if (bits->at < bits->length && bits->zeros >= 2U && bits->bytes[bits->at] == 3U)
		bits->at++;
	if (bits->at >= bits->length)
		return EINVAL;
	slice->data_offset = bits->at;
	slice->data_bytes = bits->length - bits->at;

	/* Succeeded: the segment's data is located. */
	return 0;
}

/* Copies a slice's fields (clears them for NULL). */
static void
copy_slice(
	const struct bcm2712_hevc_slice *from,
	struct bcm2712_hevc_slice *to)
{
	const uint8_t *source;
	uint8_t *destination;
	size_t index;

	/* Byte by byte: the file uses nothing of the kernel. */
	destination = (uint8_t *)to;
	source = (const uint8_t *)from;
	for (index = 0; index < sizeof(*to); index++) {
		if (from == NULL) {
			destination[index] = 0;
		} else {
			destination[index] = source[index];
		}
	}
}

/*
 * Makes a picture's CTB geometry: the CTB size, the picture in CTBs and
 * the tile boundaries (6.5.1, uniform or as sent).
 */
static void
geometry_make(
	const struct bcm2712_hevc_sps *sps,
	const struct bcm2712_hevc_pps *pps,
	struct hevc_geometry *geometry)
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
	if ((pps->flags & BCM2712_HEVC_PPS_TILES) != 0U) {
		geometry->columns = pps->tile_columns_minus1 + 1U;
		geometry->rows = pps->tile_rows_minus1 + 1U;
		uniform = (pps->flags & BCM2712_HEVC_PPS_UNIFORM) != 0U;
	}

	/* A tile is never narrower than one CTB: more tiles than CTBs are cut down. */
	if (geometry->columns > geometry->width_ctbs)
		geometry->columns = geometry->width_ctbs;
	if (geometry->rows > geometry->height_ctbs)
		geometry->rows = geometry->height_ctbs;
	if (geometry->columns > BCM2712_HEVC_TILE_COLUMNS)
		geometry->columns = BCM2712_HEVC_TILE_COLUMNS;
	if (geometry->rows > BCM2712_HEVC_TILE_ROWS)
		geometry->rows = BCM2712_HEVC_TILE_ROWS;

	/* The boundaries. */
	spacing(geometry->width_ctbs, geometry->columns, uniform, pps->column_width_minus1, geometry->column_bounds);
	spacing(geometry->height_ctbs, geometry->rows, uniform, pps->row_height_minus1, geometry->row_bounds);
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

/* Reports the part a CTB column or row is in. */
static uint32_t
tile_of(
	const uint32_t *bounds,
	uint32_t parts,
	uint32_t ctb)
{
	uint32_t part;

	/* The last part whose start is at or before the CTB. */
	part = 0;
	while (part + 1U < parts && ctb >= bounds[part + 1U])
		part++;

	/* Succeeded: the part. */
	return part;
}

/* Reports a CTB's address in tile scan (6-10) from its place in the picture. */
static uint32_t
raster_to_tile_scan(
	const struct hevc_geometry *geometry,
	uint32_t x,
	uint32_t y)
{
	uint32_t tile_x;
	uint32_t tile_y;
	uint32_t ts;
	uint32_t index;
	uint32_t tile_height;

	/* The tile the CTB is in. */
	tile_x = tile_of(geometry->column_bounds, geometry->columns, x);
	tile_y = tile_of(geometry->row_bounds, geometry->rows, y);

	/* The whole tile rows above, then the tiles to the left in its row. */
	ts = geometry->row_bounds[tile_y] * geometry->width_ctbs;
	tile_height = geometry->row_bounds[tile_y + 1U] - geometry->row_bounds[tile_y];
	for (index = 0; index < tile_x; index++)
		ts += (geometry->column_bounds[index + 1U] - geometry->column_bounds[index]) * tile_height;

	/* Succeeded: and the CTB's place inside its tile. */
	return ts + (y - geometry->row_bounds[tile_y]) * (geometry->column_bounds[tile_x + 1U] - geometry->column_bounds[tile_x]) +
		(x - geometry->column_bounds[tile_x]);
}

/* Reports the place of the CTB at a tile scan address (6-11). */
static void
tile_scan_to_raster(
	const struct hevc_geometry *geometry,
	uint32_t ts,
	uint32_t *x,
	uint32_t *y)
{
	uint32_t tile_x;
	uint32_t tile_y;
	uint32_t width;
	uint32_t height;
	uint32_t size;

	/* The tiles in scan order, until the one holding the address. */
	for (tile_y = 0; tile_y < geometry->rows; tile_y++) {
		height = geometry->row_bounds[tile_y + 1U] - geometry->row_bounds[tile_y];
		for (tile_x = 0; tile_x < geometry->columns; tile_x++) {
			width = geometry->column_bounds[tile_x + 1U] - geometry->column_bounds[tile_x];
			size = width * height;
			if (ts < size) {
				*x = geometry->column_bounds[tile_x] + ts % width;
				*y = geometry->row_bounds[tile_y] + ts / width;
				return;
			}
			ts -= size;
		}
	}

	/* Past the picture: its last CTB. */
	*x = geometry->width_ctbs - 1U;
	*y = geometry->height_ctbs - 1U;
}

/* Appends one register write to the command list, or marks it overflowed. */
static void
emit(
	struct hevc_program *program,
	uint32_t reg,
	uint32_t value)
{
	struct bcm2712_hevc_commands *commands;

	/* A full list takes nothing more. */
	commands = program->commands;
	if (commands->count >= commands->capacity) {
		commands->overflow = true;
		return;
	}

	/* The pair: the register's offset, then its value. */
	commands->pairs[2U * commands->count] = reg;
	commands->pairs[2U * commands->count + 1U] = value;
	commands->count++;
}

/*
 * Reports the constant part of a segment's SLICE word: the merge
 * candidates, the reference counts, the type, SAO and the zero L1
 * differences (the CTB sizes are added per entry point).
 *
 * Values referenced: hevc_d_h265.c (rpi-6.12.y, 7821b6a1) slice_reg_const.
 */
static uint32_t
slice_word(
	const struct bcm2712_hevc_slice *slice)
{
	uint32_t word;

	/* The counts and the type. */
	word = slice->max_merge_candidates | (slice->references[0] << 4) | (slice->references[1] << 8) | (slice->slice_type << 12);

	/* The flags. */
	if (slice->sao_luma)
		word |= 1U << 14;
	if (slice->sao_chroma)
		word |= 1U << 15;
	if (slice->slice_type == BCM2712_HEVC_SLICE_B && slice->mvd_l1_zero)
		word |= 1U << 16;

	/* Succeeded: the word. */
	return word;
}

/*
 * Writes SLICE: the constant word with the width and height of the CTB at
 * column and row (a partial CTB at the picture's right or bottom edge).
 *
 * Values referenced: hevc_d_h265.c (rpi-6.12.y, 7821b6a1) write_slice.
 */
static void
write_slice_size(
	struct hevc_program *program,
	uint32_t word,
	uint32_t column,
	uint32_t row)
{
	uint32_t ctb;
	uint32_t last_width;
	uint32_t last_height;
	uint32_t width;
	uint32_t height;

	/* The CTB, and what is left of one at the edges. */
	ctb = 1U << program->geometry.log2_ctb;
	last_width = program->sps->width & (ctb - 1U);
	last_height = program->sps->height & (ctb - 1U);
	width = ctb;
	if (column + 1U >= program->geometry.width_ctbs && last_width != 0U)
		width = last_width;
	height = ctb;
	if (row + 1U >= program->geometry.height_ctbs && last_height != 0U)
		height = last_height;

	/* The word with the sizes. */
	emit(program, REG_SLICE, word | (width << 17) | (height << 24));
}

/*
 * Writes an entry point: the tile (or wavefront row) it covers, SLICE, the
 * QP when it resets, the pause mode with the right and bottom edge flags,
 * and the first CTB, and remembers it for the fills.
 *
 * Values referenced: hevc_d_h265.c (rpi-6.12.y, 7821b6a1) new_entry_point.
 */
static void
entry_point(
	struct hevc_program *program,
	bool tile_end,
	bool reset_qp,
	uint32_t pause,
	uint32_t tile_x,
	uint32_t tile_y,
	uint32_t column,
	uint32_t row,
	uint32_t qp,
	uint32_t word)
{
	const struct hevc_geometry *geometry;
	uint32_t end_x;
	uint32_t end_y;
	uint32_t mode;

	/* The covered area's last CTB: the tile's, or the row's end for a wavefront. */
	geometry = &program->geometry;
	end_x = geometry->column_bounds[tile_x + 1U] - 1U;
	end_y = geometry->row_bounds[tile_y + 1U] - 1U;
	if (pause == PAUSE_WAVEFRONT)
		end_y = row;

	/* The tile's start and end, and the tile end of an independent segment. */
	emit(program, REG_TILESTART, geometry->column_bounds[tile_x] | (geometry->row_bounds[tile_y] << 16));
	emit(program, REG_TILEEND, end_x | (end_y << 16));
	if (tile_end)
		emit(program, REG_BEGINTILEEND, end_x | (end_y << 16));
	write_slice_size(program, word, end_x, end_y);

	/* The QP (with QpBdOffsetY, 0 at eight bits) when it starts again. */
	if (reset_qp)
		emit(program, REG_QP, 6U * program->sps->bit_depth_luma_minus8 + qp);

	/* The pause mode with the edges, then the first CTB. */
	mode = pause;
	if (end_x == geometry->width_ctbs - 1U)
		mode |= 1U << 17;
	if (end_y == geometry->height_ctbs - 1U)
		mode |= 1U << 18;
	emit(program, REG_MODE, mode);
	emit(program, REG_CONTROL, column | (row << 16));

	/* The entry point the fills repeat. */
	program->entry_tile_x = tile_x;
	program->entry_tile_y = tile_y;
	program->entry_x = column;
	program->entry_y = row;
	program->entry_qp = qp;
	program->entry_slice = word;
}

/*
 * Writes a wavefront row's pause: its status, the context saved, the mode
 * (with the bottom flag on the last row) and the control.
 *
 * Values referenced: hevc_d_h265.c (rpi-6.12.y, 7821b6a1) wpp_pause.
 */
static void
wavefront_pause(
	struct hevc_program *program,
	uint32_t row)
{
	uint32_t mode;

	/* The four writes. */
	emit(program, REG_STATUS, (row << 18) | 0x25U);
	emit(program, REG_TRANSFER, TRANSFER_BACKUP);
	mode = 0x30000U;
	if (row == program->geometry.height_ctbs - 1U)
		mode = 0x70000U;
	emit(program, REG_MODE, mode);
	emit(program, REG_CONTROL, (row << 16) + 2U);
}

/*
 * Writes the wavefront rows after the last entry point up to last_row:
 * each row's end, the context, and the next row's entry point.
 *
 * Values referenced: hevc_d_h265.c (rpi-6.12.y, 7821b6a1) wpp_entry_fill.
 */
static void
wavefront_fill(
	struct hevc_program *program,
	uint32_t last_row)
{
	uint32_t last_x;
	uint32_t transfer;

	/* Each row before last_row. */
	last_x = program->geometry.width_ctbs - 1U;
	while (program->entry_y < last_row && !program->commands->overflow) {
		/* The row's pause (wide pictures), its end, and the context the next row starts from. */
		if (program->geometry.width_ctbs > 2U)
			wavefront_pause(program, program->entry_y);
		emit(program, REG_STATUS, (program->entry_y << 18) | (last_x << 5) | 2U);
		transfer = TRANSFER_RELOAD;
		if (program->geometry.width_ctbs == 2U)
			transfer = TRANSFER_BACKUP;
		emit(program, REG_TRANSFER, transfer);

		/* The next row's entry point. */
		entry_point(program, false, true, PAUSE_WAVEFRONT, 0, 0, 0, program->entry_y + 1U, program->entry_qp, program->entry_slice);
	}
}

/*
 * Ends the previous segment of a wavefront picture: the rows up to its
 * last CTB, a pause, its end status and the context.
 *
 * Values referenced: hevc_d_h265.c (rpi-6.12.y, 7821b6a1)
 * wpp_end_previous_slice.
 */
static void
wavefront_end_previous(
	struct hevc_program *program)
{
	uint32_t width;

	/* The rows up to the previous segment's last. */
	wavefront_fill(program, program->previous_y);

	/* The pause the row needs when the entry is in its first two CTBs. */
	width = program->geometry.width_ctbs;
	if (program->entry_x < 2U &&
	    (program->entry_y < program->start_y || program->start_x > 2U) &&
	    width > 2U)
		wavefront_pause(program, program->previous_y);

	/* The end status, and the context saved where the next segment starts at the third CTB. */
	emit(program, REG_STATUS, 1U | (program->previous_x << 5) | (program->previous_y << 18));
	if (program->start_x == 2U || (width == 2U && program->entry_y < program->start_y))
		emit(program, REG_TRANSFER, TRANSFER_BACKUP);
}

/*
 * Writes one segment of a wavefront picture.
 *
 * Values referenced: hevc_d_h265.c (rpi-6.12.y, 7821b6a1)
 * wpp_decode_slice.
 */
static void
wavefront_segment(
	struct hevc_program *program,
	const struct bcm2712_hevc_slice *slice,
	uint32_t index,
	bool last)
{
	uint32_t width;
	bool reset_qp;

	/* The previous segment's end, then this one's messages and bitstream. */
	width = program->geometry.width_ctbs;
	if (program->start_ts != 0U)
		wavefront_end_previous(program);
	slice_messages(program, slice);
	write_bitstream(program, slice);

	/* The contexts: initialized, reloaded at a row's start, or carried on. */
	reset_qp = true;
	if (program->start_ts == 0U || !slice->dependent || width == 1U) {
		write_probabilities(program, slice);
	} else if (program->start_x == 0U) {
		emit(program, REG_TRANSFER, TRANSFER_RELOAD);
	} else {
		reset_qp = false;
	}

	/* The messages, the segment's sets, and its entry point. */
	write_messages(program, index);
	write_segment(program, slice);
	entry_point(program, !slice->dependent, reset_qp, PAUSE_WAVEFRONT, 0, 0, program->start_x, program->start_y, program->slice_qp, slice_word(slice));

	/* The last segment fills the rest of the picture and ends it. */
	if (last) {
		wavefront_fill(program, program->geometry.height_ctbs - 1U);
		if (program->entry_x < 2U && width > 2U)
			wavefront_pause(program, program->geometry.height_ctbs - 1U);
		emit(program, REG_STATUS, 1U | ((width - 1U) << 5) | ((program->geometry.height_ctbs - 1U) << 18));
	}
}

/*
 * Writes the tiles after the last entry point up to (last_x, last_y), in
 * tile order: each tile's end, the context reloaded, the next tile's
 * entry point.
 *
 * Values referenced: hevc_d_h265.c (rpi-6.12.y, 7821b6a1) tile_entry_fill.
 */
static void
tile_fill(
	struct hevc_program *program,
	uint32_t last_x,
	uint32_t last_y)
{
	const struct hevc_geometry *geometry;
	uint32_t tile_x;
	uint32_t tile_y;
	uint32_t end_x;
	uint32_t end_y;

	/* Each tile before the last one named. */
	geometry = &program->geometry;
	while ((program->entry_tile_y < last_y ||
		(program->entry_tile_y == last_y && program->entry_tile_x < last_x)) &&
	       !program->commands->overflow) {
		/* The tile's end and the context. */
		tile_x = program->entry_tile_x;
		tile_y = program->entry_tile_y;
		end_x = geometry->column_bounds[tile_x + 1U] - 1U;
		end_y = geometry->row_bounds[tile_y + 1U] - 1U;
		emit(program, REG_STATUS, 2U | (end_x << 5) | (end_y << 18));
		emit(program, REG_TRANSFER, TRANSFER_RELOAD);

		/* The next tile in scan order, and its entry point. */
		tile_x++;
		if (tile_x >= geometry->columns) {
			tile_x = 0;
			tile_y++;
		}
		entry_point(program, false, true, PAUSE_TILE, tile_x, tile_y, geometry->column_bounds[tile_x], geometry->row_bounds[tile_y], program->entry_qp, program->entry_slice);
	}
}

/*
 * Ends the previous segment of a picture without wavefronts: the tiles up
 * to its last CTB's, then its end status.
 *
 * Values referenced: hevc_d_h265.c (rpi-6.12.y, 7821b6a1)
 * end_previous_slice.
 */
static void
tile_end_previous(
	struct hevc_program *program)
{
	const struct hevc_geometry *geometry;

	/* The tiles, then the status with the last CTB. */
	geometry = &program->geometry;
	tile_fill(program, tile_of(geometry->column_bounds, geometry->columns, program->previous_x),
		  tile_of(geometry->row_bounds, geometry->rows, program->previous_y));
	emit(program, REG_STATUS, 1U | (program->previous_x << 5) | (program->previous_y << 18));
}

/*
 * Writes one segment of a picture without wavefronts.
 *
 * Values referenced: hevc_d_h265.c (rpi-6.12.y, 7821b6a1) decode_slice.
 */
static void
tile_segment(
	struct hevc_program *program,
	const struct bcm2712_hevc_slice *slice,
	uint32_t index,
	bool last)
{
	const struct hevc_geometry *geometry;
	uint32_t tile_x;
	uint32_t tile_y;
	bool reset_qp;

	/* The segment's tile, the previous segment's end, the messages and the bitstream. */
	geometry = &program->geometry;
	tile_x = tile_of(geometry->column_bounds, geometry->columns, program->start_x);
	tile_y = tile_of(geometry->row_bounds, geometry->rows, program->start_y);
	if (program->start_ts != 0U)
		tile_end_previous(program);
	slice_messages(program, slice);
	write_bitstream(program, slice);

	/* The contexts start again for a new picture, an independent segment or a new tile. */
	reset_qp = false;
	if (program->start_ts == 0U || !slice->dependent)
		reset_qp = true;
	if (tile_x != tile_of(geometry->column_bounds, geometry->columns, program->previous_x))
		reset_qp = true;
	if (tile_y != tile_of(geometry->row_bounds, geometry->rows, program->previous_y))
		reset_qp = true;
	if (reset_qp)
		write_probabilities(program, slice);

	/* The messages, the segment's sets, and its entry point. */
	write_messages(program, index);
	write_segment(program, slice);
	entry_point(program, !slice->dependent, reset_qp, PAUSE_TILE, tile_x, tile_y, program->start_x, program->start_y, program->slice_qp, slice_word(slice));

	/* The last segment fills the remaining tiles and ends the picture. */
	if (last) {
		tile_fill(program, geometry->columns - 1U, geometry->rows - 1U);
		emit(program, REG_STATUS, 1U | ((geometry->width_ctbs - 1U) << 5) | ((geometry->height_ctbs - 1U) << 18));
	}
}

/*
 * Makes a segment's slice messages: the slice word (type, counts, merge
 * candidates, backward prediction and collocated list), each reference
 * of both lists with its weights, the deblocking word and the chroma QP
 * offsets; and notes the collocated picture's slot.
 *
 * Values referenced: hevc_d_h265.c (rpi-6.12.y, 7821b6a1)
 * pre_slice_decode and has_backward.
 */
static void
slice_messages(
	struct hevc_program *program,
	const struct bcm2712_hevc_slice *slice)
{
	uint32_t word;
	uint32_t list;
	uint32_t index;
	uint32_t slot;
	bool from_l0;
	bool backward;

	/* The slice word's type: 1 intra, 2 P, 3 B. */
	program->message_count = 0;
	word = 1U;
	if (slice->slice_type == BCM2712_HEVC_SLICE_P)
		word = 2U;
	if (slice->slice_type == BCM2712_HEVC_SLICE_B)
		word = 3U;
	word |= (slice->references[0] << 2) | (slice->references[1] << 6) | (slice->max_merge_candidates << 11);

	/* The collocated list: L0 unless a B slice with temporal motion vectors says L1. */
	from_l0 = true;
	if (slice->temporal_mvp && slice->slice_type == BCM2712_HEVC_SLICE_B && !slice->collocated_from_l0)
		from_l0 = false;
	if (from_l0)
		word |= 1U << 14;

	/* An intra slice has the word alone. */
	if (slice->slice_type == BCM2712_HEVC_SLICE_I) {
		program->messages[program->message_count++] = (uint16_t)word;
	} else {
		/* NoBackwardPredFlag (8.5.3.2.2): no reference after the current picture. */
		backward = true;
		for (list = 0; list < 2U; list++) {
			for (index = 0; index < slice->references[list]; index++) {
				slot = slice->list[list][index];
				if (program->slots[slot].poc > program->picture->poc)
					backward = false;
			}
		}
		if (backward)
			word |= 1U << 10;
		program->messages[program->message_count++] = (uint16_t)word;

		/* The collocated picture's slot. */
		if (slice->temporal_mvp) {
			if (from_l0) {
				program->collocated_slot = slice->list[0][slice->collocated_ref_idx];
			} else {
				program->collocated_slot = slice->list[1][slice->collocated_ref_idx];
			}
		}

		/* Each list's references. */
		reference_messages(program, slice, 0U);
		reference_messages(program, slice, 1U);
	}

	/* The deblocking: offsets, off, across slices, across tiles. */
	word = ((uint32_t)slice->beta_div2 & 15U) | (((uint32_t)slice->tc_div2 & 15U) << 4);
	if (slice->deblocking_disabled)
		word |= 1U << 8;
	if (slice->loop_filter_across_slices)
		word |= 1U << 9;
	if ((program->pps->flags & BCM2712_HEVC_PPS_ACROSS_TILES) != 0U)
		word |= 1U << 10;
	program->messages[program->message_count++] = (uint16_t)word;

	/* The slice's chroma QP offsets. */
	program->messages[program->message_count++] = (uint16_t)((((uint32_t)slice->cr_qp_offset & 31U) << 5) + ((uint32_t)slice->cb_qp_offset & 31U));
}

/* Appends one list's reference messages: the slot with its long-term and weighted flags, the order count, the weights. */
static void
reference_messages(
	struct hevc_program *program,
	const struct bcm2712_hevc_slice *slice,
	uint32_t list)
{
	const struct bcm2712_hevc_weights *weights;
	uint32_t index;
	uint32_t slot;
	uint32_t word;
	uint32_t component;

	/* Each reference of the list. */
	weights = &slice->weights;
	for (index = 0; index < slice->references[list]; index++) {
		/* The slot, long-term, weighted. */
		slot = slice->list[list][index];
		word = slot;
		if (program->slots[slot].long_term)
			word |= 1U << 4;
		if (slice->weighted)
			word |= 3U << 5;
		program->messages[program->message_count++] = (uint16_t)word;
		program->messages[program->message_count++] = (uint16_t)((uint32_t)program->slots[slot].poc & 0xffffU);
		if (!slice->weighted)
			continue;

		/* The luma weight with its denominator, the luma offset, then each chroma component's. */
		program->messages[program->message_count++] =
			(uint16_t)(weights->luma_log2_denom | (((uint32_t)weights->luma_weight[list][index] & 0x1ffU) << 3));
		program->messages[program->message_count++] = (uint16_t)((uint32_t)weights->luma_offset[list][index] & 0xffU);
		for (component = 0; component < 2U; component++) {
			program->messages[program->message_count++] =
				(uint16_t)(weights->chroma_log2_denom | (((uint32_t)weights->chroma_weight[list][index][component] & 0x1ffU) << 3));
			program->messages[program->message_count++] = (uint16_t)((uint32_t)weights->chroma_offset[list][index][component] & 0xffU);
		}
	}
}

/*
 * Writes the segment's bitstream: its data's address in 64-byte units,
 * its length, and the control twice (stop, then the offset inside the 64
 * bytes with the emulation prevention bytes taken out).
 *
 * Values referenced: hevc_d_h265.c (rpi-6.12.y, 7821b6a1) write_bitstream.
 */
static void
write_bitstream(
	struct hevc_program *program,
	const struct bcm2712_hevc_slice *slice)
{
	uint64_t address;
	uint32_t offset;

	/* The data's bus address and its place in its 64 bytes. */
	address = slice->segment_physical + slice->data_offset;
	offset = (uint32_t)(address & 63U);

	/* The four writes. */
	emit(program, REG_BFBASE, (uint32_t)(address >> 6));
	emit(program, REG_BFNUM, (uint32_t)slice->data_bytes);
	emit(program, REG_BFCONTROL, offset + BITSTREAM_STOP);
	emit(program, REG_BFCONTROL, offset + BITSTREAM_UNESCAPE);
}

/*
 * Writes the context variables' initial states for the slice's QP and
 * initialization type (9.3.2.2), four to a word, then saves them.
 *
 * Values referenced: hevc_d_h265.c (rpi-6.12.y, 7821b6a1) write_prob (the
 * type from slice_type and cabac_init_flag, the state's arithmetic and
 * its limit of 124, the window at 0x1000, the save afterwards).
 */
static void
write_probabilities(
	struct hevc_program *program,
	const struct bcm2712_hevc_slice *slice)
{
	uint8_t states[PROBABILITY_BYTES];
	const uint8_t *initial;
	uint32_t type;
	uint32_t index;
	int32_t qp;
	int32_t slope;
	int32_t offset;
	int32_t state;

	/* initType: 0 for I; P 1 and B 2, swapped by cabac_init_flag. */
	type = 2U - slice->slice_type;
	if (slice->cabac_init && slice->slice_type != BCM2712_HEVC_SLICE_I)
		type = slice->slice_type + 1U;
	initial = hevc_initial_values[type];
	qp = clip(slice->qp, 0, 51);

	/* Each context's state from its initValue: preCtxState folded to the decoder's 0 to 125. */
	for (index = 0; index < PROBABILITY_VALUES; index++) {
		slope = (int32_t)(initial[index] >> 4) * 5 - 45;
		offset = ((int32_t)(initial[index] & 15U) << 3) - 16;
		state = 2 * (shift_floor(slope * qp, 4U) + offset) - 127;
		if (state < 0)
			state = -state - 1;
		if (state > 124)
			state = 124 + (state & 1);
		states[index] = (uint8_t)state;
	}
	states[PROBABILITY_VALUES] = 0;
	states[PROBABILITY_VALUES + 1U] = 0;

	/* The states four to a word, then the save. */
	for (index = 0; index < PROBABILITY_BYTES; index += 4U) {
		emit(program, WINDOW_PROBABILITIES + index,
		     (uint32_t)states[index] | ((uint32_t)states[index + 1U] << 8) |
		     ((uint32_t)states[index + 2U] << 16) | ((uint32_t)states[index + 3U] << 24));
	}
	emit(program, REG_TRANSFER, TRANSFER_BACKUP);
}

/*
 * Writes the segment's messages: their count with the segment's index,
 * then each in its word.
 *
 * Values referenced: hevc_d_h265.c (rpi-6.12.y, 7821b6a1) program_slicecmds.
 */
static void
write_messages(
	struct hevc_program *program,
	uint32_t index)
{
	uint32_t message;

	/* The count and the index, then the messages. */
	emit(program, REG_SLICECMDS, program->message_count + (index << 8));
	for (message = 0; message < program->message_count; message++)
		emit(program, WINDOW_SLICE_MESSAGES + 4U * message, program->messages[message]);
}

/*
 * Writes a segment's parameter words SPS0, SPS1 and PPS, the scaling
 * factors at the picture's first segment, and its start CTB (an
 * independent segment's own, which its dependent ones repeat).
 *
 * Values referenced: hevc_d_h265.c (rpi-6.12.y, 7821b6a1) new_slice_segment.
 */
static void
write_segment(
	struct hevc_program *program,
	const struct bcm2712_hevc_slice *slice)
{
	const struct bcm2712_hevc_sps *sps;
	const struct bcm2712_hevc_pps *pps;
	uint32_t log2_ctb;
	uint32_t log2_min_tb;
	uint32_t log2_min_pcm;
	uint32_t word;
	uint32_t index;

	/* SPS0: the coding and transform block sizes, the depths and the hierarchy depths. */
	sps = program->sps;
	pps = program->pps;
	log2_ctb = program->geometry.log2_ctb;
	log2_min_tb = sps->log2_min_tb_minus2 + 2U;
	emit(program, REG_SPS0,
	     (sps->log2_min_cb_minus3 + 3U) | (log2_ctb << 4) | (log2_min_tb << 8) |
	     ((log2_min_tb + sps->log2_diff_tb) << 12) | ((sps->bit_depth_luma_minus8 + 8U) << 16) |
	     ((sps->bit_depth_chroma_minus8 + 8U) << 20) | (sps->depth_intra << 24) | (sps->depth_inter << 28));

	/* SPS1: PCM's depths and sizes, the chroma format and the tools. */
	log2_min_pcm = sps->log2_min_pcm_minus3 + 3U;
	word = (sps->pcm_depth_luma_minus1 + 1U) | ((sps->pcm_depth_chroma_minus1 + 1U) << 4) |
	       (log2_min_pcm << 8) | ((log2_min_pcm + sps->log2_diff_pcm) << 12) | (sps->chroma_format_idc << 16);
	if ((sps->flags & BCM2712_HEVC_SPS_AMP) != 0U)
		word |= 1U << 18;
	if ((sps->flags & BCM2712_HEVC_SPS_PCM) != 0U)
		word |= 1U << 19;
	if ((sps->flags & BCM2712_HEVC_SPS_SCALING) != 0U)
		word |= 1U << 20;
	if ((sps->flags & BCM2712_HEVC_SPS_STRONG_INTRA) != 0U)
		word |= 1U << 21;
	emit(program, REG_SPS1, word);

	/* PPS: the QP group size, the tools and the chroma offsets with the slice's. */
	word = log2_ctb - pps->cu_qp_delta_depth;
	if ((pps->flags & BCM2712_HEVC_PPS_CU_QP_DELTA) != 0U)
		word |= 1U << 4;
	if ((pps->flags & BCM2712_HEVC_PPS_BYPASS) != 0U)
		word |= 1U << 5;
	if ((pps->flags & BCM2712_HEVC_PPS_TRANSFORM_SKIP) != 0U)
		word |= 1U << 6;
	if ((pps->flags & BCM2712_HEVC_PPS_SIGN_HIDING) != 0U)
		word |= 1U << 7;
	word |= ((uint32_t)(pps->cb_qp_offset + slice->cb_qp_offset) & 255U) << 8;
	word |= ((uint32_t)(pps->cr_qp_offset + slice->cr_qp_offset) & 255U) << 16;
	if ((pps->flags & BCM2712_HEVC_PPS_CONSTRAINED) != 0U)
		word |= 1U << 24;
	emit(program, REG_PPS, word);

	/* The scaling factors, once a picture, four bytes to a word. */
	if (program->start_ts == 0U && (sps->flags & BCM2712_HEVC_SPS_SCALING) != 0U) {
		for (index = 0; index < SCALING_BYTES; index += 4U) {
			emit(program, WINDOW_SCALING + index,
			     (uint32_t)program->scaling[index] | ((uint32_t)program->scaling[index + 1U] << 8) |
			     ((uint32_t)program->scaling[index + 2U] << 16) | ((uint32_t)program->scaling[index + 3U] << 24));
		}
	}

	/* The start CTB: an independent segment's own. */
	if (!slice->dependent)
		program->slice_start = (slice->address % program->geometry.width_ctbs) | ((slice->address / program->geometry.width_ctbs) << 16);
	emit(program, REG_SLICESTART, program->slice_start);
}

/*
 * Fills the decoder's scaling factor table: each matrix's lists in their
 * places, the picture's own lists, else the sequence's, else the
 * defaults (7.4.5).
 */
static void
scaling_table(
	const struct bcm2712_hevc_sps *sps,
	const struct bcm2712_hevc_pps *pps,
	uint8_t *table)
{
	const struct bcm2712_hevc_scaling *source;
	uint8_t flat[16];
	const uint8_t *fallback;
	uint32_t matrix;
	uint32_t index;

	/* The picture's lists when it sends them, else the sequence's, else none. */
	source = NULL;
	if ((sps->flags & BCM2712_HEVC_SPS_SCALING_DATA) != 0U && sps->has_scaling)
		source = &sps->scaling;
	if ((pps->flags & BCM2712_HEVC_PPS_SCALING_DATA) != 0U && pps->has_scaling)
		source = &pps->scaling;

	/* The table starts clear. */
	for (index = 0; index < SCALING_BYTES; index++)
		table[index] = 0;

	/* The defaults: flat 4x4, Table 7-6 for the larger, DC values of 16. */
	if (source == NULL) {
		for (index = 0; index < 16U; index++)
			flat[index] = HEVC_FLAT;
		for (matrix = 0; matrix < 6U; matrix++) {
			/* The intra lists are the first three, the inter lists the last three. */
			fallback = hevc_default_inter;
			if (matrix < 3U)
				fallback = hevc_default_intra;
			scaling_expand(0U, flat, 0U, table + hevc_scaling_offsets[0][matrix]);
			scaling_expand(1U, fallback, 0U, table + hevc_scaling_offsets[1][matrix]);
			scaling_expand(2U, fallback, HEVC_FLAT, table + hevc_scaling_offsets[2][matrix]);
		}
		scaling_expand(3U, hevc_default_intra, HEVC_FLAT, table + hevc_scaling_offsets[3][0]);
		scaling_expand(3U, hevc_default_inter, HEVC_FLAT, table + hevc_scaling_offsets[3][1]);
		return;
	}

	/* The set's lists of each size, with the DC values. */
	for (matrix = 0; matrix < 6U; matrix++) {
		scaling_expand(0U, source->list4[matrix], 0U, table + hevc_scaling_offsets[0][matrix]);
		scaling_expand(1U, source->list8[matrix], 0U, table + hevc_scaling_offsets[1][matrix]);
		scaling_expand(2U, source->list16[matrix], source->dc16[matrix], table + hevc_scaling_offsets[2][matrix]);
	}
	for (matrix = 0; matrix < 2U; matrix++)
		scaling_expand(3U, source->list32[matrix], source->dc32[matrix], table + hevc_scaling_offsets[3][matrix]);
}

/*
 * Writes one matrix into the table: 4x4 and 8x8 as they are; 16x16 and
 * 32x32 with each entry of the 8x8 list repeated two or four times across
 * and down, and the DC value first.
 *
 * Procedure referenced: hevc_d_h265.c (rpi-6.12.y, 7821b6a1)
 * expand_scaling_list.
 */
static void
scaling_expand(
	uint32_t size,
	const uint8_t *coded,
	uint8_t dc,
	uint8_t *out)
{
	uint32_t repeat;
	uint32_t side;
	uint32_t x;
	uint32_t y;

	/* The two smaller sizes are copied. */
	if (size == 0U || size == 1U) {
		side = 16U;
		if (size == 1U)
			side = 64U;
		for (x = 0; x < side; x++)
			out[x] = coded[x];
		return;
	}

	/* The larger: each row of the 8x8 list for repeat rows, each entry repeat times. */
	repeat = 2U;
	if (size == 3U)
		repeat = 4U;
	side = 8U * repeat;
	for (y = 0; y < side; y++) {
		for (x = 0; x < side; x++)
			out[y * side + x] = coded[(y / repeat) * 8U + x / repeat];
	}
	out[0] = dc;
}

/*
 * Makes CONFIG2: the depths and whether they pass eight bits, the CTB
 * size, the intra and smoothing flags, the motion vector write, the
 * merge level, the temporal motion vectors, PCM's loop filter, and the
 * picture's chroma QP offsets.
 *
 * Values referenced: hevc_d_h265.c (rpi-6.12.y, 7821b6a1) mk_config2.
 */
static uint32_t
config2_word(
	const struct bcm2712_hevc_sps *sps,
	const struct bcm2712_hevc_pps *pps,
	uint32_t log2_ctb,
	bool write_motion,
	bool temporal_mvp)
{
	uint32_t word;

	/* The depths, then the CTB size. */
	word = (sps->bit_depth_luma_minus8 + 8U) | ((sps->bit_depth_chroma_minus8 + 8U) << 4);
	if (sps->bit_depth_luma_minus8 != 0U)
		word |= 1U << 8;
	if (sps->bit_depth_chroma_minus8 != 0U)
		word |= 1U << 9;
	word |= log2_ctb << 10;

	/* The flags and the merge level. */
	if ((pps->flags & BCM2712_HEVC_PPS_CONSTRAINED) != 0U)
		word |= 1U << 13;
	if ((sps->flags & BCM2712_HEVC_SPS_STRONG_INTRA) != 0U)
		word |= 1U << 14;
	if (write_motion)
		word |= 1U << 15;
	word |= (pps->log2_merge_minus2 + 2U) << 16;
	if (temporal_mvp)
		word |= 1U << 19;
	if ((sps->flags & BCM2712_HEVC_SPS_PCM_FILTER_OFF) != 0U)
		word |= 1U << 20;

	/* The chroma QP offsets. */
	word |= ((uint32_t)pps->cb_qp_offset & 31U) << 21;
	word |= ((uint32_t)pps->cr_qp_offset & 31U) << 26;

	/* Succeeded: the word. */
	return word;
}
