/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The scale of the X server's Windows backend: the rounding of sizes,
 * places and the pointer, and the copy of X's pixels to the screen's.
 */

#include "userland/desktop/xserver/win32/scale.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* The fixed point of the filter's positions: 16 bits of fraction. */
#define SCALE_FIXED_SHIFT	16U
#define SCALE_FIXED_HALF	0x8000

/* The filter's weights: 8 bits, 256 being the whole. */
#define SCALE_WEIGHT_SHIFT	8U
#define SCALE_WEIGHT_ONE	256U

/* The word "auto": the scale of the screen's DPI. */
#define SCALE_AUTO		"auto"

static int64_t scale_floor_divide(int64_t numerator, int64_t denominator);
static uint32_t scale_mix(uint32_t first, uint32_t second, uint32_t weight);
static int32_t scale_clamp(int64_t value, int32_t low, int32_t high);

/*
 * Gives the scale of a screen's DPI: 96 DPI is 100 %, and the result is
 * rounded to the nearest step of 25 % and kept between 100 and 400 %
 * (Windows' display settings offer 100, 125, 150, 175 % and so on, which
 * this gives back exactly).
 */
unsigned
x11_win32_scale_of_dpi(
	unsigned dpi)
{
	unsigned percent;
	unsigned steps;

	/* Turns the DPI into a percent of the default, rounded. */
	percent = (dpi * 100U + X11_WIN32_DPI_DEFAULT / 2U) / X11_WIN32_DPI_DEFAULT;

	/* Rounds the percent to the nearest step. */
	steps = (percent + X11_WIN32_SCALE_STEP / 2U) / X11_WIN32_SCALE_STEP;
	percent = steps * X11_WIN32_SCALE_STEP;

	/* Keeps the scale in the range a server has. */
	if (percent < X11_WIN32_SCALE_MIN)
		return X11_WIN32_SCALE_MIN;
	if (percent > X11_WIN32_SCALE_MAX)
		return X11_WIN32_SCALE_MAX;

	/* Succeeded: the scale of the DPI. */
	return percent;
}

/*
 * Reads a scale from the settings: "auto" (the scale of the DPI given) or
 * a percent (digits, an optional "%" after them) that is a step of 25
 * from 100 to 400.  Returns 0 with *percent set, or EINVAL.
 */
int
x11_win32_scale_parse(
	const char *text,
	unsigned dpi,
	unsigned *percent)
{
	unsigned value;
	size_t digits;
	int same;

	/* "auto" takes the scale of the screen. */
	same = strcmp(text, SCALE_AUTO);
	if (same == 0) {
		*percent = x11_win32_scale_of_dpi(dpi);
		return 0;
	}

	/* Reads the digits, refusing a number too long to be a scale. */
	value = 0;
	digits = 0;
	while (text[digits] >= '0' && text[digits] <= '9') {
		if (digits == 3)
			return EINVAL;

		value = value * 10U + (unsigned)(text[digits] - '0');
		digits++;
	}

	/* Refuses text that is not a number with an optional percent sign. */
	if (digits == 0)
		return EINVAL;
	if (text[digits] == '%')
		digits++;
	if (text[digits] != '\0')
		return EINVAL;

	/* Refuses a scale outside the range or between two steps. */
	if (value < X11_WIN32_SCALE_MIN || value > X11_WIN32_SCALE_MAX)
		return EINVAL;
	if (value % X11_WIN32_SCALE_STEP != 0)
		return EINVAL;

	/* Succeeded: the scale named. */
	*percent = value;
	return 0;
}

/*
 * Tells whether a scale is a whole number of pixels (100, 200, 300 or
 * 400 %), which is copied pixel by pixel rather than filtered.
 */
int
x11_win32_scale_is_whole(
	unsigned percent)
{
	/* A multiple of 100 % is whole. */
	if (percent % 100U == 0)
		return 1;

	/* Any other scale is filtered. */
	return 0;
}

/*
 * Gives the screen size of an X size: rounded to the nearest pixel, and
 * at least one pixel for a size that is not zero.
 */
