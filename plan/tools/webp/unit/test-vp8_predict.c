/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The tests of vp8-predict.c: the four whole-block modes at both sizes
 * against values worked out by hand from RFC 6386 12.2 (V and H copies,
 * the clamps of TM, the four cases and the rounding of DC), the ten
 * subblock modes against pixels worked out by hand from the formulas of
 * RFC 6386 12.3 for one set of edge pixels, strides other than the block
 * width, refused modes and sizes, and random edges against a transcription
 * of the formulas of 12.3 kept in the test (pixel by pixel, with the
 * array names of the RFC).
 */

#include "check.h"
#include "vp8-predict.h"
#include "vp8-tables.h"

#include <stdlib.h>
#include <string.h>

/* The random generator's state. */
static unsigned long random_state = 0x2468ace1UL;

/* The marker the destination buffers are filled with, to see what is written. */
#define GUARD	0xa5

static int ref_avg2(int x, int y);
static int ref_avg3(int x, int y, int z);
static void reference_subblock(unsigned mode, const uint8_t a_row[9], const uint8_t l_col[4], uint8_t b[4][4]);
static void edges_fill(struct webpc_vp8_edges *edges, unsigned above_value, unsigned left_value, unsigned corner);
static void test_block_copies(void);
static void test_block_tm(void);
static void test_block_dc(void);
static void test_block_stride_and_refusal(void);
static void test_subblock_hand(void);
static void test_subblock_extremes(void);
static void test_subblock_stride_and_refusal(void);
static void test_random(void);

/*
 * Runs the tests of vp8-predict.c.
 */
int
main(
	void)
{
	int status;

	/* Each helper in turn. */
	test_block_copies();
	test_block_tm();
	test_block_dc();
	test_block_stride_and_refusal();
	test_subblock_hand();
	test_subblock_extremes();
	test_subblock_stride_and_refusal();
	test_random();

	/* The summary line. */
	status = check_finish("vp8_predict");
	return status;
}

/*
 * Averages two pixels with the rounding of RFC 6386 12.3.
 */
static int
ref_avg2(
	int x,
	int y)
{
	/* Reports the mean rounded half up. */
	return (x + y + 1) / 2;
}

/*
 * Averages three pixels, the middle one twice, with the rounding of RFC
 * 6386 12.3.
 */
static int
ref_avg3(
	int x,
	int y,
	int z)
{
	/* Reports the weighted mean rounded half up. */
	return (x + 2 * y + z + 2) / 4;
}

/*
 * The subblock modes written pixel by pixel from RFC 6386 12.3.  a_row[0]
 * is P and a_row[1..8] are A[0..7]; l_col is L[0..3].
 */
