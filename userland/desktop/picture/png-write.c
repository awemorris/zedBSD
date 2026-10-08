/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The PNG writer (ws189-p003, plan/ws189/phase001/phase.md section 4.0):
 * a picture of premultiplied 0xAARRGGBB words becomes a PNG in memory, an
 * 8-bit RGB one when every pixel is opaque and an 8-bit RGBA one (its
 * colours divided by their alpha again) otherwise.  Every row has the
 * filter None, and the rows are compressed at once by compress2 at its
 * fastest level: a picture is written when a drag starts, where time
 * matters more than size.
 */

#include "png-write.h"

#include <compat/zlib/zlib.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* PNG's colour types for 8-bit RGB and RGBA. */
#define PNG_COLOUR_RGB		2U
#define PNG_COLOUR_RGBA		6U

/* The bytes of the signature, of a chunk's length, type and checksum, and of the header's data. */
#define PNG_SIGNATURE_SIZE	8U
#define PNG_CHUNK_OVERHEAD	12U
#define PNG_HEADER_SIZE		13U

static int png_opaque(const uint32_t *pixels, int width, int height, size_t stride);
static unsigned char *png_rows(const uint32_t *pixels, int width, int height, size_t stride, int opaque, size_t *size);
static unsigned png_straight(unsigned colour, unsigned alpha);
static size_t png_chunk(unsigned char *out, const char *type, const unsigned char *data, size_t length);
static void png_put32(unsigned char *out, uint32_t value);

/*
 * Writes a picture as a PNG into memory of its own (the caller frees
 * *png).  Returns 0, EINVAL for a size of 0 or past
 * KL_PICTURE_PNG_SIDE_MAX, or ENOMEM.
 */
int
kl_picture_png(
	const uint32_t *pixels,
	int width,
	int height,
	size_t stride,
	unsigned char **png,
	size_t *size)
{
	unsigned char header[PNG_HEADER_SIZE];
	unsigned char *rows;
	unsigned char *packed;
	unsigned char *out;
	uLongf packed_size;
	size_t rows_size;
	size_t total;
	size_t at;
	int opaque;
	int status;

	/* Nothing yet. */
	*png = NULL;
	*size = 0;

	/* A picture of a size PNG and the readers take. */
	if (pixels == NULL || width <= 0 || height <= 0)
		return EINVAL;
	if (width > KL_PICTURE_PNG_SIDE_MAX || height > KL_PICTURE_PNG_SIDE_MAX)
		return EINVAL;
	if (stride < (size_t)width)
		return EINVAL;

	/* The rows as PNG has them, each after its filter byte: RGB when nothing shows through. */
	opaque = png_opaque(pixels, width, height, stride);
	rows = png_rows(pixels, width, height, stride, opaque, &rows_size);
	if (rows == NULL)
		return ENOMEM;

	/* Room for the rows compressed. */
	packed_size = compressBound((uLong)rows_size);
	packed = malloc((size_t)packed_size);
	if (packed == NULL) {
		free(rows);
		return ENOMEM;
	}

	/* The rows compressed into one zlib stream, as fast as it goes. */
	status = compress2(packed, &packed_size, rows, (uLong)rows_size, Z_BEST_SPEED);
	if (status != Z_OK) {
		free(rows);
		free(packed);
		return ENOMEM;
	}

	/* The rows laid out are not needed once compressed. */
	free(rows);

	/* Room for the signature, the header, the data and the end. */
	total = PNG_SIGNATURE_SIZE + PNG_CHUNK_OVERHEAD + PNG_HEADER_SIZE;
	total += PNG_CHUNK_OVERHEAD + (size_t)packed_size;
	total += PNG_CHUNK_OVERHEAD;
	out = malloc(total);
	if (out == NULL) {
		free(packed);
		return ENOMEM;
	}

	/* The signature. */
	memcpy(out, "\211PNG\r\n\032\n", PNG_SIGNATURE_SIZE);
	at = PNG_SIGNATURE_SIZE;

	/* The header: the size, 8 bits a sample, the colour type, deflate, the adaptive filters, no interlace. */
	png_put32(header, (uint32_t)width);
	png_put32(header + 4, (uint32_t)height);
	header[8] = 8U;
	header[9] = (unsigned char)PNG_COLOUR_RGBA;
	if (opaque)
		header[9] = (unsigned char)PNG_COLOUR_RGB;
	header[10] = 0U;
	header[11] = 0U;
	header[12] = 0U;
	at += png_chunk(out + at, "IHDR", header, PNG_HEADER_SIZE);

	/* The compressed rows, then the end. */
	at += png_chunk(out + at, "IDAT", packed, (size_t)packed_size);
	at += png_chunk(out + at, "IEND", NULL, 0U);
	free(packed);

	/* Succeeded: the PNG and its length. */
	*png = out;
	*size = at;
	return 0;
}

