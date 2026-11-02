/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The loop filter of the lossy format.
 *
 * Pixels are handled as the values 0..255 they are stored as.  The
 * format's arithmetic is written on the same values shifted by 128 (to
 * -128..127); the shift cancels in every difference and the clamp back to
 * 0..255 is the same as the format's clamp of the shifted value, so no
 * conversion is made here.
 */

#include "vp8-filter.h"

/* The widest sharpness the frame header can carry (RFC 6386 9.4). */
#define FILTER_SHARPNESS_MAX	7

/* The largest filter level (RFC 6386 9.3, 9.6). */
#define FILTER_LEVEL_MAX	63

/*
 * The weights the macroblock edge filter gives to the three pixel pairs
 * beside the edge, the nearest first.
 *
 * Values referenced: RFC 6386 (November 2011) section 15.3 (the
 * multipliers 27, 18 and 9 in MBfilter).
 */
static const int filter_mb_weights[3] = {27, 18, 9};

static int filter_clamp_signed8(int value);
static int filter_clamp_range(int value, int low, int high);
static int filter_add(int32_t base, int32_t addend);
static int filter_absolute(int value);
static int filter_simple_passes(const uint8_t *segment, ptrdiff_t across, int limit);
static int filter_interior_passes(const uint8_t *segment, ptrdiff_t across, int interior);
static int filter_high_variance(const uint8_t *segment, ptrdiff_t across, int threshold);
static int filter_adjust_common(uint8_t *segment, ptrdiff_t across, int outer_taps);
static void filter_adjust_wide(uint8_t *segment, ptrdiff_t across);
static void filter_params_fill(int level, int sharpness, struct webpc_vp8_filter_params *params);
static void filter_macroblock_simple(uint8_t *y, ptrdiff_t y_stride, unsigned mb_x, unsigned mb_y, const struct webpc_vp8_filter_params *params, int filter_inner);
static void filter_macroblock_normal(uint8_t *y, uint8_t *u, uint8_t *v, ptrdiff_t y_stride, ptrdiff_t uv_stride, unsigned mb_x, unsigned mb_y, const struct webpc_vp8_filter_params *params, int filter_inner);

/*
 * Works out a key-frame macroblock's controls: the level starts as the
 * header's, is replaced by (absolute) or added to (delta) the segment's
 * filter_level when segmentation is enabled, and is clamped to 0..63; when
 * deltas_enabled, ref_delta[0] (intra frame) is added, and mode_delta[0]
 * too for a B_PRED macroblock, and it is clamped to 0..63 again.
 *
 * Procedure referenced: RFC 6386 (November 2011) sections 15.4 and 20.6
 * (calculate_filter_parameters; the reference decoder's clamp between the
 * segment and the deltas is the one that holds).
 *
 * A segment above 3 with segmentation enabled has no value to use; the
 * macroblock gets level 0, which is not filtered.
 */
void
webpc_vp8_filter_params(
	const struct webpc_vp8_header *header,
	unsigned segment,
	int b_pred,
	struct webpc_vp8_filter_params *params)
{
	int level;
	int sharpness;

	/* Without a place to put the controls there is nothing to do. */
	if (params == NULL)
		return;

	/* Without a header the macroblock is not filtered. */
	if (header == NULL) {
		filter_params_fill(0, 0, params);
		return;
	}

	/* The sharpness is kept to the 3 bits the header has. */
	sharpness = filter_clamp_range(header->filter.sharpness, 0, FILTER_SHARPNESS_MAX);

	/* The level the whole frame starts from. */
	level = header->filter.level;

	/*
	 * Segmentation overrides the level of the macroblock's segment, or
	 * adjusts it.  A segment number the frame cannot have is not
	 * filtered.
	 */
	if (header->segmentation.enabled != 0) {
		if (segment >= WEBPC_VP8_SEGMENTS) {
			filter_params_fill(0, sharpness, params);
			return;
		}
		if (header->segmentation.absolute != 0)
			level = header->segmentation.filter_level[segment];
		else
			level = filter_add(level, header->segmentation.filter_level[segment]);
	}

	/* The first clamp, before the deltas are added. */
	level = filter_clamp_range(level, 0, FILTER_LEVEL_MAX);