static void
reference_subblock(
	unsigned mode,
	const uint8_t a_row[9],
	const uint8_t l_col[4],
	uint8_t b[4][4])
{
	const uint8_t *a;
	int p;
	int sum;
	int e[9];
	int r;
	int c;
	int v;

	/* A is addressed from 0 as in the RFC, with P just before it. */
	a = a_row + 1;
	p = a_row[0];

	/* The edge line of the RFC: L[3], L[2], L[1], L[0], P, A[0..3]. */
	e[0] = l_col[3];
	e[1] = l_col[2];
	e[2] = l_col[1];
	e[3] = l_col[0];
	e[4] = p;
	e[5] = a[0];
	e[6] = a[1];
	e[7] = a[2];
	e[8] = a[3];

	switch (mode) {
	case WEBPC_VP8_B_DC_PRED:
		/* The eight pixels, four above and four on the left, with 4 added before the shift. */
		sum = 4;
		for (r = 0; r < 4; r++) {
			sum += a[r] + l_col[r];
		}
		for (r = 0; r < 4; r++) {
			for (c = 0; c < 4; c++) {
				b[r][c] = (uint8_t)(sum >> 3);
			}
		}
		break;
	case WEBPC_VP8_B_TM_PRED:
		/* The left pixel plus the above pixel minus P, clamped. */
		for (r = 0; r < 4; r++) {
			for (c = 0; c < 4; c++) {
				v = l_col[r] + a[c] - p;
				if (v < 0)
					v = 0;
				if (v > 255)
					v = 255;
				b[r][c] = (uint8_t)v;
			}
		}
		break;
	case WEBPC_VP8_B_VE_PRED:
		/* The row above smoothed, copied down. */
		for (c = 0; c < 4; c++) {
			v = ref_avg3(c == 0 ? p : a[c - 1], a[c], a[c + 1]);
			for (r = 0; r < 4; r++) {
				b[r][c] = (uint8_t)v;
			}
		}
		break;
	case WEBPC_VP8_B_HE_PRED:
		/* The left column smoothed, P above it and its last pixel twice below. */
		for (r = 0; r < 4; r++) {
			if (r == 0)
				v = ref_avg3(p, l_col[0], l_col[1]);
			else if (r == 3)
				v = ref_avg3(l_col[2], l_col[3], l_col[3]);
			else
				v = ref_avg3(l_col[r - 1], l_col[r], l_col[r + 1]);
			for (c = 0; c < 4; c++) {
				b[r][c] = (uint8_t)v;
			}
		}
		break;
	case WEBPC_VP8_B_LD_PRED:
		/* The anti-diagonal r + c takes the smoothed A[r + c + 1]; the last repeats A[7]. */
		for (r = 0; r < 4; r++) {
			for (c = 0; c < 4; c++) {
				if (r + c == 6)
					v = ref_avg3(a[6], a[7], a[7]);
				else
					v = ref_avg3(a[r + c], a[r + c + 1], a[r + c + 2]);
				b[r][c] = (uint8_t)v;
			}
		}
		break;
	case WEBPC_VP8_B_RD_PRED:
		/* Written out with E = L[3], L[2], L[1], L[0], P, A[0..3] as the RFC does. */
		b[3][0] = (uint8_t)ref_avg3(e[0], e[1], e[2]);
		b[3][1] = b[2][0] = (uint8_t)ref_avg3(e[1], e[2], e[3]);
		b[3][2] = b[2][1] = b[1][0] = (uint8_t)ref_avg3(e[2], e[3], e[4]);
		b[3][3] = b[2][2] = b[1][1] = b[0][0] = (uint8_t)ref_avg3(e[3], e[4], e[5]);
		b[2][3] = b[1][2] = b[0][1] = (uint8_t)ref_avg3(e[4], e[5], e[6]);
		b[1][3] = b[0][2] = (uint8_t)ref_avg3(e[5], e[6], e[7]);
		b[0][3] = (uint8_t)ref_avg3(e[6], e[7], e[8]);
		break;
	case WEBPC_VP8_B_VR_PRED:
		b[3][0] = (uint8_t)ref_avg3(e[1], e[2], e[3]);
		b[2][0] = (uint8_t)ref_avg3(e[2], e[3], e[4]);
		b[3][1] = b[1][0] = (uint8_t)ref_avg3(e[3], e[4], e[5]);
		b[2][1] = b[0][0] = (uint8_t)ref_avg2(e[4], e[5]);
		b[3][2] = b[1][1] = (uint8_t)ref_avg3(e[4], e[5], e[6]);
		b[2][2] = b[0][1] = (uint8_t)ref_avg2(e[5], e[6]);
		b[3][3] = b[1][2] = (uint8_t)ref_avg3(e[5], e[6], e[7]);
		b[2][3] = b[0][2] = (uint8_t)ref_avg2(e[6], e[7]);
		b[1][3] = (uint8_t)ref_avg3(e[6], e[7], e[8]);
		b[0][3] = (uint8_t)ref_avg2(e[7], e[8]);
		break;
	case WEBPC_VP8_B_VL_PRED:
		b[0][0] = (uint8_t)ref_avg2(a[0], a[1]);
		b[1][0] = (uint8_t)ref_avg3(a[0], a[1], a[2]);
		b[2][0] = b[0][1] = (uint8_t)ref_avg2(a[1], a[2]);
		b[1][1] = b[3][0] = (uint8_t)ref_avg3(a[1], a[2], a[3]);
		b[2][1] = b[0][2] = (uint8_t)ref_avg2(a[2], a[3]);
		b[3][1] = b[1][2] = (uint8_t)ref_avg3(a[2], a[3], a[4]);
		b[2][2] = b[0][3] = (uint8_t)ref_avg2(a[3], a[4]);
		b[3][2] = b[1][3] = (uint8_t)ref_avg3(a[3], a[4], a[5]);
		b[2][3] = (uint8_t)ref_avg3(a[4], a[5], a[6]);
		b[3][3] = (uint8_t)ref_avg3(a[5], a[6], a[7]);
		break;
	case WEBPC_VP8_B_HD_PRED:
		b[3][0] = (uint8_t)ref_avg2(e[0], e[1]);
		b[3][1] = (uint8_t)ref_avg3(e[0], e[1], e[2]);
		b[2][0] = b[3][2] = (uint8_t)ref_avg2(e[1], e[2]);
		b[2][1] = b[3][3] = (uint8_t)ref_avg3(e[1], e[2], e[3]);
		b[2][2] = b[1][0] = (uint8_t)ref_avg2(e[2], e[3]);
		b[2][3] = b[1][1] = (uint8_t)ref_avg3(e[2], e[3], e[4]);
		b[1][2] = b[0][0] = (uint8_t)ref_avg2(e[3], e[4]);
		b[1][3] = b[0][1] = (uint8_t)ref_avg3(e[3], e[4], e[5]);
		b[0][2] = (uint8_t)ref_avg3(e[4], e[5], e[6]);
		b[0][3] = (uint8_t)ref_avg3(e[5], e[6], e[7]);
		break;
	default:
		b[0][0] = (uint8_t)ref_avg2(l_col[0], l_col[1]);
		b[0][1] = (uint8_t)ref_avg3(l_col[0], l_col[1], l_col[2]);
		b[0][2] = b[1][0] = (uint8_t)ref_avg2(l_col[1], l_col[2]);
		b[0][3] = b[1][1] = (uint8_t)ref_avg3(l_col[1], l_col[2], l_col[3]);
		b[1][2] = b[2][0] = (uint8_t)ref_avg2(l_col[2], l_col[3]);
		b[1][3] = b[2][1] = (uint8_t)ref_avg3(l_col[2], l_col[3], l_col[3]);
		b[2][2] = b[2][3] = b[3][0] = b[3][1] = b[3][2] = b[3][3] = l_col[3];
		break;
	}
}

/*
 * Fills the edges of a block with one value above, one on the left and a
 * corner, and both neighbours present.
 */
static void
edges_fill(
	struct webpc_vp8_edges *edges,
	unsigned above_value,
	unsigned left_value,
	unsigned corner)
{
	int i;

	/* Every pixel of both edges, and the corner. */
	for (i = 0; i < 16; i++) {
		edges->above[i] = (uint8_t)above_value;
		edges->left[i] = (uint8_t)left_value;
	}
	edges->corner = (uint8_t)corner;
	edges->have_above = 1;
	edges->have_left = 1;
}

/*
 * V_PRED and H_PRED copy the edge, at 16 and 8, with edges that differ
 * pixel from pixel; the other edge and the corner do not matter.
 */
