/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The unit test of vec-gradient: the CSS and SVG stop fixups against the
 * worked examples of CSS Images 3 section 3.4.3, the span mappings, the
 * color at a position (premultiplied and straight, hints, hard stops, the
 * ends), the average color, the ramp, the parameter t of linear, radial
 * and conic gradients through a matrix, the spread methods, the ramp
 * lookup, the line length, and a random run under the sanitizers.  Every
 * expected value is a hand calculation or follows from the formulas of the
 * specifications; no other implementation's output is used.
 */

#include "check.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>

#include "vector/vec-gradient.h"

/* The colors of the examples. */
#define RED		0xffff0000U
#define WHITE		0xffffffffU
#define BLUE		0xff0000ffU
#define BLACK		0xff000000U
#define GREEN		0xff008000U
#define TRANSPARENT	0x00000000U

/* The number of stops of the random runs, at most. */
#define RANDOM_STOPS	16

/* The random runs' counts. */
#define RANDOM_GRADIENTS	3000
#define RANDOM_POINTS		16

/* Makes a stop with an offset that is given or not. */
static struct vec_stop
make_stop(
	float offset,
	uint32_t color,
	int has_offset)
{
	struct vec_stop stop;

	stop.offset = offset;
	stop.color = color;
	stop.has_offset = has_offset;
	stop.is_hint = 0;
	stop.has_hint = 0;
	stop.hint = 0.0f;
	return stop;
}

/* Makes a CSS transition hint entry. */
static struct vec_stop
make_hint(
	float offset)
{
	struct vec_stop stop;

	stop = make_stop(offset, 0, 1);
	stop.is_hint = 1;
	return stop;
}

/* Makes a stop that already has its place and a hint after it. */
static struct vec_stop
make_stop_hint(
	float offset,
	uint32_t color,
	float hint)
{
	struct vec_stop stop;

	stop = make_stop(offset, color, 1);
	stop.has_hint = 1;
	stop.hint = hint;
	return stop;
}

/* Makes a linear gradient over the given stops with the identity matrix. */
static struct vec_gradient
make_linear(
	const struct vec_stop *stops,
	size_t count,
	enum vec_spread spread,
	int premultiplied,
	float x0,
	float y0,
	float x1,
	float y1)
{
	struct vec_gradient gradient;

	memset(&gradient, 0, sizeof(gradient));
	gradient.kind = VEC_GRADIENT_LINEAR;
	gradient.spread = spread;
	gradient.premultiplied = premultiplied;
	gradient.stops = stops;
	gradient.count = count;
	vec_matrix_identity(&gradient.to_gradient);
	gradient.start.x = x0;
	gradient.start.y = y0;
	gradient.end.x = x1;
	gradient.end.y = y1;
	return gradient;
}

/* Makes a radial gradient with the identity matrix (the stops are filled in by the caller when needed). */
static struct vec_gradient
make_radial(
	float focal_x,
	float focal_y,
	float focal_radius,
	float center_x,
	float center_y,
	float radius)
{
	struct vec_gradient gradient;

	memset(&gradient, 0, sizeof(gradient));
	gradient.kind = VEC_GRADIENT_RADIAL;
	gradient.spread = VEC_SPREAD_PAD;
	gradient.premultiplied = 1;
	vec_matrix_identity(&gradient.to_gradient);
	gradient.focal.x = focal_x;
	gradient.focal.y = focal_y;
	gradient.focal_radius = focal_radius;
	gradient.center.x = center_x;
	gradient.center.y = center_y;
	gradient.radius = radius;
	return gradient;
}

/* Makes a conic gradient with the identity matrix. */
static struct vec_gradient
make_conic(
	float center_x,
	float center_y,
	float start_angle)
{
	struct vec_gradient gradient;

	memset(&gradient, 0, sizeof(gradient));
	gradient.kind = VEC_GRADIENT_CONIC;
	gradient.spread = VEC_SPREAD_REPEAT;
	gradient.premultiplied = 1;
	vec_matrix_identity(&gradient.to_gradient);
	gradient.center.x = center_x;
	gradient.center.y = center_y;
	gradient.start_angle = start_angle;
	return gradient;
}

/* Checks the offsets of a list of stops. */
static void
check_offsets(
	const struct vec_stop *stops,
	size_t count,
	const double *expected)
{
	size_t i;

	for (i = 0; i < count; i++) {
		CHECK_NEAR(stops[i].offset, expected[i], 1e-6);
		CHECK(stops[i].has_offset == 1);
		CHECK(stops[i].is_hint == 0);
	}
}

/* Tests the CSS fixup: the worked examples of CSS Images 3 section 3.4.3 on a line of 100px. */
static void
test_fixup_css(void)
{
	struct vec_stop stops[8];
	size_t count;
	int error;
	double expected1[] = { 0.0, 0.2, 1.0 };
	double expected2[] = { 0.4, 0.6, 0.8, 1.0 };
	double expected3[] = { -0.5, 0.25, 1.0 };
	double expected5[] = { 0.2, 0.2, 0.4 };
	double expected6[] = { 0.0, 0.0, 1.5, 1.5 };
	double expected7[] = { 0.8, 0.8, 0.9, 1.0 };

	/* Example 1: linear-gradient(red, white 20%, blue). */
	stops[0] = make_stop(0.0f, RED, 0);
	stops[1] = make_stop(0.2f, WHITE, 1);
	stops[2] = make_stop(0.0f, BLUE, 0);
	count = 3;
	error = vec_gradient_fixup_css(stops, &count);
	CHECK_LONG(error, 0);
	CHECK_LONG(count, 3);
	check_offsets(stops, 3, expected1);
	CHECK(stops[0].color == RED && stops[1].color == WHITE && stops[2].color == BLUE);

	/* Example 2: red 40%, white, black, blue: step 3 spreads the middle two evenly. */
	stops[0] = make_stop(0.4f, RED, 1);
	stops[1] = make_stop(0.0f, WHITE, 0);
	stops[2] = make_stop(0.0f, BLACK, 0);
	stops[3] = make_stop(0.0f, BLUE, 0);
	count = 4;
	error = vec_gradient_fixup_css(stops, &count);
	CHECK_LONG(error, 0);
	CHECK_LONG(count, 4);
	check_offsets(stops, 4, expected2);

	/* Example 3: red -50%, white, blue: white halfway between -50% and 100%. */
	stops[0] = make_stop(-0.5f, RED, 1);
	stops[1] = make_stop(0.0f, WHITE, 0);
	stops[2] = make_stop(0.0f, BLUE, 0);
	count = 3;
	error = vec_gradient_fixup_css(stops, &count);
	CHECK_LONG(error, 0);
	check_offsets(stops, 3, expected3);

	/* Example 5: red 20px, white 0px, blue 40px: white is lifted to 20px. */
	stops[0] = make_stop(0.2f, RED, 1);
	stops[1] = make_stop(0.0f, WHITE, 1);
	stops[2] = make_stop(0.4f, BLUE, 1);
	count = 3;
	error = vec_gradient_fixup_css(stops, &count);
	CHECK_LONG(error, 0);
	check_offsets(stops, 3, expected5);

	/* Example 6: red, white -50%, black 150%, blue: the first is 0, white is lifted to it, the last is 100% lifted to 150%. */
	stops[0] = make_stop(0.0f, RED, 0);
	stops[1] = make_stop(-0.5f, WHITE, 1);
	stops[2] = make_stop(1.5f, BLACK, 1);
	stops[3] = make_stop(0.0f, BLUE, 0);
	count = 4;
	error = vec_gradient_fixup_css(stops, &count);
	CHECK_LONG(error, 0);
	check_offsets(stops, 4, expected6);

	/* Example 7: red 80px, white 0px, black, blue 100px: white is lifted to 80, black halfway to 100. */
	stops[0] = make_stop(0.8f, RED, 1);
	stops[1] = make_stop(0.0f, WHITE, 1);
	stops[2] = make_stop(0.0f, BLACK, 0);
	stops[3] = make_stop(1.0f, BLUE, 1);
	count = 4;
	error = vec_gradient_fixup_css(stops, &count);
	CHECK_LONG(error, 0);
	check_offsets(stops, 4, expected7);

	/* A single stop without a position is at 0. */
	stops[0] = make_stop(0.0f, RED, 0);
	count = 1;
	error = vec_gradient_fixup_css(stops, &count);
	CHECK_LONG(error, 0);
	CHECK_LONG(count, 1);
	CHECK_NEAR(stops[0].offset, 0.0, 0.0);
}

