/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The alpha chunk of a lossy image: raw or lossless values and the inverse
 * filter.
 */

#include "alpha.h"
#include "vp8l.h"

#include <errno.h>
#include <stdlib.h>

/*
 * The fields of the header byte, from its least significant bit up: the
 * compression method C (2 bits), the filtering method F (2 bits).  The
 * pre-processing P and reserved bits above them are not read.
 *
 * Values referenced: RFC 9649 (November 2024) 2.7.1.2, Figure 10: header
 * byte Rsv (2 bits), P (2 bits), F (2 bits), C (2 bits), most significant
 * first.
 */
#define HEADER_COMPRESSION_MASK		3U
#define HEADER_FILTER_SHIFT		2U
#define HEADER_FILTER_MASK		3U

/*
 * The compression methods.
 *
 * Values referenced: RFC 9649 2.7.1.2: 0 is no compression (raw values),
 * 1 is the lossless format; 2 and 3 are not defined.
 */
#define COMPRESSION_NONE		0U
#define COMPRESSION_LOSSLESS		1U

/* Gives the value an alpha pixel is predicted from, for the filter method, from the values already restored. */
static unsigned predict_alpha(const uint8_t *alpha, unsigned width, unsigned x, unsigned y, unsigned method);

/* Writes the green channel of count ARGB pixels as count alpha values. */
static void take_green(const uint32_t *pixels, size_t count, uint8_t *alpha);

/*
 * Decodes an ALPH chunk's payload (payload, size bytes: the header byte,
 * then the alpha bitstream) into alpha (the caller's width * height bytes,
 * rows top to bottom, width a row; width and height are the image's, each
 * 1..WEBPC_VP8_SIDE_MAX).
 *
 * Size 0 or 1 is EINVAL; the header byte's Rsv and P bits are ignored; the
 * compression method C 0 is raw values (fewer than width * height is
 * EINVAL, more are ignored), 1 is a lossless image-stream whose green
 * channel is the alpha, 2 and 3 are EINVAL.  Then the filter F is undone.
 */
int
webpc_alpha_decode(
	const uint8_t *payload,
	size_t size,
	unsigned width,
	unsigned height,
	uint8_t *alpha)
{
	uint32_t *pixels;
	size_t count;
	unsigned header;
	unsigned compression;
	unsigned method;
	size_t index;
	int error;

	/* Refuses missing buffers and an image with a side the format cannot have. */
	if (payload == NULL || alpha == NULL)
		return EINVAL;
	if (width < 1U || width > WEBPC_VP8_SIDE_MAX)
		return EINVAL;
	if (height < 1U || height > WEBPC_VP8_SIDE_MAX)
		return EINVAL;

	/* Refuses a chunk with no header byte or no alpha bitstream after it. */
	if (size < 2U)
		return EINVAL;

	/* Splits the header byte into the compression and filtering methods. */
	header = payload[0];
	compression = header & HEADER_COMPRESSION_MASK;
	method = (header >> HEADER_FILTER_SHIFT) & HEADER_FILTER_MASK;

	/* A side is at most 16383, so the number of values fits a size_t. */
	count = (size_t)width * (size_t)height;

	/* Reads the values as the compression method says. */
	if (compression == COMPRESSION_NONE) {
		/* Refuses raw values that end before the image does. */
		if (size - 1U < count)
			return EINVAL;

		/* Copies the raw values; bytes after them are ignored. */
		for (index = 0U; index < count; index++)
			alpha[index] = payload[1U + index];
	} else if (compression == COMPRESSION_LOSSLESS) {
		/* Allocates the ARGB pixels the lossless decoder writes. */
		pixels = (uint32_t *)malloc(count * sizeof(uint32_t));
		if (pixels == NULL)
			return ENOMEM;

		/* Decodes the image-stream that follows the header byte. */
		error = webpc_vp8l_decode_stream(payload + 1U, size - 1U, width, height, pixels);
		if (error != 0) {
			free(pixels);
			return error;
		}

		/* Takes the alpha values from the green channel and frees the pixels. */
		take_green(pixels, count, alpha);
		free(pixels);
	} else {
		/* Refuses a compression method the format does not define. */
		return EINVAL;
	}

	/* Undoes the prediction filter over the values. */
	webpc_alpha_unfilter(alpha, width, height, method);

	/* Succeeded: every alpha value is written. */
	return 0;
}