static void
test_block_copies(void)
{
	struct webpc_vp8_edges edges;
	uint8_t dst[16 * 16 + 1];
	unsigned sizes[2];
	unsigned s;
	unsigned size;
	unsigned r;
	unsigned c;
	int ok_v;
	int ok_h;

	sizes[0] = 16U;
	sizes[1] = 8U;

	/* Each size: distinct values along both edges, so a swap or shift shows. */
	for (s = 0; s < 2U; s++) {
		size = sizes[s];
		for (c = 0; c < 16U; c++) {
			edges.above[c] = (uint8_t)(c * 7U + 3U);
			edges.left[c] = (uint8_t)(255U - c * 11U);
		}
		edges.corner = 99;
		edges.have_above = 0;
		edges.have_left = 0;

		/* V_PRED: every row is the row above, whatever have_above says. */
		memset(dst, GUARD, sizeof(dst));
		webpc_vp8_predict_block(WEBPC_VP8_V_PRED, size, &edges, dst, (ptrdiff_t)size);
		ok_v = 1;
		for (r = 0; r < size; r++) {
			for (c = 0; c < size; c++) {
				if (dst[r * size + c] != (uint8_t)(c * 7U + 3U))
					ok_v = 0;
			}
		}
		CHECK(ok_v);
		CHECK(dst[size * size] == GUARD);

		/* H_PRED: every column is the column on the left. */
		memset(dst, GUARD, sizeof(dst));
		webpc_vp8_predict_block(WEBPC_VP8_H_PRED, size, &edges, dst, (ptrdiff_t)size);
		ok_h = 1;
		for (r = 0; r < size; r++) {
			for (c = 0; c < size; c++) {
				if (dst[r * size + c] != (uint8_t)(255U - r * 11U))
					ok_h = 0;
			}
		}
		CHECK(ok_h);
		CHECK(dst[size * size] == GUARD);
	}

	/* The last H_PRED block is size 8: its first pixel is left[0] = 255, its last row 255 - 7 * 11 = 178. */
	CHECK(dst[0] == 255);
	CHECK(dst[7 * 8] == 178);
}

/*
 * TM_PRED at 16 and 8: pixels by hand, both clamps, and the corner.
 */
static void
test_block_tm(void)
{
	struct webpc_vp8_edges edges;
	uint8_t dst[16 * 16];
	unsigned size;
	unsigned c;

	/*
	 * Size 8, corner 100, above[c] = 20 * c, left[r] = 10 * r + 5:
	 * pixel(r, c) = 10 * r + 5 + 20 * c - 100 clamped.  Pixel (0, 0) is
	 * -95 -> 0; (0, 7) is 5 + 140 - 100 = 45; (7, 0) is 75 - 100 = -25
	 * -> 0; (7, 7) is 75 + 140 - 100 = 115; (3, 4) is 35 + 80 - 100 = 15.
	 */
	size = 8U;
	for (c = 0; c < 8U; c++) {
		edges.above[c] = (uint8_t)(20U * c);
		edges.left[c] = (uint8_t)(10U * c + 5U);
	}
	edges.corner = 100;
	edges.have_above = 1;
	edges.have_left = 1;
	memset(dst, GUARD, sizeof(dst));
	webpc_vp8_predict_block(WEBPC_VP8_TM_PRED, size, &edges, dst, 8);
	CHECK(dst[0 * 8 + 0] == 0);
	CHECK(dst[0 * 8 + 7] == 45);
	CHECK(dst[7 * 8 + 0] == 0);
	CHECK(dst[7 * 8 + 7] == 115);
	CHECK(dst[3 * 8 + 4] == 15);
	CHECK(dst[64] == GUARD);

	/*
	 * Size 16: the upper clamp.  Corner 10, above 250, left 200: every
	 * pixel is 440 -> 255.  Then corner 250, above 0, left 0: -250 -> 0.
	 * Then the exact edge: corner 5, above 100, left 160 -> 255, and
	 * above 99 -> 254.
	 */
	size = 16U;
	edges_fill(&edges, 250U, 200U, 10U);
	webpc_vp8_predict_block(WEBPC_VP8_TM_PRED, size, &edges, dst, 16);
	CHECK(dst[0] == 255);
	CHECK(dst[255] == 255);
	edges_fill(&edges, 0U, 0U, 250U);
	webpc_vp8_predict_block(WEBPC_VP8_TM_PRED, size, &edges, dst, 16);
	CHECK(dst[0] == 0);
	CHECK(dst[255] == 0);
	edges_fill(&edges, 100U, 160U, 5U);
	webpc_vp8_predict_block(WEBPC_VP8_TM_PRED, size, &edges, dst, 16);
	CHECK(dst[17] == 255);
	edges.above[1] = 99;
	webpc_vp8_predict_block(WEBPC_VP8_TM_PRED, size, &edges, dst, 16);
	CHECK(dst[1] == 254);
	CHECK(dst[0] == 255);

	/* The sum that comes out at exactly 0 and exactly 255 is kept as it is. */
	edges_fill(&edges, 10U, 10U, 20U);
	webpc_vp8_predict_block(WEBPC_VP8_TM_PRED, size, &edges, dst, 16);
	CHECK(dst[0] == 0);
	edges_fill(&edges, 200U, 100U, 45U);
	webpc_vp8_predict_block(WEBPC_VP8_TM_PRED, size, &edges, dst, 16);
	CHECK(dst[0] == 255);
}

/*
 * DC_PRED: the four cases at both sizes, the rounding on each side of the
 * half, and the ignored edge.
 */