/* Tests the CSS fixup of transition hints. */
static void
test_fixup_hints(void)
{
	struct vec_stop stops[8];
	size_t count;
	int error;

	/* red, 25%, blue: the hint folds into red as the fraction 0.25 of the way to blue. */
	stops[0] = make_stop(0.0f, RED, 1);
	stops[1] = make_hint(0.25f);
	stops[2] = make_stop(1.0f, BLUE, 1);
	count = 3;
	error = vec_gradient_fixup_css(stops, &count);
	CHECK_LONG(error, 0);
	CHECK_LONG(count, 2);
	CHECK(stops[0].has_hint == 1);
	CHECK_NEAR(stops[0].hint, 0.25, 1e-6);
	CHECK(stops[0].color == RED && stops[1].color == BLUE);
	CHECK(stops[1].has_hint == 0);
	CHECK(stops[1].is_hint == 0);

	/* red 50%, 20%, blue: the hint is lifted to 50% (step 2), so it sits on red: H = 0, a hard transition after red. */
	stops[0] = make_stop(0.5f, RED, 1);
	stops[1] = make_hint(0.2f);
	stops[2] = make_stop(0.0f, BLUE, 0);
	count = 3;
	error = vec_gradient_fixup_css(stops, &count);
	CHECK_LONG(error, 0);
	CHECK_LONG(count, 2);
	CHECK_NEAR(stops[0].offset, 0.5, 1e-6);
	CHECK_NEAR(stops[1].offset, 1.0, 1e-6);
	CHECK(stops[0].has_hint == 1);
	CHECK_NEAR(stops[0].hint, 0.0, 1e-6);

	/* red 50%, 50% hint, blue 50%: the two stops share a position, so no hint remains. */
	stops[0] = make_stop(0.5f, RED, 1);
	stops[1] = make_hint(0.5f);
	stops[2] = make_stop(0.5f, BLUE, 1);
	count = 3;
	error = vec_gradient_fixup_css(stops, &count);
	CHECK_LONG(error, 0);
	CHECK_LONG(count, 2);
	CHECK(stops[0].has_hint == 0);

	/* red, 30%, white, blue: white lies between the hint (30%) and blue (100%), at 65%; red's hint is 0.3 / 0.65. */
	stops[0] = make_stop(0.0f, RED, 0);
	stops[1] = make_hint(0.3f);
	stops[2] = make_stop(0.0f, WHITE, 0);
	stops[3] = make_stop(0.0f, BLUE, 0);
	count = 4;
	error = vec_gradient_fixup_css(stops, &count);
	CHECK_LONG(error, 0);
	CHECK_LONG(count, 3);
	CHECK_NEAR(stops[0].offset, 0.0, 1e-6);
	CHECK_NEAR(stops[1].offset, 0.65, 1e-6);
	CHECK_NEAR(stops[2].offset, 1.0, 1e-6);
	CHECK(stops[0].has_hint == 1);
	CHECK_NEAR(stops[0].hint, 0.3 / 0.65, 1e-6);
	CHECK(stops[1].has_hint == 0);

	/* Two hints, in different gaps. */
	stops[0] = make_stop(0.0f, RED, 1);
	stops[1] = make_hint(0.1f);
	stops[2] = make_stop(0.4f, WHITE, 1);
	stops[3] = make_hint(0.7f);
	stops[4] = make_stop(1.0f, BLUE, 1);
	count = 5;
	error = vec_gradient_fixup_css(stops, &count);
	CHECK_LONG(error, 0);
	CHECK_LONG(count, 3);
	CHECK_NEAR(stops[0].hint, 0.25, 1e-6);
	CHECK_NEAR(stops[1].hint, 0.5, 1e-6);
	CHECK(stops[0].has_hint == 1 && stops[1].has_hint == 1 && stops[2].has_hint == 0);
	CHECK(stops[1].color == WHITE && stops[2].color == BLUE);
}

/* Tests the CSS fixup's refusals, which leave the entries and the count unchanged. */
static void
test_fixup_errors(void)
{
	struct vec_stop stops[VEC_GRADIENT_STOPS_MAX + 1];
	struct vec_stop saved[4];
	size_t count;
	size_t i;
	int error;

	/* NULL arguments. */
	count = 2;
	CHECK_LONG(vec_gradient_fixup_css(NULL, &count), EINVAL);
	stops[0] = make_stop(0.0f, RED, 1);
	CHECK_LONG(vec_gradient_fixup_css(stops, NULL), EINVAL);

	/* A count of 0 and one beyond the maximum. */
	count = 0;
	CHECK_LONG(vec_gradient_fixup_css(stops, &count), EINVAL);
	CHECK_LONG(count, 0);
	for (i = 0; i < VEC_GRADIENT_STOPS_MAX + 1; i++)
		stops[i] = make_stop(0.0f, RED, 1);
	count = VEC_GRADIENT_STOPS_MAX + 1;
	CHECK_LONG(vec_gradient_fixup_css(stops, &count), EINVAL);
	CHECK_LONG(count, VEC_GRADIENT_STOPS_MAX + 1);

	/* The maximum itself is taken. */
	count = VEC_GRADIENT_STOPS_MAX;
	CHECK_LONG(vec_gradient_fixup_css(stops, &count), 0);
	CHECK_LONG(count, VEC_GRADIENT_STOPS_MAX);

	/* A hint first, last, next to another hint, without a position, or an offset that is not finite. */
	stops[0] = make_hint(0.1f);
	stops[1] = make_stop(0.5f, RED, 1);
	stops[2] = make_stop(1.0f, BLUE, 1);
	count = 3;
	CHECK_LONG(vec_gradient_fixup_css(stops, &count), EINVAL);
	CHECK_LONG(count, 3);

	stops[0] = make_stop(0.0f, RED, 1);
	stops[1] = make_stop(0.5f, BLUE, 1);
	stops[2] = make_hint(0.7f);
	count = 3;
	CHECK_LONG(vec_gradient_fixup_css(stops, &count), EINVAL);

	stops[0] = make_stop(0.0f, RED, 1);
	stops[1] = make_hint(0.3f);
	stops[2] = make_hint(0.4f);
	stops[3] = make_stop(1.0f, BLUE, 1);
	saved[0] = stops[0];
	saved[1] = stops[1];
	saved[2] = stops[2];
	saved[3] = stops[3];
	count = 4;
	error = vec_gradient_fixup_css(stops, &count);
	CHECK_LONG(error, EINVAL);
	CHECK_LONG(count, 4);
	CHECK(memcmp(stops, saved, sizeof(saved)) == 0);

	stops[0] = make_stop(0.0f, RED, 1);
	stops[1] = make_stop(0.0f, WHITE, 0);
	stops[1].is_hint = 1;
	stops[2] = make_stop(1.0f, BLUE, 1);
	count = 3;
	CHECK_LONG(vec_gradient_fixup_css(stops, &count), EINVAL);

	stops[0] = make_stop(0.0f, RED, 1);
	stops[1] = make_stop(INFINITY, WHITE, 1);
	stops[2] = make_stop(1.0f, BLUE, 1);
	count = 3;
	CHECK_LONG(vec_gradient_fixup_css(stops, &count), EINVAL);
	stops[1] = make_stop(NAN, WHITE, 1);
	CHECK_LONG(vec_gradient_fixup_css(stops, &count), EINVAL);
	CHECK_LONG(count, 3);
	CHECK(stops[0].has_offset == 1 && stops[2].has_offset == 1);
}

/* Tests the SVG fixup. */
static void
test_fixup_svg(void)
{
	struct vec_stop stops[5];
	struct vec_stop saved[2];
	double expected[] = { 0.0, 0.3, 0.3, 1.0 };

	/* Offsets are clamped to 0..1, and one below the one before it is lifted to it. */
	stops[0] = make_stop(-0.5f, RED, 1);
	stops[1] = make_stop(0.3f, WHITE, 1);
	stops[2] = make_stop(0.2f, BLACK, 1);
	stops[3] = make_stop(1.5f, BLUE, 1);
	stops[1].has_hint = 1;
	stops[1].hint = 0.2f;
	stops[2].is_hint = 1;
	CHECK_LONG(vec_gradient_fixup_svg(stops, 4), 0);
	check_offsets(stops, 4, expected);
	CHECK(stops[1].has_hint == 0);
	CHECK(stops[0].color == RED && stops[3].color == BLUE);

	/* Errors leave the stops unchanged. */
	stops[0] = make_stop(-0.5f, RED, 1);
	stops[1] = make_stop(NAN, WHITE, 1);
	saved[0] = stops[0];
	saved[1] = stops[1];
	CHECK_LONG(vec_gradient_fixup_svg(stops, 2), EINVAL);
	CHECK(stops[0].offset == saved[0].offset);
	CHECK(memcmp(&stops[0], &saved[0], sizeof(saved[0])) == 0);
	CHECK_LONG(vec_gradient_fixup_svg(stops, 0), EINVAL);
	CHECK_LONG(vec_gradient_fixup_svg(stops, VEC_GRADIENT_STOPS_MAX + 1), EINVAL);
	CHECK_LONG(vec_gradient_fixup_svg(NULL, 2), EINVAL);
	stops[1] = make_stop(INFINITY, WHITE, 1);
	CHECK_LONG(vec_gradient_fixup_svg(stops, 2), EINVAL);
}

/* Tests the span mappings of extending and repeating gradients. */
static void
test_spans(void)
{
	struct vec_stop stops[4];
	float start;
	float length;
	int error;

	/* Stops from -50% to 150% map onto 0..1; the geometry extends from -0.5 over a length of 2. */
	stops[0] = make_stop(-0.5f, RED, 1);
	stops[1] = make_stop(0.25f, WHITE, 1);
	stops[2] = make_stop(1.5f, BLUE, 1);
	error = vec_gradient_extend_span(stops, 3, &start, &length);
	CHECK_LONG(error, 0);
	CHECK_NEAR(start, -0.5, 1e-6);
	CHECK_NEAR(length, 2.0, 1e-6);
	CHECK_NEAR(stops[0].offset, 0.0, 1e-6);
	CHECK_NEAR(stops[1].offset, 0.375, 1e-6);
	CHECK_NEAR(stops[2].offset, 1.0, 1e-6);
	CHECK(stops[1].color == WHITE);

	/* Only the first stop outside: -25% to 100% is a length of 1.25; the stop at 50% maps to 0.75 / 1.25. */
	stops[0] = make_stop(-0.25f, RED, 1);
	stops[1] = make_stop(0.5f, WHITE, 1);
	stops[2] = make_stop(1.0f, BLUE, 1);
	error = vec_gradient_extend_span(stops, 3, &start, &length);
	CHECK_LONG(error, 0);
	CHECK_NEAR(start, -0.25, 1e-6);
	CHECK_NEAR(length, 1.25, 1e-6);
	CHECK_NEAR(stops[1].offset, 0.6, 1e-6);

	/* Stops inside 0..1 are left alone. */
	stops[0] = make_stop(0.1f, RED, 1);
	stops[1] = make_stop(0.9f, BLUE, 1);
	error = vec_gradient_extend_span(stops, 2, &start, &length);
	CHECK_LONG(error, 0);
	CHECK_NEAR(start, 0.0, 0.0);
	CHECK_NEAR(length, 1.0, 0.0);
	CHECK_NEAR(stops[0].offset, 0.1, 1e-7);
	CHECK_NEAR(stops[1].offset, 0.9, 1e-7);

	/* Errors. */
	CHECK_LONG(vec_gradient_extend_span(stops, 0, &start, &length), EINVAL);
	CHECK_LONG(vec_gradient_extend_span(NULL, 2, &start, &length), EINVAL);
	CHECK_LONG(vec_gradient_extend_span(stops, 2, NULL, &length), EINVAL);
	CHECK_LONG(vec_gradient_extend_span(stops, VEC_GRADIENT_STOPS_MAX + 1, &start, &length), EINVAL);

	/* The repeat span maps the first..last range onto 0..1: 20%, 50%, 80% become 0, 0.5, 1 over a length of 0.6 from 0.2. */
	stops[0] = make_stop(0.2f, RED, 1);
	stops[1] = make_stop(0.5f, WHITE, 1);
	stops[2] = make_stop(0.8f, BLUE, 1);
	error = vec_gradient_repeat_span(stops, 3, &start, &length);
	CHECK_LONG(error, 0);
	CHECK_NEAR(start, 0.2, 1e-6);
	CHECK_NEAR(length, 0.6, 1e-6);
	CHECK_NEAR(stops[0].offset, 0.0, 1e-6);
	CHECK_NEAR(stops[1].offset, 0.5, 1e-6);
	CHECK_NEAR(stops[2].offset, 1.0, 1e-6);

	/* A length of 0 is ENOTSUP and changes nothing. */
	stops[0] = make_stop(0.5f, RED, 1);
	stops[1] = make_stop(0.5f, BLUE, 1);
	start = 7.0f;
	length = 7.0f;
	error = vec_gradient_repeat_span(stops, 2, &start, &length);
	CHECK_LONG(error, ENOTSUP);
	CHECK_NEAR(stops[0].offset, 0.5, 0.0);
	CHECK_NEAR(stops[1].offset, 0.5, 0.0);
	CHECK_NEAR(start, 7.0, 0.0);
	error = vec_gradient_repeat_span(stops, 1, &start, &length);
	CHECK_LONG(error, ENOTSUP);

	/* Errors. */
	CHECK_LONG(vec_gradient_repeat_span(stops, 0, &start, &length), EINVAL);
	CHECK_LONG(vec_gradient_repeat_span(NULL, 2, &start, &length), EINVAL);
	CHECK_LONG(vec_gradient_repeat_span(stops, 2, &start, NULL), EINVAL);
}

