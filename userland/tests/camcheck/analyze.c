/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The reading of the test pattern in a captured frame (analyze.h).
 *
 * The YUV conversions are the textbook ones of ITU-R BT.601 and BT.709:
 * Y' = Kr R + Kg G + Kb B, Cb and Cr the scaled differences of B and R
 * from Y', limited range putting Y in 16 to 235 and Cb, Cr in 16 to 240
 * (Value(s) referenced: ITU-R BT.601-7 and BT.709-6, the Kr and Kb
 * constants and the 219 and 224 quantization steps).
 */

#include "userland/tests/camcheck/analyze.h"

#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>

/* The fraction of a part kept when it is sampled: away from its edges, where scaling blurs. */
#define CAMCHECK_PATCH_INNER_START	0.25
#define CAMCHECK_PATCH_INNER_END	0.75
#define CAMCHECK_COUNTER_INNER_START	0.30
#define CAMCHECK_COUNTER_INNER_END	0.70

/* The quantization of limited range: Y over 219 steps from 16, Cb and Cr over 224 around 128. */
#define CAMCHECK_LIMITED_Y_OFFSET	16.0
#define CAMCHECK_LIMITED_Y_STEPS	219.0
#define CAMCHECK_LIMITED_C_STEPS	224.0
#define CAMCHECK_C_CENTRE		128.0
#define CAMCHECK_FULL_STEPS		255.0

/* The white of limited range. */
#define CAMCHECK_LIMITED_WHITE		235.0

/* The grey patch is drawn as 128 of 255. */
#define CAMCHECK_GREY			128.0

/* The counter's values wrap at 16 bits; a step of half that or more is taken as going down. */
#define CAMCHECK_COUNTER_MASK		0xffffU
#define CAMCHECK_COUNTER_BACKWARD	0x8000U

/*
 * The colours the pattern draws its patches with, left to right (R, G, B).
 */
static const double camcheck_patch_colours[CAMCHECK_PATCHES][3] = {
	{ 255.0, 0.0, 0.0 },
	{ 0.0, 255.0, 0.0 },
	{ 0.0, 0.0, 255.0 },
	{ 255.0, 255.0, 255.0 },
	{ 0.0, 0.0, 0.0 },
	{ CAMCHECK_GREY, CAMCHECK_GREY, CAMCHECK_GREY },
	{ 255.0, 255.0, 0.0 },
	{ 0.0, 255.0, 255.0 },
	{ 255.0, 0.0, 255.0 }
};

static void camcheck_constants(uint32_t matrix, double *kr, double *kb);
static int camcheck_bounds(uint32_t width, uint32_t height, const struct camcheck_region *region, uint32_t *x0, uint32_t *y0, uint32_t *x1, uint32_t *y1);
static double camcheck_matrix_distance(const struct camcheck_sample *patches, uint32_t matrix, uint32_t range);

/*
 * Gives the inner half of a patch (index 0 to 8), away from its edges.
 */
void
camcheck_patch_region(
	unsigned index,
	struct camcheck_region *region)
{
	double height;

	/* The patch's column, its middle half. */
	region->left = ((double)index + CAMCHECK_PATCH_INNER_START) / (double)CAMCHECK_PATCHES;
	region->right = ((double)index + CAMCHECK_PATCH_INNER_END) / (double)CAMCHECK_PATCHES;

	/* The band's middle half. */
	height = CAMCHECK_PATCH_BOTTOM - CAMCHECK_PATCH_TOP;
	region->top = CAMCHECK_PATCH_TOP + height * CAMCHECK_PATCH_INNER_START;
	region->bottom = CAMCHECK_PATCH_TOP + height * CAMCHECK_PATCH_INNER_END;
}

/*
 * Gives the inner part of a counter square (bit 0 to 15, 0 the most significant).
 */
void
camcheck_counter_region(
	unsigned bit,
	struct camcheck_region *region)
{
	double height;

	/* The square's column, its middle. */
	region->left = ((double)bit + CAMCHECK_COUNTER_INNER_START) / (double)CAMCHECK_COUNTER_BITS;
	region->right = ((double)bit + CAMCHECK_COUNTER_INNER_END) / (double)CAMCHECK_COUNTER_BITS;

	/* The band's middle. */
	height = CAMCHECK_COUNTER_BOTTOM - CAMCHECK_COUNTER_TOP;
	region->top = CAMCHECK_COUNTER_TOP + height * CAMCHECK_COUNTER_INNER_START;
	region->bottom = CAMCHECK_COUNTER_TOP + height * CAMCHECK_COUNTER_INNER_END;
}

