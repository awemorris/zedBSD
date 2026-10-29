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

/* Forward declarations of the static functions. */
static int clip3(int low, int high, int value);
static int absolute(int value);
static int shift_right(int value, unsigned count);
static uint8_t clip_sample(int value);
static int table_index(int qp_av, int offset);
static int is_intra(const struct h264sw_mb *mb);
static int luma_qp(const struct h264sw_mb *mb);
static int chroma_qp(const struct h264sw_picture_ctx *picture, const struct h264sw_mb *mb, unsigned component);
static int mv_differs(const int16_t *first, const int16_t *second);
static int motion_differs(const struct h264sw_motion *p_motion, unsigned p_blk, const struct h264sw_motion *q_motion, unsigned q_blk);
static void filter_luma_normal(uint8_t *edge, ptrdiff_t across, int beta, int tc0);
static void filter_luma_strong(uint8_t *edge, ptrdiff_t across, int alpha, int beta);
static void filter_luma_line(uint8_t *edge, ptrdiff_t across, int strength, int alpha, int beta, int tc0);
static void filter_chroma_line(uint8_t *edge, ptrdiff_t across, int strength, int alpha, int beta, int tc0);
static int picture_is_usable(const struct h264sw_picture_ctx *picture);
static int neighbour_is_filtered(const struct h264sw_picture_ctx *picture, const struct h264sw_mb *q_mb, const struct h264sw_mb *neighbour, unsigned idc);
static void deblock_luma_edges(const struct h264sw_picture_ctx *picture, unsigned mb_addr, int vertical, int filter_mb_edge, int filter_internal);
static void deblock_chroma_edges(const struct h264sw_picture_ctx *picture, unsigned mb_addr, unsigned component, int vertical, int filter_mb_edge, int filter_internal);
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
	unsigned mb_addr;
	int usable;

	/* Refuses a picture whose arrays cannot be trusted. */
	usable = picture_is_usable(picture);
	if (usable == 0)
		return;

	/* Filters the macroblocks in increasing macroblock address (8.7). */
	for (mb_addr = 0;
	     mb_addr < picture->size_mbs;
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
	unsigned q_x;
	unsigned q_y;
	unsigned p_x;
	unsigned p_y;
	unsigned q_blk;
	unsigned p_blk;
	int p_intra;
	int q_intra;
	int differs;

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

			p_mb = q_mb - 1;
		} else {
			/* A horizontal macroblock edge on the picture's upper border is not filtered. */
			if (mb_y == 0U)
				return;

			p_mb = q_mb - picture->width_mbs;
		}
	}

	/* Finds whether p0 or q0 is in a macroblock coded in an intra mode. */
	p_intra = is_intra(p_mb);
	q_intra = is_intra(q_mb);

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
			q_x = 4U * edge;
			q_y = 4U * segment;
			p_x = (q_x + 15U) % 16U;
			p_y = q_y;
		} else {
			q_x = 4U * segment;
			q_y = 4U * edge;
			p_x = q_x;
			p_y = (q_y + 15U) % 16U;
		}
		q_blk = h264sw_blk4x4_at(q_x, q_y);
		p_blk = h264sw_blk4x4_at(p_x, p_y);

		/* An intra macroblock gives bS 4 on a macroblock edge and bS 3 on an internal one. */
		if (p_intra != 0 || q_intra != 0) {
			if (edge == 0U) {
				bs[segment] = 4;
			} else {
				bs[segment] = 3;
			}
			continue;
		}

		/* A transform block with a non-zero coefficient on either side gives bS 2. */
		if (((p_mb->nonzero >> p_blk) & 1U) != 0U) {
			bs[segment] = 2;
			continue;
		}
		if (((q_mb->nonzero >> q_blk) & 1U) != 0U) {
			bs[segment] = 2;
			continue;
		}

		/* Different reference pictures, a different number of vectors, or vectors that differ enough give bS 1; the rest 0. */
		differs = motion_differs(&p_mb->motion, p_blk, &q_mb->motion, q_blk);
		if (differs != 0) {
			bs[segment] = 1;
		} else {
			bs[segment] = 0;
		}
	}
}