static void
test_block_dc(void)
{
	struct webpc_vp8_edges edges;
	uint8_t dst[16 * 16];
	unsigned i;
	unsigned r;
	unsigned c;
	int same;

	/*
	 * Size 16, above[i] = 16 * i (sum 1920), left all 5 (sum 80):
	 * both (1920 + 80 + 16) >> 5 = 63; above only (1920 + 8) >> 4 = 120;
	 * left only (80 + 8) >> 4 = 5.
	 */
	for (i = 0; i < 16U; i++) {
		edges.above[i] = (uint8_t)(16U * i);
		edges.left[i] = 5;
	}
	edges.corner = 255;
	edges.have_above = 1;
	edges.have_left = 1;
	memset(dst, GUARD, sizeof(dst));
	webpc_vp8_predict_block(WEBPC_VP8_DC_PRED, 16, &edges, dst, 16);
	same = 1;
	for (i = 0; i < 256U; i++) {
		if (dst[i] != 63)
			same = 0;
	}
	CHECK(same);

	edges.have_left = 0;
	webpc_vp8_predict_block(WEBPC_VP8_DC_PRED, 16, &edges, dst, 16);
	CHECK(dst[0] == 120);
	CHECK(dst[255] == 120);

	edges.have_above = 0;
	edges.have_left = 1;
	webpc_vp8_predict_block(WEBPC_VP8_DC_PRED, 16, &edges, dst, 16);
	CHECK(dst[0] == 5);
	CHECK(dst[255] == 5);

	/* Neither: 128, whatever the edge arrays hold. */
	edges.have_left = 0;
	webpc_vp8_predict_block(WEBPC_VP8_DC_PRED, 16, &edges, dst, 16);
	CHECK(dst[0] == 128);
	CHECK(dst[255] == 128);

	/*
	 * Size 16 rounding, above only: 7 ones (sum 7) is (7 + 8) >> 4 = 0,
	 * 8 ones is (8 + 8) >> 4 = 1.  Both edges: sum 15 is (15 + 16) >> 5
	 * = 0, sum 16 is 1.
	 */
	memset(&edges, 0, sizeof(edges));
	edges.have_above = 1;
	for (i = 0; i < 7U; i++)
		edges.above[i] = 1;
	webpc_vp8_predict_block(WEBPC_VP8_DC_PRED, 16, &edges, dst, 16);
	CHECK(dst[0] == 0);
	edges.above[7] = 1;
	webpc_vp8_predict_block(WEBPC_VP8_DC_PRED, 16, &edges, dst, 16);
	CHECK(dst[0] == 1);
	edges.have_above = 1;
	edges.have_left = 1;
	memset(edges.above, 0, sizeof(edges.above));
	memset(edges.left, 0, sizeof(edges.left));
	for (i = 0; i < 8U; i++)
		edges.above[i] = 1;
	for (i = 0; i < 7U; i++)
		edges.left[i] = 1;
	webpc_vp8_predict_block(WEBPC_VP8_DC_PRED, 16, &edges, dst, 16);
	CHECK(dst[0] == 0);
	edges.left[7] = 1;
	webpc_vp8_predict_block(WEBPC_VP8_DC_PRED, 16, &edges, dst, 16);
	CHECK(dst[0] == 1);

	/*
	 * Size 8: above[i] = 30 + i (sum 268), left all 200 (sum 1600): both
	 * (268 + 1600 + 8) >> 4 = 117 (1876 / 16 = 117.25 down to 117);
	 * above only (268 + 4) >> 3 = 34; left only (1600 + 4) >> 3 = 200.
	 */
	for (i = 0; i < 8U; i++) {
		edges.above[i] = (uint8_t)(30U + i);
		edges.left[i] = 200;
	}
	edges.have_above = 1;
	edges.have_left = 1;
	memset(dst, GUARD, sizeof(dst));
	webpc_vp8_predict_block(WEBPC_VP8_DC_PRED, 8, &edges, dst, 8);
	same = 1;
	for (i = 0; i < 64U; i++) {
		if (dst[i] != 117)
			same = 0;
	}
	CHECK(same);
	CHECK(dst[64] == GUARD);
	edges.have_left = 0;
	webpc_vp8_predict_block(WEBPC_VP8_DC_PRED, 8, &edges, dst, 8);
	CHECK(dst[0] == 34);
	edges.have_above = 0;
	edges.have_left = 1;
	webpc_vp8_predict_block(WEBPC_VP8_DC_PRED, 8, &edges, dst, 8);
	CHECK(dst[0] == 200);
	edges.have_left = 0;
	webpc_vp8_predict_block(WEBPC_VP8_DC_PRED, 8, &edges, dst, 8);
	CHECK(dst[0] == 128);

	/*
	 * Size 8 rounding: above only, sum 3 is (3 + 4) >> 3 = 0, sum 4 is 1;
	 * both, sum 7 is (7 + 8) >> 4 = 0, sum 8 is 1.
	 */
	memset(&edges, 0, sizeof(edges));
	edges.have_above = 1;
	for (i = 0; i < 3U; i++)
		edges.above[i] = 1;
	webpc_vp8_predict_block(WEBPC_VP8_DC_PRED, 8, &edges, dst, 8);
	CHECK(dst[0] == 0);
	edges.above[3] = 1;
	webpc_vp8_predict_block(WEBPC_VP8_DC_PRED, 8, &edges, dst, 8);
	CHECK(dst[0] == 1);
	memset(edges.above, 0, sizeof(edges.above));
	edges.have_left = 1;
	for (i = 0; i < 4U; i++)
		edges.above[i] = 1;
	for (i = 0; i < 3U; i++)
		edges.left[i] = 1;
	webpc_vp8_predict_block(WEBPC_VP8_DC_PRED, 8, &edges, dst, 8);
	CHECK(dst[0] == 0);
	edges.left[3] = 1;
	webpc_vp8_predict_block(WEBPC_VP8_DC_PRED, 8, &edges, dst, 8);
	CHECK(dst[0] == 1);

	/* Pixel (r, c) beyond the first of a DC block is the same as the first. */
	for (r = 0; r < 8U; r++) {
		for (c = 0; c < 8U; c++) {
			CHECK(dst[r * 8U + c] == 1);
		}
	}
}

/*
 * A stride wider than the block, the pixels beside the block untouched,
 * and the refusal of a size or a mode the format does not have.
 */
