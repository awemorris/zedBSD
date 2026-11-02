/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The tests of vp8-filter.c: the control values against tables and cases
 * worked out by hand from RFC 6386 15.4 and 20.6, the simple and normal
 * edge filters against segments worked out by hand from 15.2 and 15.3
 * (thresholds exactly at and one beyond, the clamps, the floors of the
 * shifts, the two orientations and the count), the four steps of one
 * macroblock (what each flag skips, the order), and random inputs against
 * a transcription of the RFC's narrative formulas kept in the test, which
 * copies a segment out, filters it on the shifted values and copies it
 * back.
 */

#include "check.h"
#include "vp8-filter.h"

#include <stdlib.h>
#include <string.h>

/* The random generator's state. */
static unsigned long random_state = 0x2468ace1UL;

/* Which edge filter a model call stands for. */
#define KIND_SIMPLE	0
#define KIND_MB		1
#define KIND_SUB	2

static long floor_shift(long value, unsigned shift);
static int ref_c(int value);
static int ref_u2s(int value);
static int ref_s2u(int value);
static int ref_abs(int value);
static int ref_common_adjust(int use_outer_taps, unsigned char px[8]);
static void ref_simple_segment(int limit, unsigned char px[8]);
static int ref_filter_yes(int interior, int limit, const unsigned char px[8]);
static int ref_hev(int threshold, const unsigned char px[8]);
static void ref_subblock_segment(int hev_threshold, int interior, int limit, unsigned char px[8]);
static void ref_mb_segment(int hev_threshold, int interior, int limit, unsigned char px[8]);
static void ref_edge(unsigned char *edge, ptrdiff_t across, ptrdiff_t along, unsigned count, int kind, int limit,
	int interior, int hev);
static void ref_macroblock(unsigned char *y, unsigned char *u, unsigned char *v, ptrdiff_t y_stride,
	ptrdiff_t uv_stride, unsigned mb_x, unsigned mb_y, const struct webpc_vp8_filter_params *params, int simple,
	int filter_inner, int horizontal_first);
static void ref_params(const struct webpc_vp8_header *header, unsigned segment, int b_pred,
	struct webpc_vp8_filter_params *params);
static int random_in(int low, int high);
static void header_clear(struct webpc_vp8_header *header);
static void check_params(const struct webpc_vp8_header *header, unsigned segment, int b_pred, int level, int interior,
	int hev);
static void test_params_table(void);
static void test_params_segments(void);
static void test_params_deltas(void);
static void test_params_invalid(void);
static void test_params_random(void);
static void check_simple(const unsigned char in[4], int limit, const unsigned char expected[4]);
static void test_simple_hand(void);
static void test_simple_geometry(void);
static void check_mb(const unsigned char in[8], int limit, int interior, int hev, const unsigned char expected[8]);
static void check_sub(const unsigned char in[8], int limit, int interior, int hev, const unsigned char expected[8]);
static void test_mb_hand(void);
static void test_sub_hand(void);
static void check_interior(int kind, int position, int delta, int interior, int expect_change);
static void test_interior_boundaries(void);
static void test_normal_geometry(void);
static void make_segment(unsigned char px[8]);
static void test_random_segments(void);
static void test_random_images(void);
static void frame_fill(unsigned char *frame, unsigned width_mbs, unsigned height_mbs, unsigned mb_size);
static int frames_equal(const unsigned char *left, const unsigned char *right, size_t size);
static void test_macroblock(void);
static void test_macroblock_skips(void);
static void test_macroblock_order(void);

/*
 * Runs the tests of vp8-filter.c.
 */
int
main(
	void)
{
	int status;

	/* Each helper in turn. */
	test_params_table();
	test_params_segments();
	test_params_deltas();
	test_params_invalid();
	test_params_random();
	test_simple_hand();
	test_simple_geometry();
	test_mb_hand();
	test_sub_hand();
	test_interior_boundaries();
	test_normal_geometry();
	test_random_segments();
	test_random_images();
	test_macroblock();
	test_macroblock_skips();
	test_macroblock_order();

	/* The summary line. */
	status = check_finish("vp8_filter");
	return status;
}

/*
 * Shifts right by the floor of the quotient, with division so that no shift
 * of a negative value is made.
 */
static long
floor_shift(
	long value,
	unsigned shift)
{
	long divisor;
	long quotient;

	/* The divisor is 2^shift; C division truncates toward zero. */
	divisor = 1L << shift;
	quotient = value / divisor;

	/* A negative value with a remainder truncated upward: step down. */
	if (value < 0L && quotient * divisor != value)
		quotient--;

	return quotient;
}

/*
 * The format's c(): clamp to the signed 8-bit range.
 */
static int
ref_c(
	int value)
{
	/* The three-way choice RFC 6386 15.2 writes. */
	return value < -128 ? -128 : (value < 128 ? value : 127);
}

/*
 * The format's u2s(): a pixel as a signed 8-bit number.
 */
static int
ref_u2s(
	int value)
{
	/* The shift by 128. */
	return value - 128;
}

/*
 * The format's s2u(): clamp, then back to a pixel.
 */
static int
ref_s2u(
	int value)
{
	/* The clamp and the shift back. */
	return ref_c(value) + 128;
}

/*
 * The format's abs().
 */
static int
ref_abs(
	int value)
{
	/* The sign dropped. */
	return value < 0 ? -value : value;
}

/*
 * RFC 6386 15.2's common_adjust on a segment copied into px (p3 p2 p1 p0 q0
 * q1 q2 q3 at 0..7); gives the amount q0 moved.
 */
static int
ref_common_adjust(
	int use_outer_taps,
	unsigned char px[8])
{
	int p1;
	int p0;
	int q0;
	int q1;
	int a;
	int b;

	/* The four pixels as signed numbers. */
	p1 = ref_u2s(px[2]);
	p0 = ref_u2s(px[3]);
	q0 = ref_u2s(px[4]);
	q1 = ref_u2s(px[5]);

	/* The format's a and b, each shift a floor. */
	a = ref_c((use_outer_taps ? ref_c(p1 - q1) : 0) + 3 * (q0 - p0));
	b = (int)floor_shift(ref_c(a + 3), 3);
	a = (int)floor_shift(ref_c(a + 4), 3);

	/* The two edge pixels. */
	px[4] = (unsigned char)ref_s2u(q0 - a);
	px[3] = (unsigned char)ref_s2u(p0 + b);

	return a;
}

/*
 * RFC 6386 15.2's simple_segment on a copied segment.
 */
static void
ref_simple_segment(
	int limit,
	unsigned char px[8])
{
	/* The test is the narrative's, with the integer division by 2. */
	if ((ref_abs(px[3] - px[4]) * 2 + ref_abs(px[2] - px[5]) / 2) <= limit)
		ref_common_adjust(1, px);
}

/*
 * RFC 6386 15.3's filter_yes on a copied segment.
 */
static int
ref_filter_yes(
	int interior,
	int limit,
	const unsigned char px[8])
{
	/* The narrative's conjunction. */
	return (ref_abs(px[3] - px[4]) * 2 + ref_abs(px[2] - px[5]) / 2) <= limit &&
	    ref_abs(px[0] - px[1]) <= interior && ref_abs(px[1] - px[2]) <= interior &&
	    ref_abs(px[2] - px[3]) <= interior && ref_abs(px[7] - px[6]) <= interior &&
	    ref_abs(px[6] - px[5]) <= interior && ref_abs(px[5] - px[4]) <= interior;
}

/*
 * RFC 6386 15.3's hev on a copied segment.
 */
static int
ref_hev(
	int threshold,
	const unsigned char px[8])
{
	/* Either side's step above the threshold. */
	return ref_abs(px[2] - px[3]) > threshold || ref_abs(px[5] - px[4]) > threshold;
}

/*
 * RFC 6386 15.3's subblock_filter on a copied segment.
 */