	/*
	 * The deltas of the frame header: an intra frame's reference delta
	 * for every macroblock, and the first mode delta for a macroblock
	 * predicted subblock by subblock.
	 */
	if (header->filter.deltas_enabled != 0) {
		level = filter_add(level, header->filter.ref_delta[0]);
		if (b_pred != 0)
			level = filter_add(level, header->filter.mode_delta[0]);
	}

	/* The second clamp: the level the macroblock is filtered with. */
	level = filter_clamp_range(level, 0, FILTER_LEVEL_MAX);

	/* The other controls follow from the level. */
	filter_params_fill(level, sharpness, params);
}

/*
 * Filters count segments straddling one edge with the simple filter: pixel
 * q0 of the first segment is at edge (the first pixel after the edge), p0
 * at edge - across, and so on; the next segment is along further.  A
 * segment is filtered when |p0 - q0| * 2 + (|p1 - q1| >> 1) <= limit.
 *
 * Procedure referenced: RFC 6386 (November 2011) sections 15.2
 * (simple_segment, common_adjust) and 20.6 (filter_v_edge_simple,
 * filter_h_edge_simple, filter_common).
 */
void
webpc_vp8_filter_simple_edge(
	uint8_t *edge,
	ptrdiff_t across,
	ptrdiff_t along,
	unsigned count,
	int limit)
{
	uint8_t *segment;
	unsigned index;
	int passes;

	/* Without pixels there is nothing to filter. */
	if (edge == NULL)
		return;

	/* Each segment of the edge is tested and adjusted on its own. */
	for (index = 0; index < count; index++) {
		segment = edge + (ptrdiff_t)index * along;

		/* A segment whose edge difference is above the limit is left as it is. */
		passes = filter_simple_passes(segment, across, limit);
		if (passes == 0)
			continue;

		/* The simple filter always uses the outer taps. */
		filter_adjust_common(segment, across, 1);
	}
}

/*
 * Filters count segments straddling a macroblock edge with the normal
 * filter: pixels as for the simple filter, p3..q3.  limit is the combined
 * edge limit itself.  A segment is filtered when the simple test passes
 * with limit and the six interior differences are at most interior.  With
 * high edge variance only p0 and q0 change; otherwise the weights 27, 18
 * and 9 change p2..q2.
 *
 * Procedure referenced: RFC 6386 (November 2011) sections 15.3
 * (filter_yes, hev, MBfilter) and 20.6 (filter_mb_v_edge,
 * filter_mb_h_edge, filter_mb_edge).
 */
void
webpc_vp8_filter_mb_edge(
	uint8_t *edge,
	ptrdiff_t across,
	ptrdiff_t along,
	unsigned count,
	int limit,
	int interior,
	int hev)
{
	uint8_t *segment;
	unsigned index;
	int passes;
	int variance;

	/* Without pixels there is nothing to filter. */
	if (edge == NULL)
		return;

	/* Each segment of the edge is tested and adjusted on its own. */
	for (index = 0; index < count; index++) {
		segment = edge + (ptrdiff_t)index * along;

		/* A segment whose edge difference is above the limit is left as it is. */
		passes = filter_simple_passes(segment, across, limit);
		if (passes == 0)
			continue;

		/* A segment that is not smooth on each side of the edge is a real edge, and is left. */
		passes = filter_interior_passes(segment, across, interior);
		if (passes == 0)
			continue;

		/* High edge variance means only the two pixels at the edge may move. */
		variance = filter_high_variance(segment, across, hev);
		if (variance != 0) {
			filter_adjust_common(segment, across, 1);
			continue;
		}

		/* Otherwise the change is spread over three pixels on each side. */
		filter_adjust_wide(segment, across);
	}
}

/*
 * Filters count segments straddling a subblock edge with the normal
 * filter: limit is the subblock edge limit itself; the tests as
 * webpc_vp8_filter_mb_edge; then the common adjustment, using the outer
 * taps only with high edge variance, and without it also p1 and q1 change
 * by (f1 + 1) >> 1, where f1 is the amount q0 moved.
 *
 * Procedure referenced: RFC 6386 (November 2011) sections 15.3
 * (subblock_filter) and 20.6 (filter_subblock_v_edge,
 * filter_subblock_h_edge, filter_common).
 */
