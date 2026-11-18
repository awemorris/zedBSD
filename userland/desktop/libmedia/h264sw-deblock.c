/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The deblocking filter of the software H.264 decoder (8.7).
 */

#include "h264sw-deblock.h"
#include "h264sw-context.h"
#include "h264sw-tables.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>

/* The last index of the tables of alpha', beta' and tC0': indexA and indexB are Clip3( 0, 51, ... ) (8-454, 8-455). */
#define DEBLOCK_INDEX_MAX 51

/* The largest 8-bit sample value, the upper bound of Clip1 for a bit depth of 8. */
#define DEBLOCK_SAMPLE_MAX 255

/* The strongest boundary strength (8.7.2.1); a larger value is treated as this one. */
#define DEBLOCK_BS_STRONG 4

/* The motion vector difference, in quarter luma samples, from which two vectors count as different (8.7.2.1). */
#define DEBLOCK_MV_LIMIT 4

/* The most bytes a row of a plane may take: far beyond the widest picture the limits allow, it keeps row arithmetic inside ptrdiff_t. */
#define DEBLOCK_STRIDE_MAX 1048576U

/* The modes of a line of an edge: left alone (bS 0), the filter of bS 1 to 3 (8.7.2.3), the filter of bS 4 (8.7.2.4). */
#define DEBLOCK_MODE_OFF 0
#define DEBLOCK_MODE_NORMAL 1
#define DEBLOCK_MODE_STRONG 2

/* Added to a value before it is shifted right by 3 (resp. 1) and taken off after, so that a negative value rounds toward minus infinity with a shift of a non-negative number: a multiple of 8 (resp. 2) above the most negative value there is, 1271 (resp. 510). */
#define DEBLOCK_BIAS_8 1280
#define DEBLOCK_BIAS_2 512

/*
 * The alpha' of Table 8-16 for indexA 0 to 51, four rows of 13 entries
 * (indexA 0..12, 13..25, 26..38 and 39..51).
 *
 * For 8-bit samples alpha is alpha' itself (8-456, 8-458).  The table is
 * constant and shared by every decoder.
 *
 * Values referenced: ITU-T H.264 (08/2021) Table 8-16, row alpha'
 * (indexA 0..15: 0; 16..25: 4 4 5 6 7 8 9 10 12 13; 26..51: 15 17 20 22
 * 25 28 32 36 40 45 50 56 63 71 80 90 101 113 127 144 162 182 203 226
 * 255 255).
 */
static const uint8_t deblock_alpha[52] = {
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 4, 4, 5, 6, 7, 8, 9, 10, 12, 13,
	15, 17, 20, 22, 25, 28, 32, 36, 40, 45, 50, 56, 63,
	71, 80, 90, 101, 113, 127, 144, 162, 182, 203, 226, 255, 255
};

/*
 * The beta' of Table 8-16 for indexB 0 to 51, four rows of 13 entries
 * (indexB 0..12, 13..25, 26..38 and 39..51).
 *
 * For 8-bit samples beta is beta' itself (8-457, 8-459).  The table is
 * constant and shared by every decoder.
 *
 * Values referenced: ITU-T H.264 (08/2021) Table 8-16, row beta'
 * (indexB 0..15: 0; 16..25: 2 2 2 3 3 3 3 4 4 4; 26..51: 6 6 7 7 8 8 9
 * 9 10 10 11 11 12 12 13 13 14 14 15 15 16 16 17 17 18 18).
 */
static const uint8_t deblock_beta[52] = {
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4,
	6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12,
	12, 13, 13, 14, 14, 15, 15, 16, 16, 17, 17, 18, 18
};

/*
 * The tC0' of Table 8-17 for bS 1, 2 and 3 (the first index is bS - 1)
 * and indexA 0 to 51, four rows of 13 entries each (indexA 0..12,
 * 13..25, 26..38 and 39..51).
 *
 * For 8-bit samples tC0 is tC0' itself (8-461, 8-462).  The table is
 * constant and shared by every decoder.
 *
 * Values referenced: ITU-T H.264 (08/2021) Table 8-17
 * (bS = 1, indexA 0..22: 0; 23..32: 1; 33..36: 2; 37..39: 3; 40..42: 4;
 * 43..51: 5 6 6 7 8 9 10 11 13.
 * bS = 2, indexA 0..20: 0; 21..30: 1; 31..34: 2; 35..37: 3; 38..39: 4;
 * 40..41: 5; 42..51: 6 7 8 8 10 11 12 13 15 17.
 * bS = 3, indexA 0..16: 0; 17..26: 1; 27..30: 2; 31..33: 3; 34..36: 4;
 * 37: 5; 38..39: 6; 40..51: 7 8 9 10 11 13 14 16 18 20 23 25).
 */
static const uint8_t deblock_tc0[3][52] = {
	{
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1,
		1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3,
		3, 4, 4, 4, 5, 6, 6, 7, 8, 9, 10, 11, 13
	},
	{
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1,
		1, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 4,
		4, 5, 5, 6, 7, 8, 8, 10, 11, 12, 13, 15, 17
	},
	{
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1,
		1, 2, 2, 2, 2, 3, 3, 3, 4, 4, 4, 5, 6,
		6, 7, 8, 9, 10, 11, 13, 14, 16, 18, 20, 23, 25
	}
};

/*
 * luma4x4BlkIdx of the 4x4 block at row r and column c of a macroblock,
 * in 4-sample units (6-38: 8 * ( y / 8 ) + 4 * ( x / 8 ) + 2 * ( ( y % 8 ) /
 * 4 ) + ( ( x % 8 ) / 4 )).  The table is constant and shared by every
 * decoder.
 */
static const uint8_t deblock_blk_at[4][4] = {
	{ 0, 1, 4, 5 },
	{ 2, 3, 6, 7 },
	{ 8, 9, 12, 13 },
	{ 10, 11, 14, 15 }
};

/*
 * What the filter loops need of one edge: the thresholds, and for each
 * line along the edge its mode and its tC0, so that a loop over the lines
 * reads plain arrays and decides nothing about the edge itself.
 *
 * active is 0 when no line of the edge can change (every bS is 0, or alpha
 * or beta is 0, which no sample difference is below).  normal is 1 when a
 * line has bS 1 to 3 and strong is 1 when a line has bS 4.  mode[line] is DEBLOCK_MODE_OFF, _NORMAL (bS 1 to 3) or
 * _STRONG (bS 4); tc0[line] is tC0 of Table 8-17 (0 for the other modes).
 * A luma edge has 16 lines and a chroma edge the first 8.
 */
struct deblock_edge {
	int active;
	int normal;
	int strong;
	uint8_t alpha;
	uint8_t beta;
	uint8_t mode[16];
	uint8_t tc0[16];
};

/*
 * The 16 lines of the chroma edges of Cb and Cr that one chroma filter
 * loop takes (lines 0 to 7 Cb's, 8 to 15 Cr's): the mode, tC0 and
 * thresholds of each line, which differ between the components, and
 * whether a line uses the normal filter (normal) or the strong one (strong).
 */
struct deblock_chroma_edge {
	int normal;
	int strong;
	uint8_t mode[16];
	uint8_t tc0[16];
	uint8_t alpha[16];
	uint8_t beta[16];
};

/*
 * What is known of the macroblock being filtered, for the functions that
 * filter its planes.
 *
 * q_mb holds q0; left_mb and top_mb are its neighbours across the
 * macroblock edges (NULL on the picture's border); slice is its slice's
 * parameters.  filter_left and filter_top are filterLeftMbEdgeFlag and
 * filterTopMbEdgeFlag.  vertical_bs and horizontal_bs are bS of each
 * segment of each edge (index edge, then segment).
 */
struct deblock_mb {
	const struct h264sw_picture_ctx *picture;
	const struct h264sw_mb *q_mb;
	const struct h264sw_mb *left_mb;
	const struct h264sw_mb *top_mb;
	const struct h264sw_slice_params *slice;
	unsigned mb_x;
	unsigned mb_y;
	int filter_left;
	int filter_top;
	uint8_t vertical_bs[4][4];
	uint8_t horizontal_bs[4][4];
};

/* Forward declarations of the static functions. */
static int16_t clip_signed(int16_t value, int16_t low, int16_t high);
static uint8_t sample_distance(uint8_t first, uint8_t second);
static int absolute(int value);
static uint8_t clip_sample(int16_t value);
static int table_index(int qp_av, int offset);
static int is_intra(const struct h264sw_mb *mb);
static int luma_qp(const struct h264sw_mb *mb);
static int chroma_qp(const struct h264sw_picture_ctx *picture, const struct h264sw_mb *mb, unsigned component);
static int mv_differs(const int16_t *first, const int16_t *second);
static int motion_differs(const struct h264sw_motion *p_motion, unsigned p_blk, const struct h264sw_motion *q_motion, unsigned q_blk);
static int same_motion(const struct h264sw_motion *p_motion, unsigned p_blk, const struct h264sw_motion *q_motion, unsigned q_blk);
static int segment_strength(const struct h264sw_mb *p_mb, const struct h264sw_mb *q_mb, unsigned p_blk, unsigned q_blk);
static void edge_strengths(const struct h264sw_mb *p_mb, const struct h264sw_mb *q_mb, int p_intra, int q_intra, int vertical, unsigned edge, uint8_t *bs);
static void edge_prepare(struct deblock_edge *edge, const uint8_t *bs, unsigned lines_per_segment, int qp_av, int offset_a, int offset_b);
static void filter_luma_lines(uint8_t (*rows)[16], const struct deblock_edge *edge);
static void chroma_pair(struct deblock_chroma_edge *pair, const struct deblock_edge *cb, const struct deblock_edge *cr);
static void filter_chroma_lines(uint8_t (*rows)[16], const struct deblock_chroma_edge *edge);
static int picture_is_usable(const struct h264sw_picture_ctx *picture);
static int neighbour_is_filtered(const struct h264sw_picture_ctx *picture, const struct h264sw_mb *q_mb, const struct h264sw_mb *neighbour, unsigned idc);
static int thresholds_close(int qp_av, int offset_a, int offset_b);
static int largest_qp_av(const struct deblock_mb *mb, int qp_q, int qp_left, int qp_top);
static int macroblock_is_quiet(const struct deblock_mb *mb);
static void macroblock_strengths(struct deblock_mb *mb);
static void filter_luma_row(uint8_t *q0_pointer, int mode, int alpha, int beta, int tc0);
static void filter_chroma_row(uint8_t *q0_pointer, int mode, int alpha, int beta, int tc0);
static void deblock_luma_vertical(const struct deblock_mb *mb);
static void deblock_luma_horizontal(const struct deblock_mb *mb);
static void deblock_chroma_vertical(const struct deblock_mb *mb);
static void deblock_chroma_horizontal(const struct deblock_mb *mb);
static void deblock_macroblock(const struct h264sw_picture_ctx *picture, unsigned mb_addr);

/*
 * Filters a decoded picture in place (8.7).
 *
 * A picture that cannot be filtered safely (no frame, planes or macroblock
 * array, a frame smaller than the macroblock grid, more slices than the
 * array holds) is left as it is.  A macroblock whose slice number is
 * outside the slices recorded is not filtered, and neither is the edge
 * shared with such a macroblock.
 */
void
h264sw_deblock_picture(
	struct h264sw_picture_ctx *picture)
{
	/* Refuses a missing picture. */
	if (picture == NULL)
		return;