static void
ref_subblock_segment(
	int hev_threshold,
	int interior,
	int limit,
	unsigned char px[8])
{
	int p1;
	int q1;
	int hv;
	int a;

	/* The two inner pixels as the narrative reads them, before the adjustment. */
	p1 = ref_u2s(px[2]);
	q1 = ref_u2s(px[5]);

	/* Nothing happens unless the thresholds pass. */
	if (!ref_filter_yes(interior, limit, px))
		return;

	/* The common adjustment, with the outer taps on high edge variance only. */
	hv = ref_hev(hev_threshold, px);
	a = (int)floor_shift(ref_common_adjust(hv, px) + 1, 1);

	/* Without high edge variance the inner pixels move too. */
	if (!hv) {
		px[5] = (unsigned char)ref_s2u(q1 - a);
		px[2] = (unsigned char)ref_s2u(p1 + a);
	}
}

/*
 * RFC 6386 15.3's MBfilter on a copied segment.
 */
static void
ref_mb_segment(
	int hev_threshold,
	int interior,
	int limit,
	unsigned char px[8])
{
	int p2;
	int p1;
	int p0;
	int q0;
	int q1;
	int q2;
	int w;
	int a;

	/* The six pixels the filter may move, as signed numbers. */
	p2 = ref_u2s(px[1]);
	p1 = ref_u2s(px[2]);
	p0 = ref_u2s(px[3]);
	q0 = ref_u2s(px[4]);
	q1 = ref_u2s(px[5]);
	q2 = ref_u2s(px[6]);

	/* Nothing happens unless the thresholds pass. */
	if (!ref_filter_yes(interior, limit, px))
		return;

	/* High edge variance takes the simple filter. */
	if (ref_hev(hev_threshold, px)) {
		ref_common_adjust(1, px);
		return;
	}

	/* Otherwise 27, 18 and 9 parts of w in 128, each shift a floor. */
	w = ref_c(ref_c(p1 - q1) + 3 * (q0 - p0));
	a = ref_c((int)floor_shift(27L * w + 63L, 7));
	px[4] = (unsigned char)ref_s2u(q0 - a);
	px[3] = (unsigned char)ref_s2u(p0 + a);
	a = ref_c((int)floor_shift(18L * w + 63L, 7));
	px[5] = (unsigned char)ref_s2u(q1 - a);
	px[2] = (unsigned char)ref_s2u(p1 + a);
	a = ref_c((int)floor_shift(9L * w + 63L, 7));
	px[6] = (unsigned char)ref_s2u(q2 - a);
	px[1] = (unsigned char)ref_s2u(p2 + a);
}

/*
 * Applies a filter of the given kind to count segments of an edge, by
 * copying each segment into eight bytes, filtering the copy and writing it
 * back.  A simple segment copies only its four pixels.
 */
static void
ref_edge(
	unsigned char *edge,
	ptrdiff_t across,
	ptrdiff_t along,
	unsigned count,
	int kind,
	int limit,
	int interior,
	int hev)
{
	unsigned char px[8];
	unsigned char *segment;
	unsigned index;
	int first;
	int last;
	int position;

	/* The simple filter reads p1..q1 only; the others read p3..q3. */
	first = 0;
	last = 7;
	if (kind == KIND_SIMPLE) {
		first = 2;
		last = 5;
	}

	/* Each segment: copy out, filter, copy back. */
	memset(px, 0, sizeof(px));
	for (index = 0; index < count; index++) {
		segment = edge + (ptrdiff_t)index * along;
		for (position = first; position <= last; position++)
			px[position] = segment[(position - 4) * across];
		if (kind == KIND_SIMPLE)
			ref_simple_segment(limit, px);
		else if (kind == KIND_MB)
			ref_mb_segment(hev, interior, limit, px);
		else
			ref_subblock_segment(hev, interior, limit, px);
		for (position = first; position <= last; position++)
			segment[(position - 4) * across] = px[position];
	}
}

/*
 * RFC 6386 15.1's four steps for one macroblock on the model's edges;
 * horizontal_first runs steps 3 and 4 before steps 1 and 2 (a wrong order,
 * used to show the order matters).
 */
static void
ref_macroblock(
	unsigned char *y,
	unsigned char *u,
	unsigned char *v,
	ptrdiff_t y_stride,
	ptrdiff_t uv_stride,
	unsigned mb_x,
	unsigned mb_y,
	const struct webpc_vp8_filter_params *params,
	int simple,
	int filter_inner,
	int horizontal_first)
{
	int pass;
	int vertical;
	int offset;

	/* A level of 0 is not filtered at all. */
	if (params->level == 0)
		return;

	/* Two passes: the vertical edges (steps 1 and 2) and the horizontal ones (3 and 4). */
	for (pass = 0; pass < 2; pass++) {
		vertical = (pass == 0);
		if (horizontal_first)
			vertical = !vertical;

		if (vertical) {
			/* Step 1, then step 2. */
			if (mb_x > 0) {
				ref_edge(y, 1, y_stride, 16, simple ? KIND_SIMPLE : KIND_MB, params->mb_limit,
					params->interior, params->hev);
				if (!simple) {
					ref_edge(u, 1, uv_stride, 8, KIND_MB, params->mb_limit, params->interior, params->hev);
					ref_edge(v, 1, uv_stride, 8, KIND_MB, params->mb_limit, params->interior, params->hev);
				}
			}
			if (filter_inner) {
				for (offset = 4; offset <= 12; offset += 4) {
					ref_edge(y + offset, 1, y_stride, 16, simple ? KIND_SIMPLE : KIND_SUB,
						params->sub_limit, params->interior, params->hev);
				}
				if (!simple) {
					ref_edge(u + 4, 1, uv_stride, 8, KIND_SUB, params->sub_limit, params->interior,
						params->hev);
					ref_edge(v + 4, 1, uv_stride, 8, KIND_SUB, params->sub_limit, params->interior,
						params->hev);
				}
			}
		} else {
			/* Step 3, then step 4. */
			if (mb_y > 0) {
				ref_edge(y, y_stride, 1, 16, simple ? KIND_SIMPLE : KIND_MB, params->mb_limit,
					params->interior, params->hev);
				if (!simple) {
					ref_edge(u, uv_stride, 1, 8, KIND_MB, params->mb_limit, params->interior, params->hev);
					ref_edge(v, uv_stride, 1, 8, KIND_MB, params->mb_limit, params->interior, params->hev);
				}
			}
			if (filter_inner) {
				for (offset = 4; offset <= 12; offset += 4) {
					ref_edge(y + offset * y_stride, y_stride, 1, 16, simple ? KIND_SIMPLE : KIND_SUB,
						params->sub_limit, params->interior, params->hev);
				}
				if (!simple) {
					ref_edge(u + 4 * uv_stride, uv_stride, 1, 8, KIND_SUB, params->sub_limit,
						params->interior, params->hev);
					ref_edge(v + 4 * uv_stride, uv_stride, 1, 8, KIND_SUB, params->sub_limit,
						params->interior, params->hev);
				}
			}
		}
	}
}

/*
 * RFC 6386 20.6's calculate_filter_parameters for a key frame, written out
 * from the reference decoder's text with no shared code.
 */
static void
ref_params(
	const struct webpc_vp8_header *header,
	unsigned segment,
	int b_pred,
	struct webpc_vp8_filter_params *params)
{
	long level;
	long interior;
	long hev;

	/* The frame's level, replaced or adjusted by the segment's value. */
	level = header->filter.level;
	if (header->segmentation.enabled) {
		if (!header->segmentation.absolute)
			level += header->segmentation.filter_level[segment];
		else
			level = header->segmentation.filter_level[segment];
	}
	if (level > 63L)
		level = 63L;
	else if (level < 0L)
		level = 0L;