void
webpc_vp8_filter_sub_edge(
	uint8_t *edge,
	ptrdiff_t across,
	ptrdiff_t along,
	unsigned count,
	int limit,
	int interior,
	int hev)
{
	uint8_t *segment;
	unsigned index;
	int passes;
	int variance;
	int moved;
	int inner_amount;
	int p1;
	int q1;

	/* Without pixels there is nothing to filter. */
	if (edge == NULL)
		return;

	/* Each segment of the edge is tested and adjusted on its own. */
	for (index = 0; index < count; index++) {
		segment = edge + (ptrdiff_t)index * along;

		/* A segment whose edge difference is above the limit is left as it is. */
		passes = filter_simple_passes(segment, across, limit);
		if (passes == 0)
			continue;

		/* A segment that is not smooth on each side of the edge is a real edge, and is left. */
		passes = filter_interior_passes(segment, across, interior);
		if (passes == 0)
			continue;

		/* The two edge pixels move; the outer taps count only with high edge variance. */
		variance = filter_high_variance(segment, across, hev);
		moved = filter_adjust_common(segment, across, variance);

		/* With high edge variance the pixels one step in stay as they are. */
		if (variance != 0)
			continue;

		/* The pixels one step in move by about half of what q0 moved. */
		inner_amount = webpc_asr(moved + 1, 1);
		p1 = segment[-2 * across];
		q1 = segment[across];
		segment[-2 * across] = (uint8_t)webpc_clamp255(p1 + inner_amount);
		segment[across] = (uint8_t)webpc_clamp255(q1 - inner_amount);
	}
}

/*
 * Filters one macroblock of a frame whose every macroblock is
 * reconstructed (the four steps, in order): y, u and v point at the
 * macroblock's top-left pixel in each plane.  Nothing is done when the
 * level is 0.  The left edge is filtered only when mb_x > 0, the top one
 * only when mb_y > 0, the subblock edges only when filter_inner.  simple
 * selects the simple filter, which filters luma only.
 *
 * Procedure referenced: RFC 6386 (November 2011) sections 15.1 (the
 * four steps and the skip of the inner edges) and 20.6 (filter_row_normal,
 * filter_row_simple).
 */
void
webpc_vp8_filter_macroblock(
	uint8_t *y,
	uint8_t *u,
	uint8_t *v,
	ptrdiff_t y_stride,
	ptrdiff_t uv_stride,
	unsigned mb_x,
	unsigned mb_y,
	const struct webpc_vp8_filter_params *params,
	int simple,
	int filter_inner)
{
	/* Without pixels or controls there is nothing to filter. */
	if (y == NULL)
		return;
	if (params == NULL)
		return;

	/* A level of 0 means the macroblock is skipped entirely. */
	if (params->level == 0)
		return;

	/* The simple filter touches luma only. */
	if (simple != 0) {
		filter_macroblock_simple(y, y_stride, mb_x, mb_y, params, filter_inner);
		return;
	}

	/* The normal filter needs the chroma planes as well. */
	if (u == NULL)
		return;
	if (v == NULL)
		return;
	filter_macroblock_normal(y, u, v, y_stride, uv_stride, mb_x, mb_y, params, filter_inner);
}

/*
 * Clamps a value to the signed 8-bit range, the c() of the format.
 */
static int
filter_clamp_signed8(
	int value)
{
	/* Below the range. */
	if (value < -128)
		return -128;

	/* Above the range. */
	if (value > 127)
		return 127;

	/* Within the range: the value itself. */
	return value;
}

/*
 * Clamps a value to low..high.
 */
static int
filter_clamp_range(
	int value,
	int low,
	int high)
{
	/* Below the range. */
	if (value < low)
		return low;

	/* Above the range. */
	if (value > high)
		return high;

	/* Within the range: the value itself. */
	return value;
}

/*
 * Adds a header delta to a level without overflowing: a sum beyond the
 * int32_t range is held at its end, which the clamp that follows treats the
 * same as the exact sum.
 */