	/* Filters every row of the picture. */
	h264sw_deblock_rows(picture, 0U, picture->height_mbs);
}

/*
 * Filters the macroblock rows first_row .. end_row - 1 of a decoded picture in place (8.7).
 *
 * A picture that cannot be filtered safely is left as it is, as by
 * h264sw_deblock_picture.
 */
void
h264sw_deblock_rows(
	struct h264sw_picture_ctx *picture,
	unsigned first_row,
	unsigned end_row)
{
	unsigned mb_addr;
	unsigned end_addr;
	int usable;

	/* Refuses a picture whose arrays cannot be trusted. */
	usable = picture_is_usable(picture);
	if (usable == 0)
		return;

	/* Keeps the rows inside the picture. */
	if (end_row > picture->height_mbs)
		end_row = picture->height_mbs;
	if (first_row >= end_row)
		return;

	/* Finds the address after the last macroblock of the rows. */
	end_addr = end_row * picture->width_mbs;

	/* Filters the macroblocks of the rows in increasing macroblock address (8.7). */
	for (mb_addr = first_row * picture->width_mbs;
	     mb_addr < end_addr;
	     mb_addr++) {
		/* Filters the edges the macroblock controls. */
		deblock_macroblock(picture, mb_addr);
	}
}

/*
 * Derives the boundary strength of the four segments of one edge of a macroblock (8.7.2.1, frames).
 *
 * An edge that does not exist (the picture's left or top border, a
 * macroblock address or an edge index out of range) gets bS 0 in every
 * segment.  Intra is read from the kind of a macroblock and from the
 * intra flag of its motion alike, so a macroblock that only one of them
 * marks as intra counts as intra.  Slices of type SP and SI are refused
 * by the decoder, so their rule is not applied.
 */
void
h264sw_deblock_bs(
	const struct h264sw_picture_ctx *picture,
	unsigned mb_addr,
	int vertical,
	unsigned edge,
	uint8_t *bs)
{
	const struct h264sw_mb *q_mb;
	const struct h264sw_mb *p_mb;
	unsigned mb_x;
	unsigned mb_y;
	unsigned segment;
	int p_intra;
	int q_intra;

	/* Refuses a missing output. */
	if (bs == NULL)
		return;

	/* Starts from bS 0 for every segment, the answer for an edge that is not there. */
	for (segment = 0;
	     segment < 4U;
	     segment++) {
		bs[segment] = 0;
	}

	/* Refuses a picture without macroblocks, a macroblock address outside it and an edge index out of range. */
	if (picture == NULL)
		return;
	if (picture->mbs == NULL)
		return;
	if (picture->width_mbs == 0U)
		return;
	if (mb_addr >= picture->size_mbs)
		return;
	if (edge > 3U)
		return;

	/* Finds the macroblock holding q0 and its position in the picture. */
	q_mb = &picture->mbs[mb_addr];
	mb_x = mb_addr % picture->width_mbs;
	mb_y = mb_addr / picture->width_mbs;

	/* Finds the macroblock holding p0: the left or upper neighbour for the macroblock edge, the macroblock itself inside. */
	p_mb = q_mb;
	if (edge == 0U) {
		if (vertical != 0) {
			/* A vertical macroblock edge on the picture's left border is not filtered. */
			if (mb_x == 0U)
				return;

			/* The p0 samples lie in the macroblock to the left. */
			p_mb = q_mb - 1;
		} else {
			/* A horizontal macroblock edge on the picture's upper border is not filtered. */
			if (mb_y == 0U)
				return;

			/* The p0 samples lie in the macroblock above. */
			p_mb = q_mb - picture->width_mbs;
		}
	}

	/* Reads whether either side is intra. */
	p_intra = is_intra(p_mb);
	q_intra = is_intra(q_mb);

	/* Derives bS of each 4-sample segment along the edge. */
	edge_strengths(p_mb, q_mb, p_intra, q_intra, vertical, edge, bs);
}

/*
 * Filters one luma edge of 16 samples (8.7.2.2 to 8.7.2.4, chromaEdgeFlag 0).
 *
 * A strength above 4 is taken as 4.  qp_av and the offsets are summed
 * without overflow before the clip, so any int values are safe.  The
 * samples p3 and q3 are read only when a line has bS 4.
 */
void
h264sw_deblock_luma_edge(
	uint8_t *q0,
	ptrdiff_t across,
	ptrdiff_t along,
	const uint8_t *bs,
	int qp_av,
	int offset_a,
	int offset_b)
{
	struct deblock_edge edge;
	uint8_t block[8][16];
	uint8_t *source;
	unsigned row;
	unsigned line;

	/* Refuses a missing picture or strength array. */
	if (q0 == NULL)
		return;
	if (bs == NULL)
		return;

	/* Derives the thresholds (8-454 to 8-459) and the mode and tC0 of each line. */
	edge_prepare(&edge, bs, 4U, qp_av, offset_a, offset_b);

	/* Leaves an edge alone that no line can change. */
	if (edge.active == 0)
		return;

	/* Copies the lines of the edge into a block, row 0 being p3 and row 7 being q3; p3 and q3 only matter to bS 4. */
	for (row = 0;
	     row < 8U;
	     row++) {
		/* Leaves p3 and q3 out when no line has bS 4: they are given the value of the next sample inwards. */
		if (row == 0U && edge.strong == 0)
			continue;
		if (row == 7U && edge.strong == 0)
			continue;

		/* Finds the first sample of this row: p3 is four samples before q0 across the edge. */
		source = q0 + ((ptrdiff_t)row - 4) * across;

		/* A row that lies along the picture's rows is copied whole, any other is gathered. */
		if (along == 1) {
			memcpy(block[row], source, 16);
		} else {
			for (line = 0;
			     line < 16U;
			     line++) {
				block[row][line] = source[(ptrdiff_t)line * along];
			}
		}
	}

	/* Fills p3 and q3 for an edge without bS 4, where the filter ignores them. */
	if (edge.strong == 0) {
		memcpy(block[0], block[1], 16);
		memcpy(block[7], block[6], 16);
	}

	/* Filters the 16 lines side by side. */
	filter_luma_lines(block, &edge);

	/* Writes the six rows the filter may change (p2 to q2) back. */
	for (row = 1;
	     row < 7U;
	     row++) {
		source = q0 + ((ptrdiff_t)row - 4) * across;
		if (along == 1) {
			memcpy(source, block[row], 16);
		} else {
			for (line = 0;
			     line < 16U;
			     line++) {
				source[(ptrdiff_t)line * along] = block[row][line];
			}
		}
	}
}

/*
 * Filters one chroma edge of 8 samples of 4:2:0 (8.7.2.2 to 8.7.2.4, chromaEdgeFlag 1).
 *
 * A strength above 4 is taken as 4.  Only p0 and q0 change.
 */
void
h264sw_deblock_chroma_edge(
	uint8_t *q0,
	ptrdiff_t across,
	ptrdiff_t along,
	const uint8_t *bs,
	int qp_av,
	int offset_a,
	int offset_b)
{
	struct deblock_edge edge;
	struct deblock_chroma_edge pair;
	uint8_t block[4][16];
	uint8_t *target;
	unsigned row;
	unsigned line;

	/* Refuses a missing picture or strength array. */
	if (q0 == NULL)
		return;
	if (bs == NULL)
		return;

	/* Derives the thresholds (8-454 to 8-459) and the mode and tC0 of each line; the 8 lines take two to each luma segment's strength. */
	edge_prepare(&edge, bs, 2U, qp_av, offset_a, offset_b);

	/* Leaves an edge alone that no line can change. */
	if (edge.active == 0)
		return;

	/* Makes the 8 lines the first half of the 16 the filter loop takes; the other half is left alone. */
	chroma_pair(&pair, &edge, NULL);
	memset(block, 0, sizeof(block));

	/* Copies the lines of the edge into a block of four rows: p1, p0, q0 and q1. */
	for (row = 0;
	     row < 4U;
	     row++) {
		/* Finds the first sample of this row: p1 is two samples before q0 across the edge. */
		target = q0 + ((ptrdiff_t)row - 2) * across;

		/* A row that lies along the picture's rows is copied whole, any other is gathered. */
		if (along == 1) {
			memcpy(block[row], target, 8);
		} else {
			for (line = 0;
			     line < 8U;
			     line++) {
				block[row][line] = target[(ptrdiff_t)line * along];
			}
		}
	}

	/* Filters the lines side by side. */
	filter_chroma_lines(block, &pair);

	/* Writes the two rows the filter may change (p0 and q0) back. */
	for (row = 1;
	     row < 3U;
	     row++) {
		target = q0 + ((ptrdiff_t)row - 2) * across;
		if (along == 1) {
			memcpy(target, block[row], 8);
		} else {
			for (line = 0;
			     line < 8U;
			     line++) {
				target[(ptrdiff_t)line * along] = block[row][line];
			}
		}
	}
}

/*
 * Gives the absolute value of a difference of two samples.
 *
 * It is made without a branch: for a sample line of noise whether the
 * difference is negative cannot be predicted, and a mispredicted branch
 * costs more than the few operations here.
 */
static int
absolute(
	int value)
{
	int negative;

	/* Finds whether the value is negative: 1 or 0. */
	negative = 0;
	if (value < 0)
		negative = 1;

	/* Negates a negative value (the complement plus one) and leaves another alone; its magnitude is far below INT_MAX here. */
	return (value ^ -negative) + negative;
}

/*
 * Clamps a filtered value to the 8-bit sample range (Clip1 for a bit depth of 8).
 *
 * The value is within 16 bits.  It is made without a branch, as absolute.
 */
static uint8_t
clip_sample(
	int16_t value)
{
	int clamped;
	int above;

	/* A value below the range becomes its lower bound: the value is kept when it is 0 or more. */
	clamped = value;
	if (value < 0)
		clamped = 0;

	/* A value above the range becomes its upper bound. */
	above = 0;
	if (clamped > DEBLOCK_SAMPLE_MAX)
		above = 1;
	clamped = clamped | -above;

	/* Succeeded: the clamped value fits a byte. */
	return (uint8_t)clamped;
}

/*
 * Gives indexA or indexB: Clip3( 0, 51, qPav + offset ) (8-454, 8-455).
 *
 * The sum is made in int64_t so that any int inputs are safe.
 */
static int
table_index(
	int qp_av,
	int offset)
{
	int64_t sum;

	/* Adds the average quantization parameter and the slice's filter offset. */
	sum = (int64_t)qp_av + (int64_t)offset;

	/* A sum below the table's first entry uses the first entry. */
	if (sum < 0)
		return 0;

	/* A sum above the table's last entry uses the last entry. */
	if (sum > DEBLOCK_INDEX_MAX)
		return DEBLOCK_INDEX_MAX;

	/* Succeeded: the sum is a table index. */
	return (int)sum;
}

/* Tells whether a macroblock is coded in an intra macroblock prediction mode. */
static int
is_intra(
	const struct h264sw_mb *mb)
{
	/* The decoder's concealed macroblocks carry only the kind. */
	if (mb->kind == H264SW_MB_I_NXN)
		return 1;
	if (mb->kind == H264SW_MB_I_4X4)
		return 1;
	if (mb->kind == H264SW_MB_I_8X8)
		return 1;
	if (mb->kind == H264SW_MB_I_16X16)
		return 1;
	if (mb->kind == H264SW_MB_I_PCM)
		return 1;

	/* The parser sets the intra flag of the motion for every intra macroblock. */
	if (mb->motion.intra != 0U)
		return 1;

	/* Succeeded: the macroblock is predicted from other pictures. */
	return 0;
}