	/* The deltas of an intra frame; the first mode delta for B_PRED. */
	if (header->filter.deltas_enabled) {
		level += header->filter.ref_delta[0];
		if (b_pred)
			level += header->filter.mode_delta[0];
	}
	if (level > 63L)
		level = 63L;
	else if (level < 0L)
		level = 0L;

	/* The interior limit. */
	interior = level;
	if (header->filter.sharpness) {
		interior = floor_shift(interior, header->filter.sharpness > 4 ? 2 : 1);
		if (interior > 9L - header->filter.sharpness)
			interior = 9L - header->filter.sharpness;
	}
	if (interior < 1L)
		interior = 1L;

	/* The key frame's threshold. */
	hev = (level >= 15L);
	if (level >= 40L)
		hev++;

	/* The results. */
	params->level = (int)level;
	params->interior = (int)interior;
	params->hev = (int)hev;
	params->mb_limit = (int)((level + 2L) * 2L + interior);
	params->sub_limit = (int)(level * 2L + interior);
}

/*
 * Gives a random integer in low..high.
 */
static int
random_in(
	int low,
	int high)
{
	unsigned long value;

	/* The generator's next value, folded into the range. */
	value = check_random(&random_state);
	return low + (int)(value % (unsigned long)(high - low + 1));
}

/*
 * Clears a header and gives it the state a key frame without a filter has.
 */
static void
header_clear(
	struct webpc_vp8_header *header)
{
	/* Every field zero, and the segment probabilities at their default. */
	memset(header, 0, sizeof(*header));
	header->segmentation.tree_probs[0] = 255;
	header->segmentation.tree_probs[1] = 255;
	header->segmentation.tree_probs[2] = 255;
}

/*
 * Checks the controls of one macroblock against the values worked out by
 * hand (the limits follow from the level and interior limit by the
 * formulas of 15.4).
 */
static void
check_params(
	const struct webpc_vp8_header *header,
	unsigned segment,
	int b_pred,
	int level,
	int interior,
	int hev)
{
	struct webpc_vp8_filter_params params;

	/* Poisoned first, so a field left unwritten shows. */
	memset(&params, 0x5a, sizeof(params));
	webpc_vp8_filter_params(header, segment, b_pred, &params);

	/* Each control. */
	CHECK(params.level == level);
	CHECK(params.interior == interior);
	CHECK(params.hev == hev);
	CHECK(params.mb_limit == (level + 2) * 2 + interior);
	CHECK(params.sub_limit == level * 2 + interior);
}

/*
 * Sharpness 0..7 against levels 0, 1, 14, 15, 39, 40 and 63: the interior
 * limit table worked out by hand (level shifted by 1 for sharpness 1..4 and
 * by 2 above, held to 9 - sharpness, at least 1) and the threshold (0, 1
 * from 15, 2 from 40).
 */
static void
test_params_table(void)
{
	static const int levels[7] = {0, 1, 14, 15, 39, 40, 63};
	static const int hev_expected[7] = {0, 0, 0, 1, 1, 2, 2};
	static const int interior_expected[8][7] = {
		{1, 1, 14, 15, 39, 40, 63},
		{1, 1, 7, 7, 8, 8, 8},
		{1, 1, 7, 7, 7, 7, 7},
		{1, 1, 6, 6, 6, 6, 6},
		{1, 1, 5, 5, 5, 5, 5},
		{1, 1, 3, 3, 4, 4, 4},
		{1, 1, 3, 3, 3, 3, 3},
		{1, 1, 2, 2, 2, 2, 2}
	};
	struct webpc_vp8_header header;
	int sharpness;
	int index;

	/* Every sharpness against every level. */
	for (sharpness = 0; sharpness < 8; sharpness++) {
		for (index = 0; index < 7; index++) {
			header_clear(&header);
			header.filter.level = levels[index];
			header.filter.sharpness = sharpness;
			check_params(&header, 0, 0, levels[index], interior_expected[sharpness][index],
				hev_expected[index]);
		}
	}

	/* The segment number is ignored while segmentation is off. */
	header_clear(&header);
	header.filter.level = 20;
	check_params(&header, 3, 0, 20, 20, 1);
	check_params(&header, 200, 0, 20, 20, 1);
}

/*
 * Segment values: absolute and delta, the first clamp (a delta segment of
 * base 5 and -10 clamps to 0 before the +2 of the reference delta gives
 * 2), and segmentation enabled with no data sent.
 */
static void
test_params_segments(void)
{
	struct webpc_vp8_header header;

	/* An absolute segment replaces the frame's level. */
	header_clear(&header);
	header.filter.level = 5;
	header.segmentation.enabled = 1;
	header.segmentation.update_data = 1;
	header.segmentation.absolute = 1;
	header.segmentation.filter_level[0] = 12;
	header.segmentation.filter_level[1] = 30;
	header.segmentation.filter_level[2] = 70;
	header.segmentation.filter_level[3] = 0;
	check_params(&header, 0, 0, 12, 12, 0);
	check_params(&header, 1, 0, 30, 30, 1);

	/* Above 63 is held at 63. */
	check_params(&header, 2, 0, 63, 63, 2);

	/* An absolute 0 switches the filter off for the segment. */
	check_params(&header, 3, 0, 0, 1, 0);

	/* A delta segment adds to the frame's level. */
	header.segmentation.absolute = 0;
	header.segmentation.filter_level[0] = 7;
	header.segmentation.filter_level[1] = -10;
	header.segmentation.filter_level[2] = 100;
	header.segmentation.filter_level[3] = -2;
	check_params(&header, 0, 0, 12, 12, 0);
	check_params(&header, 2, 0, 63, 63, 2);
	check_params(&header, 3, 0, 3, 3, 0);

	/* A sum below 0 is held at 0 before the deltas are added. */
	check_params(&header, 1, 0, 0, 1, 0);
	header.filter.deltas_enabled = 1;
	header.filter.ref_delta[0] = 2;
	check_params(&header, 1, 0, 2, 2, 0);

	/* Enabled with no data sent: the frame's own level for every segment. */
	header_clear(&header);
	header.filter.level = 33;
	header.segmentation.enabled = 1;
	header.segmentation.update_data = 0;
	check_params(&header, 0, 0, 33, 33, 1);
	check_params(&header, 3, 0, 33, 33, 1);
}

/*
 * The deltas: the intra reference delta for every macroblock, the first
 * mode delta for B_PRED only, the other deltas never, the second clamp, and
 * the deltas ignored when not enabled.
 */
static void
test_params_deltas(void)
{
	struct webpc_vp8_header header;

	/* The reference delta applies to every macroblock. */
	header_clear(&header);
	header.filter.level = 5;
	header.filter.deltas_enabled = 1;
	header.filter.ref_delta[0] = 7;
	header.filter.ref_delta[1] = 20;
	header.filter.ref_delta[2] = 21;
	header.filter.ref_delta[3] = 22;
	header.filter.mode_delta[0] = -3;
	header.filter.mode_delta[1] = 30;
	header.filter.mode_delta[2] = 31;
	header.filter.mode_delta[3] = 32;
	check_params(&header, 0, 0, 12, 12, 0);

	/* The first mode delta joins it for B_PRED. */
	check_params(&header, 0, 1, 9, 9, 0);

	/* The sum is held at 63 and at 0. */
	header.filter.level = 60;
	header.filter.ref_delta[0] = 10;
	check_params(&header, 0, 0, 63, 63, 2);
	header.filter.level = 3;
	header.filter.ref_delta[0] = -8;
	check_params(&header, 0, 0, 0, 1, 0);

	/* A delta can lift a level over a threshold of the high edge variance. */
	header.filter.level = 30;
	header.filter.ref_delta[0] = 10;
	check_params(&header, 0, 0, 40, 40, 2);
	header.filter.ref_delta[0] = -15;
	check_params(&header, 0, 0, 15, 15, 1);
	header.filter.ref_delta[0] = -16;
	check_params(&header, 0, 0, 14, 14, 0);

	/* Not enabled: no delta applies. */
	header.filter.deltas_enabled = 0;
	check_params(&header, 0, 1, 30, 30, 1);
}