static int
filter_add(
	int32_t base,
	int32_t addend)
{
	int32_t sum;

	/* A sum that would pass the top is held at the top. */
	if (addend > 0 && base > INT32_MAX - addend)
		return INT32_MAX;

	/* A sum that would pass the bottom is held at the bottom. */
	if (addend < 0 && base < INT32_MIN - addend)
		return INT32_MIN;

	/* In range: the exact sum. */
	sum = base + addend;
	return (int)sum;
}

/*
 * Gives the absolute value of a pixel difference.
 */
static int
filter_absolute(
	int value)
{
	/* A negative difference is negated; a pixel difference is at most 255 in size. */
	if (value < 0)
		return -value;

	/* Not negative: the value itself. */
	return value;
}

/*
 * Tells whether the four pixels straddling the edge are close enough to
 * filter: |p0 - q0| * 2 + (|p1 - q1| >> 1) <= limit.
 *
 * Procedure referenced: RFC 6386 (November 2011) sections 15.2
 * (simple_segment) and 20.6 (simple_threshold).
 */
static int
filter_simple_passes(
	const uint8_t *segment,
	ptrdiff_t across,
	int limit)
{
	int p1;
	int p0;
	int q0;
	int q1;
	int measure;

	/* The four pixels nearest the edge. */
	p1 = segment[-2 * across];
	p0 = segment[-across];
	q0 = segment[0];
	q1 = segment[across];

	/* The edge difference counts double, the one a step in half. */
	measure = filter_absolute(p0 - q0) * 2 + (filter_absolute(p1 - q1) >> 1);
	if (measure > limit)
		return 0;

	/* Succeeded: the edge is within the limit. */
	return 1;
}

/*
 * Tells whether the pixels on each side of the edge are smooth: no
 * difference between neighbours within p3..p0 or q0..q3 is above interior.
 *
 * Procedure referenced: RFC 6386 (November 2011) sections 15.3
 * (filter_yes) and 20.6 (normal_threshold).
 */
static int
filter_interior_passes(
	const uint8_t *segment,
	ptrdiff_t across,
	int interior)
{
	int pixels[8];
	int index;
	int difference;

	/* The eight pixels p3 p2 p1 p0 q0 q1 q2 q3 in order. */
	for (index = 0; index < 8; index++)
		pixels[index] = segment[(index - 4) * across];

	/* The three neighbour differences before the edge. */
	for (index = 0; index < 3; index++) {
		difference = filter_absolute(pixels[index] - pixels[index + 1]);
		if (difference > interior)
			return 0;
	}

	/* The three neighbour differences after the edge. */
	for (index = 4; index < 7; index++) {
		difference = filter_absolute(pixels[index] - pixels[index + 1]);
		if (difference > interior)
			return 0;
	}

	/* Succeeded: both sides are smooth. */
	return 1;
}

/*
 * Tells whether the edge has high edge variance: |p1 - p0| or |q1 - q0|
 * is above the threshold.
 *
 * Procedure referenced: RFC 6386 (November 2011) sections 15.3 (hev) and
 * 20.6 (high_edge_variance).
 */
static int
filter_high_variance(
	const uint8_t *segment,
	ptrdiff_t across,
	int threshold)
{
	int p1;
	int p0;
	int q0;
	int q1;
	int difference;

	/* The two pixels on each side nearest the edge. */
	p1 = segment[-2 * across];
	p0 = segment[-across];
	q0 = segment[0];
	q1 = segment[across];

	/* A step before the edge above the threshold. */
	difference = filter_absolute(p1 - p0);
	if (difference > threshold)
		return 1;

	/* A step after the edge above the threshold. */
	difference = filter_absolute(q1 - q0);
	if (difference > threshold)
		return 1;

	/* Neither side steps: the variance is low. */
	return 0;
}

/*
 * Moves p0 and q0 of one segment toward each other and gives the amount q0
 * moved (the format's f1, which the subblock filter halves for p1 and q1).
 * The outer taps (p1 - q1) are part of the difference only when outer_taps
 * is not 0.
 *
 * Procedure referenced: RFC 6386 (November 2011) sections 15.2
 * (common_adjust) and 20.6 (filter_common).
 */