/*
 * Averages Y, Cb and Cr of a YUYV frame over a region.  Each pair of
 * pixels shares the Cb and Cr of its four bytes (Y0 Cb Y1 Cr).
 */
int
camcheck_sample_yuyv(
	const uint8_t *frame,
	uint32_t width,
	uint32_t height,
	uint32_t stride,
	const struct camcheck_region *region,
	struct camcheck_sample *sample)
{
	const uint8_t *line;
	const uint8_t *pair;
	double sums[3];
	uint64_t pixels;
	uint32_t x0;
	uint32_t y0;
	uint32_t x1;
	uint32_t y1;
	uint32_t x;
	uint32_t y;
	int error;

	/* A frame whose lines hold its pixels, and the region in pixels. */
	if (frame == NULL ||
	    sample == NULL ||
	    stride < width * 2U)
		return EINVAL;
	error = camcheck_bounds(width, height, region, &x0, &y0, &x1, &y1);
	if (error != 0)
		return error;

	/* Every pixel of the region, adding its Y and its pair's Cb and Cr. */
	sums[0] = 0.0;
	sums[1] = 0.0;
	sums[2] = 0.0;
	pixels = 0U;
	for (y = y0; y < y1; y++) {
		line = frame + (size_t)y * stride;
		for (x = x0; x < x1; x++) {
			pair = line + (size_t)(x & ~1U) * 2U;
			sums[0] += (double)line[(size_t)x * 2U];
			sums[1] += (double)pair[1];
			sums[2] += (double)pair[3];
			pixels++;
		}
	}

	/* The averages. */
	sample->value[0] = sums[0] / (double)pixels;
	sample->value[1] = sums[1] / (double)pixels;
	sample->value[2] = sums[2] / (double)pixels;

	/* Succeeded: the sample holds the region's average. */
	return 0;
}

/*
 * Averages R, G and B of a packed RGB frame over a region.
 */
int
camcheck_sample_rgb(
	const uint8_t *rgb,
	uint32_t width,
	uint32_t height,
	const struct camcheck_region *region,
	struct camcheck_sample *sample)
{
	const uint8_t *pixel;
	double sums[3];
	uint64_t pixels;
	uint32_t x0;
	uint32_t y0;
	uint32_t x1;
	uint32_t y1;
	uint32_t x;
	uint32_t y;
	int error;

	/* A frame, and the region in pixels. */
	if (rgb == NULL || sample == NULL)
		return EINVAL;
	error = camcheck_bounds(width, height, region, &x0, &y0, &x1, &y1);
	if (error != 0)
		return error;

	/* Every pixel of the region. */
	sums[0] = 0.0;
	sums[1] = 0.0;
	sums[2] = 0.0;
	pixels = 0U;
	for (y = y0; y < y1; y++) {
		for (x = x0; x < x1; x++) {
			pixel = rgb + ((size_t)y * width + x) * 3U;
			sums[0] += (double)pixel[0];
			sums[1] += (double)pixel[1];
			sums[2] += (double)pixel[2];
			pixels++;
		}
	}

	/* The averages. */
	sample->value[0] = sums[0] / (double)pixels;
	sample->value[1] = sums[1] / (double)pixels;
	sample->value[2] = sums[2] / (double)pixels;

	/* Succeeded: the sample holds the region's average. */
	return 0;
}

/*
 * Gives the Y, Cb and Cr an RGB colour has in one matrix and range.
 */
void
camcheck_rgb_to_yuv(
	const double *rgb,
	uint32_t matrix,
	uint32_t range,
	double *yuv)
{
	double luma;
	double blue;
	double red;
	double kr;
	double kb;

	/* Y' and the two differences, full scale. */
	camcheck_constants(matrix, &kr, &kb);
	luma = kr * rgb[0] + (1.0 - kr - kb) * rgb[1] + kb * rgb[2];
	blue = (rgb[2] - luma) / (2.0 * (1.0 - kb));
	red = (rgb[0] - luma) / (2.0 * (1.0 - kr));

	/* Limited range: Y from 16 over 219 steps, Cb and Cr over 224. */
	if (range == CAMCHECK_RANGE_LIMITED) {
		yuv[0] = CAMCHECK_LIMITED_Y_OFFSET + luma * CAMCHECK_LIMITED_Y_STEPS / CAMCHECK_FULL_STEPS;
		yuv[1] = CAMCHECK_C_CENTRE + blue * CAMCHECK_LIMITED_C_STEPS / CAMCHECK_FULL_STEPS;
		yuv[2] = CAMCHECK_C_CENTRE + red * CAMCHECK_LIMITED_C_STEPS / CAMCHECK_FULL_STEPS;
		return;
	}

	/* Full range. */
	yuv[0] = luma;
	yuv[1] = CAMCHECK_C_CENTRE + blue;
	yuv[2] = CAMCHECK_C_CENTRE + red;
}