/* Tests the color at a position: interpolation, hints, hard stops and the ends. */
static void
test_color_at(void)
{
	struct vec_stop stops[6];
	uint32_t color;

	/* Opaque red to opaque blue, premultiplied: the middle is 127.5 in both channels. */
	stops[0] = make_stop(0.0f, RED, 1);
	stops[1] = make_stop(1.0f, BLUE, 1);
	CHECK_COLOR(vec_gradient_color_at(stops, 2, 1, 0.0f), RED, 0);
	CHECK_COLOR(vec_gradient_color_at(stops, 2, 1, 1.0f), BLUE, 0);
	CHECK_COLOR(vec_gradient_color_at(stops, 2, 1, 0.5f), 0xff800080U, 1);
	CHECK_COLOR(vec_gradient_color_at(stops, 2, 1, 0.25f), 0xffbf0040U, 0);
	CHECK_COLOR(vec_gradient_color_at(stops, 2, 1, 0.75f), 0xff4000bfU, 0);

	/* A narrow span, off the rounding ties: red at 0.2 and blue at 0.3, position 0.24 is 40% of the way: red 153, blue 102. */
	stops[0] = make_stop(0.2f, RED, 1);
	stops[1] = make_stop(0.3f, BLUE, 1);
	CHECK_COLOR(vec_gradient_color_at(stops, 2, 1, 0.24f), 0xff990066U, 0);
	CHECK_COLOR(vec_gradient_color_at(stops, 2, 1, 0.28f), 0xff3300ccU, 0);
	stops[0] = make_stop(0.0f, RED, 1);
	stops[1] = make_stop(1.0f, BLUE, 1);

	/* The CSS example of premultiplied interpolation: opaque red to transparent, halfway: a = 0.5, premultiplied red = 0.5. */
	stops[0] = make_stop(0.0f, RED, 1);
	stops[1] = make_stop(1.0f, TRANSPARENT, 1);
	color = vec_gradient_color_at(stops, 2, 1, 0.5f);
	CHECK_COLOR(color, 0x80800000U, 1);

	/* The same stops straight (SVG): the straight rgba average is (127.5, 0, 0, 0.5), premultiplied (64, 0, 0, 128). */
	color = vec_gradient_color_at(stops, 2, 0, 0.5f);
	CHECK_COLOR(color, 0x80400000U, 1);

	/* Straight transparent red keeps its color: the average is rgba(255, 0, 0, 0.5), premultiplied (128, 0, 0, 128). */
	stops[1] = make_stop(1.0f, 0x00ff0000U, 1);
	color = vec_gradient_color_at(stops, 2, 0, 0.5f);
	CHECK_COLOR(color, 0x80800000U, 1);
	color = vec_gradient_color_at(stops, 2, 1, 0.5f);
	CHECK_COLOR(color, 0x80800000U, 1);

	/* Partly transparent stops, both ways: alpha 128 red to opaque blue, halfway. */
	stops[0] = make_stop(0.0f, 0x80ff0000U, 1);
	stops[1] = make_stop(1.0f, BLUE, 1);
	color = vec_gradient_color_at(stops, 2, 1, 0.5f);
	CHECK_COLOR(color, 0xc0400080U, 1);
	color = vec_gradient_color_at(stops, 2, 0, 0.5f);
	CHECK_COLOR(color, 0xc0600060U, 1);

	/* A middle stop: red, white at 0.5, blue; the quarter is halfway from red to white. */
	stops[0] = make_stop(0.0f, RED, 1);
	stops[1] = make_stop(0.5f, WHITE, 1);
	stops[2] = make_stop(1.0f, BLUE, 1);
	CHECK_COLOR(vec_gradient_color_at(stops, 3, 1, 0.25f), 0xffff8080U, 1);
	CHECK_COLOR(vec_gradient_color_at(stops, 3, 1, 0.75f), 0xff8080ffU, 1);
	CHECK_COLOR(vec_gradient_color_at(stops, 3, 1, 0.5f), WHITE, 0);

	/* The ends, outside and a NaN: before the first stop, after the last, and NaN as 0. */
	stops[0] = make_stop(0.25f, RED, 1);
	stops[1] = make_stop(0.75f, BLUE, 1);
	CHECK_COLOR(vec_gradient_color_at(stops, 2, 1, 0.0f), RED, 0);
	CHECK_COLOR(vec_gradient_color_at(stops, 2, 1, 0.1f), RED, 0);
	CHECK_COLOR(vec_gradient_color_at(stops, 2, 1, 0.9f), BLUE, 0);
	CHECK_COLOR(vec_gradient_color_at(stops, 2, 1, 1.0f), BLUE, 0);
	CHECK_COLOR(vec_gradient_color_at(stops, 2, 1, -5.0f), RED, 0);
	CHECK_COLOR(vec_gradient_color_at(stops, 2, 1, 5.0f), BLUE, 0);
	CHECK_COLOR(vec_gradient_color_at(stops, 2, 1, NAN), RED, 0);
	CHECK_COLOR(vec_gradient_color_at(stops, 2, 1, INFINITY), BLUE, 0);
	CHECK_COLOR(vec_gradient_color_at(stops, 2, 1, 0.5f), 0xff800080U, 1);

	/* A single stop is the color everywhere. */
	CHECK_COLOR(vec_gradient_color_at(stops, 1, 1, 0.9f), RED, 0);

	/* Stops the model does not take give 0. */
	CHECK_LONG(vec_gradient_color_at(NULL, 2, 1, 0.5f), 0);
	CHECK_LONG(vec_gradient_color_at(stops, 0, 1, 0.5f), 0);
	CHECK_LONG(vec_gradient_color_at(stops, VEC_GRADIENT_STOPS_MAX + 1, 1, 0.5f), 0);

	/* Hard stops: red | blue at 0.5; below, at and above. */
	stops[0] = make_stop(0.0f, RED, 1);
	stops[1] = make_stop(0.5f, RED, 1);
	stops[2] = make_stop(0.5f, BLUE, 1);
	stops[3] = make_stop(1.0f, BLUE, 1);
	CHECK_COLOR(vec_gradient_color_at(stops, 4, 1, 0.4999f), RED, 0);
	CHECK_COLOR(vec_gradient_color_at(stops, 4, 1, 0.5f), BLUE, 0);
	CHECK_COLOR(vec_gradient_color_at(stops, 4, 1, 0.5001f), BLUE, 0);
	CHECK_COLOR(vec_gradient_color_at(stops, 4, 1, 0.0f), RED, 0);
	CHECK_COLOR(vec_gradient_color_at(stops, 4, 1, 1.0f), BLUE, 0);

	/* A hard stop with different colors on both sides, first and last at the same offset. */
	stops[0] = make_stop(0.5f, WHITE, 1);
	stops[1] = make_stop(0.5f, BLACK, 1);
	CHECK_COLOR(vec_gradient_color_at(stops, 2, 1, 0.3f), WHITE, 0);
	CHECK_COLOR(vec_gradient_color_at(stops, 2, 1, 0.5f), BLACK, 0);
	CHECK_COLOR(vec_gradient_color_at(stops, 2, 1, 0.7f), BLACK, 0);
}