/*
 * Values the controls must survive: no result pointer, no header, a
 * segment out of range, sums beyond the int range, a sharpness out of its
 * 3 bits.
 */
static void
test_params_invalid(void)
{
	struct webpc_vp8_header header;
	struct webpc_vp8_filter_params params;

	/* No place for the answer: nothing happens. */
	header_clear(&header);
	webpc_vp8_filter_params(&header, 0, 0, NULL);
	CHECK(1);

	/* No header: not filtered. */
	memset(&params, 0x5a, sizeof(params));
	webpc_vp8_filter_params(NULL, 0, 0, &params);
	CHECK(params.level == 0);

	/* A segment the frame cannot have, with segmentation on: not filtered. */
	header.filter.level = 40;
	header.segmentation.enabled = 1;
	memset(&params, 0x5a, sizeof(params));
	webpc_vp8_filter_params(&header, 4, 0, &params);
	CHECK(params.level == 0);
	CHECK(params.interior == 1);
	CHECK(params.hev == 0);

	/* Sums beyond the int range are held (no overflow; UBSan stops one). */
	header_clear(&header);
	header.filter.level = 63;
	header.segmentation.enabled = 1;
	header.segmentation.filter_level[1] = 2147483647L;
	check_params(&header, 1, 0, 63, 63, 2);
	header.segmentation.filter_level[1] = -2147483647L - 1L;
	check_params(&header, 1, 0, 0, 1, 0);
	header.segmentation.filter_level[1] = 0;
	header.filter.deltas_enabled = 1;
	header.filter.ref_delta[0] = 2147483647L;
	header.filter.mode_delta[0] = 2147483647L;
	check_params(&header, 1, 1, 63, 63, 2);
	header.filter.ref_delta[0] = -2147483647L - 1L;
	header.filter.mode_delta[0] = -2147483647L - 1L;
	check_params(&header, 1, 1, 0, 1, 0);

	/* A sharpness outside 0..7 is held to it: 100 acts as 7. */
	header_clear(&header);
	header.filter.level = 40;
	header.filter.sharpness = 100;
	check_params(&header, 0, 0, 40, 2, 2);
	header.filter.sharpness = -5;
	check_params(&header, 0, 0, 40, 40, 2);
}

/*
 * Random headers, segments and flags against the model of the reference
 * decoder's text.
 */
static void
test_params_random(void)
{
	struct webpc_vp8_header header;
	struct webpc_vp8_filter_params actual;
	struct webpc_vp8_filter_params expected;
	unsigned iteration;
	unsigned segment;
	int b_pred;
	int index;

	/* Many random headers. */
	for (iteration = 0; iteration < 20000U; iteration++) {
		header_clear(&header);
		header.filter.level = random_in(0, 63);
		header.filter.sharpness = random_in(0, 7);
		header.filter.deltas_enabled = random_in(0, 1);
		header.segmentation.enabled = random_in(0, 1);
		header.segmentation.absolute = random_in(0, 1);
		for (index = 0; index < 4; index++) {
			header.segmentation.filter_level[index] = random_in(-80, 80);
			header.filter.ref_delta[index] = random_in(-70, 70);
			header.filter.mode_delta[index] = random_in(-70, 70);
		}
		segment = (unsigned)random_in(0, 3);
		b_pred = random_in(0, 1);

		memset(&actual, 0x5a, sizeof(actual));
		webpc_vp8_filter_params(&header, segment, b_pred, &actual);
		ref_params(&header, segment, b_pred, &expected);
		CHECK(actual.level == expected.level);
		CHECK(actual.interior == expected.interior);
		CHECK(actual.hev == expected.hev);
		CHECK(actual.mb_limit == expected.mb_limit);
		CHECK(actual.sub_limit == expected.sub_limit);
	}
}

/*
 * Filters four pixels (p1 p0 q0 q1) with the simple filter and compares.
 * The array has exactly four bytes so a read of p2 or q2 is an error under
 * the sanitizer.
 */
static void
check_simple(
	const unsigned char in[4],
	int limit,
	const unsigned char expected[4])
{
	unsigned char px[4];

	/* The edge is the third pixel (q0); across is one byte. */
	memcpy(px, in, 4);
	webpc_vp8_filter_simple_edge(px + 2, 1, 4, 1, limit);
	CHECK(memcmp(px, expected, 4) == 0);
}

/*
 * The simple filter on single segments worked out by hand from 15.2.
 */
static void
test_simple_hand(void)
{
	/* a = c(-10 + 30) = 20; q0 -= 20+4 >> 3 = 3; p0 += 20+3 >> 3 = 2. */
	static const unsigned char case_a[4] = {100, 100, 110, 110};
	static const unsigned char case_a_out[4] = {100, 102, 107, 110};
	/* An odd |p1 - q1| is halved down: 0 + (11 >> 1) = 5; a = -11, f1 = -1, f2 = -1. */
	static const unsigned char case_odd[4] = {100, 100, 100, 111};
	static const unsigned char case_odd_out[4] = {100, 99, 101, 111};
	/* a = 4: f1 = 8 >> 3 = 1, f2 = 7 >> 3 = 0. */
	static const unsigned char case_four[4] = {104, 100, 100, 100};
	static const unsigned char case_four_out[4] = {104, 100, 99, 100};
	/* a = -4: f1 = 0 >> 3 = 0, f2 = -1 >> 3 = -1 (a floor, not a truncation). */
	static const unsigned char case_minus_four[4] = {100, 100, 100, 104};
	static const unsigned char case_minus_four_out[4] = {100, 99, 100, 104};
	/* a clamps to 127: f1 = f2 = 127 >> 3 = 15. */
	static const unsigned char case_high[4] = {0, 0, 255, 255};
	static const unsigned char case_high_out[4] = {0, 15, 240, 255};
	/* a clamps to -128: f1 = -124 >> 3 = -16, f2 = -125 >> 3 = -16. */
	static const unsigned char case_low[4] = {255, 255, 0, 0};
	static const unsigned char case_low_out[4] = {255, 239, 16, 0};
	/* p1 - q1 = 255 clamps to 127; a = c(127 + 3) = 127; p0 = 254 + 15 clamps to 255. */
	static const unsigned char case_clamp_p[4] = {255, 254, 255, 0};
	static const unsigned char case_clamp_p_out[4] = {255, 255, 240, 0};
	/* The mirror: p0 = 1 - 16 clamps to 0. */
	static const unsigned char case_clamp_q[4] = {0, 1, 0, 255};
	static const unsigned char case_clamp_q_out[4] = {0, 0, 16, 255};
	unsigned char px[4];

	/* The threshold: 2 * 10 + (10 >> 1) = 25, so 25 filters and 24 does not. */
	check_simple(case_a, 25, case_a_out);
	check_simple(case_a, 24, case_a);
	check_simple(case_a, 1000, case_a_out);
	check_simple(case_a, 0, case_a);

	/* The halved difference at its limit: 5 filters, 4 does not. */
	check_simple(case_odd, 5, case_odd_out);
	check_simple(case_odd, 4, case_odd);

	/* The rounding between the two shifts, positive and negative. */
	check_simple(case_four, 2, case_four_out);
	check_simple(case_four, 1, case_four);
	check_simple(case_minus_four, 2, case_minus_four_out);

	/* The clamps of the signed values. */
	check_simple(case_high, 1000, case_high_out);
	check_simple(case_low, 1000, case_low_out);
	check_simple(case_clamp_p, 129, case_clamp_p_out);
	check_simple(case_clamp_p, 128, case_clamp_p);
	check_simple(case_clamp_q, 129, case_clamp_q_out);

	/* A count of 0 touches nothing. */
	memcpy(px, case_a, 4);
	webpc_vp8_filter_simple_edge(px + 2, 1, 4, 0, 1000);
	CHECK(memcmp(px, case_a, 4) == 0);
}