unsigned
x11_win32_scale_size(
	unsigned size,
	unsigned percent)
{
	uint64_t scaled;

	/* Scales and rounds the size. */
	scaled = ((uint64_t)size * percent + 50U) / 100U;

	/* Keeps a window that has pixels at one pixel at least. */
	if (scaled == 0 && size != 0)
		scaled = 1;

	/* Keeps the result in an unsigned. */
	if (scaled > 0xFFFFFFFFU)
		return 0xFFFFFFFFU;

	/* Succeeded: the screen size. */
	return (unsigned)scaled;
}

/*
 * Gives the screen position of an X position (a place on the root
 * window), rounded to the nearest pixel the same way on both sides of
 * zero (the virtual screen has negative places left of the main monitor).
 */
int32_t
x11_win32_scale_to_screen(
	int32_t position,
	unsigned percent)
{
	int64_t scaled;
	int32_t result;

	/* Scales the position and rounds it, half a pixel up. */
	scaled = scale_floor_divide((int64_t)position * percent + 50, 100);

	/* Keeps the result in an int32. */
	result = scale_clamp(scaled, INT32_MIN, INT32_MAX);
	return result;
}

/*
 * Gives the X position of a screen position: the X pixel the screen pixel
 * falls in (rounded down, also below zero).  A screen pixel of a window's
 * client area always falls in an X pixel of the window, whose screen size
 * x11_win32_scale_size gave.
 */
int32_t
x11_win32_scale_to_x(
	int32_t position,
	unsigned percent)
{
	int64_t scaled;
	int32_t result;

	/* Divides out the scale, rounding down. */
	scaled = scale_floor_divide((int64_t)position * 100, (int64_t)percent);

	/* Keeps the result in an int32. */
	result = scale_clamp(scaled, INT32_MIN, INT32_MAX);
	return result;
}

/*
 * Gives the screen pixels a changed box of X pixels touches when an image
 * of source_width by source_height is shown at width by height: the box
 * scaled out to whole pixels and one pixel more on each side (the filter
 * reads the neighbours), within the image.
 */
void
x11_win32_scale_box(
	const struct x11_win32_box *source,
	unsigned source_width,
	unsigned source_height,
	unsigned width,
	unsigned height,
	struct x11_win32_box *result)
{
	int64_t first;
	int64_t last;

	/* An empty image touches nothing. */
	if (source_width == 0 || source_height == 0) {
		memset(result, 0, sizeof(*result));
		return;
	}

	/* Scales the left and right edges out and widens them by a pixel. */
	first = scale_floor_divide((int64_t)source->x0 * width, source_width) - 1;
	last = -scale_floor_divide(-(int64_t)source->x1 * width, source_width) + 1;
	result->x0 = scale_clamp(first, 0, (int32_t)width);
	result->x1 = scale_clamp(last, 0, (int32_t)width);

	/* Scales the top and bottom edges the same way. */
	first = scale_floor_divide((int64_t)source->y0 * height, source_height) - 1;
	last = -scale_floor_divide(-(int64_t)source->y1 * height, source_height) + 1;
	result->y0 = scale_clamp(first, 0, (int32_t)height);
	result->y1 = scale_clamp(last, 0, (int32_t)height);
}

/*
 * Copies an image of X's pixels to the screen's at another size.
 *
 * source is source_width by source_height pixels, rows source_stride
 * pixels apart; pixels is width by height, rows stride pixels apart.
 * Only the screen pixels of box are written.  whole copies each screen
 * pixel from the X pixel its centre falls in (nearest); otherwise each is
 * the bilinear blend of the four X pixels around its centre, the edges
 * repeated.  Both images are 32-bit pixels whose four bytes are blended
 * alike.
 */