/*
 * Gives the RGB colour of Y, Cb and Cr in one matrix and range.
 */
void
camcheck_yuv_to_rgb(
	const double *yuv,
	uint32_t matrix,
	uint32_t range,
	double *rgb)
{
	double luma;
	double blue;
	double red;
	double kr;
	double kb;

	/* Y' and the two differences, full scale. */
	camcheck_constants(matrix, &kr, &kb);
	if (range == CAMCHECK_RANGE_LIMITED) {
		luma = (yuv[0] - CAMCHECK_LIMITED_Y_OFFSET) * CAMCHECK_FULL_STEPS / CAMCHECK_LIMITED_Y_STEPS;
		blue = (yuv[1] - CAMCHECK_C_CENTRE) * CAMCHECK_FULL_STEPS / CAMCHECK_LIMITED_C_STEPS;
		red = (yuv[2] - CAMCHECK_C_CENTRE) * CAMCHECK_FULL_STEPS / CAMCHECK_LIMITED_C_STEPS;
	} else {
		luma = yuv[0];
		blue = yuv[1] - CAMCHECK_C_CENTRE;
		red = yuv[2] - CAMCHECK_C_CENTRE;
	}

	/* R and B from their differences, G from what Y' leaves. */
	rgb[0] = luma + 2.0 * (1.0 - kr) * red;
	rgb[2] = luma + 2.0 * (1.0 - kb) * blue;
	rgb[1] = (luma - kr * rgb[0] - kb * rgb[2]) / (1.0 - kr - kb);
}

/*
 * Guesses the matrix and range of a YUV frame from its patches: the range
 * whose black and white lie nearer the measured ones, then the matrix
 * whose primaries and secondaries lie nearer in that range.
 */
void
camcheck_estimate(
	const struct camcheck_sample *patches,
	uint32_t *matrix,
	uint32_t *range)
{
	double limited;
	double full;
	double distance601;
	double distance709;
	double black;
	double white;

	/* The range from the black and white patches' Y. */
	black = patches[CAMCHECK_PATCH_BLACK].value[0];
	white = patches[CAMCHECK_PATCH_WHITE].value[0];
	limited = fabs(black - CAMCHECK_LIMITED_Y_OFFSET) + fabs(white - CAMCHECK_LIMITED_WHITE);
	full = fabs(black) + fabs(white - CAMCHECK_FULL_STEPS);
	*range = CAMCHECK_RANGE_FULL;
	if (limited <= full)
		*range = CAMCHECK_RANGE_LIMITED;

	/* The matrix from the coloured patches. */
	distance601 = camcheck_matrix_distance(patches, CAMCHECK_MATRIX_BT601, *range);
	distance709 = camcheck_matrix_distance(patches, CAMCHECK_MATRIX_BT709, *range);
	*matrix = CAMCHECK_MATRIX_BT709;
	if (distance601 <= distance709)
		*matrix = CAMCHECK_MATRIX_BT601;
}

/*
 * Gives the RGB colour the pattern draws a patch with.
 */
void
camcheck_patch_expected(
	unsigned index,
	double *rgb)
{
	/* A patch past the last is black. */
	if (index >= CAMCHECK_PATCHES) {
		rgb[0] = 0.0;
		rgb[1] = 0.0;
		rgb[2] = 0.0;
		return;
	}

	/* The table's colour. */
	rgb[0] = camcheck_patch_colours[index][0];
	rgb[1] = camcheck_patch_colours[index][1];
	rgb[2] = camcheck_patch_colours[index][2];
}

/*
 * Reports the largest difference of any patch's colour, in any of R, G
 * and B, from what the pattern drew, and which patch it was.
 */
double
camcheck_patch_error(
	const struct camcheck_sample *rgb_patches,
	unsigned *worst)
{
	double expected[3];
	double difference;
	double largest;
	unsigned channel;
	unsigned index;

	/* Every channel of every patch. */
	largest = 0.0;
	*worst = 0U;
	for (index = 0U; index < CAMCHECK_PATCHES; index++) {
		camcheck_patch_expected(index, expected);
		for (channel = 0U; channel < 3U; channel++) {
			difference = fabs(rgb_patches[index].value[channel] - expected[channel]);
			if (difference > largest) {
				largest = difference;
				*worst = index;
			}
		}
	}

	/* Succeeded: the largest difference. */
	return largest;
}