/* Tests the transition hint: C = P^(log_H 0.5), so the 50% blend is at the hint. */
static void
test_hint(void)
{
	struct vec_stop stops[2];
	double exponent;
	double weight;
	int i;
	uint32_t color;
	float hints[] = { 0.1f, 0.25f, 0.4f, 0.5f, 0.6f, 0.75f, 0.9f };
	float fractions[] = { 0.05f, 0.3f, 0.5f, 0.8f, 0.95f };
	int red;
	int blue;
	int j;

	/* The halfway color is at the hint: for each hint, the position H is the 50% blend. */
	for (i = 0; i < 7; i++) {
		stops[0] = make_stop_hint(0.0f, RED, hints[i]);
		stops[1] = make_stop(1.0f, BLUE, 1);
		color = vec_gradient_color_at(stops, 2, 1, hints[i]);
		CHECK_COLOR(color, 0xff800080U, 1);
	}

	/* The weight follows C = P^(ln 0.5 / ln H) at other fractions (the red channel is 255 (1 - C), the blue 255 C). */
	for (i = 0; i < 7; i++) {
		for (j = 0; j < 5; j++) {
			stops[0] = make_stop_hint(0.0f, RED, hints[i]);
			stops[1] = make_stop(1.0f, BLUE, 1);
			color = vec_gradient_color_at(stops, 2, 1, fractions[j]);
			exponent = log(0.5) / log((double)hints[i]);
			weight = pow((double)fractions[j], exponent);
			red = (int)floor(255.0 * (1.0 - weight) + 0.5);
			blue = (int)floor(255.0 * weight + 0.5);
			CHECK_COLOR(color, 0xff000000U | ((uint32_t)red << 16) | (uint32_t)blue, 1);
		}
	}

	/* H = 0.25 at P = 0.5: C = 0.5^0.5 = 0.7071, blue 180, red 75. */
	stops[0] = make_stop_hint(0.0f, RED, 0.25f);
	stops[1] = make_stop(1.0f, BLUE, 1);
	CHECK_COLOR(vec_gradient_color_at(stops, 2, 1, 0.5f), 0xff4b00b4U, 1);

	/* H = 0.5 is the same as no hint. */
	stops[0] = make_stop_hint(0.0f, RED, 0.5f);
	CHECK_COLOR(vec_gradient_color_at(stops, 2, 1, 0.3f), 0xffb2004cU, 1);

	/* H = 0: every position after the first stop is the second color; the position of the first stop is the first. */
	stops[0] = make_stop_hint(0.0f, RED, 0.0f);
	CHECK_COLOR(vec_gradient_color_at(stops, 2, 1, 0.0f), RED, 0);
	CHECK_COLOR(vec_gradient_color_at(stops, 2, 1, 0.01f), BLUE, 0);
	CHECK_COLOR(vec_gradient_color_at(stops, 2, 1, 0.5f), BLUE, 0);
	CHECK_COLOR(vec_gradient_color_at(stops, 2, 1, 0.99f), BLUE, 0);

	/* H = 1: every position before the second stop is the first color. */
	stops[0] = make_stop_hint(0.0f, RED, 1.0f);
	CHECK_COLOR(vec_gradient_color_at(stops, 2, 1, 0.0f), RED, 0);
	CHECK_COLOR(vec_gradient_color_at(stops, 2, 1, 0.5f), RED, 0);
	CHECK_COLOR(vec_gradient_color_at(stops, 2, 1, 0.99f), RED, 0);
	CHECK_COLOR(vec_gradient_color_at(stops, 2, 1, 1.0f), BLUE, 0);

	/* A hint that is not a number is ignored. */
	stops[0] = make_stop_hint(0.0f, RED, NAN);
	CHECK_COLOR(vec_gradient_color_at(stops, 2, 1, 0.5f), 0xff800080U, 1);
}

/* Tests the average color. */
static void
test_average(void)
{
	struct vec_stop stops[4];

	/* Two stops: red and blue average to half of each. */
	stops[0] = make_stop(0.0f, RED, 1);
	stops[1] = make_stop(1.0f, BLUE, 1);
	CHECK_COLOR(vec_gradient_average(stops, 2, 1), 0xff800080U, 1);

	/* A hard stop in the middle: half red, half blue. */
	stops[0] = make_stop(0.0f, RED, 1);
	stops[1] = make_stop(0.5f, RED, 1);
	stops[2] = make_stop(0.5f, BLUE, 1);
	stops[3] = make_stop(1.0f, BLUE, 1);
	CHECK_COLOR(vec_gradient_average(stops, 4, 1), 0xff800080U, 1);

	/* CSS Images 3 section 3.3: red, white and blue all at 0px average to rgb(75%, 50%, 75%). */
	stops[0] = make_stop(0.0f, RED, 1);
	stops[1] = make_stop(0.0f, WHITE, 1);
	stops[2] = make_stop(0.0f, BLUE, 1);
	CHECK_COLOR(vec_gradient_average(stops, 3, 1), 0xffbf80bfU, 1);

	/* The stops cover only 0.5..1: the first 0.5 is red, the rest averages red and blue (191.25, 0, 63.75). */
	stops[0] = make_stop(0.5f, RED, 1);
	stops[1] = make_stop(1.0f, BLUE, 1);
	CHECK_COLOR(vec_gradient_average(stops, 2, 1), 0xffbf0040U, 1);

	/* The stops cover 0..0.5: the last half is blue. */
	stops[0] = make_stop(0.0f, RED, 1);
	stops[1] = make_stop(0.5f, BLUE, 1);
	CHECK_COLOR(vec_gradient_average(stops, 2, 1), 0xff4000bfU, 1);

	/* A hint at 0.25: the mean weight of blue is 1 / (k + 1) with k = 0.5, 2/3: red 85, blue 170. */
	stops[0] = make_stop_hint(0.0f, RED, 0.25f);
	stops[1] = make_stop(1.0f, BLUE, 1);
	CHECK_COLOR(vec_gradient_average(stops, 2, 1), 0xff5500aaU, 1);

	/* A hint at 0 is all the second color and at 1 all the first. */
	stops[0] = make_stop_hint(0.0f, RED, 0.0f);
	CHECK_COLOR(vec_gradient_average(stops, 2, 1), BLUE, 1);
	stops[0] = make_stop_hint(0.0f, RED, 1.0f);
	CHECK_COLOR(vec_gradient_average(stops, 2, 1), RED, 1);

	/* A single stop is its own average. */
	stops[0] = make_stop(0.3f, 0x80ff0000U, 1);
	CHECK_COLOR(vec_gradient_average(stops, 1, 1), 0x80800000U, 1);
	CHECK_COLOR(vec_gradient_average(stops, 1, 0), 0x80800000U, 1);

	/*
	 * Straight stops, opaque red to transparent black: the premultiplied
	 * red at weight C is 255 (1 - C)^2, whose mean is 255 / 3 = 85, and
	 * the alpha mean is 127.5.
	 */
	stops[0] = make_stop(0.0f, RED, 1);
	stops[1] = make_stop(1.0f, TRANSPARENT, 1);
	CHECK_COLOR(vec_gradient_average(stops, 2, 0), 0x80550000U, 1);
	CHECK_COLOR(vec_gradient_average(stops, 2, 1), 0x80800000U, 1);

	/* Stops the model does not take give 0. */
	CHECK_LONG(vec_gradient_average(NULL, 2, 1), 0);
	CHECK_LONG(vec_gradient_average(stops, 0, 1), 0);
	CHECK_LONG(vec_gradient_average(stops, VEC_GRADIENT_STOPS_MAX + 1, 1), 0);
}

/* Tests the ramp width and the ramp. */
static void
test_ramp(void)
{
	struct vec_stop stops[2];
	struct vec_gradient gradient;
	uint32_t ramp[VEC_RAMP_WIDTH_MAX + 1];
	uint32_t before[8];
	int i;
	int width;
	int error;
	double t;
	int red;
	int blue;

	/* The width is the length rounded up, held to 2..2048. */
	CHECK_LONG(vec_gradient_ramp_width(0.0f), 2);
	CHECK_LONG(vec_gradient_ramp_width(1.0f), 2);
	CHECK_LONG(vec_gradient_ramp_width(2.0f), 2);
	CHECK_LONG(vec_gradient_ramp_width(2.5f), 3);
	CHECK_LONG(vec_gradient_ramp_width(100.0f), 100);
	CHECK_LONG(vec_gradient_ramp_width(100.25f), 101);
	CHECK_LONG(vec_gradient_ramp_width(2047.5f), 2048);
	CHECK_LONG(vec_gradient_ramp_width(2048.0f), 2048);
	CHECK_LONG(vec_gradient_ramp_width(5000.0f), 2048);
	CHECK_LONG(vec_gradient_ramp_width(-7.0f), 2);
	CHECK_LONG(vec_gradient_ramp_width(NAN), 2048);
	CHECK_LONG(vec_gradient_ramp_width(INFINITY), 2048);
	CHECK_LONG(vec_gradient_ramp_width(-INFINITY), 2048);

	/* A ramp of 4 texels of red to blue: texel i is the color at (i + 0.5) / 4. */
	stops[0] = make_stop(0.0f, RED, 1);
	stops[1] = make_stop(1.0f, BLUE, 1);
	gradient = make_linear(stops, 2, VEC_SPREAD_PAD, 1, 0.0f, 0.0f, 4.0f, 0.0f);
	error = vec_gradient_ramp(&gradient, 4, ramp);
	CHECK_LONG(error, 0);
	for (i = 0; i < 4; i++) {
		t = ((double)i + 0.5) / 4.0;
		red = (int)floor(255.0 * (1.0 - t) + 0.5);
		blue = (int)floor(255.0 * t + 0.5);
		CHECK_COLOR(ramp[i], 0xff000000U | ((uint32_t)red << 16) | (uint32_t)blue, 0);
	}

	/* The widest ramp is written whole, and the texel after it is not touched. */
	ramp[VEC_RAMP_WIDTH_MAX] = 0x12345678U;
	error = vec_gradient_ramp(&gradient, VEC_RAMP_WIDTH_MAX, ramp);
	CHECK_LONG(error, 0);
	CHECK_LONG(ramp[VEC_RAMP_WIDTH_MAX], 0x12345678U);
	for (width = 0; width < VEC_RAMP_WIDTH_MAX; width += 97) {
		t = ((double)width + 0.5) / (double)VEC_RAMP_WIDTH_MAX;
		red = (int)floor(255.0 * (1.0 - t) + 0.5);
		blue = (int)floor(255.0 * t + 0.5);
		CHECK_COLOR(ramp[width], 0xff000000U | ((uint32_t)red << 16) | (uint32_t)blue, 1);
	}

	/* Errors leave the ramp unwritten. */
	for (i = 0; i < 8; i++)
		before[i] = 0xdeadbeefU;
	memcpy(ramp, before, sizeof(before));
	CHECK_LONG(vec_gradient_ramp(&gradient, 1, ramp), EINVAL);
	CHECK_LONG(vec_gradient_ramp(&gradient, VEC_RAMP_WIDTH_MAX + 1, ramp), EINVAL);
	CHECK_LONG(vec_gradient_ramp(&gradient, 0, ramp), EINVAL);
	CHECK_LONG(vec_gradient_ramp(&gradient, -4, ramp), EINVAL);
	CHECK_LONG(vec_gradient_ramp(&gradient, 4, NULL), EINVAL);
	CHECK_LONG(vec_gradient_ramp(NULL, 4, ramp), EINVAL);
	gradient.count = 0;
	CHECK_LONG(vec_gradient_ramp(&gradient, 4, ramp), EINVAL);
	gradient.count = VEC_GRADIENT_STOPS_MAX + 1;
	CHECK_LONG(vec_gradient_ramp(&gradient, 4, ramp), EINVAL);
	gradient.count = 2;
	gradient.stops = NULL;
	CHECK_LONG(vec_gradient_ramp(&gradient, 4, ramp), EINVAL);
	CHECK(memcmp(ramp, before, sizeof(before)) == 0);

	/* The smallest ramp. */
	gradient.stops = stops;
	CHECK_LONG(vec_gradient_ramp(&gradient, 2, ramp), 0);
	CHECK_COLOR(ramp[0], 0xffbf0040U, 1);
	CHECK_COLOR(ramp[1], 0xff4000bfU, 1);
}