/* Gives qPp or qPq for a luma edge: QPY of the macroblock, 0 for I_PCM (8.7.2). */
static int
luma_qp(
	const struct h264sw_mb *mb)
{
	/* An I_PCM macroblock counts as quantization parameter 0. */
	if (mb->kind == H264SW_MB_I_PCM)
		return 0;

	/* Succeeded: the macroblock's own QPY. */
	return (int)mb->qp;
}

/*
 * Gives qPp or qPq for a chroma edge: QPC of the macroblock's QPY (8.7.2, 8.5.8).
 *
 * component is 0 for Cb and 1 for Cr, whose offsets differ.  An I_PCM
 * macroblock uses QPC for a QPY of 0.
 */
static int
chroma_qp(
	const struct h264sw_picture_ctx *picture,
	const struct h264sw_mb *mb,
	unsigned component)
{
	int qp_y;
	int qp_i;

	/* Finds QPY of the macroblock. */
	qp_y = luma_qp(mb);

	/* Adds the component's offset and clips to the table (8-310, 8-311 with QpBdOffsetC 0). */
	qp_i = table_index(qp_y, picture->chroma_qp_offset[component]);

	/* Succeeded: QPC of Table 8-15. */
	return (int)h264sw_chroma_qp[qp_i];
}

/* Tells whether two motion vectors differ by 4 or more quarter samples in either component (8.7.2.1). */
static int
mv_differs(
	const int16_t *first,
	const int16_t *second)
{
	int horizontal;
	int vertical;

	/* Finds the absolute differences of the two components. */
	horizontal = absolute((int)first[0] - (int)second[0]);
	vertical = absolute((int)first[1] - (int)second[1]);

	/* A horizontal difference of the limit or more is a difference. */
	if (horizontal >= DEBLOCK_MV_LIMIT)
		return 1;

	/* A vertical difference of the limit or more is a difference. */
	if (vertical >= DEBLOCK_MV_LIMIT)
		return 1;

	/* Succeeded: both components are within the limit. */
	return 0;
}

/*
 * Tells whether the prediction of the partition holding p0 differs from
 * that of the partition holding q0 enough for bS 1 (8.7.2.1, frames).
 *
 * The 4x4 blocks p_blk and q_blk are luma4x4BlkIdx; the reference index
 * of a block is that of its 8x8 quadrant, the quarter of the macroblock
 * that blk / 4 names.  The reference pictures are compared by the frame id
 * each reference index named, and as a set, so the list and the position
 * in the list do not matter.
 */
static int
motion_differs(
	const struct h264sw_motion *p_motion,
	unsigned p_blk,
	const struct h264sw_motion *q_motion,
	unsigned q_blk)
{
	const int16_t *p_mv[2];
	const int16_t *q_mv[2];
	uint32_t p_ref[2];
	uint32_t q_ref[2];
	unsigned p_count;
	unsigned q_count;
	unsigned list;
	unsigned p_quad;
	unsigned q_quad;
	int same_set;
	int straight_p0;
	int straight_p1;
	int cross_p0;
	int cross_p1;
	int straight;
	int cross;

	/* Finds the quadrant of each block, which holds its reference indices. */
	p_quad = p_blk / 4U;
	q_quad = q_blk / 4U;

	/* Gathers the reference picture and vector of each list the p0 partition uses, list 0 first. */
	p_count = 0;
	for (list = 0;
	     list < 2U;
	     list++) {
		if (p_motion->ref_idx[list][p_quad] < 0)
			continue;

		/* Appends the reference picture and vector of this list. */
		p_ref[p_count] = p_motion->ref_id[list][p_quad];
		p_mv[p_count] = p_motion->mv[list][p_blk];
		p_count++;
	}

	/* Gathers the reference picture and vector of each list the q0 partition uses, list 0 first. */
	q_count = 0;
	for (list = 0;
	     list < 2U;
	     list++) {
		if (q_motion->ref_idx[list][q_quad] < 0)
			continue;

		/* Appends the reference picture and vector of this list. */
		q_ref[q_count] = q_motion->ref_id[list][q_quad];
		q_mv[q_count] = q_motion->mv[list][q_blk];
		q_count++;
	}

	/* A different number of motion vectors is a difference. */
	if (p_count != q_count)
		return 1;

	/* Two partitions that use no vector at all have nothing to differ in. */
	if (p_count == 0U)
		return 0;

	/* One vector each: the reference pictures must be the same and the vectors close. */
	if (p_count == 1U) {
		if (p_ref[0] != q_ref[0])
			return 1;

		/* Reports whether the one pair of vectors differs. */
		straight_p0 = mv_differs(p_mv[0], q_mv[0]);

		/* Succeeded: the vectors decide. */
		return straight_p0;
	}

	/* Two vectors each: the same two reference pictures, in either order, must be used. */
	same_set = 0;
	if (p_ref[0] == q_ref[0] && p_ref[1] == q_ref[1])
		same_set = 1;
	if (p_ref[0] == q_ref[1] && p_ref[1] == q_ref[0])
		same_set = 1;
	if (same_set == 0)
		return 1;

	/* Compares every pairing of a p0 vector with a q0 vector. */
	straight_p0 = mv_differs(p_mv[0], q_mv[0]);
	straight_p1 = mv_differs(p_mv[1], q_mv[1]);
	cross_p0 = mv_differs(p_mv[0], q_mv[1]);
	cross_p1 = mv_differs(p_mv[1], q_mv[0]);

	/* Two different reference pictures: each vector is compared with the one that names the same picture. */
	if (p_ref[0] != p_ref[1]) {
		if (p_ref[0] == q_ref[0]) {
			/* The lists agree on which picture comes first. */
			if (straight_p0 != 0)
				return 1;
			if (straight_p1 != 0)
				return 1;

			/* Both vectors are close, so bS is 0. */
			return 0;
		}

		/* The lists name the pictures in the opposite order. */
		if (cross_p0 != 0)
			return 1;
		if (cross_p1 != 0)
			return 1;

		/* Both crossed vectors are close, so bS is 0. */
		return 0;
	}

	/* The same picture twice: a pairing in which both vectors are close is enough for bS 0. */
	straight = 0;
	if (straight_p0 != 0 || straight_p1 != 0)
		straight = 1;
	cross = 0;
	if (cross_p0 != 0 || cross_p1 != 0)
		cross = 1;

	/* Both pairings differ: the partitions differ. */
	if (straight != 0 && cross != 0)
		return 1;

	/* Succeeded: one of the pairings is close, so the partitions do not differ. */
	return 0;
}

/*
 * Tells whether two blocks have exactly the same motion: the same lists in
 * use, the same reference picture and the same vector in each.
 *
 * Such blocks are one partition as far as bS is concerned, whether they
 * lie in one macroblock or in two, and motion_differs would answer 0 for
 * them; the exact comparison is much cheaper than gathering the motion.
 */
static int
same_motion(
	const struct h264sw_motion *p_motion,
	unsigned p_blk,
	const struct h264sw_motion *q_motion,
	unsigned q_blk)
{
	unsigned p_quad;
	unsigned q_quad;
	unsigned list;

	/* Finds the quadrant of each block, which holds its reference indices. */
	p_quad = p_blk / 4U;
	q_quad = q_blk / 4U;

	/* Compares each list: whether it is used, the reference picture and the vector. */
	for (list = 0;
	     list < 2U;
	     list++) {
		/* A list used by one block only is a difference. */
		if ((p_motion->ref_idx[list][p_quad] < 0) != (q_motion->ref_idx[list][q_quad] < 0))
			return 0;

		/* A list neither uses has nothing to compare. */
		if (p_motion->ref_idx[list][p_quad] < 0)
			continue;

		/* A different reference picture is a difference. */
		if (p_motion->ref_id[list][p_quad] != q_motion->ref_id[list][q_quad])
			return 0;

		/* A vector component that differs at all is a difference. */
		if (p_motion->mv[list][p_blk][0] != q_motion->mv[list][q_blk][0])
			return 0;
		if (p_motion->mv[list][p_blk][1] != q_motion->mv[list][q_blk][1])
			return 0;
	}

	/* Succeeded: the motion is the same. */
	return 1;
}

/*
 * Derives bS of one 4-sample segment of an edge between inter macroblocks (8.7.2.1, frames): 2, 1 or 0.
 *
 * p_blk and q_blk are the luma4x4BlkIdx of the blocks holding p0 and q0
 * of the segment's first line.  A transform block with a non-zero
 * coefficient on either side gives 2; else different reference pictures,
 * a different number of vectors or vectors that differ enough give 1.
 */
static int
segment_strength(
	const struct h264sw_mb *p_mb,
	const struct h264sw_mb *q_mb,
	unsigned p_blk,
	unsigned q_blk)
{
	int vectors_equal;
	int differs;

	/* A transform block with a non-zero coefficient on either side gives bS 2. */
	if (((p_mb->nonzero >> p_blk) & 1U) != 0U)
		return 2;
	if (((q_mb->nonzero >> q_blk) & 1U) != 0U)
		return 2;

	/* Two blocks with the same motion are one partition: bS 0 without gathering the motion. */
	vectors_equal = same_motion(&p_mb->motion, p_blk, &q_mb->motion, q_blk);
	if (vectors_equal != 0)
		return 0;

	/* Different reference pictures, a different number of vectors, or vectors that differ enough give bS 1; the rest 0. */
	differs = motion_differs(&p_mb->motion, p_blk, &q_mb->motion, q_blk);
	if (differs != 0)
		return 1;

	/* Succeeded: the partitions do not differ. */
	return 0;
}

/*
 * Derives bS of the four segments of one edge (8.7.2.1, frames) whose p0
 * lies in p_mb and q0 in q_mb (the same macroblock for an internal edge).
 *
 * The caller has checked that the edge exists and tells whether either
 * side is intra, read from the kind of a macroblock and from the intra
 * flag of its motion alike.
 */
static void
edge_strengths(
	const struct h264sw_mb *p_mb,
	const struct h264sw_mb *q_mb,
	int p_intra,
	int q_intra,
	int vertical,
	unsigned edge,
	uint8_t *bs)
{
	unsigned segment;
	unsigned q_blk;
	unsigned p_blk;
	int strength;

	/* An intra macroblock on either side gives bS 4 on a macroblock edge and bS 3 on an internal one, in every segment. */
	if (p_intra != 0 || q_intra != 0) {
		/* Gives every segment the intra strength; the macroblock edge takes the stronger one. */
		for (segment = 0;
		     segment < 4U;
		     segment++) {
			bs[segment] = 3;
			if (edge == 0U)
				bs[segment] = 4;
		}

		/* Succeeded: an intra edge needs no look at the blocks. */
		return;
	}

	/* Derives bS of each 4-sample segment along the edge. */
	for (segment = 0;
	     segment < 4U;
	     segment++) {
		/*
		 * Locates the 4x4 blocks holding q0 and p0 of the segment's
		 * first line: p0 is the sample before q0 across the edge, in
		 * the neighbour's last row or column for a macroblock edge.
		 */
		if (vertical != 0) {
			q_blk = deblock_blk_at[segment][edge];
			p_blk = deblock_blk_at[segment][(edge + 3U) % 4U];
		} else {
			q_blk = deblock_blk_at[edge][segment];
			p_blk = deblock_blk_at[(edge + 3U) % 4U][segment];
		}

		/* Derives bS of the segment from the coefficients and the motion of the two blocks. */
		strength = segment_strength(p_mb, q_mb, p_blk, q_blk);
		bs[segment] = (uint8_t)strength;
	}
}