void
x11_win32_scale_pixels(
	const uint32_t *source,
	size_t source_stride,
	unsigned source_width,
	unsigned source_height,
	uint32_t *pixels,
	size_t stride,
	unsigned width,
	unsigned height,
	int whole,
	const struct x11_win32_box *box)
{
	int32_t x;
	int32_t y;
	int64_t position_x;
	int64_t position_y;
	int64_t last_x;
	int64_t last_y;
	uint32_t column;
	uint32_t row;
	uint32_t next_column;
	uint32_t next_row;
	uint32_t weight_x;
	uint32_t weight_y;
	uint32_t top;
	uint32_t bottom;
	const uint32_t *line;
	const uint32_t *next_line;

	/* Nothing is drawn from or to an empty image. */
	if (source_width == 0 || source_height == 0 || width == 0 || height == 0)
		return;

	/* The last X pixel's position in the filter's fixed point. */
	last_x = (int64_t)(source_width - 1U) << SCALE_FIXED_SHIFT;
	last_y = (int64_t)(source_height - 1U) << SCALE_FIXED_SHIFT;

	/* Fills each row of the box. */
	for (y = box->y0; y < box->y1; y++) {
		/* The nearest copy: the X row the screen row's centre falls in. */
		if (whole) {
			row = (uint32_t)(((int64_t)(2 * y + 1) * source_height) / (2 * (int64_t)height));
			line = source + (size_t)row * source_stride;

			/* Copies each pixel of the row from the X pixel its centre falls in. */
			for (x = box->x0; x < box->x1; x++) {
				column = (uint32_t)(((int64_t)(2 * x + 1) * source_width) / (2 * (int64_t)width));
				pixels[(size_t)y * stride + (size_t)x] = line[column];
			}
			continue;
		}

		/* The filtered copy: the X rows above and below the screen row's centre. */
		position_y = ((int64_t)(2 * y + 1) * source_height << SCALE_FIXED_SHIFT) / (2 * (int64_t)height) - SCALE_FIXED_HALF;
		if (position_y < 0)
			position_y = 0;
		if (position_y > last_y)
			position_y = last_y;
		row = (uint32_t)(position_y >> SCALE_FIXED_SHIFT);
		next_row = row;
		if (row + 1U < source_height)
			next_row = row + 1U;
		weight_y = (uint32_t)(position_y & 0xFFFF) >> SCALE_WEIGHT_SHIFT;
		line = source + (size_t)row * source_stride;
		next_line = source + (size_t)next_row * source_stride;

		/* Blends each pixel of the row from the four X pixels around its centre. */
		for (x = box->x0; x < box->x1; x++) {
			position_x = ((int64_t)(2 * x + 1) * source_width << SCALE_FIXED_SHIFT) / (2 * (int64_t)width) - SCALE_FIXED_HALF;
			if (position_x < 0)
				position_x = 0;
			if (position_x > last_x)
				position_x = last_x;
			column = (uint32_t)(position_x >> SCALE_FIXED_SHIFT);
			next_column = column;
			if (column + 1U < source_width)
				next_column = column + 1U;
			weight_x = (uint32_t)(position_x & 0xFFFF) >> SCALE_WEIGHT_SHIFT;

			top = scale_mix(line[column], line[next_column], weight_x);
			bottom = scale_mix(next_line[column], next_line[next_column], weight_x);
			pixels[(size_t)y * stride + (size_t)x] = scale_mix(top, bottom, weight_y);
		}
	}
}

/* Divides, rounding toward minus infinity (the denominator is positive). */
static int64_t
scale_floor_divide(
	int64_t numerator,
	int64_t denominator)
{
	int64_t quotient;

	/* C rounds toward zero; a negative quotient with a remainder is one lower. */
	quotient = numerator / denominator;
	if (numerator % denominator != 0 && numerator < 0)
		quotient--;

	/* Succeeded: the rounded-down quotient. */
	return quotient;
}

/* Blends two pixels byte by byte, weight (0 to 255) of the second. */
static uint32_t
scale_mix(
	uint32_t first,
	uint32_t second,
	uint32_t weight)
{
	uint32_t result;
	uint32_t shift;
	uint32_t a;
	uint32_t b;

	/* Blends each of the four bytes. */
	result = 0;
	for (shift = 0; shift < 32U; shift += 8U) {
		a = (first >> shift) & 0xFFU;
		b = (second >> shift) & 0xFFU;
		result |= (((a * (SCALE_WEIGHT_ONE - weight) + b * weight) >> SCALE_WEIGHT_SHIFT) & 0xFFU) << shift;
	}

	/* Succeeded: the blended pixel. */
	return result;
}

/* Keeps a value between two bounds. */
static int32_t
scale_clamp(
	int64_t value,
	int32_t low,
	int32_t high)
{
	/* A value below the range is its low bound. */
	if (value < low)
		return low;

	/* A value above the range is its high bound. */
	if (value > high)
		return high;

	/* A value in the range is kept. */
	return (int32_t)value;
}