/*
 * The simple filter on a vertical edge (across 1, along the stride) and on
 * a horizontal one (across the stride, along 1) over five segments, the
 * count limiting how many are done.
 */
static void
test_simple_geometry(void)
{
	/* Five segments as p1 p0 q0 q1: filtered, too far apart, rounding, filtered, rounding. */
	static const unsigned char rows_in[5][4] = {
		{100, 100, 110, 110},
		{100, 100, 200, 200},
		{104, 100, 100, 100},
		{100, 100, 110, 110},
		{104, 100, 100, 100}
	};
	static const unsigned char rows_out[5][4] = {
		{100, 102, 107, 110},
		{100, 100, 200, 200},
		{104, 100, 99, 100},
		{100, 102, 107, 110},
		{104, 100, 99, 100}
	};
	unsigned char vertical[5 * 6];
	unsigned char horizontal[6 * 5];
	unsigned count;
	int row;
	int column;
	unsigned char expected;

	/* Several counts: the first count segments change, the rest stay. */
	for (count = 0; count <= 5; count++) {
		memset(vertical, 50, sizeof(vertical));
		memset(horizontal, 50, sizeof(horizontal));
		for (row = 0; row < 5; row++) {
			for (column = 0; column < 4; column++) {
				vertical[row * 6 + column] = rows_in[row][column];
				horizontal[column * 5 + row] = rows_in[row][column];
			}
		}

		/* Vertical edge at column 2 of 6; horizontal edge at row 2 of 6. */
		webpc_vp8_filter_simple_edge(vertical + 2, 1, 6, count, 30);
		webpc_vp8_filter_simple_edge(horizontal + 2 * 5, 5, 1, count, 30);

		/* The segments beyond the count and the pixels outside the four stay as they were. */
		for (row = 0; row < 5; row++) {
			for (column = 0; column < 6; column++) {
				if (column >= 4)
					expected = 50;
				else if ((unsigned)row < count)
					expected = rows_out[row][column];
				else
					expected = rows_in[row][column];
				CHECK(vertical[row * 6 + column] == expected);
				if (column < 4)
					CHECK(horizontal[column * 5 + row] == expected);
				else
					CHECK(horizontal[column * 5 + row] == 50);
			}
		}
	}
}

/*
 * Filters eight pixels (p3..q3) with the macroblock edge filter and
 * compares.  The array has exactly eight bytes.
 */
static void
check_mb(
	const unsigned char in[8],
	int limit,
	int interior,
	int hev,
	const unsigned char expected[8])
{
	unsigned char px[8];

	/* The edge is the fifth pixel (q0). */
	memcpy(px, in, 8);
	webpc_vp8_filter_mb_edge(px + 4, 1, 8, 1, limit, interior, hev);
	CHECK(memcmp(px, expected, 8) == 0);
}

/*
 * Filters eight pixels (p3..q3) with the subblock edge filter and compares.
 */
static void
check_sub(
	const unsigned char in[8],
	int limit,
	int interior,
	int hev,
	const unsigned char expected[8])
{
	unsigned char px[8];

	/* The edge is the fifth pixel (q0). */
	memcpy(px, in, 8);
	webpc_vp8_filter_sub_edge(px + 4, 1, 8, 1, limit, interior, hev);
	CHECK(memcmp(px, expected, 8) == 0);
}

/*
 * The macroblock edge filter on single segments worked out by hand from
 * 15.3: the simple adjustment under high edge variance, the 27, 18 and 9
 * weights without it (positive and negative, each shift a floor), and the
 * limit and the threshold of the variance at their boundaries.
 */
static void
test_mb_hand(void)
{
	/* w = c(-10 + 24) = 14: a = 441 >> 7 = 3, 315 >> 7 = 2, 189 >> 7 = 1. */
	static const unsigned char plus_in[8] = {100, 100, 100, 100, 108, 110, 110, 110};
	static const unsigned char plus_out[8] = {100, 101, 102, 103, 105, 108, 109, 110};
	/* High edge variance (|q1 - q0| = 2 > 1): a = 14, f1 = 18 >> 3 = 2, f2 = 17 >> 3 = 2. */
	static const unsigned char plus_hev_out[8] = {100, 100, 100, 102, 106, 110, 110, 110};
	/* w = c(10 - 24) = -14: a = -315 >> 7 = -3, -189 >> 7 = -2, -63 >> 7 = -1 (floors). */
	static const unsigned char minus_in[8] = {110, 110, 110, 108, 100, 100, 100, 100};
	static const unsigned char minus_out[8] = {110, 109, 108, 105, 103, 102, 101, 100};
	/* High edge variance on the other side: a = c(10 - 24) = -14, f1 = -10 >> 3 = -2, f2 = -11 >> 3 = -2. */
	static const unsigned char minus_hev_out[8] = {110, 110, 110, 106, 102, 100, 100, 100};
	/* |p3 - p2| = 10, the interior limit, still filters; 11 does not. */
	static const unsigned char interior_at_in[8] = {90, 100, 100, 100, 108, 110, 110, 110};
	static const unsigned char interior_at_out[8] = {90, 101, 102, 103, 105, 108, 109, 110};
	static const unsigned char interior_over_in[8] = {89, 100, 100, 100, 108, 110, 110, 110};

	/* The weights, positive. */
	check_mb(plus_in, 30, 10, 2, plus_out);

	/* The weights, negative: a floor, not a truncation toward 0. */
	check_mb(minus_in, 30, 10, 2, minus_out);

	/* High edge variance: threshold 1 is exceeded by a step of 2, threshold 2 is not. */
	check_mb(plus_in, 30, 10, 1, plus_hev_out);
	check_mb(plus_in, 30, 10, 2, plus_out);
	check_mb(minus_in, 30, 10, 1, minus_hev_out);

	/* The edge limit: 2 * 8 + (10 >> 1) = 21 filters, 20 does not. */
	check_mb(plus_in, 21, 10, 2, plus_out);
	check_mb(plus_in, 20, 10, 2, plus_in);

	/* The interior limit: a difference of exactly 10 filters, 11 does not. */
	check_mb(interior_at_in, 30, 10, 2, interior_at_out);
	check_mb(interior_over_in, 30, 10, 2, interior_over_in);
}

/*
 * The subblock edge filter on single segments worked out by hand from
 * 15.3: the inner pixels moving by (f1 + 1) >> 1 (a floor for a negative
 * f1), none moving under high edge variance, and the limit's boundary.
 */