/*
 * Derives what the filter loops need of one edge (8.7.2.2): thresholds,
 * and the mode and tC0 of each line.
 *
 * bs holds the four segment strengths and lines_per_segment is how many
 * lines each segment covers (4 for luma, 2 for 4:2:0 chroma).  A strength
 * above 4 is taken as 4.  alpha is alpha' and beta is beta' (8-456 to
 * 8-459 for 8 bits).  An edge with alpha or beta 0 is inactive: the test
 * of filterSamplesFlag needs a difference below it (8-460).
 */
static void
edge_prepare(
	struct deblock_edge *edge,
	const uint8_t *bs,
	unsigned lines_per_segment,
	int qp_av,
	int offset_a,
	int offset_b)
{
	unsigned segment;
	int index_a;
	int index_b;
	int strength;
	int mode;
	int tc0;

	/* Starts as an edge that no line can change. */
	edge->active = 0;
	edge->normal = 0;
	edge->strong = 0;

	/* An edge whose segments all have bS 0 changes nothing. */
	if ((bs[0] | bs[1] | bs[2] | bs[3]) == 0U)
		return;

	/* Derives indexA and indexB (8-454, 8-455) and the thresholds alpha and beta (Table 8-16). */
	index_a = table_index(qp_av, offset_a);
	index_b = table_index(qp_av, offset_b);
	edge->alpha = deblock_alpha[index_a];
	edge->beta = deblock_beta[index_b];

	/* A threshold of 0 lets no line through. */
	if (edge->alpha == 0)
		return;
	if (edge->beta == 0)
		return;

	/* Gives each segment's lines their mode and tC0 (Table 8-17 for the strengths below 4). */
	for (segment = 0;
	     segment < 4U;
	     segment++) {
		/* Treats a strength above the strongest as the strongest. */
		strength = bs[segment];
		if (strength > DEBLOCK_BS_STRONG)
			strength = DEBLOCK_BS_STRONG;

		/* A segment with bS 0 is left alone. */
		mode = DEBLOCK_MODE_OFF;
		tc0 = 0;
		if (strength == DEBLOCK_BS_STRONG) {
			/* The strength 4 uses the strong filter, which has no tC0. */
			mode = DEBLOCK_MODE_STRONG;
			edge->active = 1;
			edge->strong = 1;
		} else if (strength != 0) {
			/* The strengths below 4 use the normal filter with tC0 of the table. */
			mode = DEBLOCK_MODE_NORMAL;
			tc0 = deblock_tc0[strength - 1][index_a];
			edge->active = 1;
			edge->normal = 1;
		}

		/* Gives the segment's lines the mode and tC0: four lines of a luma segment, two of a chroma one. */
		if (lines_per_segment == 4U) {
			memset(&edge->mode[segment * 4U], mode, 4);
			memset(&edge->tc0[segment * 4U], tc0, 4);
		} else {
			memset(&edge->mode[segment * 2U], mode, 2);
			memset(&edge->tc0[segment * 2U], tc0, 2);
		}
	}
}

/* Gives the absolute difference of two samples. */
static uint8_t
sample_distance(
	uint8_t first,
	uint8_t second)
{
	/* The larger sample minus the smaller one. */
	if (first > second)
		return (uint8_t)(first - second);

	/* Succeeded: the other way round. */
	return (uint8_t)(second - first);
}

/* Clamps a value to the range low..high (Clip3), in 16 bits. */
static int16_t
clip_signed(
	int16_t value,
	int16_t low,
	int16_t high)
{
	/* A value below the range becomes its lower bound. */
	if (value < low)
		return low;

	/* A value above the range becomes its upper bound. */
	if (value > high)
		return high;

	/* Succeeded: the value is inside the range. */
	return value;
}

/*
 * Filters the 16 lines of a luma edge (8.7.2.3 and 8.7.2.4).
 *
 * rows[0..7][line] are p3 p2 p1 p0 q0 q1 q2 q3 of each line, so a loop
 * over the lines reads and writes consecutive bytes.  The work is split
 * into short loops of a fixed count over the 16 lines, each producing one
 * kind of result into an array, and loops that take each line's result
 * from the filter it uses: no line depends on another, and the loops are
 * short enough that the compiler runs them on 8-bit and 16-bit lanes
 * instead of widening every sample.  Only the passes the edge needs run.
 * A line that the thresholds do not let through (8-460) keeps its samples.
 */
static void
filter_luma_lines(
	uint8_t (*rows)[16],
	const struct deblock_edge *edge)
{
	uint8_t use_normal[16];
	uint8_t use_strong[16];
	uint8_t flat_p[16];
	uint8_t flat_q[16];
	uint8_t wide_p[16];
	uint8_t wide_q[16];
	uint8_t normal_p0[16];
	uint8_t normal_q0[16];
	uint8_t normal_p1[16];
	uint8_t normal_q1[16];
	uint8_t narrow_p0[16];
	uint8_t narrow_q0[16];
	uint8_t wide_p0[16];
	uint8_t wide_p1[16];
	uint8_t wide_p2[16];
	uint8_t wide_q0[16];
	uint8_t wide_q1[16];
	uint8_t wide_q2[16];
	uint8_t alpha;
	uint8_t beta;
	uint8_t gap_limit;
	uint8_t p0;
	uint8_t p1;
	uint8_t p2;
	uint8_t q0;
	uint8_t q1;
	uint8_t q2;
	uint8_t mode;
	uint8_t tc0;
	uint8_t step;
	uint8_t p_slope;
	uint8_t q_slope;
	uint8_t p_flatness;
	uint8_t q_flatness;
	uint8_t on;
	uint8_t new_sample;
	unsigned sum;
	int16_t tc;
	int16_t unclipped;
	int16_t delta;
	int16_t average;
	int line;

	/* Takes the thresholds of the edge: alpha' and beta' (8-456 to 8-459), and the largest step the strong filter takes (8-476, 8-483). */
	alpha = edge->alpha;
	beta = edge->beta;
	gap_limit = (uint8_t)((alpha >> 2) + 2);

	/* Decides for each line which filter it takes and how flat each side is (8-460, 8-463, 8-464). */
	for (line = 0;
	     line < 16;
	     line++) {
		/* Reads the samples that decide and the line's mode. */
		p2 = rows[1][line];
		p1 = rows[2][line];
		p0 = rows[3][line];
		q0 = rows[4][line];
		q1 = rows[5][line];
		q2 = rows[6][line];
		mode = edge->mode[line];

		/* Lets the line through unless its strength is 0 or the step looks like a picture detail rather than a coding artifact (8-460). */
		step = sample_distance(p0, q0);
		p_slope = sample_distance(p1, p0);
		q_slope = sample_distance(q1, q0);
		on = 1;
		if (mode == DEBLOCK_MODE_OFF)
			on = 0;
		if (step >= alpha)
			on = 0;
		if (p_slope >= beta)
			on = 0;
		if (q_slope >= beta)
			on = 0;

		/* Finds how flat each side is next to the edge (8-463, 8-464). */
		p_flatness = sample_distance(p2, p0);
		q_flatness = sample_distance(q2, q0);
		flat_p[line] = 0;
		if (p_flatness < beta)
			flat_p[line] = 1;
		flat_q[line] = 0;
		if (q_flatness < beta)
			flat_q[line] = 1;

		/* Takes the normal filter for bS 1 to 3 and the strong one for bS 4. */
		use_normal[line] = 0;
		if (on != 0 && mode == DEBLOCK_MODE_NORMAL)
			use_normal[line] = 1;
		use_strong[line] = 0;
		if (on != 0 && mode == DEBLOCK_MODE_STRONG)
			use_strong[line] = 1;

		/* The strong filter changes three samples of a side when that side is flat and the step is small (8-476, 8-483). */
		wide_p[line] = 0;
		if (step < gap_limit && flat_p[line] != 0)
			wide_p[line] = 1;
		wide_q[line] = 0;
		if (step < gap_limit && flat_q[line] != 0)
			wide_q[line] = 1;
	}

	/* Filters the lines with bS 1 to 3 (8.7.2.3), when there are any. */
	if (edge->normal != 0) {
		/* Moves p0 and q0 toward each other by the step's eighth, clipped to tC (8-465 to 8-469). */
		for (line = 0;
		     line < 16;
		     line++) {
			p1 = rows[2][line];
			p0 = rows[3][line];
			q0 = rows[4][line];
			q1 = rows[5][line];

			/* tC grows by one for each flat side (8-465); the bias makes the shift of a negative value round toward minus infinity. */
			tc = (int16_t)(edge->tc0[line] + flat_p[line] + flat_q[line]);
			unclipped = (int16_t)(((uint16_t)((q0 - p0) * 4 + (p1 - q1) + 4 + DEBLOCK_BIAS_8) >> 3) - DEBLOCK_BIAS_8 / 8);
			delta = clip_signed(unclipped, (int16_t)-tc, tc);
			normal_p0[line] = clip_sample((int16_t)(p0 + delta));
			normal_q0[line] = clip_sample((int16_t)(q0 - delta));
		}

		/* Moves p1 as well when the p side is flat, by at most tC0 (8-470, 8-471). */
		for (line = 0;
		     line < 16;
		     line++) {
			p2 = rows[1][line];
			p1 = rows[2][line];
			p0 = rows[3][line];
			q0 = rows[4][line];
			tc0 = edge->tc0[line];
			average = (int16_t)((p0 + q0 + 1) >> 1);
			unclipped = (int16_t)(((uint16_t)(p2 + average - p1 * 2 + DEBLOCK_BIAS_2) >> 1) - DEBLOCK_BIAS_2 / 2);
			delta = clip_signed(unclipped, (int16_t)-tc0, (int16_t)tc0);
			normal_p1[line] = clip_sample((int16_t)(p1 + delta));
		}

		/* Moves q1 as well when the q side is flat (8-472, 8-473). */
		for (line = 0;
		     line < 16;
		     line++) {
			q2 = rows[6][line];
			q1 = rows[5][line];
			p0 = rows[3][line];
			q0 = rows[4][line];
			tc0 = edge->tc0[line];
			average = (int16_t)((p0 + q0 + 1) >> 1);
			unclipped = (int16_t)(((uint16_t)(q2 + average - q1 * 2 + DEBLOCK_BIAS_2) >> 1) - DEBLOCK_BIAS_2 / 2);
			delta = clip_signed(unclipped, (int16_t)-tc0, (int16_t)tc0);
			normal_q1[line] = clip_sample((int16_t)(q1 + delta));
		}

		/* Takes the results for the lines that use the normal filter, one row at a time; p1 and q1 change only on a flat side. */
		for (line = 0;
		     line < 16;
		     line++) {
			new_sample = rows[2][line];
			if (use_normal[line] != 0 && flat_p[line] != 0)
				new_sample = normal_p1[line];
			rows[2][line] = new_sample;
		}
		for (line = 0;
		     line < 16;
		     line++) {
			new_sample = rows[3][line];
			if (use_normal[line] != 0)
				new_sample = normal_p0[line];
			rows[3][line] = new_sample;
		}
		for (line = 0;
		     line < 16;
		     line++) {
			new_sample = rows[4][line];
			if (use_normal[line] != 0)
				new_sample = normal_q0[line];
			rows[4][line] = new_sample;
		}
		for (line = 0;
		     line < 16;
		     line++) {
			new_sample = rows[5][line];
			if (use_normal[line] != 0 && flat_q[line] != 0)
				new_sample = normal_q1[line];
			rows[5][line] = new_sample;
		}
	}

	/* Filters the lines with bS 4 (8.7.2.4), when there are any; their samples are still as they were, a line using only one of the two filters. */
	if (edge->strong != 0) {
		/* The p0 and q0 of a side that is not wide: they take the neighbour on their own side and one beyond (8-482, 8-489). */
		for (line = 0;
		     line < 16;
		     line++) {
			sum = (unsigned)rows[2][line] * 2U + rows[3][line] + rows[5][line] + 2U;
			narrow_p0[line] = (uint8_t)(sum >> 2);
			sum = (unsigned)rows[5][line] * 2U + rows[4][line] + rows[2][line] + 2U;
			narrow_q0[line] = (uint8_t)(sum >> 2);
		}

		/* The three samples of a wide p side (8-477 to 8-479). */
		for (line = 0;
		     line < 16;
		     line++) {
			sum = (unsigned)rows[1][line] + rows[2][line] * 2U + rows[3][line] * 2U + rows[4][line] * 2U + rows[5][line] + 4U;
			wide_p0[line] = (uint8_t)(sum >> 3);
		}
		for (line = 0;
		     line < 16;
		     line++) {
			sum = (unsigned)rows[1][line] + rows[2][line] + rows[3][line] + rows[4][line] + 2U;
			wide_p1[line] = (uint8_t)(sum >> 2);
		}
		for (line = 0;
		     line < 16;
		     line++) {
			sum = (unsigned)rows[0][line] * 2U + rows[1][line] * 3U + rows[2][line] + rows[3][line] + rows[4][line] + 4U;
			wide_p2[line] = (uint8_t)(sum >> 3);
		}

		/* The three samples of a wide q side (8-484 to 8-486). */
		for (line = 0;
		     line < 16;
		     line++) {
			sum = (unsigned)rows[2][line] + rows[3][line] * 2U + rows[4][line] * 2U + rows[5][line] * 2U + rows[6][line] + 4U;
			wide_q0[line] = (uint8_t)(sum >> 3);
		}
		for (line = 0;
		     line < 16;
		     line++) {
			sum = (unsigned)rows[3][line] + rows[4][line] + rows[5][line] + rows[6][line] + 2U;
			wide_q1[line] = (uint8_t)(sum >> 2);
		}
		for (line = 0;
		     line < 16;
		     line++) {
			sum = (unsigned)rows[7][line] * 2U + rows[6][line] * 3U + rows[5][line] + rows[4][line] + rows[3][line] + 4U;
			wide_q2[line] = (uint8_t)(sum >> 3);
		}

		/* Takes the results for the lines that use the strong filter, one row at a time; p2 and p1 change on a wide p side only, p0 on every side in the filter of its width. */
		for (line = 0;
		     line < 16;
		     line++) {
			new_sample = rows[1][line];
			if (use_strong[line] != 0 && wide_p[line] != 0)
				new_sample = wide_p2[line];
			rows[1][line] = new_sample;
		}
		for (line = 0;
		     line < 16;
		     line++) {
			new_sample = rows[2][line];
			if (use_strong[line] != 0 && wide_p[line] != 0)
				new_sample = wide_p1[line];
			rows[2][line] = new_sample;
		}
		for (line = 0;
		     line < 16;
		     line++) {
			new_sample = rows[3][line];
			if (use_strong[line] != 0)
				new_sample = narrow_p0[line];
			rows[3][line] = new_sample;
		}
		for (line = 0;
		     line < 16;
		     line++) {
			new_sample = rows[3][line];
			if (use_strong[line] != 0 && wide_p[line] != 0)
				new_sample = wide_p0[line];
			rows[3][line] = new_sample;
		}

		/* The q side the same way. */
		for (line = 0;
		     line < 16;
		     line++) {
			new_sample = rows[4][line];
			if (use_strong[line] != 0)
				new_sample = narrow_q0[line];
			rows[4][line] = new_sample;
		}
		for (line = 0;
		     line < 16;
		     line++) {
			new_sample = rows[4][line];
			if (use_strong[line] != 0 && wide_q[line] != 0)
				new_sample = wide_q0[line];
			rows[4][line] = new_sample;
		}
		for (line = 0;
		     line < 16;
		     line++) {
			new_sample = rows[5][line];
			if (use_strong[line] != 0 && wide_q[line] != 0)
				new_sample = wide_q1[line];
			rows[5][line] = new_sample;
		}
		for (line = 0;
		     line < 16;
		     line++) {
			new_sample = rows[6][line];
			if (use_strong[line] != 0 && wide_q[line] != 0)
				new_sample = wide_q2[line];
			rows[6][line] = new_sample;
		}
	}
}