static void
test_block_stride_and_refusal(void)
{
	struct webpc_vp8_edges edges;
	uint8_t dst[40 * 17];
	uint8_t expect[16 * 16];
	unsigned strides[3];
	unsigned t;
	unsigned stride;
	unsigned r;
	unsigned c;
	unsigned mode;
	int ok;
	int guard_ok;

	strides[0] = 16U;
	strides[1] = 17U;
	strides[2] = 40U;

	/* A ramp edge, so a wrong row start shows in the value. */
	for (c = 0; c < 16U; c++) {
		edges.above[c] = (uint8_t)(c + 1U);
		edges.left[c] = (uint8_t)(100U + c);
	}
	edges.corner = 50;
	edges.have_above = 1;
	edges.have_left = 1;

	/* Every mode at every stride gives the same pixels as at stride 16 and leaves the gap alone. */
	for (mode = 0; mode < 4U; mode++) {
		webpc_vp8_predict_block(mode, 16, &edges, expect, 16);
		for (t = 0; t < 3U; t++) {
			stride = strides[t];
			memset(dst, GUARD, sizeof(dst));
			webpc_vp8_predict_block(mode, 16, &edges, dst, (ptrdiff_t)stride);
			ok = 1;
			guard_ok = 1;
			for (r = 0; r < 16U; r++) {
				for (c = 0; c < stride; c++) {
					if (c < 16U) {
						if (dst[r * stride + c] != expect[r * 16U + c])
							ok = 0;
					} else if (dst[r * stride + c] != GUARD) {
						guard_ok = 0;
					}
				}
			}
			CHECK(ok);
			CHECK(guard_ok);
		}

		/* The same at size 8, stride 17. */
		webpc_vp8_predict_block(mode, 8, &edges, expect, 8);
		memset(dst, GUARD, sizeof(dst));
		webpc_vp8_predict_block(mode, 8, &edges, dst, 17);
		ok = 1;
		guard_ok = 1;
		for (r = 0; r < 8U; r++) {
			for (c = 0; c < 17U; c++) {
				if (c < 8U) {
					if (dst[r * 17U + c] != expect[r * 8U + c])
						ok = 0;
				} else if (dst[r * 17U + c] != GUARD) {
					guard_ok = 0;
				}
			}
		}
		CHECK(ok);
		CHECK(guard_ok);
	}

	/* A size that is not 8 or 16, or a mode that is not one of the four, writes nothing. */
	memset(dst, GUARD, sizeof(dst));
	webpc_vp8_predict_block(WEBPC_VP8_DC_PRED, 4, &edges, dst, 16);
	webpc_vp8_predict_block(WEBPC_VP8_DC_PRED, 0, &edges, dst, 16);
	webpc_vp8_predict_block(WEBPC_VP8_DC_PRED, 32, &edges, dst, 16);
	webpc_vp8_predict_block(WEBPC_VP8_B_PRED, 16, &edges, dst, 16);
	webpc_vp8_predict_block(5U, 16, &edges, dst, 16);
	webpc_vp8_predict_block(0xffffffffU, 8, &edges, dst, 16);
	ok = 1;
	for (t = 0; t < sizeof(dst); t++) {
		if (dst[t] != GUARD)
			ok = 0;
	}
	CHECK(ok);
}

/*
 * The ten subblock modes for one hand-worked set of edge pixels:
 * P = 50, A[0..7] = 10 20 40 80 90 100 200 250, L[0..3] = 30 60 120 240.
 * The values of the sums are worked out in the comment of each mode.
 */