/* Tests the parameter of a linear gradient. */
static void
test_parameter_linear(void)
{
	struct vec_stop stops[2];
	struct vec_gradient gradient;
	struct vec_matrix matrix;
	int painted;
	float t;

	stops[0] = make_stop(0.0f, RED, 1);
	stops[1] = make_stop(1.0f, BLUE, 1);

	/* Along the x axis: t is x / 10, whatever y. */
	gradient = make_linear(stops, 2, VEC_SPREAD_PAD, 1, 0.0f, 0.0f, 10.0f, 0.0f);
	t = vec_gradient_parameter(&gradient, 5.0f, 3.0f, &painted);
	CHECK_NEAR(t, 0.5, 1e-6);
	CHECK_LONG(painted, 1);
	t = vec_gradient_parameter(&gradient, 15.0f, -40.0f, &painted);
	CHECK_NEAR(t, 1.5, 1e-6);
	t = vec_gradient_parameter(&gradient, -5.0f, 100.0f, &painted);
	CHECK_NEAR(t, -0.5, 1e-6);
	t = vec_gradient_parameter(&gradient, 3.0f, 77.0f, NULL);
	CHECK_NEAR(t, 0.3, 1e-6);

	/* A diagonal line (0,0) to (3,4): its end is 1, its middle 0.5, and a point on the perpendicular at the start is 0. */
	gradient = make_linear(stops, 2, VEC_SPREAD_PAD, 1, 0.0f, 0.0f, 3.0f, 4.0f);
	CHECK_NEAR(vec_gradient_parameter(&gradient, 3.0f, 4.0f, &painted), 1.0, 1e-6);
	CHECK_NEAR(vec_gradient_parameter(&gradient, 1.5f, 2.0f, &painted), 0.5, 1e-6);
	CHECK_NEAR(vec_gradient_parameter(&gradient, 4.0f, -3.0f, &painted), 0.0, 1e-6);
	CHECK_NEAR(vec_gradient_parameter(&gradient, 4.0f + 3.0f, -3.0f + 4.0f, &painted), 1.0, 1e-6);

	/* A line not through the origin: (10, 10) to (10, 30). */
	gradient = make_linear(stops, 2, VEC_SPREAD_PAD, 1, 10.0f, 10.0f, 10.0f, 30.0f);
	CHECK_NEAR(vec_gradient_parameter(&gradient, -50.0f, 20.0f, &painted), 0.5, 1e-6);
	CHECK_NEAR(vec_gradient_parameter(&gradient, 0.0f, 30.0f, &painted), 1.0, 1e-6);

	/* Through the matrix: to_gradient scales device pixels by 0.5, so device x = 20 is gradient x = 10. */
	gradient = make_linear(stops, 2, VEC_SPREAD_PAD, 1, 0.0f, 0.0f, 10.0f, 0.0f);
	vec_matrix_scale(&matrix, 0.5f, 0.5f);
	gradient.to_gradient = matrix;
	CHECK_NEAR(vec_gradient_parameter(&gradient, 20.0f, 0.0f, &painted), 1.0, 1e-6);
	CHECK_NEAR(vec_gradient_parameter(&gradient, 10.0f, 0.0f, &painted), 0.5, 1e-6);

	/* A translation: device x = 105 is gradient x = 5. */
	vec_matrix_translate(&matrix, -100.0f, 0.0f);
	gradient.to_gradient = matrix;
	CHECK_NEAR(vec_gradient_parameter(&gradient, 105.0f, 0.0f, &painted), 0.5, 1e-6);

	/* A rotation by 90 degrees: the line along gradient x runs down the device y axis. */
	vec_matrix_rotate(&matrix, -90.0f);
	gradient.to_gradient = matrix;
	CHECK_NEAR(vec_gradient_parameter(&gradient, 0.0f, 5.0f, &painted), 0.5, 1e-5);

	/* The same start and end: every point has t = 1 and is painted. */
	gradient = make_linear(stops, 2, VEC_SPREAD_REPEAT, 1, 4.0f, 4.0f, 4.0f, 4.0f);
	t = vec_gradient_parameter(&gradient, 100.0f, -5.0f, &painted);
	CHECK_NEAR(t, 1.0, 0.0);
	CHECK_LONG(painted, 1);
	gradient = make_linear(stops, 2, VEC_SPREAD_PAD, 1, 4.0f, 4.0f, 4.0f + 1e-7f, 4.0f);
	t = vec_gradient_parameter(&gradient, 0.0f, 0.0f, &painted);
	CHECK_NEAR(t, 1.0, 0.0);
}

/* Tests the parameter of a radial gradient. */
static void
test_parameter_radial(void)
{
	struct vec_gradient gradient;
	struct vec_matrix matrix;
	int painted;
	float t;

	/* The CSS case: focal at the center, radius 0: t is the distance over the radius. */
	gradient = make_radial(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 10.0f);
	t = vec_gradient_parameter(&gradient, 3.0f, 4.0f, &painted);
	CHECK_NEAR(t, 0.5, 1e-6);
	CHECK_LONG(painted, 1);
	CHECK_NEAR(vec_gradient_parameter(&gradient, 0.0f, 10.0f, &painted), 1.0, 1e-6);
	CHECK_NEAR(vec_gradient_parameter(&gradient, -20.0f, 0.0f, &painted), 2.0, 1e-6);
	CHECK_LONG(painted, 1);

	/* The center itself has r(w) = 0 on its only circle: not painted. */
	t = vec_gradient_parameter(&gradient, 0.0f, 0.0f, &painted);
	CHECK_LONG(painted, 0);
	CHECK_NEAR(t, 0.0, 0.0);

	/* An ellipse by the matrix that maps it to a circle: half as wide in gradient space, so device x = 10 is gradient x = 5. */
	vec_matrix_scale(&matrix, 0.5f, 1.0f);
	gradient.to_gradient = matrix;
	CHECK_NEAR(vec_gradient_parameter(&gradient, 10.0f, 0.0f, &painted), 0.5, 1e-6);
	CHECK_NEAR(vec_gradient_parameter(&gradient, 0.0f, 5.0f, &painted), 0.5, 1e-6);
	CHECK_NEAR(vec_gradient_parameter(&gradient, 20.0f, 0.0f, &painted), 1.0, 1e-6);

	/* A focal point inside the end circle: focal (5, 0), end circle at the origin of radius 10; a point on the end circle has t = 1. */
	gradient = make_radial(5.0f, 0.0f, 0.0f, 0.0f, 0.0f, 10.0f);
	CHECK_NEAR(vec_gradient_parameter(&gradient, 10.0f, 0.0f, &painted), 1.0, 1e-5);
	CHECK_LONG(painted, 1);
	CHECK_NEAR(vec_gradient_parameter(&gradient, 0.0f, 10.0f, &painted), 1.0, 1e-5);
	CHECK_NEAR(vec_gradient_parameter(&gradient, -10.0f, 0.0f, &painted), 1.0, 1e-5);

	/* The circle of w = 0.5 has center (2.5, 0) and radius 5: the point (2.5, 5) is on it (the roots are 0.5 and -5/6). */
	CHECK_NEAR(vec_gradient_parameter(&gradient, 2.5f, 5.0f, &painted), 0.5, 1e-5);
	CHECK_LONG(painted, 1);

	/* The focal point itself is the circle of radius 0: not painted. */
	vec_gradient_parameter(&gradient, 5.0f, 0.0f, &painted);
	CHECK_LONG(painted, 0);

	/* A focal circle outside the end circle makes a cone: focal (-20, 0), radius 0, end circle at the origin of radius 10. */
	gradient = make_radial(-20.0f, 0.0f, 0.0f, 0.0f, 0.0f, 10.0f);
	CHECK_NEAR(vec_gradient_parameter(&gradient, -10.0f, 0.0f, &painted), 1.0, 1e-5);
	CHECK_LONG(painted, 1);
	CHECK_NEAR(vec_gradient_parameter(&gradient, 10.0f, 0.0f, &painted), 3.0, 1e-4);
	CHECK_LONG(painted, 1);

	/* Outside the cone, to the side and behind the focal point: not painted. */
	t = vec_gradient_parameter(&gradient, -20.0f, 15.0f, &painted);
	CHECK_LONG(painted, 0);
	CHECK_NEAR(t, 0.0, 0.0);
	vec_gradient_parameter(&gradient, -30.0f, 0.0f, &painted);
	CHECK_LONG(painted, 0);

	/* Two circles that are the same paint nothing. */
	gradient = make_radial(3.0f, 3.0f, 8.0f, 3.0f, 3.0f, 8.0f);
	vec_gradient_parameter(&gradient, 5.0f, 5.0f, &painted);
	CHECK_LONG(painted, 0);
	vec_gradient_parameter(&gradient, 3.0f, 3.0f, &painted);
	CHECK_LONG(painted, 0);

	/* Concentric circles with a focal radius: radius 5 to 10; at distance 5 t = 0, at 10 t = 1, at 7.5 t = 0.5, at 2 t = -0.6. */
	gradient = make_radial(0.0f, 0.0f, 5.0f, 0.0f, 0.0f, 10.0f);
	CHECK_NEAR(vec_gradient_parameter(&gradient, 5.0f, 0.0f, &painted), 0.0, 1e-6);
	CHECK_NEAR(vec_gradient_parameter(&gradient, 0.0f, 10.0f, &painted), 1.0, 1e-6);
	CHECK_NEAR(vec_gradient_parameter(&gradient, 7.5f, 0.0f, &painted), 0.5, 1e-6);
	CHECK_NEAR(vec_gradient_parameter(&gradient, 0.0f, 2.0f, &painted), -0.6, 1e-6);
	CHECK_LONG(painted, 1);

	/* The center of that gradient lies on only the circle of radius 0 (w = -1): not painted. */
	vec_gradient_parameter(&gradient, 0.0f, 0.0f, &painted);
	CHECK_LONG(painted, 0);

	/* A focal point near the center: the point on the end circle still has t = 1. */
	gradient = make_radial(0.001f, 0.0f, 0.0f, 0.0f, 0.0f, 10.0f);
	CHECK_NEAR(vec_gradient_parameter(&gradient, 0.0f, 10.0f, &painted), 1.0, 1e-4);
	CHECK_NEAR(vec_gradient_parameter(&gradient, 10.0f, 0.0f, &painted), 1.0, 1e-4);

	/* Circles that are nearly the same: radius 9.999 to 10 at the same center; distance 9.9995 is halfway. */
	gradient = make_radial(0.0f, 0.0f, 9.999f, 0.0f, 0.0f, 10.0f);
	CHECK_NEAR(vec_gradient_parameter(&gradient, 9.9995f, 0.0f, &painted), 0.5, 2e-3);
	CHECK_LONG(painted, 1);

	/*
	 * The focal point on the end circle makes a = 0 (the equation is
	 * linear): focal (0, 0) of radius 0, end circle at (10, 0) of radius
	 * 10.  The point (5, 0) is on the circle of w = 0.25 (center (2.5, 0),
	 * radius 2.5), (20, 0) on that of w = 1, and (-5, 0) only on one of
	 * negative radius: not painted.
	 */
	gradient = make_radial(0.0f, 0.0f, 0.0f, 10.0f, 0.0f, 10.0f);
	CHECK_NEAR(vec_gradient_parameter(&gradient, 5.0f, 0.0f, &painted), 0.25, 1e-5);
	CHECK_LONG(painted, 1);
	CHECK_NEAR(vec_gradient_parameter(&gradient, 20.0f, 0.0f, &painted), 1.0, 1e-5);
	CHECK_LONG(painted, 1);
	vec_gradient_parameter(&gradient, -5.0f, 0.0f, &painted);
	CHECK_LONG(painted, 0);

	/*
	 * A tiny a (the focal point a hair off the end circle) is solved as a
	 * line too, so the far root of the quadratic is not taken: center
	 * (10.000001, 0) in single precision, and the same point as above.
	 */
	gradient = make_radial(0.0f, 0.0f, 0.0f, 10.000001f, 0.0f, 10.0f);
	CHECK_NEAR(vec_gradient_parameter(&gradient, 5.0f, 0.0f, &painted), 0.25, 1e-3);
	CHECK_LONG(painted, 1);

	/*
	 * A discriminant of exactly 0 is a point on the edge of the cone:
	 * focal (0, 0) of radius 0, end circle at (5, 0) of radius 3, the
	 * point (4, 3) has the double root w = 20 / 16 = 1.25 (the circle of
	 * center (6.25, 0) and radius 3.75 touches it).
	 */
	gradient = make_radial(0.0f, 0.0f, 0.0f, 5.0f, 0.0f, 3.0f);
	CHECK_NEAR(vec_gradient_parameter(&gradient, 4.0f, 3.0f, &painted), 1.25, 1e-6);
	CHECK_LONG(painted, 1);

	/* Circles a hair apart (under the limit) are the same circle: nothing is painted. */
	gradient = make_radial(0.0f, 0.0f, 10.0f, 1.0e-7f, 0.0f, 10.0f);
	vec_gradient_parameter(&gradient, 5.0f, 0.0f, &painted);
	CHECK_LONG(painted, 0);

	/* A negative radius paints nothing. */
	gradient = make_radial(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, -10.0f);
	vec_gradient_parameter(&gradient, 5.0f, 0.0f, &painted);
	CHECK_LONG(painted, 0);
	gradient = make_radial(0.0f, 0.0f, -1.0f, 0.0f, 0.0f, 10.0f);
	vec_gradient_parameter(&gradient, 5.0f, 0.0f, &painted);
	CHECK_LONG(painted, 0);
}