/*
 * Filters one luma edge of 16 samples (8.7.2.2 to 8.7.2.4, chromaEdgeFlag 0).
 *
 * A strength above 4 is taken as 4.  qp_av and the offsets are summed
 * without overflow before the clip, so any int values are safe.
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
	int index_a;
	int index_b;
	int alpha;
	int beta;
	int tc0;
	int strength;
	int line;

	/* Refuses a missing picture or strength array. */
	if (q0 == NULL)
		return;
	if (bs == NULL)
		return;

	/* Derives indexA and indexB (8-454, 8-455) and the thresholds alpha and beta (Table 8-16). */
	index_a = table_index(qp_av, offset_a);
	index_b = table_index(qp_av, offset_b);
	alpha = deblock_alpha[index_a];
	beta = deblock_beta[index_b];

	/* Filters the 16 lines across the edge, each with the strength of its 4-line segment. */
	for (line = 0;
	     line < 16;
	     line++) {
		/* Leaves a line alone whose segment has bS 0. */
		strength = bs[line / 4];
		if (strength == 0)
			continue;

		/* Treats a strength above the strongest as the strongest. */
		if (strength > DEBLOCK_BS_STRONG)
			strength = DEBLOCK_BS_STRONG;

		/* Finds tC0 (Table 8-17) for the strengths below 4. */
		tc0 = 0;
		if (strength < DEBLOCK_BS_STRONG)
			tc0 = deblock_tc0[strength - 1][index_a];

		/* Filters the samples of this line. */
		filter_luma_line(q0 + (ptrdiff_t)line * along, across, strength, alpha, beta, tc0);
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
	int index_a;
	int index_b;
	int alpha;
	int beta;
	int tc0;
	int strength;
	int line;

	/* Refuses a missing picture or strength array. */
	if (q0 == NULL)
		return;
	if (bs == NULL)
		return;

	/* Derives indexA and indexB (8-454, 8-455) and the thresholds alpha and beta (Table 8-16). */
	index_a = table_index(qp_av, offset_a);
	index_b = table_index(qp_av, offset_b);
	alpha = deblock_alpha[index_a];
	beta = deblock_beta[index_b];

	/* Filters the 8 lines across the edge, two lines to each luma segment's strength. */
	for (line = 0;
	     line < 8;
	     line++) {
		/* Leaves a line alone whose segment has bS 0. */
		strength = bs[line / 2];
		if (strength == 0)
			continue;

		/* Treats a strength above the strongest as the strongest. */
		if (strength > DEBLOCK_BS_STRONG)
			strength = DEBLOCK_BS_STRONG;

		/* Finds tC0 (Table 8-17) for the strengths below 4. */
		tc0 = 0;
		if (strength < DEBLOCK_BS_STRONG)
			tc0 = deblock_tc0[strength - 1][index_a];

		/* Filters the samples of this line. */
		filter_chroma_line(q0 + (ptrdiff_t)line * along, across, strength, alpha, beta, tc0);
	}
}

/* Clamps a value to the range low..high (Clip3). */
static int
clip3(
	int low,
	int high,
	int value)
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

/* Gives the absolute value of a difference of two samples. */
static int
absolute(
	int value)
{
	/* A negative value is negated; its magnitude is far below INT_MAX here. */
	if (value < 0)
		return -value;

	/* Succeeded: a non-negative value is its own absolute value. */
	return value;
}

/*
 * Shifts a value right by count bits, rounding toward minus infinity.
 *
 * It is written with arithmetic so that a negative value does not rely on
 * the implementation-defined right shift of a negative number.
 */
static int
shift_right(
	int value,
	unsigned count)
{
	/* A non-negative value shifts directly. */
	if (value >= 0)
		return value >> count;

	/* A negative value v is -( ( -v - 1 ) >> count ) - 1, which is floor( v / 2^count ). */
	return -((-value - 1) >> count) - 1;
}