static void
test_subblock_hand(void)
{
	static const uint8_t above_row[9] = { 50, 10, 20, 40, 80, 90, 100, 200, 250 };
	static const uint8_t left_col[4] = { 30, 60, 120, 240 };

	/*
	 * B_DC_PRED: (10 + 20 + 40 + 80 + 30 + 60 + 120 + 240 + 4) >> 3 =
	 * 604 >> 3 = 75.
	 */
	static const uint8_t expect_dc[16] = {
		75, 75, 75, 75,
		75, 75, 75, 75,
		75, 75, 75, 75,
		75, 75, 75, 75
	};

	/*
	 * B_TM_PRED: L[r] + A[c] - 50 clamped.  Row 0 is 30 + A[c] - 50 =
	 * -10 -> 0, 0, 20, 60; row 1 is 60 + A - 50 = 20 30 50 90; row 2 is
	 * 120 + A - 50 = 80 90 110 150; row 3 is 240 + A - 50 = 200 210 230
	 * 270 -> 255.
	 */
	static const uint8_t expect_tm[16] = {
		0, 0, 20, 60,
		20, 30, 50, 90,
		80, 90, 110, 150,
		200, 210, 230, 255
	};

	/*
	 * B_VE_PRED: column c is (A[c-1] + 2 A[c] + A[c+1] + 2) >> 2 with
	 * A[-1] = P and A[4] = 90: c0 (50 + 20 + 20 + 2) >> 2 = 23, c1 (10 +
	 * 40 + 40 + 2) >> 2 = 23, c2 (20 + 80 + 80 + 2) >> 2 = 45, c3 (40 +
	 * 160 + 90 + 2) >> 2 = 73.
	 */
	static const uint8_t expect_ve[16] = {
		23, 23, 45, 73,
		23, 23, 45, 73,
		23, 23, 45, 73,
		23, 23, 45, 73
	};

	/*
	 * B_HE_PRED: row 0 (P + 2 L0 + L1 + 2) >> 2 = (50 + 60 + 60 + 2) >> 2
	 * = 43, row 1 (30 + 120 + 120 + 2) >> 2 = 68, row 2 (60 + 240 + 240
	 * + 2) >> 2 = 135, row 3 (L2 + 3 L3 + 2) >> 2 = (120 + 720 + 2) >> 2
	 * = 210.
	 */
	static const uint8_t expect_he[16] = {
		43, 43, 43, 43,
		68, 68, 68, 68,
		135, 135, 135, 135,
		210, 210, 210, 210
	};

	/*
	 * B_LD_PRED: anti-diagonal k = r + c is (A[k] + 2 A[k+1] + A[k+2] +
	 * 2) >> 2 : k0 (10 + 40 + 40 + 2) >> 2 = 23, k1 (20 + 80 + 80 + 2)
	 * >> 2 = 45, k2 (40 + 160 + 90 + 2) >> 2 = 73, k3 (80 + 180 + 100 +
	 * 2) >> 2 = 90, k4 (90 + 200 + 200 + 2) >> 2 = 123, k5 (100 + 400 +
	 * 250 + 2) >> 2 = 188, k6 (200 + 500 + 250 + 2) >> 2 = 238.
	 */
	static const uint8_t expect_ld[16] = {
		23, 45, 73, 90,
		45, 73, 90, 123,
		73, 90, 123, 188,
		90, 123, 188, 238
	};

	/*
	 * B_RD_PRED: the diagonal c - r = d.  d0 (L0 + 2 P + A0 + 2) >> 2 =
	 * (30 + 100 + 10 + 2) >> 2 = 35; d1 (P + 2 A0 + A1 + 2) >> 2 = (50 +
	 * 20 + 20 + 2) >> 2 = 23; d2 (A0 + 2 A1 + A2 + 2) >> 2 = (10 + 40 +
	 * 40 + 2) >> 2 = 23; d3 (A1 + 2 A2 + A3 + 2) >> 2 = (20 + 80 + 80 +
	 * 2) >> 2 = 45; d-1 (L1 + 2 L0 + P + 2) >> 2 = (60 + 60 + 50 + 2) >>
	 * 2 = 43; d-2 (L2 + 2 L1 + L0 + 2) >> 2 = (120 + 120 + 30 + 2) >> 2
	 * = 68; d-3 (L3 + 2 L2 + L1 + 2) >> 2 = (240 + 240 + 60 + 2) >> 2 =
	 * 135.
	 */
	static const uint8_t expect_rd[16] = {
		35, 23, 23, 45,
		43, 35, 23, 23,
		68, 43, 35, 23,
		135, 68, 43, 35
	};

	/*
	 * B_VR_PRED: row 0 is avg2 of neighbours along the edge from P:
	 * avg2(P, A0) = (50 + 10 + 1) >> 1 = 30, avg2(A0, A1) = 15, avg2(A1,
	 * A2) = 30, avg2(A2, A3) = 60.  Row 1 is avg3 centred on P, A0, A1, A2:
	 * (L0 + 2 P + A0 + 2) >> 2 = (30 + 100 + 10 + 2) >> 2 = 35, then 23, 23
	 * and 45 as in B_RD_PRED.  Row 2 is row 0 shifted right by one with
	 * (2,0) centred on L0, (L1 + 2 L0 + P + 2) >> 2 = 43; row 3 is row 1
	 * shifted right by one with (3,0) centred on L1, (L2 + 2 L1 + L0 + 2)
	 * >> 2 = 68.
	 */
	static const uint8_t expect_vr[16] = {
		30, 15, 30, 60,
		35, 23, 23, 45,
		43, 30, 15, 30,
		68, 35, 23, 23
	};

	/*
	 * B_VL_PRED: row 0 avg2(A[k], A[k+1]) = 15 30 60 85; row 1 avg3
	 * centred on A[k+1] = 23 45 73 90 (the last (80 + 180 + 100 + 2) >> 2
	 * = 90); row 2 is row 0 shifted left by one with its last (A4 + 2 A5
	 * + A6 + 2) >> 2 = (90 + 200 + 200 + 2) >> 2 = 123; row 3 is row 1
	 * shifted left by one with its last (A5 + 2 A6 + A7 + 2) >> 2 =
	 * (100 + 400 + 250 + 2) >> 2 = 188.  Row 0's last avg2(A3, A4) =
	 * (80 + 90 + 1) >> 1 = 85.
	 */
	static const uint8_t expect_vl[16] = {
		15, 30, 60, 85,
		23, 45, 73, 90,
		30, 60, 85, 123,
		45, 73, 90, 188
	};

	/*
	 * B_HD_PRED: (0,0) avg2(L0, P) = (30 + 50 + 1) >> 1 = 40; (0,1) (L0
	 * + 2 P + A0 + 2) >> 2 = 35; (0,2) (P + 2 A0 + A1 + 2) >> 2 = 23;
	 * (0,3) (A0 + 2 A1 + A2 + 2) >> 2 = 23; (1,0) avg2(L1, L0) = 45;
	 * (1,1) (L1 + 2 L0 + P + 2) >> 2 = 43; rows 1..3 shift the rows
	 * above to the right by two; (2,0) avg2(L2, L1) = 90; (2,1) (L2 + 2
	 * L1 + L0 + 2) >> 2 = 68; (3,0) avg2(L3, L2) = 180; (3,1) (L3 + 2 L2
	 * + L1 + 2) >> 2 = (240 + 240 + 60 + 2) >> 2 = 135.
	 */
	static const uint8_t expect_hd[16] = {
		40, 35, 23, 23,
		45, 43, 40, 35,
		90, 68, 45, 43,
		180, 135, 90, 68
	};

	/*
	 * B_HU_PRED: (0,0) avg2(L0, L1) = 45; (0,1) (L0 + 2 L1 + L2 + 2) >>
	 * 2 = (30 + 120 + 120 + 2) >> 2 = 68; (0,2) avg2(L1, L2) = 90; (0,3)
	 * (L1 + 2 L2 + L3 + 2) >> 2 = (60 + 240 + 240 + 2) >> 2 = 135; row 1
	 * is row 0 shifted left by two then avg2(L2, L3) = 180 and (L2 + 3
	 * L3 + 2) >> 2 = 210; row 2 is 180 210 and then L3 = 240 twice; row 3
	 * all L3 = 240.
	 */
	static const uint8_t expect_hu[16] = {
		45, 68, 90, 135,
		90, 135, 180, 210,
		180, 210, 240, 240,
		240, 240, 240, 240
	};

	static const uint8_t *const expects[10] = {
		expect_dc, expect_tm, expect_ve, expect_he, expect_ld,
		expect_rd, expect_vr, expect_vl, expect_hd, expect_hu
	};
	uint8_t dst[4 * 4];
	unsigned mode;
	unsigned i;
	int ok;

	/* Each mode against its hand-worked pixels (the modes are numbered as the table indexes them). */
	for (mode = 0; mode < 10U; mode++) {
		memset(dst, GUARD, sizeof(dst));
		webpc_vp8_predict_subblock(mode, above_row + 1, left_col, dst, 4);
		ok = 1;
		for (i = 0; i < 16U; i++) {
			if (dst[i] != expects[mode][i])
				ok = 0;
		}
		CHECK(ok);
	}
}