/* Tells whether every pixel of a picture is opaque (1) or some show through (0). */
static int
png_opaque(
	const uint32_t *pixels,
	int width,
	int height,
	size_t stride)
{
	const uint32_t *row;
	int x;
	int y;

	/* Each row's pixels, until one is not opaque. */
	for (y = 0; y < height; y++) {
		row = pixels + (size_t)y * stride;
		for (x = 0; x < width; x++) {
			/* An alpha below 255 shows what is under it. */
			if ((row[x] >> 24) != 0xffU)
				return 0;
		}
	}

	/* Every pixel is opaque. */
	return 1;
}

/*
 * Lays out a picture's rows as PNG compresses them: a filter byte of 0
 * (None) before each row, then red, green and blue (and alpha, divided
 * out again, when the picture is not opaque).  Returns them in memory of
 * their own with their size, or NULL without memory.
 */
static unsigned char *
png_rows(
	const uint32_t *pixels,
	int width,
	int height,
	size_t stride,
	int opaque,
	size_t *size)
{
	const uint32_t *row;
	unsigned char *rows;
	unsigned char *out;
	unsigned alpha;
	unsigned channels;
	uint32_t pixel;
	int x;
	int y;

	/* Three bytes a pixel without alpha, four with it, and a filter byte a row. */
	channels = 4U;
	if (opaque)
		channels = 3U;
	*size = (size_t)height * (1U + (size_t)width * channels);
	rows = malloc(*size);
	if (rows == NULL)
		return NULL;

	/* Each row: its filter byte, then its pixels' samples. */
	out = rows;
	for (y = 0; y < height; y++) {
		row = pixels + (size_t)y * stride;
		*out++ = 0U;
		for (x = 0; x < width; x++) {
			pixel = row[x];
			alpha = (unsigned)(pixel >> 24);

			/* An opaque picture's colours are as they are. */
			if (opaque) {
				*out++ = (unsigned char)((pixel >> 16) & 0xffU);
				*out++ = (unsigned char)((pixel >> 8) & 0xffU);
				*out++ = (unsigned char)(pixel & 0xffU);
				continue;
			}

			/* A picture that shows through has its colours divided by their alpha again. */
			*out++ = (unsigned char)png_straight((unsigned)((pixel >> 16) & 0xffU), alpha);
			*out++ = (unsigned char)png_straight((unsigned)((pixel >> 8) & 0xffU), alpha);
			*out++ = (unsigned char)png_straight((unsigned)(pixel & 0xffU), alpha);
			*out++ = (unsigned char)alpha;
		}
	}

	/* Succeeded: the rows. */
	return rows;
}

/* Divides a premultiplied colour by its alpha again (rounded, at most 255). */
static unsigned
png_straight(
	unsigned colour,
	unsigned alpha)
{
	unsigned straight;

	/* A clear pixel has no colour to keep. */
	if (alpha == 0U)
		return 0U;

	/* The colour before the alpha was multiplied in; a colour above its alpha is clamped. */
	straight = (colour * 255U + alpha / 2U) / alpha;
	if (straight > 255U)
		straight = 255U;

	/* Succeeded: the straight colour. */
	return straight;
}

/*
 * Writes one chunk: its length, its type, its data and the checksum of
 * the type and the data.  Returns the bytes written.
 */
static size_t
png_chunk(
	unsigned char *out,
	const char *type,
	const unsigned char *data,
	size_t length)
{
	uLong checksum;

	/* The length and the type. */
	png_put32(out, (uint32_t)length);
	memcpy(out + 4, type, 4U);

	/* The data, when there is any. */
	if (length != 0U)
		memcpy(out + 8, data, length);

	/* The checksum covers the type and the data. */
	checksum = crc32(0L, NULL, 0U);
	checksum = crc32(checksum, out + 4, (uInt)(4U + length));
	png_put32(out + 8 + length, (uint32_t)checksum);

	/* Succeeded: the chunk's bytes. */
	return PNG_CHUNK_OVERHEAD + length;
}

/* Writes a word big-endian, as PNG keeps its numbers. */
static void
png_put32(
	unsigned char *out,
	uint32_t value)
{
	/* The bytes from the most significant. */
	out[0] = (unsigned char)((value >> 24) & 0xffU);
	out[1] = (unsigned char)((value >> 16) & 0xffU);
	out[2] = (unsigned char)((value >> 8) & 0xffU);
	out[3] = (unsigned char)(value & 0xffU);
}