/*
 * Undoes an alpha prediction filter in place (alpha as webpc_alpha_decode
 * lays it out; method WEBPC_ALPHA_FILTER_*), in scan-line order so each
 * prediction uses values already restored.
 *
 * Procedure referenced: RFC 9649 2.7.1.2, Figure 11: value = (predictor +
 * stored) modulo 256, the predictor 0 for method 0, A (left) for 1, B
 * (above) for 2, clip(A + B - C) for 3 (C above-left).  Pixel (0, 0) is
 * predicted 0; for methods 1 and 3 the rest of the left column is predicted
 * from the value above, for methods 2 and 3 the rest of the top row from
 * the value on the left.  A method above 3 changes nothing.
 */
void
webpc_alpha_unfilter(
	uint8_t *alpha,
	unsigned width,
	unsigned height,
	unsigned method)
{
	unsigned x;
	unsigned y;
	unsigned predicted;
	size_t offset;

	/* Leaves the values alone when there is no filter, no image or no buffer. */
	if (alpha == NULL)
		return;
	if (method == WEBPC_ALPHA_FILTER_NONE || method > WEBPC_ALPHA_FILTER_GRADIENT)
		return;
	if (width == 0U || height == 0U)
		return;

	/* Restores each value from its stored difference, rows top to bottom, each row left to right. */
	for (y = 0U; y < height; y++) {
		for (x = 0U; x < width; x++) {
			/* Predicts from values already restored, then adds the stored value modulo 256. */
			predicted = predict_alpha(alpha, width, x, y, method);
			offset = (size_t)y * (size_t)width + x;
			alpha[offset] = (uint8_t)((predicted + alpha[offset]) & 0xffU);
		}
	}
}

/*
 * Gives the value the pixel (x, y) is predicted from (method 1, 2 or 3),
 * from the values at its left, above and above-left, which are restored.
 */
static unsigned
predict_alpha(
	const uint8_t *alpha,
	unsigned width,
	unsigned x,
	unsigned y,
	unsigned method)
{
	size_t offset;
	int left;
	int above;
	int above_left;
	int sum;

	/* The first pixel has no neighbour and is predicted 0. */
	if (x == 0U && y == 0U)
		return 0U;

	/* The top row has only the left neighbour, whatever the method. */
	offset = (size_t)y * (size_t)width + x;
	if (y == 0U)
		return alpha[offset - 1U];

	/* The left column has only the neighbour above, whatever the method. */
	if (x == 0U)
		return alpha[offset - width];

	/* Inside the image: the left value, the value above or the clipped gradient. */
	left = alpha[offset - 1U];
	above = alpha[offset - width];
	above_left = alpha[offset - width - 1U];
	if (method == WEBPC_ALPHA_FILTER_HORIZONTAL)
		return (unsigned)left;
	if (method == WEBPC_ALPHA_FILTER_VERTICAL)
		return (unsigned)above;

	/* The gradient A + B - C, clipped to 0..255. */
	sum = left + above - above_left;
	sum = webpc_clamp255(sum);
	return (unsigned)sum;
}

/* Writes the green channel (bits 8..15) of count ARGB pixels as count alpha values. */
static void
take_green(
	const uint32_t *pixels,
	size_t count,
	uint8_t *alpha)
{
	size_t index;

	/* Takes the second byte of each pixel. */
	for (index = 0U; index < count; index++)
		alpha[index] = (uint8_t)((pixels[index] >> 8) & 0xffU);
}