/*
 * Edge pixels at the extremes: a flat edge gives a flat block in every
 * mode, and TM clamps both ways.
 */
static void
test_subblock_extremes(void)
{
	uint8_t above_row[9];
	uint8_t left_col[4];
	uint8_t dst[16];
	unsigned mode;
	unsigned i;
	unsigned level;
	unsigned value;
	int flat;

	/* A flat edge of 0, of 127 (the value above a frame), of 129 and of 255. */
	for (level = 0; level < 4U; level++) {
		value = 0U;
		if (level == 1U)
			value = 127U;
		if (level == 2U)
			value = 129U;
		if (level == 3U)
			value = 255U;
		memset(above_row, (int)value, sizeof(above_row));
		memset(left_col, (int)value, sizeof(left_col));
		for (mode = 0; mode < 10U; mode++) {
			memset(dst, GUARD, sizeof(dst));
			webpc_vp8_predict_subblock(mode, above_row + 1, left_col, dst, 4);
			flat = 1;
			for (i = 0; i < 16U; i++) {
				if (dst[i] != (uint8_t)value)
					flat = 0;
			}
			CHECK(flat);
		}
	}

	/* TM with P = 0 and everything else 255: 255 + 255 - 0 -> 255; with P = 255 and the rest 0: -255 -> 0. */
	memset(above_row, 255, sizeof(above_row));
	above_row[0] = 0;
	memset(left_col, 255, sizeof(left_col));
	webpc_vp8_predict_subblock(WEBPC_VP8_B_TM_PRED, above_row + 1, left_col, dst, 4);
	CHECK(dst[0] == 255);
	CHECK(dst[15] == 255);
	memset(above_row, 0, sizeof(above_row));
	above_row[0] = 255;
	memset(left_col, 0, sizeof(left_col));
	webpc_vp8_predict_subblock(WEBPC_VP8_B_TM_PRED, above_row + 1, left_col, dst, 4);
	CHECK(dst[0] == 0);
	CHECK(dst[15] == 0);

	/*
	 * Rounding of the averages: A = 1 0 0 0 .. with P = 0: B_VE_PRED
	 * column 0 is (0 + 2 + 0 + 2) >> 2 = 1, column 1 is (1 + 0 + 0 + 2)
	 * >> 2 = 0.  B_LD_PRED's last pixel with A6 = 1, A7 = 0 is (1 + 0 +
	 * 0 + 2) >> 2 = 0, and with A7 = 1, A6 = 0 is (0 + 2 + 1 + 2) >> 2 =
	 * 1.
	 */
	memset(above_row, 0, sizeof(above_row));
	memset(left_col, 0, sizeof(left_col));
	above_row[1] = 1;
	webpc_vp8_predict_subblock(WEBPC_VP8_B_VE_PRED, above_row + 1, left_col, dst, 4);
	CHECK(dst[0] == 1);
	CHECK(dst[1] == 0);
	memset(above_row, 0, sizeof(above_row));
	above_row[7] = 1;
	webpc_vp8_predict_subblock(WEBPC_VP8_B_LD_PRED, above_row + 1, left_col, dst, 4);
	CHECK(dst[15] == 0);
	memset(above_row, 0, sizeof(above_row));
	above_row[8] = 1;
	webpc_vp8_predict_subblock(WEBPC_VP8_B_LD_PRED, above_row + 1, left_col, dst, 4);
	CHECK(dst[15] == 1);
}

/*
 * The subblock at strides other than 4 (the pixels beside it untouched),
 * and a mode the format does not have.
 */
static void
test_subblock_stride_and_refusal(void)
{
	static const uint8_t above_row[9] = { 50, 10, 20, 40, 80, 90, 100, 200, 250 };
	static const uint8_t left_col[4] = { 30, 60, 120, 240 };
	uint8_t expect[16];
	uint8_t dst[4 * 40];
	unsigned strides[4];
	unsigned t;
	unsigned stride;
	unsigned mode;
	unsigned r;
	unsigned c;
	int ok;
	int guard_ok;

	strides[0] = 4U;
	strides[1] = 16U;
	strides[2] = 17U;
	strides[3] = 40U;

	/* Every mode at every stride gives the pixels of stride 4 in its own rows. */
	for (mode = 0; mode < 10U; mode++) {
		webpc_vp8_predict_subblock(mode, above_row + 1, left_col, expect, 4);
		for (t = 0; t < 4U; t++) {
			stride = strides[t];
			memset(dst, GUARD, sizeof(dst));
			webpc_vp8_predict_subblock(mode, above_row + 1, left_col, dst, (ptrdiff_t)stride);
			ok = 1;
			guard_ok = 1;
			for (r = 0; r < 4U; r++) {
				for (c = 0; c < stride; c++) {
					if (c < 4U) {
						if (dst[r * stride + c] != expect[r * 4U + c])
							ok = 0;
					} else if (dst[r * stride + c] != GUARD) {
						guard_ok = 0;
					}
				}
			}
			CHECK(ok);
			CHECK(guard_ok);
		}
	}

	/* A mode of 10 or more writes nothing. */
	memset(dst, GUARD, sizeof(dst));
	webpc_vp8_predict_subblock(10U, above_row + 1, left_col, dst, 4);
	webpc_vp8_predict_subblock(0xffffffffU, above_row + 1, left_col, dst, 4);
	ok = 1;
	for (t = 0; t < sizeof(dst); t++) {
		if (dst[t] != GUARD)
			ok = 0;
	}
	CHECK(ok);
}