/*
 * Puts the lines of the Cb edge and of the Cr edge side by side as the 16
 * lines of one chroma filter loop: lines 0 to 7 are Cb's, 8 to 15 Cr's.
 *
 * cr is NULL for an edge of one component, whose lines 8 to 15 are then
 * left alone.  A component whose edge is not active is left alone too.
 * Each line carries its own thresholds, which differ between the two
 * components.
 */
static void
chroma_pair(
	struct deblock_chroma_edge *pair,
	const struct deblock_edge *cb,
	const struct deblock_edge *cr)
{
	unsigned line;
	unsigned half;
	const struct deblock_edge *source;

	/* Starts without lines for either filter. */
	pair->normal = 0;
	pair->strong = 0;

	/* Fills each half from its component's edge, or with lines that are left alone. */
	for (half = 0;
	     half < 2U;
	     half++) {
		/* Finds the component's edge: Cb for the first half, Cr for the second. */
		source = cb;
		if (half == 1U)
			source = cr;

		/* A missing or inactive component leaves its lines alone. */
		if (source == NULL || source->active == 0) {
			for (line = 0;
			     line < 8U;
			     line++) {
				pair->mode[half * 8U + line] = DEBLOCK_MODE_OFF;
				pair->tc0[half * 8U + line] = 0;
				pair->alpha[half * 8U + line] = 0;
				pair->beta[half * 8U + line] = 0;
			}
			continue;
		}

		/* Takes over which filters the component's lines use. */
		if (source->normal != 0)
			pair->normal = 1;
		if (source->strong != 0)
			pair->strong = 1;

		/* Copies the component's lines and gives each its thresholds. */
		for (line = 0;
		     line < 8U;
		     line++) {
			pair->mode[half * 8U + line] = source->mode[line];
			pair->tc0[half * 8U + line] = source->tc0[line];
			pair->alpha[half * 8U + line] = source->alpha;
			pair->beta[half * 8U + line] = source->beta;
		}
	}
}

/*
 * Filters the 16 lines of a chroma edge pair (8.7.2.3 and 8.7.2.4 with chromaStyleFilteringFlag 1).
 *
 * rows[0..3][line] are p1 p0 q0 q1 of each line.  Built as
 * filter_luma_lines is, but only p0 and q0 change and tC is tC0 + 1 (8-466).
 */
static void
filter_chroma_lines(
	uint8_t (*rows)[16],
	const struct deblock_chroma_edge *edge)
{
	uint8_t use_normal[16];
	uint8_t use_strong[16];
	uint8_t normal_p0[16];
	uint8_t normal_q0[16];
	uint8_t strong_p0[16];
	uint8_t strong_q0[16];
	uint8_t p0;
	uint8_t p1;
	uint8_t q0;
	uint8_t q1;
	uint8_t mode;
	uint8_t step;
	uint8_t p_slope;
	uint8_t q_slope;
	uint8_t on;
	uint8_t new_sample;
	unsigned sum;
	int16_t tc;
	int16_t unclipped;
	int16_t delta;
	int line;

	/* Decides for each line which filter it takes (8-460). */
	for (line = 0;
	     line < 16;
	     line++) {
		/* Reads the four samples of the line and its mode. */
		p1 = rows[0][line];
		p0 = rows[1][line];
		q0 = rows[2][line];
		q1 = rows[3][line];
		mode = edge->mode[line];

		/* Lets the line through unless its strength is 0 or the step looks like a picture detail rather than a coding artifact (8-460). */
		step = sample_distance(p0, q0);
		p_slope = sample_distance(p1, p0);
		q_slope = sample_distance(q1, q0);
		on = 1;
		if (mode == DEBLOCK_MODE_OFF)
			on = 0;
		if (step >= edge->alpha[line])
			on = 0;
		if (p_slope >= edge->beta[line])
			on = 0;
		if (q_slope >= edge->beta[line])
			on = 0;

		/* Takes the normal filter for bS 1 to 3 and the strong one for bS 4. */
		use_normal[line] = 0;
		if (on != 0 && mode == DEBLOCK_MODE_NORMAL)
			use_normal[line] = 1;
		use_strong[line] = 0;
		if (on != 0 && mode == DEBLOCK_MODE_STRONG)
			use_strong[line] = 1;
	}

	/* Filters the lines with bS 1 to 3, when there are any: p0 and q0 move toward each other, clipped to tC0 + 1 (8-466 to 8-469). */
	if (edge->normal != 0) {
		/* Works out the new p0 and q0; the bias makes the shift of a negative value round toward minus infinity. */
		for (line = 0;
		     line < 16;
		     line++) {
			p1 = rows[0][line];
			p0 = rows[1][line];
			q0 = rows[2][line];
			q1 = rows[3][line];
			tc = (int16_t)(edge->tc0[line] + 1);
			unclipped = (int16_t)(((uint16_t)((q0 - p0) * 4 + (p1 - q1) + 4 + DEBLOCK_BIAS_8) >> 3) - DEBLOCK_BIAS_8 / 8);
			delta = clip_signed(unclipped, (int16_t)-tc, tc);
			normal_p0[line] = clip_sample((int16_t)(p0 + delta));
			normal_q0[line] = clip_sample((int16_t)(q0 - delta));
		}

		/* Takes them for the lines that use the normal filter, one row at a time. */
		for (line = 0;
		     line < 16;
		     line++) {
			new_sample = rows[1][line];
			if (use_normal[line] != 0)
				new_sample = normal_p0[line];
			rows[1][line] = new_sample;
		}
		for (line = 0;
		     line < 16;
		     line++) {
			new_sample = rows[2][line];
			if (use_normal[line] != 0)
				new_sample = normal_q0[line];
			rows[2][line] = new_sample;
		}
	}

	/* Filters the lines with bS 4, when there are any: the 3-tap averages (8-480, 8-487); their samples are still as they were. */
	if (edge->strong != 0) {
		/* Works out the new p0 and q0. */
		for (line = 0;
		     line < 16;
		     line++) {
			sum = (unsigned)rows[0][line] * 2U + rows[1][line] + rows[3][line] + 2U;
			strong_p0[line] = (uint8_t)(sum >> 2);
			sum = (unsigned)rows[3][line] * 2U + rows[2][line] + rows[0][line] + 2U;
			strong_q0[line] = (uint8_t)(sum >> 2);
		}

		/* Takes them for the lines that use the strong filter, one row at a time. */
		for (line = 0;
		     line < 16;
		     line++) {
			new_sample = rows[1][line];
			if (use_strong[line] != 0)
				new_sample = strong_p0[line];
			rows[1][line] = new_sample;
		}
		for (line = 0;
		     line < 16;
		     line++) {
			new_sample = rows[2][line];
			if (use_strong[line] != 0)
				new_sample = strong_q0[line];
			rows[2][line] = new_sample;
		}
	}
}