static void
test_sub_hand(void)
{
	/* a = 3 * 8 = 24: f1 = 28 >> 3 = 3, f2 = 27 >> 3 = 3; the inner amount is 4 >> 1 = 2. */
	static const unsigned char plus_in[8] = {100, 100, 100, 100, 108, 110, 110, 110};
	static const unsigned char plus_out[8] = {100, 100, 102, 103, 105, 108, 110, 110};
	/* High edge variance: the outer taps give a = 14, f1 = f2 = 2, and p1 and q1 stay. */
	static const unsigned char plus_hev_out[8] = {100, 100, 100, 102, 106, 110, 110, 110};
	/* a = -24: f1 = -20 >> 3 = -3, f2 = -21 >> 3 = -3; the inner amount is -2 >> 1 = -1. */
	static const unsigned char minus_in[8] = {110, 110, 110, 108, 100, 100, 100, 100};
	static const unsigned char minus_out[8] = {110, 110, 109, 105, 103, 101, 100, 100};
	/* a = -15: f1 = -11 >> 3 = -2, f2 = -12 >> 3 = -2; the inner amount is -1 >> 1 = -1. */
	static const unsigned char floor_in[8] = {105, 105, 105, 105, 100, 100, 100, 100};
	static const unsigned char floor_out[8] = {105, 105, 104, 103, 102, 101, 100, 100};
	/* a = 15: f1 = 19 >> 3 = 2, f2 = 18 >> 3 = 2; the inner amount is 3 >> 1 = 1. */
	static const unsigned char two_in[8] = {100, 100, 100, 100, 105, 105, 105, 105};
	static const unsigned char two_out[8] = {100, 100, 101, 102, 103, 104, 105, 105};

	/* Positive and negative edges. */
	check_sub(plus_in, 30, 10, 2, plus_out);
	check_sub(minus_in, 30, 10, 2, minus_out);

	/* The two roundings of the halved amount. */
	check_sub(floor_in, 30, 10, 2, floor_out);
	check_sub(two_in, 30, 10, 2, two_out);

	/* High edge variance: only the two edge pixels, with the outer taps. */
	check_sub(plus_in, 30, 10, 1, plus_hev_out);

	/* The edge limit: 21 filters, 20 does not. */
	check_sub(plus_in, 21, 10, 2, plus_out);
	check_sub(plus_in, 20, 10, 2, plus_in);

	/* The step of the variance: exactly the threshold is not high. */
	check_sub(plus_in, 30, 10, 2, plus_out);
}

/*
 * Filters a flat segment (100 before the edge, 120 after) in which one
 * pixel is moved by delta, with a normal filter (kind 1 the macroblock
 * edge filter, 2 the subblock one) that never sees high edge variance, and
 * checks that the segment changes exactly when expect_change.
 */
static void
check_interior(
	int kind,
	int position,
	int delta,
	int interior,
	int expect_change)
{
	unsigned char px[8];
	unsigned char before[8];

	/* A flat 100 before the edge and a flat 120 after it, with one pixel moved. */
	memset(px, 100, 4);
	memset(px + 4, 120, 4);
	px[position] = (unsigned char)(px[position] + delta);
	memcpy(before, px, 8);

	/* The threshold of the variance is out of reach, the edge limit far above the step. */
	if (kind == KIND_MB)
		webpc_vp8_filter_mb_edge(px + 4, 1, 8, 1, 500, interior, 200);
	else
		webpc_vp8_filter_sub_edge(px + 4, 1, 8, 1, 500, interior, 200);

	/* Filtered exactly when the difference is within the limit. */
	if (expect_change)
		CHECK(memcmp(px, before, 8) != 0);
	else
		CHECK(memcmp(px, before, 8) == 0);
}

/*
 * Each of the six interior differences, exactly at the interior limit
 * (the edge is filtered) and one beyond (it is left alone), raised and
 * lowered, for both normal filters.
 */
static void
test_interior_boundaries(void)
{
	static const int positions[6] = {0, 1, 2, 5, 6, 7};
	int which;
	int kind;

	/* Each pixel whose difference to its neighbour is tested, on each side and both filters. */
	for (kind = KIND_MB; kind <= KIND_SUB; kind++) {
		for (which = 0; which < 6; which++) {
			check_interior(kind, positions[which], 5, 5, 1);
			check_interior(kind, positions[which], 6, 5, 0);
			check_interior(kind, positions[which], -5, 5, 1);
			check_interior(kind, positions[which], -6, 5, 0);
		}
	}
}

/*
 * The normal filters on a vertical edge (across 1, along the row length)
 * and a horizontal one (across the column length, along 1) over four
 * segments, with the count limiting how many are done.
 */
static void
test_normal_geometry(void)
{
	/* Four segments p3..q3: weights, high edge variance (a step of 3), too far apart, negative weights. */
	static const unsigned char in[4][8] = {
		{100, 100, 100, 100, 108, 110, 110, 110},
		{100, 100, 100, 100, 108, 111, 111, 111},
		{100, 100, 100, 100, 200, 200, 200, 200},
		{110, 110, 110, 108, 100, 100, 100, 100}
	};
	static const unsigned char mb_out[4][8] = {
		{100, 101, 102, 103, 105, 108, 109, 110},
		{100, 100, 100, 102, 106, 111, 111, 111},
		{100, 100, 100, 100, 200, 200, 200, 200},
		{110, 109, 108, 105, 103, 102, 101, 100}
	};
	static const unsigned char sub_out[4][8] = {
		{100, 100, 102, 103, 105, 108, 110, 110},
		{100, 100, 100, 102, 106, 111, 111, 111},
		{100, 100, 100, 100, 200, 200, 200, 200},
		{110, 110, 109, 105, 103, 101, 100, 100}
	};
	unsigned char vertical[4 * 8];
	unsigned char horizontal[8 * 4];
	unsigned count;
	int kind;
	int row;
	int column;
	unsigned char expected;

	/* Both filters, several counts. */
	for (kind = 0; kind < 2; kind++) {
		for (count = 0; count <= 4; count++) {
			for (row = 0; row < 4; row++) {
				for (column = 0; column < 8; column++) {
					vertical[row * 8 + column] = in[row][column];
					horizontal[column * 4 + row] = in[row][column];
				}
			}

			/* The vertical edge is at column 4 of 8; the horizontal edge is at row 4 of 8. */
			if (kind == 0) {
				webpc_vp8_filter_mb_edge(vertical + 4, 1, 8, count, 30, 10, 2);
				webpc_vp8_filter_mb_edge(horizontal + 4 * 4, 4, 1, count, 30, 10, 2);
			} else {
				webpc_vp8_filter_sub_edge(vertical + 4, 1, 8, count, 30, 10, 2);
				webpc_vp8_filter_sub_edge(horizontal + 4 * 4, 4, 1, count, 30, 10, 2);
			}

			/* The first count segments are filtered, the rest stay. */
			for (row = 0; row < 4; row++) {
				for (column = 0; column < 8; column++) {
					if ((unsigned)row >= count)
						expected = in[row][column];
					else if (kind == 0)
						expected = mb_out[row][column];
					else
						expected = sub_out[row][column];
					CHECK(vertical[row * 8 + column] == expected);
					CHECK(horizontal[column * 4 + row] == expected);
				}
			}
		}
	}
}

/*
 * Makes a segment of eight pixels: a base, a step at the edge, noise of a
 * randomly chosen size on each pixel, clamped to a pixel's range.
 */
static void
make_segment(
	unsigned char px[8])
{
	static const int noise_sizes[6] = {0, 1, 3, 8, 30, 255};
	int base;
	int step;
	int noise;
	int index;
	int value;

	/* A base, and a step across the edge for some segments. */
	base = random_in(0, 255);
	step = 0;
	if (random_in(0, 1) == 1)
		step = random_in(-60, 60);
	noise = noise_sizes[random_in(0, 5)];

	/* Eight pixels with noise; the step goes to the pixels after the edge. */
	for (index = 0; index < 8; index++) {
		value = base + (index >= 4 ? step : 0) + random_in(-noise, noise);
		if (value < 0)
			value = 0;
		if (value > 255)
			value = 255;
		px[index] = (unsigned char)value;
	}
}

/*
 * Random single segments, through each filter, against the model.
 */