/*
 * Random edges: every subblock mode against the pixel-by-pixel
 * transcription, with buffers of exactly the size read or written so that
 * the sanitizer sees a stray access; and the whole-block modes against
 * the straightforward per-pixel formulas.
 */
static void
test_random(void)
{
	struct webpc_vp8_edges edges;
	uint8_t expect[4][4];
	unsigned iteration;
	unsigned mode;
	unsigned stride;
	unsigned r;
	unsigned c;
	unsigned size;
	unsigned sum;
	unsigned value;
	uint8_t *above_row;
	uint8_t *left_col;
	uint8_t *dst;
	int ok;
	int i;
	int v;

	/* Subblocks: random edge pixels over the whole range, random mode and stride. */
	for (iteration = 0; iteration < 3000U; iteration++) {
		above_row = (uint8_t *)malloc(9U);
		left_col = (uint8_t *)malloc(4U);
		mode = (unsigned)(check_random(&random_state) % 10UL);
		stride = 4U + (unsigned)(check_random(&random_state) % 13UL);
		dst = (uint8_t *)malloc(3U * stride + 4U);
		if (above_row == NULL || left_col == NULL || dst == NULL) {
			CHECK(0);
			free(above_row);
			free(left_col);
			free(dst);
			return;
		}
		for (i = 0; i < 9; i++) {
			above_row[i] = (uint8_t)(check_random(&random_state) & 0xffUL);
		}
		for (i = 0; i < 4; i++) {
			left_col[i] = (uint8_t)(check_random(&random_state) & 0xffUL);
		}
		/* Every fourth iteration uses extreme pixels only, to reach the clamps. */
		if (iteration % 4U == 0U) {
			for (i = 0; i < 9; i++) {
				above_row[i] = (uint8_t)((above_row[i] & 1) != 0 ? 255 : 0);
			}
			for (i = 0; i < 4; i++) {
				left_col[i] = (uint8_t)((left_col[i] & 1) != 0 ? 255 : 0);
			}
		}

		memset(dst, GUARD, 3U * stride + 4U);
		webpc_vp8_predict_subblock(mode, above_row + 1, left_col, dst, (ptrdiff_t)stride);
		reference_subblock(mode, above_row, left_col, expect);
		ok = 1;
		for (r = 0; r < 4U; r++) {
			for (c = 0; c < 4U; c++) {
				if (dst[r * stride + c] != expect[r][c])
					ok = 0;
			}
		}
		CHECK(ok);
		free(above_row);
		free(left_col);
		free(dst);
	}

	/* Whole blocks: random edges, both sizes, every mode, every have_* combination. */
	for (iteration = 0; iteration < 2000U; iteration++) {
		size = 16U;
		if ((check_random(&random_state) & 1UL) != 0UL)
			size = 8U;
		mode = (unsigned)(check_random(&random_state) % 4UL);
		for (i = 0; i < 16; i++) {
			edges.above[i] = (uint8_t)(check_random(&random_state) & 0xffUL);
			edges.left[i] = (uint8_t)(check_random(&random_state) & 0xffUL);
		}
		edges.corner = (uint8_t)(check_random(&random_state) & 0xffUL);
		edges.have_above = (int)(check_random(&random_state) & 1UL);
		edges.have_left = (int)(check_random(&random_state) & 1UL);
		if (iteration % 4U == 0U) {
			for (i = 0; i < 16; i++) {
				edges.above[i] = (uint8_t)((edges.above[i] & 1) != 0 ? 255 : 0);
				edges.left[i] = (uint8_t)((edges.left[i] & 1) != 0 ? 255 : 0);
			}
		}

		stride = size + (unsigned)(check_random(&random_state) % 9UL);
		dst = (uint8_t *)malloc((size - 1U) * stride + size);
		if (dst == NULL) {
			CHECK(0);
			return;
		}
		webpc_vp8_predict_block(mode, size, &edges, dst, (ptrdiff_t)stride);

		/* The DC value by its definition: the mean of the present edges, rounded half up. */
		sum = 0U;
		value = 128U;
		if (mode == WEBPC_VP8_DC_PRED) {
			if (edges.have_above != 0) {
				for (c = 0; c < size; c++)
					sum += edges.above[c];
			}
			if (edges.have_left != 0) {
				for (c = 0; c < size; c++)
					sum += edges.left[c];
			}
			if (edges.have_above != 0 && edges.have_left != 0)
				value = (sum + size) / (2U * size);
			else if (edges.have_above != 0 || edges.have_left != 0)
				value = (sum + size / 2U) / size;
		}

		ok = 1;
		for (r = 0; r < size; r++) {
			for (c = 0; c < size; c++) {
				v = 0;
				if (mode == WEBPC_VP8_DC_PRED) {
					v = (int)value;
				} else if (mode == WEBPC_VP8_V_PRED) {
					v = edges.above[c];
				} else if (mode == WEBPC_VP8_H_PRED) {
					v = edges.left[r];
				} else {
					v = (int)edges.left[r] + (int)edges.above[c] - (int)edges.corner;
					if (v < 0)
						v = 0;
					if (v > 255)
						v = 255;
				}
				if (dst[r * stride + c] != (uint8_t)v)
					ok = 0;
			}
		}
		CHECK(ok);
		free(dst);
	}
}
