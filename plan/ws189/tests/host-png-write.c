/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the PNG writer (ws189, userland/desktop/picture/
 * png-write.c compiled unchanged against the host's zlib): each picture
 * written is read back here -- its signature, its chunks and their
 * checksums, its header, and its rows inflated -- and compared with the
 * pixels it was written from.
 */

#include "png-write.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

static int test_failures;

static void check(int condition, const char *what);
static uint32_t read32(const unsigned char *bytes);
static void round_trip(const char *name, const uint32_t *pixels, int width, int height, size_t stride, int expect_opaque);
static void test_opaque(void);
static void test_alpha(void);
static void test_edges(void);
static void test_refused(void);
static void test_fit(void);
static void test_rows(void);

int
main(
	void)
{
	/* Each case. */
	test_opaque();
	test_alpha();
	test_edges();
	test_refused();
	test_fit();
	test_rows();

	/* The verdict. */
	if (test_failures != 0) {
		printf("host-png-write: %d failures\n", test_failures);
		return 1;
	}
	printf("host-png-write: ok\n");
	return 0;
}

/* Counts a failed check and names it. */
static void
check(
	int condition,
	const char *what)
{
	/* A passed check says nothing. */
	if (condition)
		return;
	printf("FAIL %s\n", what);
	test_failures++;
}

/* Reads a big-endian word. */
static uint32_t
read32(
	const unsigned char *bytes)
{
	/* The bytes from the most significant. */
	return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) | ((uint32_t)bytes[2] << 8) | (uint32_t)bytes[3];
}

/* Writes a picture, reads it back and compares every sample. */
static void
round_trip(
	const char *name,
	const uint32_t *pixels,
	int width,
	int height,
	size_t stride,
	int expect_opaque)
{
	unsigned char *png;
	unsigned char *rows;
	unsigned char *idat;
	size_t size;
	size_t at;
	size_t idat_size;
	uLongf rows_size;
	uint32_t length;
	uint32_t crc;
	unsigned channels;
	unsigned alpha;
	unsigned expect;
	uint32_t pixel;
	const unsigned char *sample;
	int error;
	int seen_header;
	int seen_end;
	int x;
	int y;
	char what[160];

	/* The PNG. */
	error = kl_picture_png(pixels, width, height, stride, &png, &size);
	snprintf(what, sizeof(what), "%s: written", name);
	check(error == 0, what);
	if (error != 0)
		return;

	/* The signature. */
	snprintf(what, sizeof(what), "%s: signature", name);
	check(size > 8U && memcmp(png, "\211PNG\r\n\032\n", 8U) == 0, what);

	/* The chunks: each checksum right, the header first, the data gathered, the end last. */
	idat = NULL;
	idat_size = 0;
	seen_header = 0;
	seen_end = 0;
	channels = 4U;
	at = 8U;
	while (at + 12U <= size) {
		length = read32(png + at);
		if (at + 12U + length > size)
			break;
		crc = (uint32_t)crc32(crc32(0L, NULL, 0U), png + at + 4U, length + 4U);
		snprintf(what, sizeof(what), "%s: crc of %.4s", name, (const char *)(png + at + 4U));
		check(crc == read32(png + at + 8U + length), what);
		if (memcmp(png + at + 4U, "IHDR", 4U) == 0) {
			seen_header = 1;
			snprintf(what, sizeof(what), "%s: header", name);
			check(length == 13U && read32(png + at + 8U) == (uint32_t)width && read32(png + at + 12U) == (uint32_t)height && png[at + 16U] == 8U, what);
			channels = png[at + 17U] == 2U ? 3U : 4U;
			snprintf(what, sizeof(what), "%s: colour type", name);
			check(expect_opaque ? png[at + 17U] == 2U : png[at + 17U] == 6U, what);
		} else if (memcmp(png + at + 4U, "IDAT", 4U) == 0) {
			idat = realloc(idat, idat_size + length);
			memcpy(idat + idat_size, png + at + 8U, length);
			idat_size += length;
		} else if (memcmp(png + at + 4U, "IEND", 4U) == 0) {
			seen_end = 1;
		}
		at += 12U + length;
	}
	snprintf(what, sizeof(what), "%s: header and end", name);
	check(seen_header && seen_end && at == size, what);

	/* The rows inflated. */
	rows_size = (uLongf)((size_t)height * (1U + (size_t)width * channels));
	rows = malloc((size_t)rows_size + 1U);
	error = uncompress(rows, &rows_size, idat, (uLong)idat_size);
	snprintf(what, sizeof(what), "%s: inflate", name);
	check(error == Z_OK && rows_size == (uLongf)((size_t)height * (1U + (size_t)width * channels)), what);

	/* Every sample against its pixel. */
	for (y = 0; error == Z_OK && y < height; y++) {
		sample = rows + (size_t)y * (1U + (size_t)width * channels);
		snprintf(what, sizeof(what), "%s: filter of row %d", name, y);
		check(sample[0] == 0U, what);
		sample++;
		for (x = 0; x < width; x++) {
			pixel = pixels[(size_t)y * stride + (size_t)x];
			alpha = pixel >> 24;
			snprintf(what, sizeof(what), "%s: pixel %d,%d", name, x, y);
			if (channels == 3U) {
				check(sample[0] == ((pixel >> 16) & 0xffU) && sample[1] == ((pixel >> 8) & 0xffU) && sample[2] == (pixel & 0xffU), what);
			} else {
				expect = alpha == 0U ? 0U : (((pixel >> 16) & 0xffU) * 255U + alpha / 2U) / alpha;
				if (expect > 255U)
					expect = 255U;
				check(sample[0] == expect && sample[3] == alpha, what);
			}
			sample += channels;
		}
	}

	free(rows);
	free(idat);
	free(png);
}