/* Tests the parameter of a conic gradient. */
static void
test_parameter_conic(void)
{
	struct vec_gradient gradient;
	int painted;
	int i;
	float t;
	float angle;

	/* Up is 0, right 0.25, down 0.5, left 0.75, clockwise from up (the y axis points down). */
	gradient = make_conic(50.0f, 50.0f, 0.0f);
	CHECK_NEAR(vec_gradient_parameter(&gradient, 50.0f, 40.0f, &painted), 0.0, 1e-5);
	CHECK_LONG(painted, 1);
	CHECK_NEAR(vec_gradient_parameter(&gradient, 60.0f, 50.0f, &painted), 0.25, 1e-5);
	CHECK_NEAR(vec_gradient_parameter(&gradient, 50.0f, 60.0f, &painted), 0.5, 1e-5);
	CHECK_NEAR(vec_gradient_parameter(&gradient, 40.0f, 50.0f, &painted), 0.75, 1e-5);

	/* Diagonals: down-right is 0.375, up-left is 0.875, up-right 0.125, down-left 0.625. */
	CHECK_NEAR(vec_gradient_parameter(&gradient, 60.0f, 60.0f, &painted), 0.375, 1e-5);
	CHECK_NEAR(vec_gradient_parameter(&gradient, 40.0f, 40.0f, &painted), 0.875, 1e-5);
	CHECK_NEAR(vec_gradient_parameter(&gradient, 60.0f, 40.0f, &painted), 0.125, 1e-5);
	CHECK_NEAR(vec_gradient_parameter(&gradient, 40.0f, 60.0f, &painted), 0.625, 1e-5);

	/* The distance does not matter. */
	CHECK_NEAR(vec_gradient_parameter(&gradient, 50.0f, 40000.0f, &painted), 0.5, 1e-5);

	/* The start angle (a quarter turn) moves the zero: right is 0, up is 0.75, left is 0.5. */
	gradient = make_conic(50.0f, 50.0f, VEC_PI / 2.0f);
	CHECK_NEAR(vec_gradient_parameter(&gradient, 60.0f, 50.0f, &painted), 0.0, 1e-5);
	CHECK_NEAR(vec_gradient_parameter(&gradient, 50.0f, 40.0f, &painted), 0.75, 1e-5);
	CHECK_NEAR(vec_gradient_parameter(&gradient, 40.0f, 50.0f, &painted), 0.5, 1e-5);
	CHECK_NEAR(vec_gradient_parameter(&gradient, 50.0f, 60.0f, &painted), 0.25, 1e-5);

	/* A negative start angle and one over a turn. */
	gradient = make_conic(0.0f, 0.0f, -VEC_PI / 2.0f);
	CHECK_NEAR(vec_gradient_parameter(&gradient, 0.0f, -5.0f, &painted), 0.25, 1e-5);
	gradient = make_conic(0.0f, 0.0f, 2.0f * VEC_PI + VEC_PI / 2.0f);
	CHECK_NEAR(vec_gradient_parameter(&gradient, 0.0f, -5.0f, &painted), 0.75, 1e-4);

	/* The center itself has t = 0 and is painted. */
	gradient = make_conic(50.0f, 50.0f, 1.0f);
	CHECK_NEAR(vec_gradient_parameter(&gradient, 50.0f, 50.0f, &painted), 0.0, 0.0);
	CHECK_LONG(painted, 1);
	CHECK_NEAR(vec_gradient_parameter(&gradient, 50.0000005f, 50.0f, &painted), 0.0, 0.0);

	/* t is always in [0, 1). */
	gradient = make_conic(50.0f, 50.0f, 1.0f);
	for (i = 0; i < 360; i++) {
		angle = (float)i * VEC_PI / 180.0f;
		t = vec_gradient_parameter(&gradient, 50.0f + 10.0f * sinf(angle), 50.0f - 10.0f * cosf(angle), &painted);
		CHECK(t >= 0.0f && t < 1.0f);
	}
}

/* Tests the parameter for input that is not valid. */
static void
test_parameter_invalid(void)
{
	struct vec_gradient gradient;
	int painted;
	float t;

	/* A missing gradient. */
	painted = 1;
	t = vec_gradient_parameter(NULL, 1.0f, 1.0f, &painted);
	CHECK_NEAR(t, 0.0, 0.0);
	CHECK_LONG(painted, 0);

	/* A point that is not a number. */
	gradient = make_linear(NULL, 0, VEC_SPREAD_PAD, 1, 0.0f, 0.0f, 10.0f, 0.0f);
	painted = 1;
	t = vec_gradient_parameter(&gradient, NAN, 1.0f, &painted);
	CHECK_LONG(painted, 0);
	CHECK_NEAR(t, 0.0, 0.0);
	vec_gradient_parameter(&gradient, 1.0f, INFINITY, &painted);
	CHECK_LONG(painted, 0);

	/* Geometry that is not finite. */
	gradient = make_linear(NULL, 0, VEC_SPREAD_PAD, 1, NAN, 0.0f, 10.0f, 0.0f);
	vec_gradient_parameter(&gradient, 1.0f, 1.0f, &painted);
	CHECK_LONG(painted, 0);
	gradient = make_radial(0.0f, 0.0f, 0.0f, INFINITY, 0.0f, 10.0f);
	vec_gradient_parameter(&gradient, 1.0f, 1.0f, &painted);
	CHECK_LONG(painted, 0);
	gradient = make_conic(0.0f, NAN, 0.0f);
	vec_gradient_parameter(&gradient, 1.0f, 1.0f, &painted);
	CHECK_LONG(painted, 0);
	gradient = make_conic(0.0f, 0.0f, NAN);
	vec_gradient_parameter(&gradient, 1.0f, 1.0f, &painted);
	CHECK_LONG(painted, 0);

	/* A matrix that is not finite, and one whose result overflows. */
	gradient = make_linear(NULL, 0, VEC_SPREAD_PAD, 1, 0.0f, 0.0f, 10.0f, 0.0f);
	gradient.to_gradient.a = NAN;
	vec_gradient_parameter(&gradient, 1.0f, 1.0f, &painted);
	CHECK_LONG(painted, 0);
	vec_matrix_identity(&gradient.to_gradient);
	gradient.to_gradient.a = VEC_COORD_MAX;
	vec_gradient_parameter(&gradient, 3.0e38f, 1.0f, &painted);
	CHECK_LONG(painted, 0);

	/* A kind this model does not have. */
	gradient = make_linear(NULL, 0, VEC_SPREAD_PAD, 1, 0.0f, 0.0f, 10.0f, 0.0f);
	gradient.kind = (enum vec_gradient_kind)99;
	vec_gradient_parameter(&gradient, 1.0f, 1.0f, &painted);
	CHECK_LONG(painted, 0);

	/* Huge but finite numbers give a finite t. */
	gradient = make_linear(NULL, 0, VEC_SPREAD_PAD, 1, 0.0f, 0.0f, 1.0f, 0.0f);
	t = vec_gradient_parameter(&gradient, 3.0e38f, 0.0f, &painted);
	CHECK(vec_finite(t));
	CHECK_LONG(painted, 1);
}