/* Clamps a filtered value to the 8-bit sample range (Clip1 for a bit depth of 8). */
static uint8_t
clip_sample(
	int value)
{
	int clipped;

	/* Clamps the value to 0..255 before it is narrowed to a byte. */
	clipped = clip3(0, DEBLOCK_SAMPLE_MAX, value);

	/* Succeeded: the clamped value fits a byte. */
	return (uint8_t)clipped;
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

			return 0;
		}

		/* The lists name the pictures in the opposite order. */
		if (cross_p0 != 0)
			return 1;
		if (cross_p1 != 0)
			return 1;

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
 * Filters one line of a luma edge with bS 1, 2 or 3 (8.7.2.3).
 *
 * edge points to q0 and across is the distance from a sample to the next
 * one across the edge.  The thresholds have already let the line through.
 */
static void
filter_luma_normal(
	uint8_t *edge,
	ptrdiff_t across,
	int beta,
	int tc0)
{
	int p0;
	int p1;
	int p2;
	int q0;
	int q1;
	int q2;
	int ap;
	int aq;
	int tc;
	int delta;
	int unclipped;

	/* Reads the samples the filter uses. */
	p0 = edge[-across];
	p1 = edge[-2 * across];
	p2 = edge[-3 * across];
	q0 = edge[0];
	q1 = edge[across];
	q2 = edge[2 * across];

	/* Finds how flat each side is next to the edge (8-463, 8-464). */
	ap = absolute(p2 - p0);
	aq = absolute(q2 - q0);

	/* Widens the clip of the change by one for each flat side (8-465). */
	tc = tc0;
	if (ap < beta)
		tc++;
	if (aq < beta)
		tc++;

	/* Moves p0 and q0 toward each other (8-467 to 8-469). */
	unclipped = shift_right((q0 - p0) * 4 + (p1 - q1) + 4, 3);
	delta = clip3(-tc, tc, unclipped);
	edge[-across] = clip_sample(p0 + delta);
	edge[0] = clip_sample(q0 - delta);

	/* Moves p1 as well when the p side is flat (8-470). */
	if (ap < beta) {
		unclipped = shift_right(p2 + ((p0 + q0 + 1) >> 1) - p1 * 2, 1);
		delta = clip3(-tc0, tc0, unclipped);
		edge[-2 * across] = clip_sample(p1 + delta);
	}

	/* Moves q1 as well when the q side is flat (8-472). */
	if (aq < beta) {
		unclipped = shift_right(q2 + ((p0 + q0 + 1) >> 1) - q1 * 2, 1);
		delta = clip3(-tc0, tc0, unclipped);
		edge[across] = clip_sample(q1 + delta);
	}
}

/*
 * Filters one line of a luma edge with bS 4 (8.7.2.4).
 *
 * edge points to q0 and across is the distance from a sample to the next
 * one across the edge.  The thresholds have already let the line through.
 */
static void
filter_luma_strong(
	uint8_t *edge,
	ptrdiff_t across,
	int alpha,
	int beta)
{
	int p0;
	int p1;
	int p2;
	int p3;
	int q0;
	int q1;
	int q2;
	int q3;
	int ap;
	int aq;
	int step;
	int small_gap;

	/* Reads the samples the filter uses. */
	p0 = edge[-across];
	p1 = edge[-2 * across];
	p2 = edge[-3 * across];
	p3 = edge[-4 * across];
	q0 = edge[0];
	q1 = edge[across];
	q2 = edge[2 * across];
	q3 = edge[3 * across];

	/* Finds how flat each side is next to the edge (8-463, 8-464). */
	ap = absolute(p2 - p0);
	aq = absolute(q2 - q0);

	/* Finds whether the step is small enough for the strong filter (8-476, 8-483). */
	step = absolute(p0 - q0);
	small_gap = 0;
	if (step < (alpha >> 2) + 2)
		small_gap = 1;

	/* Filters the p side: three samples when it is flat and the step small, else p0 only (8-477 to 8-482). */
	if (ap < beta && small_gap != 0) {
		edge[-across] = clip_sample((p2 + 2 * p1 + 2 * p0 + 2 * q0 + q1 + 4) >> 3);
		edge[-2 * across] = clip_sample((p2 + p1 + p0 + q0 + 2) >> 2);
		edge[-3 * across] = clip_sample((2 * p3 + 3 * p2 + p1 + p0 + q0 + 4) >> 3);
	} else {
		edge[-across] = clip_sample((2 * p1 + p0 + q1 + 2) >> 2);
	}

	/* Filters the q side the same way (8-484 to 8-489). */
	if (aq < beta && small_gap != 0) {
		edge[0] = clip_sample((p1 + 2 * p0 + 2 * q0 + 2 * q1 + q2 + 4) >> 3);
		edge[across] = clip_sample((p0 + q0 + q1 + q2 + 2) >> 2);
		edge[2 * across] = clip_sample((2 * q3 + 3 * q2 + q1 + q0 + p0 + 4) >> 3);
	} else {
		edge[0] = clip_sample((2 * q1 + q0 + p1 + 2) >> 2);
	}
}