static void
test_random_segments(void)
{
	unsigned char original[8];
	unsigned char actual[8];
	unsigned char expected[8];
	unsigned iteration;
	unsigned long changed[3];
	int kind;
	int limit;
	int interior;
	int hev;

	/* How many segments each filter changed, so the test cannot pass without filtering. */
	changed[0] = 0UL;
	changed[1] = 0UL;
	changed[2] = 0UL;

	/* Many segments, with the controls drawn so that both outcomes are common. */
	for (iteration = 0; iteration < 60000U; iteration++) {
		make_segment(original);
		kind = random_in(0, 2);
		limit = random_in(0, 1) == 1 ? random_in(0, 80) : random_in(0, 600);
		interior = random_in(1, 70);
		hev = random_in(0, 4);

		memcpy(actual, original, 8);
		memcpy(expected, original, 8);
		if (kind == KIND_SIMPLE) {
			/* Only p1..q1 exist for the simple filter's buffer in the real use; the copy has them all. */
			webpc_vp8_filter_simple_edge(actual + 4, 1, 8, 1, limit);
		} else if (kind == KIND_MB) {
			webpc_vp8_filter_mb_edge(actual + 4, 1, 8, 1, limit, interior, hev);
		} else {
			webpc_vp8_filter_sub_edge(actual + 4, 1, 8, 1, limit, interior, hev);
		}
		ref_edge(expected + 4, 1, 8, 1, kind, limit, interior, hev);

		CHECK(memcmp(actual, expected, 8) == 0);
		if (memcmp(actual, original, 8) != 0)
			changed[kind]++;
	}

	/* Each filter changed a fair share of the segments. */
	CHECK(changed[KIND_SIMPLE] > 1000UL);
	CHECK(changed[KIND_MB] > 1000UL);
	CHECK(changed[KIND_SUB] > 1000UL);
}

/*
 * Random 16 x 16 images through each filter in both orientations and
 * several counts, against the model.
 */
static void
test_random_images(void)
{
	unsigned char original[16 * 16];
	unsigned char actual[16 * 16];
	unsigned char expected[16 * 16];
	unsigned iteration;
	unsigned count;
	int kind;
	int horizontal;
	int limit;
	int interior;
	int hev;
	int index;
	int base;
	int offset;
	ptrdiff_t across;
	ptrdiff_t along;

	/* Many images. */
	for (iteration = 0; iteration < 4000U; iteration++) {
		/* Smooth noise around a base, so that the filters often apply. */
		base = random_in(20, 230);
		for (index = 0; index < 256; index++)
			original[index] = (unsigned char)(base + random_in(-6, 6) + (index % 16 >= 8 ? random_in(0, 12) : 0));

		kind = random_in(0, 2);
		horizontal = random_in(0, 1);
		limit = random_in(0, 200);
		interior = random_in(1, 30);
		hev = random_in(0, 4);
		count = (unsigned)random_in(0, 8);

		/* A vertical edge at column 8 (across 1) or a horizontal edge at row 8 (across 16). */
		across = 1;
		along = 16;
		offset = 8;
		if (horizontal) {
			across = 16;
			along = 1;
			offset = 8 * 16;
		}

		memcpy(actual, original, sizeof(actual));
		memcpy(expected, original, sizeof(expected));
		if (kind == KIND_SIMPLE)
			webpc_vp8_filter_simple_edge(actual + offset, across, along, count, limit);
		else if (kind == KIND_MB)
			webpc_vp8_filter_mb_edge(actual + offset, across, along, count, limit, interior, hev);
		else
			webpc_vp8_filter_sub_edge(actual + offset, across, along, count, limit, interior, hev);
		ref_edge(expected + offset, across, along, count, kind, limit, interior, hev);
		CHECK(memcmp(actual, expected, sizeof(actual)) == 0);
	}
}

/*
 * Fills a frame of whole macroblocks with smooth noise and a random offset
 * per macroblock, so that macroblock edges differ and both filters apply.
 * mb_size is 16 for luma and 8 for a chroma plane; the frame is
 * width_mbs * mb_size wide.
 */
static void
frame_fill(
	unsigned char *frame,
	unsigned width_mbs,
	unsigned height_mbs,
	unsigned mb_size)
{
	int offsets[9];
	unsigned width;
	unsigned height;
	unsigned x;
	unsigned y;
	unsigned count;
	int value;

	/* One offset per macroblock; the frames in the tests are at most 3 x 3. */
	count = width_mbs * height_mbs;
	for (x = 0; x < count; x++)
		offsets[x] = random_in(-10, 10);

	/* Every pixel: the macroblock's level plus a small noise. */
	width = width_mbs * mb_size;
	height = height_mbs * mb_size;
	for (y = 0; y < height; y++) {
		for (x = 0; x < width; x++) {
			value = 110 + offsets[(y / mb_size) * width_mbs + x / mb_size] + random_in(-3, 3);
			frame[y * width + x] = (unsigned char)value;
		}
	}
}

/*
 * Tells whether two byte ranges are the same.
 */
static int
frames_equal(
	const unsigned char *left,
	const unsigned char *right,
	size_t size)
{
	/* The comparison. */
	return memcmp(left, right, size) == 0;
}

/*
 * Filters macroblocks of a 3 x 3 frame (planes allocated at their exact
 * sizes, so a read outside is an error under the sanitizer) against the
 * model, for each position class, filter type and inner flag.
 */
static void
test_macroblock(void)
{
	unsigned char *y;
	unsigned char *u;
	unsigned char *v;
	unsigned char *y_before;
	unsigned char *u_before;
	unsigned char *v_before;
	unsigned char *ry;
	unsigned char *ru;
	unsigned char *rv;
	struct webpc_vp8_header header;
	struct webpc_vp8_filter_params params;
	unsigned mb_x;
	unsigned mb_y;
	int simple;
	int inner;
	int level;
	ptrdiff_t y_offset;
	ptrdiff_t uv_offset;

	/* The frame and its copies. */
	y = malloc(48U * 48U);
	u = malloc(24U * 24U);
	v = malloc(24U * 24U);
	y_before = malloc(48U * 48U);
	u_before = malloc(24U * 24U);
	v_before = malloc(24U * 24U);
	ry = malloc(48U * 48U);
	ru = malloc(24U * 24U);
	rv = malloc(24U * 24U);
	CHECK(y != NULL && u != NULL && v != NULL && y_before != NULL && u_before != NULL && v_before != NULL);
	CHECK(ry != NULL && ru != NULL && rv != NULL);
	if (y == NULL || u == NULL || v == NULL || y_before == NULL || u_before == NULL || v_before == NULL ||
	    ry == NULL || ru == NULL || rv == NULL)
		goto out;

	/* The positions: the first column, the first row, both, and an inner macroblock. */
	for (level = 0; level < 4; level++) {
		for (mb_y = 0; mb_y < 3U; mb_y++) {
			for (mb_x = 0; mb_x < 3U; mb_x++) {
				for (simple = 0; simple < 2; simple++) {
					for (inner = 0; inner < 2; inner++) {
						frame_fill(y, 3, 3, 16);
						frame_fill(u, 3, 3, 8);
						frame_fill(v, 3, 3, 8);
						memcpy(y_before, y, 48U * 48U);
						memcpy(u_before, u, 24U * 24U);
						memcpy(v_before, v, 24U * 24U);
						memcpy(ry, y, 48U * 48U);
						memcpy(ru, u, 24U * 24U);
						memcpy(rv, v, 24U * 24U);

						/* Controls from the real function, with level 0 and three others. */
						header_clear(&header);
						header.filter.level = level == 0 ? 0 : (level == 1 ? 12 : (level == 2 ? 33 : 63));
						header.filter.sharpness = level == 2 ? 3 : 0;
						webpc_vp8_filter_params(&header, 0, 0, &params);

						y_offset = (ptrdiff_t)(mb_y * 16U * 48U + mb_x * 16U);
						uv_offset = (ptrdiff_t)(mb_y * 8U * 24U + mb_x * 8U);
						webpc_vp8_filter_macroblock(y + y_offset, u + uv_offset, v + uv_offset, 48, 24, mb_x,
							mb_y, &params, simple, inner);
						ref_macroblock(ry + y_offset, ru + uv_offset, rv + uv_offset, 48, 24, mb_x, mb_y,
							&params, simple, inner, 0);

						/* The whole frame matches the model. */
						CHECK(frames_equal(y, ry, 48U * 48U));
						CHECK(frames_equal(u, ru, 24U * 24U));
						CHECK(frames_equal(v, rv, 24U * 24U));

						/* Level 0 changes nothing. */
						if (level == 0) {
							CHECK(frames_equal(y, y_before, 48U * 48U));
							CHECK(frames_equal(u, u_before, 24U * 24U));
							CHECK(frames_equal(v, v_before, 24U * 24U));
						}

						/* The simple filter leaves chroma alone. */
						if (simple) {
							CHECK(frames_equal(u, u_before, 24U * 24U));
							CHECK(frames_equal(v, v_before, 24U * 24U));
						}

						/* An inner macroblock with the normal filter and every edge does change pixels. */
						if (level == 2 && mb_x == 1U && mb_y == 1U && !simple && inner) {
							CHECK(!frames_equal(y, y_before, 48U * 48U));
							CHECK(!frames_equal(u, u_before, 24U * 24U));
							CHECK(!frames_equal(v, v_before, 24U * 24U));
						}
					}
				}
			}
		}
	}

out:
	free(y);
	free(u);
	free(v);
	free(y_before);
	free(u_before);
	free(v_before);
	free(ry);
	free(ru);
	free(rv);
}