/*
 * Tells whether a picture's arrays are consistent enough to filter.
 *
 * The macroblock grid must match size_mbs, the planes must exist and
 * hold the grid, and the slice array must hold the slices counted.
 */
static int
picture_is_usable(
	const struct h264sw_picture_ctx *picture)
{
	const struct h264sw_frame *frame;
	unsigned luma_width;
	unsigned luma_height;

	/* Refuses a missing picture. */
	if (picture == NULL)
		return 0;

	/* Refuses missing arrays. */
	if (picture->mbs == NULL)
		return 0;
	if (picture->slices == NULL)
		return 0;
	if (picture->frame == NULL)
		return 0;

	/* Refuses a grid of no macroblocks or beyond the decoder's limits. */
	if (picture->width_mbs == 0U)
		return 0;
	if (picture->height_mbs == 0U)
		return 0;
	if (picture->width_mbs > H264SW_MAX_SIDE_MBS)
		return 0;
	if (picture->height_mbs > H264SW_MAX_SIDE_MBS)
		return 0;

	/* Refuses a macroblock count that is not the grid's. */
	if (picture->size_mbs != picture->width_mbs * picture->height_mbs)
		return 0;

	/* Refuses more slices than the array holds. */
	if (picture->slice_count > picture->slice_capacity)
		return 0;

	/* Finds the luma size the grid needs. */
	frame = picture->frame;
	luma_width = picture->width_mbs * 16U;
	luma_height = picture->height_mbs * 16U;

	/* Refuses a frame smaller than the grid. */
	if (frame->width < luma_width)
		return 0;
	if (frame->height < luma_height)
		return 0;

	/* Refuses planes that are missing. */
	if (frame->plane[0] == NULL)
		return 0;
	if (frame->plane[1] == NULL)
		return 0;
	if (frame->plane[2] == NULL)
		return 0;

	/* Refuses rows too short for the grid or too long for the pointer arithmetic. */
	if (frame->stride[0] < luma_width)
		return 0;
	if (frame->stride[1] < luma_width / 2U)
		return 0;
	if (frame->stride[2] < luma_width / 2U)
		return 0;
	if (frame->stride[0] > DEBLOCK_STRIDE_MAX)
		return 0;
	if (frame->stride[1] > DEBLOCK_STRIDE_MAX)
		return 0;
	if (frame->stride[2] > DEBLOCK_STRIDE_MAX)
		return 0;

	/* Succeeded: the picture can be filtered. */
	return 1;
}

/*
 * Tells whether the edge shared with a neighbouring macroblock is filtered (8.7, steps 2.c and 2.d).
 *
 * The neighbour must be a decoded macroblock of a recorded slice; with
 * disable_deblocking_filter_idc 2 it must also be in the current
 * macroblock's slice (another slice's macroblock is "not available").
 */
static int
neighbour_is_filtered(
	const struct h264sw_picture_ctx *picture,
	const struct h264sw_mb *q_mb,
	const struct h264sw_mb *neighbour,
	unsigned idc)
{
	/* A neighbour that was never decoded cannot be filtered against. */
	if (neighbour->slice < 0)
		return 0;

	/* A neighbour whose slice is not recorded cannot be trusted. */
	if ((uint32_t)neighbour->slice >= picture->slice_count)
		return 0;

	/* With idc 2 a neighbour in another slice is not available. */
	if (idc == 2U && neighbour->slice != q_mb->slice)
		return 0;

	/* Succeeded: the edge is filtered. */
	return 1;
}

/*
 * Tells whether an edge with this qPav lets no line through: alpha or beta
 * is 0 (Table 8-16 for indexA or indexB below 16), and the test of
 * filterSamplesFlag needs a difference below both (8-460).
 */
static int
thresholds_close(
	int qp_av,
	int offset_a,
	int offset_b)
{
	int index_a;
	int index_b;

	/* Derives indexA and indexB (8-454, 8-455). */
	index_a = table_index(qp_av, offset_a);
	index_b = table_index(qp_av, offset_b);

	/* Either threshold 0 closes the edge. */
	if (deblock_alpha[index_a] == 0)
		return 1;
	if (deblock_beta[index_b] == 0)
		return 1;

	/* Succeeded: some line may change. */
	return 0;
}

/*
 * Gives the largest qPav (8-453) of the edges a macroblock filters: the
 * internal edges' qp_q and the macroblock edges' averages with the left
 * and the upper neighbour when those edges are filtered.
 */
static int
largest_qp_av(
	const struct deblock_mb *mb,
	int qp_q,
	int qp_left,
	int qp_top)
{
	int largest;
	int qp_av;

	/* The internal edges use the macroblock's own. */
	largest = qp_q;

	/* The left macroblock edge, when filtered. */
	if (mb->filter_left != 0) {
		qp_av = (qp_left + qp_q + 1) >> 1;
		if (qp_av > largest)
			largest = qp_av;
	}

	/* The top macroblock edge, when filtered. */
	if (mb->filter_top != 0) {
		qp_av = (qp_top + qp_q + 1) >> 1;
		if (qp_av > largest)
			largest = qp_av;
	}

	/* Succeeded: the largest of them. */
	return largest;
}

/*
 * Tells whether no edge of a macroblock can change a sample, from the
 * quantization parameters alone, so that its strengths need not be
 * derived.
 *
 * indexA and indexB grow with qPav and alpha' and beta' with them (Table
 * 8-16), so an edge with a smaller qPav than one whose alpha or beta is 0
 * has a threshold 0 too: the largest qPav of the luma edges, and of each
 * chroma component's, decides for all of them.  The offsets are the
 * slice's, the same for every edge of the macroblock.
 */
static int
macroblock_is_quiet(
	const struct deblock_mb *mb)
{
	unsigned component;
	int qp_q;
	int qp_left;
	int qp_top;
	int qp_av;
	int closed;

	/* The luma edges: the macroblocks' QPY. */
	qp_left = 0;
	qp_top = 0;
	if (mb->filter_left != 0)
		qp_left = luma_qp(mb->left_mb);
	if (mb->filter_top != 0)
		qp_top = luma_qp(mb->top_mb);
	qp_q = luma_qp(mb->q_mb);
	qp_av = largest_qp_av(mb, qp_q, qp_left, qp_top);
	closed = thresholds_close(qp_av, mb->slice->filter_offset_a, mb->slice->filter_offset_b);
	if (closed == 0)
		return 0;

	/* The chroma edges of Cb and Cr: each component's QPC. */
	for (component = 0;
	     component < 2U;
	     component++) {
		/* The macroblock's QPC, and the neighbours' where their edges are filtered. */
		qp_q = chroma_qp(mb->picture, mb->q_mb, component);
		if (mb->filter_left != 0)
			qp_left = chroma_qp(mb->picture, mb->left_mb, component);
		if (mb->filter_top != 0)
			qp_top = chroma_qp(mb->picture, mb->top_mb, component);

		/* The component's largest qPav decides its edges. */
		qp_av = largest_qp_av(mb, qp_q, qp_left, qp_top);
		closed = thresholds_close(qp_av, mb->slice->filter_offset_a, mb->slice->filter_offset_b);
		if (closed == 0)
			return 0;
	}

	/* Succeeded: every edge is closed. */
	return 1;
}

/*
 * Derives bS of every edge of the macroblock once (8.7.2.1).
 *
 * They depend only on the macroblocks, not on the samples the filter
 * changes, and the chroma edges take the luma edges' strengths.  A
 * macroblock edge that is not filtered keeps bS 0, and so do the internal
 * edges at 4 and 12 samples of a macroblock with 8x8 transform blocks.
 * Whether each macroblock is intra is read once, not once per edge.
 */
static void
macroblock_strengths(
	struct deblock_mb *mb)
{
	const struct h264sw_mb *q_mb;
	unsigned edge;
	int q_intra;
	int p_intra;

	/* Starts from bS 0 everywhere. */
	memset(mb->vertical_bs, 0, sizeof(mb->vertical_bs));
	memset(mb->horizontal_bs, 0, sizeof(mb->horizontal_bs));

	/* Reads whether the macroblock itself is intra. */
	q_mb = mb->q_mb;
	q_intra = is_intra(q_mb);

	/* Derives the left macroblock edge against the left neighbour when it is filtered. */
	if (mb->filter_left != 0) {
		p_intra = is_intra(mb->left_mb);
		edge_strengths(mb->left_mb, q_mb, p_intra, q_intra, 1, 0U, mb->vertical_bs[0]);
	}

	/* Derives the top macroblock edge against the upper neighbour when it is filtered. */
	if (mb->filter_top != 0) {
		p_intra = is_intra(mb->top_mb);
		edge_strengths(mb->top_mb, q_mb, p_intra, q_intra, 0, 0U, mb->horizontal_bs[0]);
	}

	/* Derives the internal edges; those at 4 and 12 samples exist only for 4x4 transform blocks. */
	for (edge = 1;
	     edge < 4U;
	     edge++) {
		/* Skips the edges at 4 and 12 samples of a macroblock with 8x8 transform blocks. */
		if ((edge & 1U) != 0U && q_mb->transform_8x8 != 0U)
			continue;

		/* Derives the vertical and the horizontal internal edge at this offset. */
		edge_strengths(q_mb, q_mb, q_intra, q_intra, 1, edge, mb->vertical_bs[edge]);
		edge_strengths(q_mb, q_mb, q_intra, q_intra, 0, edge, mb->horizontal_bs[edge]);
	}
}

/*
 * Filters one line of a luma vertical edge (8.7.2.3 and 8.7.2.4), a line
 * being a row of the picture and the edge a column.
 *
 * q0_pointer points to the sample q0, the samples p3 to q3 being the eight
 * bytes from q0_pointer - 4.  mode is DEBLOCK_MODE_NORMAL (bS 1 to 3) or
 * _STRONG (bS 4), alpha and beta the thresholds, tc0 the tC0 of the normal
 * filter.  A vertical edge's samples lie next to each other in the row, so
 * the line is filtered where it lies instead of being turned a quarter
 * into a block for the loops that filter 16 lines side by side.  The
 * choices inside a line are made by selecting among values worked out
 * anyway, so that the line has one hard-to-predict branch, the one that
 * leaves it alone.
 */