/*
 * Filters the samples across one line of a luma edge (8.7.2.2 to 8.7.2.4).
 *
 * edge points to q0; across is the distance from a sample to the next
 * one across the edge.  strength is bS 1..4, alpha and beta the
 * thresholds and tc0 the value of tC0 for strengths below 4.
 */
static void
filter_luma_line(
	uint8_t *edge,
	ptrdiff_t across,
	int strength,
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

	/* Reads the samples that decide whether the line is filtered. */
	p0 = edge[-across];
	p1 = edge[-2 * across];
	q0 = edge[0];
	q1 = edge[across];

	/* Measures the step across the edge and the slope on each side of it. */
	step = absolute(p0 - q0);
	p_slope = absolute(p1 - p0);
	q_slope = absolute(q1 - q0);

	/* Leaves the line alone unless the step across the edge looks like a coding artifact (8-460). */
	if (step >= alpha)
		return;
	if (p_slope >= beta)
		return;
	if (q_slope >= beta)
		return;

	/* Filters by the strength: bS 4 and bS below 4 have their own filters. */
	if (strength < DEBLOCK_BS_STRONG) {
		filter_luma_normal(edge, across, beta, tc0);
	} else {
		filter_luma_strong(edge, across, alpha, beta);
	}
}

/*
 * Filters the samples across one line of a chroma edge (8.7.2.2 to 8.7.2.4).
 *
 * As filter_luma_line, but chromaStyleFilteringFlag is 1: only p0 and q0
 * change and tC is tC0 + 1.
 */