/* Tests the spread methods. */
static void
test_spread(void)
{
	/* Pad holds t to 0..1. */
	CHECK_NEAR(vec_gradient_spread(VEC_SPREAD_PAD, 0.3f), 0.3, 1e-7);
	CHECK_NEAR(vec_gradient_spread(VEC_SPREAD_PAD, 1.5f), 1.0, 0.0);
	CHECK_NEAR(vec_gradient_spread(VEC_SPREAD_PAD, -0.25f), 0.0, 0.0);

	/* Reflect: 1.5 is 0.5, -0.25 is 0.25, 2.25 is 0.25, 3.25 is 0.75, and 2 is 0. */
	CHECK_NEAR(vec_gradient_spread(VEC_SPREAD_REFLECT, 0.3f), 0.3, 1e-7);
	CHECK_NEAR(vec_gradient_spread(VEC_SPREAD_REFLECT, 1.5f), 0.5, 1e-7);
	CHECK_NEAR(vec_gradient_spread(VEC_SPREAD_REFLECT, -0.25f), 0.25, 1e-7);
	CHECK_NEAR(vec_gradient_spread(VEC_SPREAD_REFLECT, 2.25f), 0.25, 1e-7);
	CHECK_NEAR(vec_gradient_spread(VEC_SPREAD_REFLECT, 3.25f), 0.75, 1e-7);
	CHECK_NEAR(vec_gradient_spread(VEC_SPREAD_REFLECT, 2.0f), 0.0, 0.0);
	CHECK_NEAR(vec_gradient_spread(VEC_SPREAD_REFLECT, 1.0f), 1.0, 0.0);
	CHECK_NEAR(vec_gradient_spread(VEC_SPREAD_REFLECT, -1.75f), 0.25, 1e-7);

	/* Repeat: 1.25 is 0.25, -0.25 is 0.75, and 3 is 0. */
	CHECK_NEAR(vec_gradient_spread(VEC_SPREAD_REPEAT, 0.3f), 0.3, 1e-7);
	CHECK_NEAR(vec_gradient_spread(VEC_SPREAD_REPEAT, 1.25f), 0.25, 1e-7);
	CHECK_NEAR(vec_gradient_spread(VEC_SPREAD_REPEAT, -0.25f), 0.75, 1e-7);
	CHECK_NEAR(vec_gradient_spread(VEC_SPREAD_REPEAT, 3.0f), 0.0, 0.0);
	CHECK_NEAR(vec_gradient_spread(VEC_SPREAD_REPEAT, -3.5f), 0.5, 1e-7);

	/* A t that is not finite is 0 in every method. */
	CHECK_NEAR(vec_gradient_spread(VEC_SPREAD_PAD, NAN), 0.0, 0.0);
	CHECK_NEAR(vec_gradient_spread(VEC_SPREAD_REFLECT, NAN), 0.0, 0.0);
	CHECK_NEAR(vec_gradient_spread(VEC_SPREAD_REPEAT, NAN), 0.0, 0.0);
	CHECK_NEAR(vec_gradient_spread(VEC_SPREAD_PAD, INFINITY), 0.0, 0.0);
	CHECK_NEAR(vec_gradient_spread(VEC_SPREAD_REPEAT, -INFINITY), 0.0, 0.0);

	/* Large values stay in 0..1. */
	CHECK(vec_gradient_spread(VEC_SPREAD_REFLECT, 1.0e30f) >= 0.0f);
	CHECK(vec_gradient_spread(VEC_SPREAD_REPEAT, -1.0e30f) <= 1.0f);
}

/* Tests the lookup of a color from a ramp. */
static void
test_sample(void)
{
	struct vec_stop stops[2];
	struct vec_gradient gradient;
	uint32_t ramp[4];

	stops[0] = make_stop(0.0f, RED, 1);
	stops[1] = make_stop(1.0f, BLUE, 1);

	/* A ramp of four whole colors, so that the index is visible. */
	ramp[0] = 0xff000010U;
	ramp[1] = 0xff000020U;
	ramp[2] = 0xff000030U;
	ramp[3] = 0xff000040U;

	/* Pad: t = 0.5 is index 2, t = 1 is the last texel, below 0 and above 1 are the ends. */
	gradient = make_linear(stops, 2, VEC_SPREAD_PAD, 1, 0.0f, 0.0f, 10.0f, 0.0f);
	CHECK_LONG(vec_gradient_sample(&gradient, ramp, 4, 0.0f, 0.0f), 0xff000010U);
	CHECK_LONG(vec_gradient_sample(&gradient, ramp, 4, 2.4f, 0.0f), 0xff000010U);
	CHECK_LONG(vec_gradient_sample(&gradient, ramp, 4, 2.6f, 0.0f), 0xff000020U);
	CHECK_LONG(vec_gradient_sample(&gradient, ramp, 4, 5.0f, 0.0f), 0xff000030U);
	CHECK_LONG(vec_gradient_sample(&gradient, ramp, 4, 9.9f, 0.0f), 0xff000040U);
	CHECK_LONG(vec_gradient_sample(&gradient, ramp, 4, 10.0f, 0.0f), 0xff000040U);
	CHECK_LONG(vec_gradient_sample(&gradient, ramp, 4, 100.0f, 0.0f), 0xff000040U);
	CHECK_LONG(vec_gradient_sample(&gradient, ramp, 4, -100.0f, 0.0f), 0xff000010U);

	/* Reflect and repeat: t = 1.25 is 0.75 reflected (index 3), 0.25 repeated (index 1); t = 1.75 is 0.25 and 0.75. */
	gradient.spread = VEC_SPREAD_REFLECT;
	CHECK_LONG(vec_gradient_sample(&gradient, ramp, 4, 12.5f, 0.0f), 0xff000040U);
	CHECK_LONG(vec_gradient_sample(&gradient, ramp, 4, 17.5f, 0.0f), 0xff000020U);
	gradient.spread = VEC_SPREAD_REPEAT;
	CHECK_LONG(vec_gradient_sample(&gradient, ramp, 4, 12.5f, 0.0f), 0xff000020U);
	CHECK_LONG(vec_gradient_sample(&gradient, ramp, 4, 17.5f, 0.0f), 0xff000040U);
	CHECK_LONG(vec_gradient_sample(&gradient, ramp, 4, -2.5f, 0.0f), 0xff000040U);

	/* A line of no length is the last texel whatever the spread. */
	gradient = make_linear(stops, 2, VEC_SPREAD_REPEAT, 1, 3.0f, 3.0f, 3.0f, 3.0f);
	CHECK_LONG(vec_gradient_sample(&gradient, ramp, 4, 0.0f, 0.0f), 0xff000040U);
	CHECK_LONG(vec_gradient_sample(&gradient, ramp, 4, 77.0f, -5.0f), 0xff000040U);
	gradient.spread = VEC_SPREAD_REFLECT;
	CHECK_LONG(vec_gradient_sample(&gradient, ramp, 4, 77.0f, -5.0f), 0xff000040U);

	/* A point the gradient does not paint is transparent. */
	gradient = make_radial(-20.0f, 0.0f, 0.0f, 0.0f, 0.0f, 10.0f);
	gradient.stops = stops;
	gradient.count = 2;
	CHECK_LONG(vec_gradient_sample(&gradient, ramp, 4, -20.0f, 15.0f), 0);
	CHECK_LONG(vec_gradient_sample(&gradient, ramp, 4, -10.0f, 0.0f), 0xff000040U);

	/* A conic gradient: up is the first texel, down the third. */
	gradient = make_conic(0.0f, 0.0f, 0.0f);
	gradient.stops = stops;
	gradient.count = 2;
	CHECK_LONG(vec_gradient_sample(&gradient, ramp, 4, 0.0f, -5.0f), 0xff000010U);
	CHECK_LONG(vec_gradient_sample(&gradient, ramp, 4, 0.0f, 5.0f), 0xff000030U);

	/* A missing gradient or ramp and a width outside the limits give 0. */
	CHECK_LONG(vec_gradient_sample(NULL, ramp, 4, 0.0f, 0.0f), 0);
	CHECK_LONG(vec_gradient_sample(&gradient, NULL, 4, 0.0f, 0.0f), 0);
	CHECK_LONG(vec_gradient_sample(&gradient, ramp, 1, 0.0f, 0.0f), 0);
	CHECK_LONG(vec_gradient_sample(&gradient, ramp, VEC_RAMP_WIDTH_MAX + 1, 0.0f, 0.0f), 0);
}