static void
filter_luma_row(
	uint8_t *q0_pointer,
	int mode,
	int alpha,
	int beta,
	int tc0)
{
	int p0;
	int p1;
	int p2;
	int p3;
	int q0;
	int q1;
	int q2;
	int q3;
	int step;
	int p_slope;
	int q_slope;
	int ap;
	int aq;
	int tc;
	int delta;
	int unclipped;
	int average;
	int small_gap;
	int new_p0;
	int new_p1;
	int new_p2;
	int new_q0;
	int new_q1;
	int new_q2;

	/* Reads the samples that decide whether the line is filtered. */
	p0 = q0_pointer[-1];
	p1 = q0_pointer[-2];
	q0 = q0_pointer[0];
	q1 = q0_pointer[1];
	step = absolute(p0 - q0);
	p_slope = absolute(p1 - p0);
	q_slope = absolute(q1 - q0);

	/* Leaves the line alone unless the step across the edge and the slopes beside it are small enough (8-460): a negative difference from a threshold, in any of the three, ends it. */
	if (((alpha - 1 - step) | (beta - 1 - p_slope) | (beta - 1 - q_slope)) < 0)
		return;

	/* Reads the rest of the samples and finds how flat each side is next to the edge (8-463, 8-464). */
	p2 = q0_pointer[-3];
	q2 = q0_pointer[2];
	p3 = q0_pointer[-4];
	q3 = q0_pointer[3];
	ap = absolute(p2 - p0);
	aq = absolute(q2 - q0);

	/* Filters a line with bS 1 to 3 (8.7.2.3). */
	if (mode == DEBLOCK_MODE_NORMAL) {
		/* Widens the clip of the change by one for each flat side (8-465). */
		tc = tc0;
		if (ap < beta)
			tc++;
		if (aq < beta)
			tc++;

		/* Moves p0 and q0 toward each other (8-467 to 8-469); the bias makes the shift of a negative value round toward minus infinity. */
		unclipped = ((((q0 - p0) * 4 + (p1 - q1) + 4) + DEBLOCK_BIAS_8) >> 3) - DEBLOCK_BIAS_8 / 8;
		delta = clip_signed((int16_t)unclipped, (int16_t)-tc, (int16_t)tc);
		new_p0 = clip_sample((int16_t)(p0 + delta));
		new_q0 = clip_sample((int16_t)(q0 - delta));

		/* Moves p1 as well when the p side is flat (8-470): by 0 when it is not. */
		average = (p0 + q0 + 1) >> 1;
		unclipped = ((p2 + average - p1 * 2 + DEBLOCK_BIAS_2) >> 1) - DEBLOCK_BIAS_2 / 2;
		delta = clip_signed((int16_t)unclipped, (int16_t)-tc0, (int16_t)tc0);
		if (ap >= beta)
			delta = 0;
		new_p1 = clip_sample((int16_t)(p1 + delta));

		/* Moves q1 as well when the q side is flat (8-472). */
		unclipped = ((q2 + average - q1 * 2 + DEBLOCK_BIAS_2) >> 1) - DEBLOCK_BIAS_2 / 2;
		delta = clip_signed((int16_t)unclipped, (int16_t)-tc0, (int16_t)tc0);
		if (aq >= beta)
			delta = 0;
		new_q1 = clip_sample((int16_t)(q1 + delta));

		/* Writes the four samples that may have changed. */
		q0_pointer[-2] = (uint8_t)new_p1;
		q0_pointer[-1] = (uint8_t)new_p0;
		q0_pointer[0] = (uint8_t)new_q0;
		q0_pointer[1] = (uint8_t)new_q1;

		/* Succeeded: the line is filtered. */
		return;
	}

	/* Filters a line with bS 4 (8.7.2.4): finds whether the step is small enough for the strong filter (8-476, 8-483). */
	small_gap = 0;
	if (step < (alpha >> 2) + 2)
		small_gap = 1;

	/* The p side: p0 from its neighbours (8-482), and three samples when it is flat and the step small (8-477 to 8-479). */
	new_p0 = (2 * p1 + p0 + q1 + 2) >> 2;
	new_p1 = p1;
	new_p2 = p2;
	if (ap < beta && small_gap != 0) {
		new_p0 = (p2 + 2 * p1 + 2 * p0 + 2 * q0 + q1 + 4) >> 3;
		new_p1 = (p2 + p1 + p0 + q0 + 2) >> 2;
		new_p2 = (2 * p3 + 3 * p2 + p1 + p0 + q0 + 4) >> 3;
	}

	/* The q side the same way (8-484 to 8-489). */
	new_q0 = (2 * q1 + q0 + p1 + 2) >> 2;
	new_q1 = q1;
	new_q2 = q2;
	if (aq < beta && small_gap != 0) {
		new_q0 = (p1 + 2 * p0 + 2 * q0 + 2 * q1 + q2 + 4) >> 3;
		new_q1 = (p0 + q0 + q1 + q2 + 2) >> 2;
		new_q2 = (2 * q3 + 3 * q2 + q1 + q0 + p0 + 4) >> 3;
	}

	/* Writes the six samples that may have changed. */
	q0_pointer[-3] = (uint8_t)new_p2;
	q0_pointer[-2] = (uint8_t)new_p1;
	q0_pointer[-1] = (uint8_t)new_p0;
	q0_pointer[0] = (uint8_t)new_q0;
	q0_pointer[1] = (uint8_t)new_q1;
	q0_pointer[2] = (uint8_t)new_q2;
}

/*
 * Filters the four luma vertical edges of a macroblock (8.7, step 3.a).
 *
 * The edges at 0, 4, 8 and 12 samples go left to right, each reading what
 * the one before changed, and each line of an edge is filtered where it
 * lies in the picture's row (filter_luma_row).
 */
static void
deblock_luma_vertical(
	const struct deblock_mb *mb)
{
	struct deblock_edge edge;
	const struct h264sw_frame *frame;
	uint8_t *plane;
	uint8_t *line_pointer;
	size_t stride;
	unsigned edge_index;
	unsigned segment;
	unsigned line;
	int qp_av;
	int qp_p;

	/* Finds the plane and its row distance. */
	frame = mb->picture->frame;
	plane = frame->plane[0];
	stride = frame->stride[0];

	/* Filters the edges left to right. */
	for (edge_index = 0;
	     edge_index < 4U;
	     edge_index++) {
		/* The macroblock edge is filtered only when its flag allows it. */
		if (edge_index == 0U && mb->filter_left == 0)
			continue;

		/* The edges at 4 and 12 samples belong to 4x4 transform blocks only. */
		if ((edge_index & 1U) != 0U && mb->q_mb->transform_8x8 != 0U)
			continue;

		/* Averages the quantization parameters of the macroblocks holding p0 and q0 (8-453): the macroblock's own for an internal edge. */
		qp_p = luma_qp(mb->q_mb);
		if (edge_index == 0U)
			qp_p = luma_qp(mb->left_mb);
		qp_av = (qp_p + luma_qp(mb->q_mb) + 1) >> 1;

		/* Derives the edge with the offsets of the slice holding q0, and leaves it alone when no line can change. */
		edge_prepare(&edge, mb->vertical_bs[edge_index], 4U, qp_av, mb->slice->filter_offset_a, mb->slice->filter_offset_b);
		if (edge.active == 0)
			continue;

		/* Filters the 16 rows of the edge, the four rows of a segment sharing their strength. */
		line_pointer = plane + (size_t)mb->mb_y * 16U * stride + (size_t)mb->mb_x * 16U + 4U * edge_index;
		for (segment = 0;
		     segment < 4U;
		     segment++) {
			/* A segment with bS 0 is left alone. */
			if (edge.mode[segment * 4U] == DEBLOCK_MODE_OFF) {
				line_pointer += 4U * stride;
				continue;
			}

			/* Filters the four rows of the segment. */
			for (line = 0;
			     line < 4U;
			     line++) {
				filter_luma_row(line_pointer, edge.mode[segment * 4U], edge.alpha, edge.beta, edge.tc0[segment * 4U]);
				line_pointer += stride;
			}
		}
	}
}

/*
 * Filters the four luma horizontal edges of a macroblock (8.7, step 3.b).
 *
 * The macroblock's 16 rows and the four rows above it are copied into 20
 * rows of 16 samples; each edge is 8 such rows (p3 to q3) filtered by
 * filter_luma_lines, the edges at 0, 4, 8 and 12 samples going top to
 * bottom in the same block, each reading what the one before changed.
 */
static void
deblock_luma_horizontal(
	const struct deblock_mb *mb)
{
	struct deblock_edge edges[4];
	uint8_t rows[20][16];
	const struct h264sw_frame *frame;
	uint8_t *plane;
	uint8_t *row_start;
	size_t stride;
	unsigned edge;
	unsigned first;
	unsigned end;
	unsigned row;
	int any;
	int qp_av;
	int qp_p;

	/* Finds the plane and its row distance. */
	frame = mb->picture->frame;
	plane = frame->plane[0];
	stride = frame->stride[0];

	/* Prepares each edge: whether it is filtered, its thresholds, and the mode and tC0 of each line. */
	any = 0;
	for (edge = 0;
	     edge < 4U;
	     edge++) {
		edges[edge].active = 0;

		/* The macroblock edge is filtered only when its flag allows it. */
		if (edge == 0U && mb->filter_top == 0)
			continue;

		/* The edges at 4 and 12 samples belong to 4x4 transform blocks only. */
		if ((edge & 1U) != 0U && mb->q_mb->transform_8x8 != 0U)
			continue;

		/* Averages the quantization parameters of the macroblocks holding p0 and q0 (8-453): the macroblock's own for an internal edge. */
		qp_p = luma_qp(mb->q_mb);
		if (edge == 0U)
			qp_p = luma_qp(mb->top_mb);
		qp_av = (qp_p + luma_qp(mb->q_mb) + 1) >> 1;

		/* Derives the edge with the offsets of the slice holding q0. */
		edge_prepare(&edges[edge], mb->horizontal_bs[edge], 4U, qp_av, mb->slice->filter_offset_a, mb->slice->filter_offset_b);
		if (edges[edge].active != 0)
			any = 1;
	}

	/* Leaves a macroblock alone whose edges change nothing. */
	if (any == 0)
		return;

	/* Takes only the rows the active edges need: edge n has p3 in row 4 * n and q3 in row 4 * n + 7. */
	first = 20;
	end = 0;
	for (edge = 0;
	     edge < 4U;
	     edge++) {
		if (edges[edge].active == 0)
			continue;
		if (4U * edge < first)
			first = 4U * edge;
		end = 4U * edge + 8U;
	}

	/* Copies the rows: rows[k] is the row k - 4 rows from the macroblock's top edge. */
	for (row = first;
	     row < end;
	     row++) {
		row_start = plane + ((size_t)mb->mb_y * 16U + row - 4U) * stride + (size_t)mb->mb_x * 16U;
		memcpy(rows[row], row_start, 16);
	}

	/* Filters the edges top to bottom; edge n has p3 in row 4 * n. */
	for (edge = 0;
	     edge < 4U;
	     edge++) {
		if (edges[edge].active != 0)
			filter_luma_lines(&rows[4U * edge], &edges[edge]);
	}

	/* Copies the rows back. */
	for (row = first;
	     row < end;
	     row++) {
		row_start = plane + ((size_t)mb->mb_y * 16U + row - 4U) * stride + (size_t)mb->mb_x * 16U;
		memcpy(row_start, rows[row], 16);
	}
}

/*
 * Filters one line of a chroma vertical edge (8.7.2.3 and 8.7.2.4 with
 * chromaStyleFilteringFlag 1): only p0 and q0 change.
 *
 * q0_pointer, mode, alpha, beta and tc0 are as for filter_luma_row.
 */