static int
filter_adjust_common(
	uint8_t *segment,
	ptrdiff_t across,
	int outer_taps)
{
	int p1;
	int p0;
	int q0;
	int q1;
	int difference;
	int amount_q;
	int amount_p;

	/* The four pixels nearest the edge. */
	p1 = segment[-2 * across];
	p0 = segment[-across];
	q0 = segment[0];
	q1 = segment[across];

	/* About twice the edge difference with the outer taps, three times it without. */
	difference = 3 * (q0 - p0);
	if (outer_taps != 0)
		difference += filter_clamp_signed8(p1 - q1);
	difference = filter_clamp_signed8(difference);

	/*
	 * The amount q0 moves is the difference over 8 rounded up from a
	 * half; p0 moves by that amount rounded the other way, so a half
	 * is shared by the two.
	 */
	amount_q = webpc_asr(filter_clamp_signed8(difference + 4), 3);
	amount_p = webpc_asr(filter_clamp_signed8(difference + 3), 3);

	/* The two edge pixels move toward each other, held to a pixel's range. */
	segment[-across] = (uint8_t)webpc_clamp255(p0 + amount_p);
	segment[0] = (uint8_t)webpc_clamp255(q0 - amount_q);

	/* Reports how far q0 moved. */
	return amount_q;
}

/*
 * Moves three pixels on each side of the edge toward the other side by
 * 27, 18 and 9 parts in about 128 of the edge difference, the nearest pair
 * the most.
 *
 * Procedure referenced: RFC 6386 (November 2011) sections 15.3 (MBfilter)
 * and 20.6 (filter_mb_edge).
 */
static void
filter_adjust_wide(
	uint8_t *segment,
	ptrdiff_t across)
{
	int p1;
	int p0;
	int q0;
	int q1;
	int edge_difference;
	int amount;
	int index;
	ptrdiff_t before;
	ptrdiff_t after;

	/* The four pixels nearest the edge give the edge difference. */
	p1 = segment[-2 * across];
	p0 = segment[-across];
	q0 = segment[0];
	q1 = segment[across];
	edge_difference = filter_clamp_signed8(filter_clamp_signed8(p1 - q1) + 3 * (q0 - p0));

	/* Each pair, from the edge outward, moves by its share of the difference. */
	for (index = 0; index < 3; index++) {
		amount = filter_clamp_signed8(webpc_asr(filter_mb_weights[index] * edge_difference + 63, 7));
		before = -(ptrdiff_t)(index + 1) * across;
		after = (ptrdiff_t)index * across;
		p0 = segment[before];
		q0 = segment[after];
		segment[before] = (uint8_t)webpc_clamp255(p0 + amount);
		segment[after] = (uint8_t)webpc_clamp255(q0 - amount);
	}
}

/*
 * Fills the controls that follow from a level and the sharpness.
 *
 * Procedure referenced: RFC 6386 (November 2011) section 15.4 (the
 * interior limit, the high edge variance threshold and the two edge
 * limits for a key frame) and 20.6 (calculate_filter_parameters).
 */
static void
filter_params_fill(
	int level,
	int sharpness,
	struct webpc_vp8_filter_params *params)
{
	int interior;
	int hev;
	int cap;

	/* The interior limit starts at the level. */
	interior = level;

	/* Sharpness narrows it: shifted down, and held at most 9 - sharpness. */
	if (sharpness != 0) {
		if (sharpness > 4)
			interior = webpc_asr(interior, 2);
		else
			interior = webpc_asr(interior, 1);
		cap = 9 - sharpness;
		if (interior > cap)
			interior = cap;
	}

	/* The limit is never below 1. */
	if (interior < 1)
		interior = 1;

	/* A key frame's high edge variance threshold steps up at levels 15 and 40. */
	hev = 0;
	if (level >= 40)
		hev = 2;
	else if (level >= 15)
		hev = 1;

	/* The controls, with the two edge limits the interior limit adds to. */
	params->level = level;
	params->interior = interior;
	params->hev = hev;
	params->mb_limit = (level + 2) * 2 + interior;
	params->sub_limit = level * 2 + interior;
}

/*
 * Filters one macroblock's luma with the simple filter: the four steps of
 * the order, with the subblock edges only when filter_inner.
 */