/* Tests the line length. */
static void
test_line_length(void)
{
	struct vec_stop stops[2];
	struct vec_gradient gradient;
	struct vec_rect area;
	struct vec_matrix matrix;
	float length;

	stops[0] = make_stop(0.0f, RED, 1);
	stops[1] = make_stop(1.0f, BLUE, 1);
	area.left = 0.0f;
	area.top = 0.0f;
	area.right = 6.0f;
	area.bottom = 8.0f;

	/* Linear: the distance from start to end; a gradient-space scale of 0.5 doubles the device length. */
	gradient = make_linear(stops, 2, VEC_SPREAD_PAD, 1, 0.0f, 0.0f, 3.0f, 4.0f);
	CHECK_NEAR(vec_gradient_line_length(&gradient, &area), 5.0, 1e-5);
	CHECK_NEAR(vec_gradient_line_length(&gradient, NULL), 5.0, 1e-5);
	vec_matrix_scale(&matrix, 0.5f, 0.5f);
	gradient.to_gradient = matrix;
	length = vec_gradient_line_length(&gradient, &area);
	CHECK_NEAR(length, 10.0, 1e-4);

	/* Radial: the distance from focal to center plus the end radius. */
	gradient = make_radial(0.0f, 0.0f, 0.0f, 6.0f, 8.0f, 10.0f);
	CHECK_NEAR(vec_gradient_line_length(&gradient, &area), 20.0, 1e-4);
	gradient.to_gradient = matrix;
	CHECK_NEAR(vec_gradient_line_length(&gradient, &area), 40.0, 1e-3);
	vec_matrix_scale(&matrix, 2.0f, 2.0f);
	gradient.to_gradient = matrix;
	CHECK_NEAR(vec_gradient_line_length(&gradient, &area), 10.0, 1e-4);

	/* Conic: a full turn at the farthest corner of the area (here (6, 8), distance 10), in device pixels. */
	gradient = make_conic(0.0f, 0.0f, 0.0f);
	CHECK_NEAR(vec_gradient_line_length(&gradient, &area), 2.0 * 3.14159265358979 * 10.0, 1e-3);
	vec_matrix_scale(&matrix, 0.5f, 0.5f);
	gradient.to_gradient = matrix;
	CHECK_NEAR(vec_gradient_line_length(&gradient, &area), 2.0 * 3.14159265358979 * 10.0, 1e-3);

	/* A conic gradient needs the area. */
	CHECK_NEAR(vec_gradient_line_length(&gradient, NULL), 0.0, 0.0);

	/* A matrix that cannot be inverted, numbers that are not finite and a missing gradient give 0. */
	gradient = make_linear(stops, 2, VEC_SPREAD_PAD, 1, 0.0f, 0.0f, 3.0f, 4.0f);
	gradient.to_gradient.a = 0.0f;
	gradient.to_gradient.d = 0.0f;
	CHECK_NEAR(vec_gradient_line_length(&gradient, &area), 0.0, 0.0);
	gradient = make_linear(stops, 2, VEC_SPREAD_PAD, 1, 0.0f, 0.0f, NAN, 4.0f);
	CHECK_NEAR(vec_gradient_line_length(&gradient, &area), 0.0, 0.0);
	CHECK_NEAR(vec_gradient_line_length(NULL, &area), 0.0, 0.0);
}

/* Fills a random color. */
static uint32_t
random_color(
	uint32_t *state)
{
	return check_random(state);
}

/* Checks the invariants of a color: no color channel above alpha. */
static int
color_is_premultiplied(
	uint32_t color)
{
	uint32_t alpha;

	alpha = (color >> 24) & 0xffU;
	if (((color >> 16) & 0xffU) > alpha)
		return 0;
	if (((color >> 8) & 0xffU) > alpha)
		return 0;
	if ((color & 0xffU) > alpha)
		return 0;
	return 1;
}

/* Runs random gradients through every function under the sanitizers. */
static void
test_random(void)
{
	uint32_t state;
	struct vec_stop stops[RANDOM_STOPS + 2];
	struct vec_gradient gradient;
	struct vec_rect area;
	uint32_t ramp[64];
	size_t count;
	size_t n;
	size_t i;
	int g;
	int p;
	int premultiplied;
	int kind;
	int error;
	int painted;
	int width;
	float start;
	float length;
	float t;
	float x;
	float y;
	uint32_t color;
	uint32_t sampled;
	int bad;

	state = 0x2545f491U;
	for (g = 0; g < RANDOM_GRADIENTS; g++) {
		/* Random stops: positions given or not, some hints. */
		n = 1 + (size_t)(check_random(&state) % RANDOM_STOPS);
		count = 0;
		for (i = 0; i < n; i++) {
			if (i > 0 && i + 1 < n && count > 0 && !stops[count - 1].is_hint && check_random(&state) % 5 == 0) {
				stops[count] = make_hint(check_random_float(&state, -0.5f, 1.5f));
			} else {
				stops[count] = make_stop(check_random_float(&state, -0.5f, 1.5f), random_color(&state), check_random(&state) % 3 != 0);
			}
			count++;
		}

		/* Keeps the grammar: the last entry is a stop. */
		if (stops[count - 1].is_hint)
			stops[count - 1] = make_stop(0.5f, random_color(&state), 0);

		/* Fixes up, and checks the result's invariants. */
		error = vec_gradient_fixup_css(stops, &count);
		CHECK_LONG(error, 0);
		if (error != 0)
			continue;
		bad = 0;
		for (i = 0; i < count; i++) {
			if (!stops[i].has_offset || stops[i].is_hint)
				bad = 1;
			if (i > 0 && stops[i].offset < stops[i - 1].offset)
				bad = 1;
			if (stops[i].has_hint && (stops[i].hint < 0.0f || stops[i].hint > 1.0f))
				bad = 1;
		}
		CHECK(!bad);

		/* Maps the span, or the repeat span. */
		premultiplied = (int)(check_random(&state) % 2);
		kind = (int)(check_random(&state) % 3);
		if (check_random(&state) % 2 == 0) {
			error = vec_gradient_extend_span(stops, count, &start, &length);
			CHECK_LONG(error, 0);
		} else {
			error = vec_gradient_repeat_span(stops, count, &start, &length);
			CHECK(error == 0 || error == ENOTSUP);
		}

		/* Colors along the line: no color channel above alpha. */
		for (p = 0; p < RANDOM_POINTS; p++) {
			color = vec_gradient_color_at(stops, count, premultiplied, check_random_float(&state, -0.2f, 1.2f));
			CHECK(color_is_premultiplied(color));
		}
		color = vec_gradient_average(stops, count, premultiplied);
		CHECK(color_is_premultiplied(color));

		/* A random gradient of a random kind. */
		memset(&gradient, 0, sizeof(gradient));
		gradient.kind = (enum vec_gradient_kind)kind;
		gradient.spread = (enum vec_spread)(check_random(&state) % 3);
		gradient.premultiplied = premultiplied;
		gradient.stops = stops;
		gradient.count = count;
		gradient.to_gradient.a = check_random_float(&state, -3.0f, 3.0f);
		gradient.to_gradient.b = check_random_float(&state, -1.0f, 1.0f);
		gradient.to_gradient.c = check_random_float(&state, -1.0f, 1.0f);
		gradient.to_gradient.d = check_random_float(&state, -3.0f, 3.0f);
		gradient.to_gradient.e = check_random_float(&state, -50.0f, 50.0f);
		gradient.to_gradient.f = check_random_float(&state, -50.0f, 50.0f);
		gradient.start.x = check_random_float(&state, -50.0f, 50.0f);
		gradient.start.y = check_random_float(&state, -50.0f, 50.0f);
		gradient.end.x = check_random_float(&state, -50.0f, 50.0f);
		gradient.end.y = check_random_float(&state, -50.0f, 50.0f);
		gradient.focal.x = check_random_float(&state, -50.0f, 50.0f);
		gradient.focal.y = check_random_float(&state, -50.0f, 50.0f);
		gradient.focal_radius = check_random_float(&state, 0.0f, 40.0f);
		gradient.center.x = check_random_float(&state, -50.0f, 50.0f);
		gradient.center.y = check_random_float(&state, -50.0f, 50.0f);
		gradient.radius = check_random_float(&state, 0.0f, 60.0f);
		gradient.start_angle = check_random_float(&state, -10.0f, 10.0f);

		/* The line length, the width and the ramp. */
		area.left = -20.0f;
		area.top = -20.0f;
		area.right = 70.0f;
		area.bottom = 60.0f;
		length = vec_gradient_line_length(&gradient, &area);
		CHECK(vec_finite(length) && length >= 0.0f);
		width = vec_gradient_ramp_width(length);
		CHECK(width >= VEC_RAMP_WIDTH_MIN && width <= VEC_RAMP_WIDTH_MAX);
		if (width > 64)
			width = 64;
		error = vec_gradient_ramp(&gradient, width, ramp);
		CHECK_LONG(error, 0);

		/* Samples at random points: the color is premultiplied, and a painted radial point is on its circle. */
		for (p = 0; p < RANDOM_POINTS; p++) {
			x = check_random_float(&state, -100.0f, 150.0f);
			y = check_random_float(&state, -100.0f, 150.0f);
			t = vec_gradient_parameter(&gradient, x, y, &painted);
			CHECK(vec_finite(t));
			sampled = vec_gradient_sample(&gradient, ramp, width, x, y);
			CHECK(color_is_premultiplied(sampled));
			if (!painted)
				CHECK_LONG(sampled, 0);
			CHECK(vec_gradient_spread(gradient.spread, t) >= 0.0f);
			CHECK(vec_gradient_spread(gradient.spread, t) <= 1.0f);
		}
	}

	/* Hostile stops given straight to the evaluators: offsets that are not numbers or out of order, odd hints. */
	for (g = 0; g < 500; g++) {
		count = 1 + (size_t)(check_random(&state) % RANDOM_STOPS);
		for (i = 0; i < count; i++) {
			stops[i] = make_stop(check_random_float(&state, -2.0f, 3.0f), random_color(&state), 1);
			if (check_random(&state) % 7 == 0)
				stops[i].offset = NAN;
			if (check_random(&state) % 11 == 0)
				stops[i].offset = INFINITY;
			if (check_random(&state) % 4 == 0) {
				stops[i].has_hint = 1;
				stops[i].hint = check_random_float(&state, -1.0f, 2.0f);
				if (check_random(&state) % 5 == 0)
					stops[i].hint = NAN;
			}
		}
		for (p = 0; p < 8; p++) {
			color = vec_gradient_color_at(stops, count, p % 2, check_random_float(&state, -1.0f, 2.0f));
			CHECK(color_is_premultiplied(color));
		}
		color = vec_gradient_average(stops, count, g % 2);
		CHECK(color_is_premultiplied(color));
	}
}

int
main(void)
{
	test_fixup_css();
	test_fixup_hints();
	test_fixup_errors();
	test_fixup_svg();
	test_spans();
	test_color_at();
	test_hint();
	test_average();
	test_ramp();
	test_parameter_linear();
	test_parameter_radial();
	test_parameter_conic();
	test_parameter_invalid();
	test_spread();
	test_sample();
	test_line_length();
	test_random();

	return check_finish("gradient");
}