/* An opaque picture with a stride wider than its rows is written as RGB. */
static void
test_opaque(void)
{
	uint32_t pixels[37 * 21];
	int x;
	int y;

	/* A gradient, the last 4 words of each row past the picture. */
	for (y = 0; y < 21; y++) {
		for (x = 0; x < 37; x++)
			pixels[y * 37 + x] = 0xff000000U | ((uint32_t)(x * 7) << 16) | ((uint32_t)(y * 11) << 8) | (uint32_t)((x + y) & 0xff);
	}
	round_trip("opaque", pixels, 33, 21, 37U, 1);
}

/* A picture with clear and half-clear pixels is written as RGBA, its colours straight again. */
static void
test_alpha(void)
{
	uint32_t pixels[16 * 16];
	unsigned alpha;
	unsigned red;
	int x;
	int y;

	/* Alpha across, a premultiplied red down. */
	for (y = 0; y < 16; y++) {
		for (x = 0; x < 16; x++) {
			alpha = (unsigned)(x * 17);
			red = (alpha * (unsigned)(y * 17)) / 255U;
			pixels[y * 16 + x] = ((uint32_t)alpha << 24) | ((uint32_t)red << 16) | ((uint32_t)(alpha / 2U) << 8);
		}
	}
	round_trip("alpha", pixels, 16, 16, 16U, 0);
}

/* The smallest pictures, and a long thin one. */
static void
test_edges(void)
{
	static uint32_t wide[2048];
	uint32_t one;
	int x;

	/* One opaque pixel, one clear pixel. */
	one = 0xff123456U;
	round_trip("1x1 opaque", &one, 1, 1, 1U, 1);
	one = 0x00000000U;
	round_trip("1x1 clear", &one, 1, 1, 1U, 0);

	/* A row of 2048. */
	for (x = 0; x < 2048; x++)
		wide[x] = 0xff000000U | (uint32_t)x;
	round_trip("2048x1", wide, 2048, 1, 2048U, 1);
}

/* The sizes refused. */
static void
test_refused(void)
{
	unsigned char *png;
	uint32_t one;
	size_t size;
	int error;

	/* No pixels, a side of 0, a side past the limit, a stride below the width. */
	one = 0xffffffffU;
	error = kl_picture_png(NULL, 1, 1, 1U, &png, &size);
	check(error == EINVAL, "refused: no pixels");
	error = kl_picture_png(&one, 0, 1, 1U, &png, &size);
	check(error == EINVAL, "refused: width 0");
	error = kl_picture_png(&one, 1, KL_PICTURE_PNG_SIDE_MAX + 1, 1U, &png, &size);
	check(error == EINVAL, "refused: height past the limit");
	error = kl_picture_png(&one, 2, 1, 1U, &png, &size);
	check(error == EINVAL && png == NULL && size == 0U, "refused: stride below width");
}

/* A picture shrunk to a side keeps its shape and averages what it covers; one that fits is copied. */
static void
test_fit(void)
{
	static uint32_t big[100 * 50];
	uint32_t *out;
	int width;
	int height;
	int error;
	int index;
	int same;

	/* Half the columns one colour, half another: the shrunk halves keep them. */
	for (index = 0; index < 100 * 50; index++)
		big[index] = (index % 100) < 50 ? 0xff102030U : 0xff405060U;
	error = kl_picture_fit(big, 100, 50, 100U, 20, &out, &width, &height);
	check(error == 0 && width == 20 && height == 10, "fit: size");
	if (error == 0) {
		check(out[0] == 0xff102030U && out[19] == 0xff405060U, "fit: averages");
		free(out);
	}

	/* A picture that fits is copied as it is. */
	error = kl_picture_fit(big, 100, 50, 100U, 200, &out, &width, &height);
	check(error == 0 && width == 100 && height == 50, "fit: copy size");
	if (error == 0) {
		same = memcmp(out, big, sizeof(big));
		check(same == 0, "fit: copy pixels");
		free(out);
	}
}

/* Rows a PNG has already are wrapped: gray and RGB, with the header saying so, and inflated back to the rows. */
static void
test_rows(void)
{
	unsigned char rows[2 * (1 + 3)];
	unsigned char packed[256];
	unsigned char back[64];
	unsigned char *png;
	uLongf packed_size;
	uLongf back_size;
	size_t size;
	int error;

	/* Two rows of three gray samples, each after its filter byte, compressed. */
	memcpy(rows, "\0\x10\x20\x30\0\x40\x50\x60", sizeof(rows));
	packed_size = sizeof(packed);
	(void)compress(packed, &packed_size, rows, sizeof(rows));

	/* Wrapped as gray. */
	error = kl_picture_png_rows(packed, (size_t)packed_size, 3, 2, 1, &png, &size);
	check(error == 0, "rows: gray written");
	if (error == 0) {
		check(png[8 + 4 + 4 + 8 + 1] == 0U, "rows: gray colour type");
		back_size = sizeof(back);
		error = uncompress(back, &back_size, png + 8 + 12 + 13 + 8, (uLong)packed_size);
		check(error == Z_OK && back_size == sizeof(rows) && memcmp(back, rows, sizeof(rows)) == 0, "rows: data as given");
		free(png);
	}

	/* Four components are not a PNG's rows here. */
	error = kl_picture_png_rows(packed, (size_t)packed_size, 3, 2, 4, &png, &size);
	check(error == EINVAL, "rows: four components refused");
}
