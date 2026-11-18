/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The preprocessing: the image is resized by r = min(S / h, S / w) with
 * bilinear interpolation into the top-left of an S x S input filled with
 * the pad value, as RGB samples of 0 to 255 (no mean or scale).
 *
 * The coefficients are computed in binary64 and the samples in integers,
 * in 11-bit fixed point: for each output column dx, fx = (dx + 0.5) *
 * scale - 0.5 with scale = 1 / (dw / w), sx = floor(fx), a = fx - sx,
 * clamped at both edges with a = 0, c1 = round(a * 2048), c0 = 2048 - c1,
 * and the same for the rows.  A sample is ((s00 c0 + s01 c1) d0 + (s10 c0
 * + s11 c1) d1 + 2^21) >> 22.
 *
 * Procedure referenced: OpenCV imgproc resize (INTER_LINEAR): the pixel
 * centre mapping and the 11-bit fixed-point weights, as the model's
 * reference preprocessing uses them; the code is independent.
 */

#include <math.h>
#include <stdlib.h>

#include "yolox-internal.h"

/* The fixed point of the interpolation weights. */
#define PRE_WEIGHT_BITS		11U
#define PRE_WEIGHT_ONE		(1 << PRE_WEIGHT_BITS)
#define PRE_ROUND		(1 << (2U * PRE_WEIGHT_BITS - 1U))

/* One output column's or row's source position and weight. */
struct pre_tap {
	uint32_t first;
	uint32_t second;
	int32_t weight;
};

static int pre_taps(uint32_t source, uint32_t scaled, struct pre_tap **taps);
static void pre_sample(const struct yolox_image *image, const uint8_t *pixels, uint32_t x, uint32_t y, int32_t *rgb);

/*
 * Computes the letterbox of an image at an input size.
 */
int
yolox_letterbox_compute(
	uint32_t width,
	uint32_t height,
	uint32_t size,
	struct yolox_letterbox *box)
{
	double ratio;

	/* An empty or oversized image cannot be scaled. */
	if (width == 0U || height == 0U || width > YOLOX_IMAGE_MAX || height > YOLOX_IMAGE_MAX)
		return YOLOX_ERROR_ARGUMENT;

	/* r in binary64, then the scaled size truncated as the reference does. */
	ratio = fmin((double)size / height, (double)size / width);
	box->ratio = ratio;
	box->scaled_width = (uint32_t)(width * ratio);
	box->scaled_height = (uint32_t)(height * ratio);
	if (box->scaled_width == 0U || box->scaled_height == 0U)
		return YOLOX_ERROR_ARGUMENT;

	/* Succeeded: the image fits the input. */
	return 0;
}

/*
 * Resizes and pads an image into the 3 x S x S RGB float input.
 */
int
yolox_preprocess(
	const struct yolox_image *image,
	const void *pixels,
	uint32_t size,
	uint32_t pad_value,
	const struct yolox_letterbox *box,
	float *input)
{
	struct pre_tap *columns;
	struct pre_tap *rows;
	int32_t s00[3];
	int32_t s01[3];
	int32_t s10[3];
	int32_t s11[3];
	int64_t top;
	int64_t bottom;
	int64_t value;
	size_t plane;
	uint32_t x;
	uint32_t y;
	uint32_t c;
	int32_t c0;
	int32_t d0;
	int error;

	/* The columns' and the rows' taps. */
	error = pre_taps(image->width, box->scaled_width, &columns);
	if (error != 0)
		return error;

	error = pre_taps(image->height, box->scaled_height, &rows);
	if (error != 0) {
		free(columns);
		return error;
	}

	/* Each input sample: interpolated inside the scaled image, the pad outside. */
	plane = (size_t)size * size;
	for (y = 0U; y < size; y++) {
		for (x = 0U; x < size; x++) {
			if (x >= box->scaled_width || y >= box->scaled_height) {
				for (c = 0U; c < 3U; c++)
					input[c * plane + (size_t)y * size + x] = (float)pad_value;
				continue;
			}

			/* The four source samples and their weights. */
			pre_sample(image, pixels, columns[x].first, rows[y].first, s00);
			pre_sample(image, pixels, columns[x].second, rows[y].first, s01);
			pre_sample(image, pixels, columns[x].first, rows[y].second, s10);
			pre_sample(image, pixels, columns[x].second, rows[y].second, s11);
			c0 = PRE_WEIGHT_ONE - columns[x].weight;
			d0 = PRE_WEIGHT_ONE - rows[y].weight;

			/* Each colour, in integers. */
			for (c = 0U; c < 3U; c++) {
				top = (int64_t)s00[c] * c0 + (int64_t)s01[c] * columns[x].weight;
				bottom = (int64_t)s10[c] * c0 + (int64_t)s11[c] * columns[x].weight;
				value = (top * d0 + bottom * rows[y].weight + PRE_ROUND) >> (2U * PRE_WEIGHT_BITS);
				input[c * plane + (size_t)y * size + x] = (float)value;
			}
		}
	}

	/* Succeeded: the input is filled. */
	free(columns);
	free(rows);
	return 0;
}

/* Computes the taps of one axis: scaled output positions over source positions. */
static int
pre_taps(
	uint32_t source,
	uint32_t scaled,
	struct pre_tap **result)
{
	struct pre_tap *taps;
	double scale;
	double position;
	double fraction;
	double floor_position;
	uint32_t index;
	int64_t first;

	/* One tap an output position. */
	taps = malloc((size_t)scaled * sizeof(*taps));
	if (taps == NULL)
		return YOLOX_ERROR_MEMORY;

	/* The scale as the reference computes it. */
	scale = 1.0 / ((double)scaled / source);
	for (index = 0U; index < scaled; index++) {
		position = ((double)index + 0.5) * scale - 0.5;
		floor_position = floor(position);
		fraction = position - floor_position;
		first = (int64_t)floor_position;

		/* Clamped at both edges, without weight. */
		if (first < 0) {
			first = 0;
			fraction = 0.0;
		}
		if (first >= (int64_t)source - 1) {
			first = (int64_t)source - 1;
			fraction = 0.0;
		}

		/* The two taps and the second's weight. */
		taps[index].first = (uint32_t)first;
		taps[index].second = (uint32_t)first + 1U;
		if (taps[index].second > source - 1U)
			taps[index].second = source - 1U;
		taps[index].weight = (int32_t)lround(fraction * PRE_WEIGHT_ONE);
	}

	/* Succeeded: the caller frees the taps. */
	*result = taps;
	return 0;
}

/* Reads one pixel's red, green and blue. */
static void
pre_sample(
	const struct yolox_image *image,
	const uint8_t *pixels,
	uint32_t x,
	uint32_t y,
	int32_t *rgb)
{
	const uint8_t *pixel;

	/* Decides the pixel's layout. */
	switch (image->format) {
	case YOLOX_PIXEL_RGB8:
		pixel = pixels + (size_t)y * image->row_bytes + (size_t)x * 3U;
		rgb[0] = pixel[0];
		rgb[1] = pixel[1];
		rgb[2] = pixel[2];
		break;
	case YOLOX_PIXEL_RGBA8:
		pixel = pixels + (size_t)y * image->row_bytes + (size_t)x * 4U;
		rgb[0] = pixel[0];
		rgb[1] = pixel[1];
		rgb[2] = pixel[2];
		break;
	default:
		pixel = pixels + (size_t)y * image->row_bytes + (size_t)x * 4U;
		rgb[0] = pixel[2];
		rgb[1] = pixel[1];
		rgb[2] = pixel[0];
		break;
	}
}