/*
 * What each skip leaves untouched: the left edge without mb_x, the top edge
 * without mb_y, the subblock edges without the inner flag (judged by the
 * pixels no other edge reaches), and that the first macroblock of a frame
 * allocated exactly never reads before or after its planes.
 */
static void
test_macroblock_skips(void)
{
	unsigned char *y;
	unsigned char *u;
	unsigned char *v;
	unsigned char *y_before;
	unsigned char *u_before;
	unsigned char *v_before;
	struct webpc_vp8_header header;
	struct webpc_vp8_filter_params params;
	int run;
	int row;
	int column;
	int simple;
	int changed;

	/* The frame, 2 x 2 macroblocks; the macroblock under test is the one at (1, 1). */
	y = malloc(32U * 32U);
	u = malloc(16U * 16U);
	v = malloc(16U * 16U);
	y_before = malloc(32U * 32U);
	u_before = malloc(16U * 16U);
	v_before = malloc(16U * 16U);
	CHECK(y != NULL && u != NULL && v != NULL && y_before != NULL && u_before != NULL && v_before != NULL);
	if (y == NULL || u == NULL || v == NULL || y_before == NULL || u_before == NULL || v_before == NULL)
		goto out;

	/* Strong controls so that edges are filtered where allowed. */
	header_clear(&header);
	header.filter.level = 40;
	webpc_vp8_filter_params(&header, 0, 0, &params);

	/* Four runs: (mb_x, mb_y) of (1, 1), (0, 1), (1, 0) and (1, 1) without the inner edges. */
	for (simple = 0; simple < 2; simple++) {
		for (run = 0; run < 4; run++) {
			frame_fill(y, 2, 2, 16);
			frame_fill(u, 2, 2, 8);
			frame_fill(v, 2, 2, 8);
			memcpy(y_before, y, 32U * 32U);
			memcpy(u_before, u, 16U * 16U);
			memcpy(v_before, v, 16U * 16U);

			/* The same pointers every time; only the flags differ. */
			webpc_vp8_filter_macroblock(y + 16 * 32 + 16, u + 8 * 16 + 8, v + 8 * 16 + 8, 32, 16,
				run == 1 ? 0U : 1U, run == 2 ? 0U : 1U, &params, simple, run == 3 ? 0 : 1);

			/* With mb_x 0 the three pixels before the left edge stay; with mb_x 1 some change. */
			changed = 0;
			for (row = 16; row < 32; row++) {
				for (column = 13; column < 16; column++) {
					if (y[row * 32 + column] != y_before[row * 32 + column])
						changed = 1;
				}
			}
			if (run == 1)
				CHECK(changed == 0);
			else
				CHECK(changed == 1);

			/* With mb_y 0 the three rows above the top edge stay; with mb_y 1 some change. */
			changed = 0;
			for (row = 13; row < 16; row++) {
				for (column = 16; column < 32; column++) {
					if (y[row * 32 + column] != y_before[row * 32 + column])
						changed = 1;
				}
			}
			if (run == 2)
				CHECK(changed == 0);
			else
				CHECK(changed == 1);

			/* Without the inner flag the pixels past the macroblock edges' reach do not change. */
			if (run == 3) {
				for (row = 19; row < 32; row++) {
					for (column = 19; column < 32; column++)
						CHECK(y[row * 32 + column] == y_before[row * 32 + column]);
				}
			}
		}
	}

out:
	free(y);
	free(u);
	free(v);
	free(y_before);
	free(u_before);
	free(v_before);
}

/*
 * The order of the steps matters: on a macroblock where both the
 * vertical and horizontal edges filter strongly, the right order matches
 * the model and the horizontal-first order differs from it.
 */
static void
test_macroblock_order(void)
{
	unsigned char *y;
	unsigned char *u;
	unsigned char *v;
	unsigned char *ry;
	unsigned char *ru;
	unsigned char *rv;
	unsigned char *wy;
	unsigned char *wu;
	unsigned char *wv;
	struct webpc_vp8_header header;
	struct webpc_vp8_filter_params params;
	int trial;
	int differing;

	/* The frame, 2 x 2 macroblocks, three copies. */
	y = malloc(32U * 32U);
	u = malloc(16U * 16U);
	v = malloc(16U * 16U);
	ry = malloc(32U * 32U);
	ru = malloc(16U * 16U);
	rv = malloc(16U * 16U);
	wy = malloc(32U * 32U);
	wu = malloc(16U * 16U);
	wv = malloc(16U * 16U);
	CHECK(y != NULL && u != NULL && v != NULL && ry != NULL && ru != NULL && rv != NULL);
	CHECK(wy != NULL && wu != NULL && wv != NULL);
	if (y == NULL || u == NULL || v == NULL || ry == NULL || ru == NULL || rv == NULL || wy == NULL ||
	    wu == NULL || wv == NULL)
		goto out;

	/* Strong controls. */
	header_clear(&header);
	header.filter.level = 50;
	webpc_vp8_filter_params(&header, 0, 0, &params);

	/* Several fillings: the real order always matches; the wrong order differs at least once. */
	differing = 0;
	for (trial = 0; trial < 20; trial++) {
		frame_fill(y, 2, 2, 16);
		frame_fill(u, 2, 2, 8);
		frame_fill(v, 2, 2, 8);
		memcpy(ry, y, 32U * 32U);
		memcpy(ru, u, 16U * 16U);
		memcpy(rv, v, 16U * 16U);
		memcpy(wy, y, 32U * 32U);
		memcpy(wu, u, 16U * 16U);
		memcpy(wv, v, 16U * 16U);

		webpc_vp8_filter_macroblock(y + 16 * 32 + 16, u + 8 * 16 + 8, v + 8 * 16 + 8, 32, 16, 1, 1, &params, 0, 1);
		ref_macroblock(ry + 16 * 32 + 16, ru + 8 * 16 + 8, rv + 8 * 16 + 8, 32, 16, 1, 1, &params, 0, 1, 0);
		ref_macroblock(wy + 16 * 32 + 16, wu + 8 * 16 + 8, wv + 8 * 16 + 8, 32, 16, 1, 1, &params, 0, 1, 1);

		CHECK(frames_equal(y, ry, 32U * 32U));
		CHECK(frames_equal(u, ru, 16U * 16U));
		CHECK(frames_equal(v, rv, 16U * 16U));
		if (!frames_equal(y, wy, 32U * 32U))
			differing++;
	}

	/* The order shows in the result for most fillings. */
	CHECK(differing >= 15);

out:
	free(y);
	free(u);
	free(v);
	free(ry);
	free(ru);
	free(rv);
	free(wy);
	free(wu);
	free(wv);
}