/*
 * Reads the counter from its squares' brightness: a square brighter than
 * half way from black to white is a 1.
 */
uint32_t
camcheck_counter_decode(
	const double *brightness,
	double black,
	double white)
{
	double threshold;
	uint32_t value;
	unsigned bit;

	/* Each square, the most significant first. */
	threshold = (black + white) / 2.0;
	value = 0U;
	for (bit = 0U; bit < CAMCHECK_COUNTER_BITS; bit++) {
		value <<= 1;
		if (brightness[bit] > threshold)
			value |= 1U;
	}

	/* Succeeded: the counter's value. */
	return value;
}

/*
 * Adds one counter value to a run: a step back counts as a decrease, a
 * step forward by its size (the PC draws faster than the device captures,
 * so steps of 2 and more are normal).
 */
void
camcheck_counter_add(
	struct camcheck_counter_run *run,
	uint32_t value)
{
	uint32_t step;

	/* The first value only starts the run. */
	if (!run->started) {
		run->started = 1;
		run->last = value;
		return;
	}

	/* The step, modulo the counter's 16 bits. */
	step = (value - run->last) & CAMCHECK_COUNTER_MASK;
	if (step >= CAMCHECK_COUNTER_BACKWARD) {
		run->decreases++;
	} else if (step >= CAMCHECK_INCREMENT_KINDS - 1U) {
		run->increments[CAMCHECK_INCREMENT_KINDS - 1U]++;
	} else {
		run->increments[step]++;
	}

	/* The value is the one the next is compared with. */
	run->last = value;
}

/* Gives a matrix's Kr and Kb. */
static void
camcheck_constants(
	uint32_t matrix,
	double *kr,
	double *kb)
{
	/* BT.709. */
	if (matrix == CAMCHECK_MATRIX_BT709) {
		*kr = 0.2126;
		*kb = 0.0722;
		return;
	}

	/* BT.601. */
	*kr = 0.299;
	*kb = 0.114;
}

/* Turns a region into pixel bounds inside the frame; EINVAL when it holds no pixel. */
static int
camcheck_bounds(
	uint32_t width,
	uint32_t height,
	const struct camcheck_region *region,
	uint32_t *x0,
	uint32_t *y0,
	uint32_t *x1,
	uint32_t *y1)
{
	/* A region within the unit square. */
	if (region == NULL ||
	    width == 0U ||
	    height == 0U)
		return EINVAL;
	if (region->left < 0.0 ||
	    region->top < 0.0 ||
	    region->right > 1.0 ||
	    region->bottom > 1.0)
		return EINVAL;

	/* The pixels it covers. */
	*x0 = (uint32_t)(region->left * (double)width);
	*x1 = (uint32_t)(region->right * (double)width);
	*y0 = (uint32_t)(region->top * (double)height);
	*y1 = (uint32_t)(region->bottom * (double)height);
	if (*x1 > width)
		*x1 = width;
	if (*y1 > height)
		*y1 = height;

	/* A region with no pixel. */
	if (*x1 <= *x0 || *y1 <= *y0)
		return EINVAL;

	/* Succeeded: the bounds are set. */
	return 0;
}

/* Adds the squared differences of the coloured patches from what a matrix and range give them. */
static double
camcheck_matrix_distance(
	const struct camcheck_sample *patches,
	uint32_t matrix,
	uint32_t range)
{
	static const unsigned coloured[] = {
		CAMCHECK_PATCH_RED, CAMCHECK_PATCH_GREEN, CAMCHECK_PATCH_BLUE,
		CAMCHECK_PATCH_YELLOW, CAMCHECK_PATCH_CYAN, CAMCHECK_PATCH_MAGENTA
	};
	double expected[3];
	double yuv[3];
	double difference;
	double distance;
	unsigned channel;
	unsigned index;

	/* Each coloured patch's Y, Cb and Cr. */
	distance = 0.0;
	for (index = 0U; index < sizeof(coloured) / sizeof(coloured[0]); index++) {
		camcheck_patch_expected(coloured[index], expected);
		camcheck_rgb_to_yuv(expected, matrix, range, yuv);
		for (channel = 0U; channel < 3U; channel++) {
			difference = patches[coloured[index]].value[channel] - yuv[channel];
			distance += difference * difference;
		}
	}

	/* Succeeded: the distance. */
	return distance;
}