static void
filter_chroma_row(
	uint8_t *q0_pointer,
	int mode,
	int alpha,
	int beta,
	int tc0)
{
	int p0;
	int p1;
	int q0;
	int q1;
	int step;
	int p_slope;
	int q_slope;
	int tc;
	int delta;
	int unclipped;
	int new_p0;
	int new_q0;

	/* Reads the four samples of the line and measures the step and the slopes. */
	p0 = q0_pointer[-1];
	p1 = q0_pointer[-2];
	q0 = q0_pointer[0];
	q1 = q0_pointer[1];

	step = absolute(p0 - q0);
	p_slope = absolute(p1 - p0);
	q_slope = absolute(q1 - q0);

	/* Leaves the line alone unless the step across the edge and the slopes beside it are small enough (8-460): a negative difference from a threshold, in any of the three, ends it. */
	if (((alpha - 1 - step) | (beta - 1 - p_slope) | (beta - 1 - q_slope)) < 0)
		return;

	/* Works out the filter of bS 1 to 3: tC is tC0 + 1 (8-466) and p0 and q0 move toward each other (8-467 to 8-469); the bias makes the shift of a negative value round toward minus infinity. */
	tc = tc0 + 1;
	unclipped = ((((q0 - p0) * 4 + (p1 - q1) + 4) + DEBLOCK_BIAS_8) >> 3) - DEBLOCK_BIAS_8 / 8;
	delta = clip_signed((int16_t)unclipped, (int16_t)-tc, (int16_t)tc);
	new_p0 = clip_sample((int16_t)(p0 + delta));
	new_q0 = clip_sample((int16_t)(q0 - delta));

	/* Works out the filter of bS 4 instead: the 3-tap averages (8-480, 8-487). */
	if (mode != DEBLOCK_MODE_NORMAL) {
		new_p0 = (2 * p1 + p0 + q1 + 2) >> 2;
		new_q0 = (2 * q1 + q0 + p1 + 2) >> 2;
	}

	/* Writes the two samples that may have changed. */
	q0_pointer[-1] = (uint8_t)new_p0;
	q0_pointer[0] = (uint8_t)new_q0;
}

/*
 * Filters the two vertical edges of Cb and Cr of a macroblock (8.7, step 3.e, 4:2:0).
 *
 * The chroma edges are at 0 and 4 samples; the one at 4 corresponds to the
 * luma edge at 8 samples and takes its strengths, and it is filtered
 * whatever transform_size_8x8_flag is.  Each line of an edge is filtered
 * where it lies in the picture's row (filter_chroma_row).
 */
static void
deblock_chroma_vertical(
	const struct deblock_mb *mb)
{
	struct deblock_edge edge;
	const struct h264sw_frame *frame;
	uint8_t *line_pointer;
	size_t stride;
	unsigned component;
	unsigned chroma_edge;
	unsigned segment;
	unsigned line;
	int qp_av;
	int qp_p;
	int qp_q;

	/* Filters each component's edges: Cb and Cr never read each other's samples. */
	frame = mb->picture->frame;
	for (component = 0;
	     component < 2U;
	     component++) {
		/* Finds the plane's row distance and the chroma quantization parameter of the macroblock holding q0. */
		stride = frame->stride[1U + component];
		qp_q = chroma_qp(mb->picture, mb->q_mb, component);

		/* Filters the edges left to right. */
		for (chroma_edge = 0;
		     chroma_edge < 2U;
		     chroma_edge++) {
			/* The macroblock edge is filtered only when its flag allows it. */
			if (chroma_edge == 0U && mb->filter_left == 0)
				continue;

			/* Averages the chroma quantization parameters (8-453): the macroblock's own for the internal edge. */
			qp_p = qp_q;
			if (chroma_edge == 0U)
				qp_p = chroma_qp(mb->picture, mb->left_mb, component);
			qp_av = (qp_p + qp_q + 1) >> 1;

			/* Derives the edge from the strengths of the luma edge it corresponds to, with the offsets of the slice holding q0; leaves it alone when no line can change. */
			edge_prepare(&edge, mb->vertical_bs[2U * chroma_edge], 2U, qp_av, mb->slice->filter_offset_a, mb->slice->filter_offset_b);
			if (edge.active == 0)
				continue;

			/* Filters the 8 rows of the edge, the two rows of a segment sharing their strength. */
			line_pointer = frame->plane[1U + component] + (size_t)mb->mb_y * 8U * stride + (size_t)mb->mb_x * 8U + 4U * chroma_edge;
			for (segment = 0;
			     segment < 4U;
			     segment++) {
				/* A segment with bS 0 is left alone. */
				if (edge.mode[segment * 2U] == DEBLOCK_MODE_OFF) {
					line_pointer += 2U * stride;
					continue;
				}

				/* Filters the two rows of the segment. */
				for (line = 0;
				     line < 2U;
				     line++) {
					filter_chroma_row(line_pointer, edge.mode[segment * 2U], edge.alpha, edge.beta, edge.tc0[segment * 2U]);
					line_pointer += stride;
				}
			}
		}
	}
}

/*
 * Filters the two horizontal edges of Cb and Cr of a macroblock (8.7, step 3.e, 4:2:0).
 *
 * As deblock_chroma_vertical with rows: the 8 rows of each component and
 * the two rows above the macroblock are copied into 10 rows of 16 samples
 * (Cb's columns 0 to 7, Cr's 8 to 15).
 */
static void
deblock_chroma_horizontal(
	const struct deblock_mb *mb)
{
	struct deblock_edge edges[2][2];
	struct deblock_chroma_edge pairs[2];
	uint8_t rows[10][16];
	const struct h264sw_frame *frame;
	uint8_t *row_start;
	size_t stride;
	unsigned component;
	unsigned chroma_edge;
	unsigned first;
	unsigned end;
	unsigned row;
	int any;
	int qp_av;
	int qp_p;
	int qp_q;

	/* Prepares each edge of each component. */
	frame = mb->picture->frame;
	any = 0;
	for (component = 0;
	     component < 2U;
	     component++) {
		/* Finds the chroma quantization parameter of the macroblock holding q0. */
		qp_q = chroma_qp(mb->picture, mb->q_mb, component);
		for (chroma_edge = 0;
		     chroma_edge < 2U;
		     chroma_edge++) {
			edges[component][chroma_edge].active = 0;

			/* The macroblock edge is filtered only when its flag allows it. */
			if (chroma_edge == 0U && mb->filter_top == 0)
				continue;

			/* Averages the chroma quantization parameters (8-453): the macroblock's own for the internal edge. */
			qp_p = qp_q;
			if (chroma_edge == 0U)
				qp_p = chroma_qp(mb->picture, mb->top_mb, component);
			qp_av = (qp_p + qp_q + 1) >> 1;

			/* Derives the edge from the strengths of the luma edge it corresponds to, with the offsets of the slice holding q0. */
			edge_prepare(&edges[component][chroma_edge], mb->horizontal_bs[2U * chroma_edge], 2U, qp_av, mb->slice->filter_offset_a, mb->slice->filter_offset_b);
			if (edges[component][chroma_edge].active != 0)
				any = 1;
		}
	}

	/* Leaves a macroblock alone whose edges change nothing. */
	if (any == 0)
		return;

	/* Puts Cb's and Cr's lines of each edge side by side. */
	for (chroma_edge = 0;
	     chroma_edge < 2U;
	     chroma_edge++) {
		chroma_pair(&pairs[chroma_edge], &edges[0][chroma_edge], &edges[1][chroma_edge]);
	}

	/* Takes only the rows the active edges need: the edge at 4 samples has p1 in row 4 and q1 in row 7. */
	first = 10;
	end = 0;
	for (chroma_edge = 0;
	     chroma_edge < 2U;
	     chroma_edge++) {
		if (edges[0][chroma_edge].active == 0 && edges[1][chroma_edge].active == 0)
			continue;
		if (4U * chroma_edge < first)
			first = 4U * chroma_edge;
		end = 4U * chroma_edge + 4U;
	}

	/* Copies the rows of each component: rows[k] is the row k - 2 rows from the macroblock's top edge. */
	for (component = 0;
	     component < 2U;
	     component++) {
		stride = frame->stride[1U + component];
		for (row = first;
		     row < end;
		     row++) {
			row_start = frame->plane[1U + component] + ((size_t)mb->mb_y * 8U + row - 2U) * stride + (size_t)mb->mb_x * 8U;
			memcpy(&rows[row][component * 8U], row_start, 8);
		}
	}

	/* Filters the edges top to bottom; the edge at 4 samples has p1 in row 4. */
	for (chroma_edge = 0;
	     chroma_edge < 2U;
	     chroma_edge++) {
		if (edges[0][chroma_edge].active != 0 || edges[1][chroma_edge].active != 0)
			filter_chroma_lines(&rows[4U * chroma_edge], &pairs[chroma_edge]);
	}

	/* Copies the rows back. */
	for (component = 0;
	     component < 2U;
	     component++) {
		stride = frame->stride[1U + component];
		for (row = first;
		     row < end;
		     row++) {
			row_start = frame->plane[1U + component] + ((size_t)mb->mb_y * 8U + row - 2U) * stride + (size_t)mb->mb_x * 8U;
			memcpy(row_start, &rows[row][component * 8U], 8);
		}
	}
}

/*
 * Filters the edges one macroblock controls (8.7).
 *
 * Derives filterLeftMbEdgeFlag, filterTopMbEdgeFlag and
 * filterInternalEdgesFlag from the slice's disable_deblocking_filter_idc,
 * then filters the luma vertical edges, the luma horizontal edges, and
 * for Cb and then Cr the vertical and horizontal chroma edges.
 */
static void
deblock_macroblock(
	const struct h264sw_picture_ctx *picture,
	unsigned mb_addr)
{
	struct deblock_mb mb;
	unsigned idc;
	int quiet;

	/* Finds the macroblock and its position. */
	mb.picture = picture;
	mb.q_mb = &picture->mbs[mb_addr];
	mb.mb_x = mb_addr % picture->width_mbs;
	mb.mb_y = mb_addr / picture->width_mbs;
	mb.left_mb = NULL;
	mb.top_mb = NULL;

	/* Leaves a macroblock whose slice is unknown alone. */
	if (mb.q_mb->slice < 0)
		return;
	if ((uint32_t)mb.q_mb->slice >= picture->slice_count)
		return;

	/* Reads the slice's parameters, among them disable_deblocking_filter_idc. */
	mb.slice = &picture->slices[mb.q_mb->slice];
	idc = mb.slice->disable_deblocking;

	/* Idc 1 switches the filter off for the macroblock's edges altogether. */
	if (idc == 1U)
		return;

	/* Decides on the left macroblock edge (8.7, step 2.c): the picture's border switches it off, and so does an unavailable neighbour. */
	mb.filter_left = 0;
	if (mb.mb_x != 0U) {
		mb.left_mb = mb.q_mb - 1;
		mb.filter_left = neighbour_is_filtered(picture, mb.q_mb, mb.left_mb, idc);
	}

	/* Decides on the top macroblock edge (8.7, step 2.d) the same way. */
	mb.filter_top = 0;
	if (mb.mb_y != 0U) {
		mb.top_mb = mb.q_mb - picture->width_mbs;
		mb.filter_top = neighbour_is_filtered(picture, mb.q_mb, mb.top_mb, idc);
	}

	/* A macroblock whose quantization parameters close every edge changes nothing (8-460): its strengths are not derived. */
	quiet = macroblock_is_quiet(&mb);
	if (quiet != 0)
		return;

	/* Derives bS of every edge. */
	macroblock_strengths(&mb);

	/* Filters the luma vertical edges, then the luma horizontal edges (8.7, steps 3.a to 3.d). */
	deblock_luma_vertical(&mb);
	deblock_luma_horizontal(&mb);

	/* Filters Cb and Cr together, the vertical edges first (8.7, step 3.e): each component's edges keep their order and the two never read each other. */
	deblock_chroma_vertical(&mb);
	deblock_chroma_horizontal(&mb);
}