static void
filter_chroma_line(
	uint8_t *edge,
	ptrdiff_t across,
	int strength,
	int alpha,
	int beta,
	int tc0)
{
	int p0;
	int p1;
	int q0;
	int q1;
	int tc;
	int delta;
	int unclipped;
	int step;
	int p_slope;
	int q_slope;

	/* Reads the samples that decide and take part in the filter. */
	p0 = edge[-across];
	p1 = edge[-2 * across];
	q0 = edge[0];
	q1 = edge[across];

	/* Measures the step across the edge and the slope on each side of it. */
	step = absolute(p0 - q0);
	p_slope = absolute(p1 - p0);
	q_slope = absolute(q1 - q0);

	/* Leaves the line alone unless the step across the edge looks like a coding artifact (8-460). */
	if (step >= alpha)
		return;
	if (p_slope >= beta)
		return;
	if (q_slope >= beta)
		return;

	/* Filters a line with bS below 4: tC is tC0 + 1 (8-466) and only p0 and q0 move (8-467 to 8-469); bS 4 takes the 3-tap averages (8-480, 8-487). */
	if (strength < DEBLOCK_BS_STRONG) {
		tc = tc0 + 1;
		unclipped = shift_right((q0 - p0) * 4 + (p1 - q1) + 4, 3);
		delta = clip3(-tc, tc, unclipped);
		edge[-across] = clip_sample(p0 + delta);
		edge[0] = clip_sample(q0 - delta);
	} else {
		edge[-across] = clip_sample((2 * p1 + p0 + q1 + 2) >> 2);
		edge[0] = clip_sample((2 * q1 + q0 + p1 + 2) >> 2);
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
 * Filters the luma edges of one direction of a macroblock (8.7, steps 3.a to 3.d).
 *
 * filter_mb_edge is filterLeftMbEdgeFlag for vertical edges and
 * filterTopMbEdgeFlag for horizontal ones; filter_internal is
 * filterInternalEdgesFlag.  The edges go from the macroblock edge to the
 * one at 12 samples, and the ones at 4 and 12 are skipped for an 8x8
 * transform.
 */
static void
deblock_luma_edges(
	const struct h264sw_picture_ctx *picture,
	unsigned mb_addr,
	int vertical,
	int filter_mb_edge,
	int filter_internal)
{
	const struct h264sw_mb *q_mb;
	const struct h264sw_mb *p_mb;
	const struct h264sw_slice_params *slice;
	const struct h264sw_frame *frame;
	uint8_t *mb_origin;
	uint8_t *edge_origin;
	uint8_t bs[4];
	ptrdiff_t stride;
	unsigned mb_x;
	unsigned mb_y;
	unsigned edge;
	int qp_av;

	/* Finds the macroblock being filtered, its slice and its position. */
	q_mb = &picture->mbs[mb_addr];
	slice = &picture->slices[q_mb->slice];
	frame = picture->frame;
	mb_x = mb_addr % picture->width_mbs;
	mb_y = mb_addr / picture->width_mbs;
	stride = (ptrdiff_t)frame->stride[0];

	/* Finds the upper-left luma sample of the macroblock. */
	mb_origin = frame->plane[0] + (size_t)mb_y * 16U * frame->stride[0] + (size_t)mb_x * 16U;

	/* Filters the edges at 0, 4, 8 and 12 samples in geometric order. */
	for (edge = 0;
	     edge < 4U;
	     edge++) {
		/* The macroblock edge is filtered only when its flag allows it. */
		if (edge == 0U && filter_mb_edge == 0)
			continue;

		/* The internal edges are filtered only when their flag allows it. */
		if (edge != 0U && filter_internal == 0)
			continue;

		/* The edges at 4 and 12 samples belong to 4x4 transform blocks only. */
		if ((edge & 1U) != 0U && q_mb->transform_8x8 != 0U)
			continue;

		/* Finds the macroblock that holds p0: the neighbour across the macroblock edge, else the macroblock itself. */
		p_mb = q_mb;
		if (edge == 0U) {
			if (vertical != 0) {
				p_mb = q_mb - 1;
			} else {
				p_mb = q_mb - picture->width_mbs;
			}
		}

		/* Derives bS of the edge's four segments. */
		h264sw_deblock_bs(picture, mb_addr, vertical, edge, bs);

		/* Averages the two macroblocks' quantization parameters (8-453). */
		qp_av = (luma_qp(p_mb) + luma_qp(q_mb) + 1) >> 1;

		/* Filters the 16 lines of the edge, with the offsets of the slice holding q0. */
		if (vertical != 0) {
			edge_origin = mb_origin + 4U * edge;
			h264sw_deblock_luma_edge(edge_origin, 1, stride, bs, qp_av, slice->filter_offset_a, slice->filter_offset_b);
		} else {
			edge_origin = mb_origin + (size_t)(4U * edge) * frame->stride[0];
			h264sw_deblock_luma_edge(edge_origin, stride, 1, bs, qp_av, slice->filter_offset_a, slice->filter_offset_b);
		}
	}
}

/*
 * Filters the edges of one direction of one chroma component of a macroblock (8.7, steps 3.e.i to 3.e.iv, 4:2:0).
 *
 * component is 0 for Cb and 1 for Cr.  The chroma edges are at 0 and 4
 * samples; the one at 4 corresponds to the luma edge at 8 samples and
 * takes its strengths, and it is filtered whatever transform_size_8x8_flag
 * is.
 */
static void
deblock_chroma_edges(
	const struct h264sw_picture_ctx *picture,
	unsigned mb_addr,
	unsigned component,
	int vertical,
	int filter_mb_edge,
	int filter_internal)
{
	const struct h264sw_mb *q_mb;
	const struct h264sw_mb *p_mb;
	const struct h264sw_slice_params *slice;
	const struct h264sw_frame *frame;
	uint8_t *mb_origin;
	uint8_t *edge_origin;
	uint8_t bs[4];
	ptrdiff_t stride;
	unsigned mb_x;
	unsigned mb_y;
	unsigned chroma_edge;
	int qp_av;

	/* Finds the macroblock being filtered, its slice and its position. */
	q_mb = &picture->mbs[mb_addr];
	slice = &picture->slices[q_mb->slice];
	frame = picture->frame;
	mb_x = mb_addr % picture->width_mbs;
	mb_y = mb_addr / picture->width_mbs;
	stride = (ptrdiff_t)frame->stride[1U + component];

	/* Finds the upper-left sample of the macroblock's 8x8 chroma block. */
	mb_origin = frame->plane[1U + component] + (size_t)mb_y * 8U * frame->stride[1U + component] + (size_t)mb_x * 8U;

	/* Filters the edges at 0 and 4 samples. */
	for (chroma_edge = 0;
	     chroma_edge < 2U;
	     chroma_edge++) {
		/* The macroblock edge is filtered only when its flag allows it. */
		if (chroma_edge == 0U && filter_mb_edge == 0)
			continue;

		/* The internal edge is filtered only when its flag allows it. */
		if (chroma_edge != 0U && filter_internal == 0)
			continue;

		/* Finds the macroblock that holds p0: the neighbour across the macroblock edge, else the macroblock itself. */
		p_mb = q_mb;
		if (chroma_edge == 0U) {
			if (vertical != 0) {
				p_mb = q_mb - 1;
			} else {
				p_mb = q_mb - picture->width_mbs;
			}
		}

		/* Takes the strengths of the luma edge the chroma edge corresponds to (8.7.2). */
		h264sw_deblock_bs(picture, mb_addr, vertical, 2U * chroma_edge, bs);

		/* Averages the two macroblocks' chroma quantization parameters (8-453). */
		qp_av = (chroma_qp(picture, p_mb, component) + chroma_qp(picture, q_mb, component) + 1) >> 1;

		/* Filters the 8 lines of the edge, with the offsets of the slice holding q0. */
		if (vertical != 0) {
			edge_origin = mb_origin + 4U * chroma_edge;
			h264sw_deblock_chroma_edge(edge_origin, 1, stride, bs, qp_av, slice->filter_offset_a, slice->filter_offset_b);
		} else {
			edge_origin = mb_origin + (size_t)(4U * chroma_edge) * frame->stride[1U + component];
			h264sw_deblock_chroma_edge(edge_origin, stride, 1, bs, qp_av, slice->filter_offset_a, slice->filter_offset_b);
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
	const struct h264sw_mb *q_mb;
	unsigned mb_x;
	unsigned mb_y;
	unsigned component;
	unsigned idc;
	int filter_internal;
	int filter_left;
	int filter_top;

	/* Finds the macroblock and its position. */
	q_mb = &picture->mbs[mb_addr];
	mb_x = mb_addr % picture->width_mbs;
	mb_y = mb_addr / picture->width_mbs;

	/* Leaves a macroblock whose slice is unknown alone. */
	if (q_mb->slice < 0)
		return;
	if ((uint32_t)q_mb->slice >= picture->slice_count)
		return;

	/* Reads the slice's disable_deblocking_filter_idc. */
	idc = picture->slices[q_mb->slice].disable_deblocking;

	/* Idc 1 switches the filter off for the macroblock's edges altogether. */
	if (idc == 1U)
		return;

	/* The internal edges are filtered whenever the filter is on (8.7, step 2.b). */
	filter_internal = 1;

	/* Decides on the left macroblock edge (8.7, step 2.c): the picture's border switches it off, and so does an unavailable neighbour. */
	filter_left = 0;
	if (mb_x != 0U)
		filter_left = neighbour_is_filtered(picture, q_mb, q_mb - 1, idc);

	/* Decides on the top macroblock edge (8.7, step 2.d) the same way. */
	filter_top = 0;
	if (mb_y != 0U)
		filter_top = neighbour_is_filtered(picture, q_mb, q_mb - picture->width_mbs, idc);

	/* Filters the luma vertical edges, then the luma horizontal edges (8.7, steps 3.a to 3.d). */
	deblock_luma_edges(picture, mb_addr, 1, filter_left, filter_internal);
	deblock_luma_edges(picture, mb_addr, 0, filter_top, filter_internal);

	/* Filters Cb and then Cr, each with the vertical edges first (8.7, step 3.e). */
	for (component = 0;
	     component < 2U;
	     component++) {
		deblock_chroma_edges(picture, mb_addr, component, 1, filter_left, filter_internal);
		deblock_chroma_edges(picture, mb_addr, component, 0, filter_top, filter_internal);
	}
}
