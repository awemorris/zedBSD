/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Draws a linear gradient (https://drafts.csswg.org/css-images-3/#linear-gradients)
 * into a bitmap the size of its tile: each pixel takes the color at its
 * centre's place along the gradient line, mixed between the two stops around
 * it with premultiplied alpha.
 */

#include "paint/paint.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* The ratio of the half turn's radians to degrees. */
#define GRADIENT_RADIANS_PER_DEGREE 0.017453292519943295f

static void gradient_positions(const struct css_gradient *gradient, float line, float *at);
static uint32_t gradient_color(const struct css_gradient *gradient, const float *at, float t);

/*
 * Fills a bitmap of width by height pixels (its pixels in the arena) with the gradient.
 */
int
paint_gradient_render(
	struct wb_arena *arena,
	const struct css_gradient *gradient,
	int width,
	int height,
	struct img_bitmap *bitmap)
{
	float at[CSS_GRADIENT_STOPS];
	float direction_x;
	float direction_y;
	float norm;
	float radians;
	float line;
	float t;
	size_t count;
	int x;
	int y;

	/* The pixels. */
	count = (size_t)width * (size_t)height;
	if (width <= 0 || height <= 0 || count > IMG_PIXELS_MAX)
		return EINVAL;
	bitmap->pixels = wb_arena_alloc(arena, count * sizeof(uint32_t));
	if (bitmap->pixels == NULL)
		return ENOMEM;
	bitmap->width = width;
	bitmap->height = height;
	img_bitmap_renew(bitmap);

	/* The direction of the gradient line, pointing where the color goes to 100%. */
	if (gradient->corner == 0) {
		radians = gradient->angle * GRADIENT_RADIANS_PER_DEGREE;
		direction_x = sinf(radians);
		direction_y = -cosf(radians);
	} else if ((gradient->corner & (CSS_GRADIENT_TOP | CSS_GRADIENT_BOTTOM)) == 0) {
		direction_x = (gradient->corner & CSS_GRADIENT_RIGHT) != 0 ? 1.0f : -1.0f;
		direction_y = 0.0f;
	} else if ((gradient->corner & (CSS_GRADIENT_LEFT | CSS_GRADIENT_RIGHT)) == 0) {
		direction_x = 0.0f;
		direction_y = (gradient->corner & CSS_GRADIENT_BOTTOM) != 0 ? 1.0f : -1.0f;
	} else {
		/* A corner: the 50% line passes through the other two corners. */
		norm = sqrtf((float)width * (float)width + (float)height * (float)height);
		direction_x = ((gradient->corner & CSS_GRADIENT_RIGHT) != 0 ? 1.0f : -1.0f) * (float)height / norm;
		direction_y = ((gradient->corner & CSS_GRADIENT_BOTTOM) != 0 ? 1.0f : -1.0f) * (float)width / norm;
	}

	/* The line's length: the extent of the box along it. */
	line = fabsf((float)width * direction_x) + fabsf((float)height * direction_y);
	if (line <= 0.0f)
		line = 1.0f;
	gradient_positions(gradient, line, at);

	/* Each pixel's centre. */
	for (y = 0; y < height; y++) {
		for (x = 0; x < width; x++) {
			t = (((float)x + 0.5f - (float)width / 2.0f) * direction_x + ((float)y + 0.5f - (float)height / 2.0f) * direction_y) / line + 0.5f;
			bitmap->pixels[(size_t)y * (size_t)width + (size_t)x] = gradient_color(gradient, at, t);
		}
	}

	/* Succeeded: the bitmap is drawn. */
	return 0;
}

/* Works out where each stop is along the line (0 to 1): the ones not given spread evenly, and none goes back. */
static void
gradient_positions(
	const struct css_gradient *gradient,
	float line,
	float *at)
{
	unsigned index;
	unsigned from;
	unsigned to;
	unsigned between;
	float highest;

	/* The positions given, as fractions of the line. */
	for (index = 0; index < gradient->count; index++) {
		at[index] = -1.0f;
		if (gradient->stops[index].unit == CSS_GRADIENT_AT_PX)
			at[index] = gradient->stops[index].position / line;
		if (gradient->stops[index].unit == CSS_GRADIENT_AT_PERCENT)
			at[index] = gradient->stops[index].position / 100.0f;
	}

	/* The first and the last default to the ends. */
	if (gradient->stops[0].unit == CSS_GRADIENT_AT_AUTO)
		at[0] = 0.0f;
	if (gradient->stops[gradient->count - 1U].unit == CSS_GRADIENT_AT_AUTO)
		at[gradient->count - 1U] = 1.0f;

	/* No stop is before the one before it. */
	highest = at[0];
	for (index = 1; index < gradient->count; index++) {
		if (gradient->stops[index].unit == CSS_GRADIENT_AT_AUTO)
			continue;
		if (at[index] < highest)
			at[index] = highest;
		highest = at[index];
	}

	/* A run of stops without positions is spread evenly between the ones around it. */
	from = 0;
	while (from + 1U < gradient->count) {
		to = from + 1U;
		while (to < gradient->count && gradient->stops[to].unit == CSS_GRADIENT_AT_AUTO && to != gradient->count - 1U)
			to++;
		for (between = from + 1U; between < to; between++)
			at[between] = at[from] + (at[to] - at[from]) * (float)(between - from) / (float)(to - from);
		from = to;
	}
}

/* The color at a place along the line. */
static uint32_t
gradient_color(
	const struct css_gradient *gradient,
	const float *at,
	float t)
{
	const struct css_gradient_stop *low;
	const struct css_gradient_stop *high;
	float low_alpha;
	float high_alpha;
	float share;
	float alpha;
	float channel[3];
	unsigned index;
	unsigned shift;
	unsigned out;
	float a;
	float b;

	/* A repeating gradient starts over after its last stop. */
	if (gradient->repeating != 0 && at[gradient->count - 1U] > at[0]) {
		t = (t - at[0]) / (at[gradient->count - 1U] - at[0]);
		t = t - floorf(t);
		t = at[0] + t * (at[gradient->count - 1U] - at[0]);
	}

	/* Before the first and after the last stop, their colors. */
	if (t <= at[0])
		return gradient->stops[0].color;
	if (t >= at[gradient->count - 1U])
		return gradient->stops[gradient->count - 1U].color;

	/* The two stops around it. */
	for (index = 1; index < gradient->count; index++) {
		if (t < at[index])
			break;
	}
	low = &gradient->stops[index - 1U];
	high = &gradient->stops[index];
	share = 0.0f;
	if (at[index] > at[index - 1U])
		share = (t - at[index - 1U]) / (at[index] - at[index - 1U]);

	/* Mixed with premultiplied alpha. */
	low_alpha = (float)(low->color >> 24) / 255.0f;
	high_alpha = (float)(high->color >> 24) / 255.0f;
	alpha = low_alpha + (high_alpha - low_alpha) * share;
	out = 0;
	for (shift = 0; shift < 3U; shift++) {
		a = (float)((low->color >> (shift * 8U)) & 0xffU) * low_alpha;
		b = (float)((high->color >> (shift * 8U)) & 0xffU) * high_alpha;
		channel[shift] = a + (b - a) * share;
		if (alpha > 0.0f)
			channel[shift] /= alpha;
		if (channel[shift] > 255.0f)
			channel[shift] = 255.0f;
		out |= (unsigned)(channel[shift] + 0.5f) << (shift * 8U);
	}
	return ((uint32_t)(alpha * 255.0f + 0.5f) << 24) | out;
}