static void
filter_macroblock_simple(
	uint8_t *y,
	ptrdiff_t y_stride,
	unsigned mb_x,
	unsigned mb_y,
	const struct webpc_vp8_filter_params *params,
	int filter_inner)
{
	int offset;

	/* Step 1: the left macroblock edge, unless the macroblock is in the first column. */
	if (mb_x > 0)
		webpc_vp8_filter_simple_edge(y, 1, y_stride, 16, params->mb_limit);

	/* Step 2: the three vertical subblock edges of luma. */
	if (filter_inner != 0) {
		/* The edges at 4, 8 and 12 pixels from the left. */
		for (offset = 4; offset < 16; offset += 4)
			webpc_vp8_filter_simple_edge(y + offset, 1, y_stride, 16, params->sub_limit);
	}

	/* Step 3: the top macroblock edge, unless the macroblock is in the first row. */
	if (mb_y > 0)
		webpc_vp8_filter_simple_edge(y, y_stride, 1, 16, params->mb_limit);

	/* Step 4: the three horizontal subblock edges of luma. */
	if (filter_inner != 0) {
		/* The edges at 4, 8 and 12 rows from the top. */
		for (offset = 4; offset < 16; offset += 4) {
			webpc_vp8_filter_simple_edge(y + offset * y_stride, y_stride, 1, 16,
				params->sub_limit);
		}
	}
}

/*
 * Filters one macroblock's three planes with the normal filter: the four
 * steps of the order, with the subblock edges only when filter_inner.
 */
static void
filter_macroblock_normal(
	uint8_t *y,
	uint8_t *u,
	uint8_t *v,
	ptrdiff_t y_stride,
	ptrdiff_t uv_stride,
	unsigned mb_x,
	unsigned mb_y,
	const struct webpc_vp8_filter_params *params,
	int filter_inner)
{
	int offset;

	/* Step 1: the left macroblock edge of the three planes, unless in the first column. */
	if (mb_x > 0) {
		webpc_vp8_filter_mb_edge(y, 1, y_stride, 16, params->mb_limit, params->interior, params->hev);
		webpc_vp8_filter_mb_edge(u, 1, uv_stride, 8, params->mb_limit, params->interior, params->hev);
		webpc_vp8_filter_mb_edge(v, 1, uv_stride, 8, params->mb_limit, params->interior, params->hev);
	}

	/* Step 2: the vertical subblock edges: three in luma, the middle one in each chroma plane. */
	if (filter_inner != 0) {
		/* The luma edges at 4, 8 and 12 pixels from the left. */
		for (offset = 4; offset < 16; offset += 4) {
			webpc_vp8_filter_sub_edge(y + offset, 1, y_stride, 16, params->sub_limit,
				params->interior, params->hev);
		}
		webpc_vp8_filter_sub_edge(u + 4, 1, uv_stride, 8, params->sub_limit, params->interior, params->hev);
		webpc_vp8_filter_sub_edge(v + 4, 1, uv_stride, 8, params->sub_limit, params->interior, params->hev);
	}

	/* Step 3: the top macroblock edge of the three planes, unless in the first row. */
	if (mb_y > 0) {
		webpc_vp8_filter_mb_edge(y, y_stride, 1, 16, params->mb_limit, params->interior, params->hev);
		webpc_vp8_filter_mb_edge(u, uv_stride, 1, 8, params->mb_limit, params->interior, params->hev);
		webpc_vp8_filter_mb_edge(v, uv_stride, 1, 8, params->mb_limit, params->interior, params->hev);
	}

	/* Step 4: the horizontal subblock edges, as step 2 for the rows. */
	if (filter_inner != 0) {
		/* The luma edges at 4, 8 and 12 rows from the top. */
		for (offset = 4; offset < 16; offset += 4) {
			webpc_vp8_filter_sub_edge(y + offset * y_stride, y_stride, 1, 16, params->sub_limit,
				params->interior, params->hev);
		}
		webpc_vp8_filter_sub_edge(u + 4 * uv_stride, uv_stride, 1, 8, params->sub_limit, params->interior,
			params->hev);
		webpc_vp8_filter_sub_edge(v + 4 * uv_stride, uv_stride, 1, 8, params->sub_limit, params->interior,
			params->hev);
	}
}
