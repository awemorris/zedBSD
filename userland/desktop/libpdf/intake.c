/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The PNG files the editor takes as they are (ws175-p006, plan/ws175/
 * phase001/design.md section 5.1 and [L6]): an 8-bit Gray or RGB PNG
 * without alpha, palette, transparency (tRNS) or interlace keeps its
 * compressed rows -- its IDAT chunks joined --, which a PDF image reads
 * through FlateDecode with the PNG predictor (Predictor 15), since a PNG's
 * row filters are the predictor's.  Every other PNG the caller decodes to
 * RGBA itself (ENOTSUP).
 *
 * The file is not trusted: before its rows go into a PDF, each chunk's CRC
 * is checked, and the rows are inflated once to check that there are as
 * many as the header says, each of its length with a known filter, and
 * nothing after them.  The colour chunks (iCCP, gAMA, cHRM, sRGB) are left
 * out (design.md section 9).
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <compat/zlib/zlib.h>
#include <pdf.h>

#include "internal.h"

/* The signature, a chunk's frame (its length, type and CRC), and the header's length. */
#define INTAKE_SIGNATURE_SIZE	8U
#define INTAKE_CHUNK_FRAME	12U
#define INTAKE_HEADER_SIZE	13U

/* The largest side of an image, the reader's, and the room the rows are inflated into a slice at a time. */
#define INTAKE_SIDE_MAX	16384U
#define INTAKE_SLICE	65536U

/* The highest of the PNG row filters (None, Sub, Up, Average, Paeth). */
#define INTAKE_FILTER_MAX	4U

static uint32_t intake_u32(const unsigned char *bytes);
static int intake_type(const unsigned char *chunk, const char *type);
static int intake_header(const unsigned char *chunk, uint32_t length, size_t *width, size_t *height, int *components);

/*
 * Reads a PNG file whose compressed rows a PDF can take as they are: its
 * size, its components (1 Gray, 3 RGB), and its IDAT chunks joined into a
 * new buffer (*rows, *length; the caller frees it), checked by
 * pdf_png_check_rows.  Returns 0, EINVAL for a file that is not a sound
 * PNG, E2BIG past the largest side, ENOTSUP for a PNG of another kind
 * (alpha, palette, a depth other than 8, interlaced, tRNS), or ENOMEM.
 */
int
pdf_png_rows(
	const unsigned char *png,
	size_t size,
	size_t *width,
	size_t *height,
	int *components,
	unsigned char **rows,
	size_t *length)
{
	static const unsigned char signature[INTAKE_SIGNATURE_SIZE] = { 137, 80, 78, 71, 13, 10, 26, 10 };
	const unsigned char *chunk;
	unsigned char *joined;
	unsigned char *grown;
	uint32_t chunk_length;
	uint32_t crc;
	size_t position;
	size_t joined_length;
	size_t capacity;
	size_t png_width;
	size_t png_height;
	int png_components;
	uint32_t stored;
	int differs;
	int is_header;
	int is_transparency;
	int is_data;
	int is_end;
	int seen_header;
	int seen_end;
	int error;

	/* The signature. */
	if (png == NULL || size < INTAKE_SIGNATURE_SIZE)
		return EINVAL;
	differs = memcmp(png, signature, INTAKE_SIGNATURE_SIZE);
	if (differs != 0)
		return EINVAL;

	/* Chunk after chunk, to IEND. */
	joined = NULL;
	joined_length = 0;
	capacity = 0;
	png_width = 0;
	png_height = 0;
	png_components = 0;
	seen_header = 0;
	seen_end = 0;
	error = 0;
	position = INTAKE_SIGNATURE_SIZE;
	while (error == 0 && !seen_end) {
		/* The chunk's frame within the file. */
		if (size - position < INTAKE_CHUNK_FRAME)
			error = EINVAL;
		if (error != 0)
			break;
		chunk = png + position;
		chunk_length = intake_u32(chunk);
		if (chunk_length > size - position - INTAKE_CHUNK_FRAME)
			error = EINVAL;
		if (error != 0)
			break;

		/* Its CRC over its type and data. */
		crc = (uint32_t)crc32(0UL, chunk + 4, (uInt)(chunk_length + 4U));
		stored = intake_u32(chunk + 8U + chunk_length);
		if (crc != stored)
			error = EINVAL;
		position += INTAKE_CHUNK_FRAME + chunk_length;

		/* The header first, once. */
		is_header = intake_type(chunk, "IHDR");
		if (error == 0 && seen_header == is_header)
			error = EINVAL;
		if (error == 0 && is_header)
			error = intake_header(chunk, chunk_length, &png_width, &png_height, &png_components);
		if (error != 0 || is_header) {
			seen_header = 1;
			continue;
		}

		/* Transparency asks for a mask: decoded by the caller. */
		is_transparency = intake_type(chunk, "tRNS");
		if (is_transparency)
			error = ENOTSUP;

		/* The end. */
		is_end = intake_type(chunk, "IEND");
		if (is_end)
			seen_end = 1;

		/* Rows: joined to the others, in a buffer grown as they come. */
		is_data = intake_type(chunk, "IDAT");
		if (error != 0 || !is_data)
			continue;
		if (joined_length + chunk_length > capacity) {
			capacity = joined_length + chunk_length;
			capacity += capacity / 2U + 4096U;
			grown = realloc(joined, capacity);
			if (grown == NULL)
				error = ENOMEM;
			if (error != 0)
				break;
			joined = grown;
		}

		/* The chunk's data. */
		memcpy(joined + joined_length, chunk + 8, chunk_length);
		joined_length += chunk_length;
	}

	/* A file cut short, or without rows. */
	if (error == 0 && (!seen_end || joined_length == 0))
		error = EINVAL;

	/* The rows checked. */
	if (error == 0)
		error = pdf_png_check_rows(joined, joined_length, png_width, png_height, png_components);
	if (error != 0) {
		free(joined);
		return error;
	}

	/* Succeeded: the rows, the size and the components. */
	*rows = joined;
	*length = joined_length;
	*width = png_width;
	*height = png_height;
	*components = png_components;
	return 0;
}

/*
 * Checks a PNG's compressed rows (a zlib stream): inflated, they are
 * height rows of a filter byte (0 to 4) and width times components
 * samples, and the stream ends with the last of them.  Returns 0, EINVAL,
 * or ENOMEM.
 */
int
pdf_png_check_rows(
	const unsigned char *rows,
	size_t length,
	size_t width,
	size_t height,
	int components)
{
	z_stream stream;
	unsigned char *slice;
	size_t row_length;
	size_t expected;
	size_t total;
	size_t got;
	size_t at;
	size_t in_row;
	int status;

	/* A size and components a PDF image of the rows can have. */
	if (rows == NULL || length == 0 || width == 0 || height == 0 || (components != 1 && components != 3))
		return EINVAL;
	row_length = width * (size_t)components + 1U;
	expected = row_length * height;

	/* The room for a slice, and the inflater. */
	slice = malloc(INTAKE_SLICE);
	if (slice == NULL)
		return ENOMEM;
	memset(&stream, 0, sizeof(stream));
	status = inflateInit(&stream);
	if (status != Z_OK) {
		free(slice);
		return ENOMEM;
	}

	/* The rows a slice at a time: each row's first byte a known filter, no more bytes than the rows'. */
	stream.next_in = (Bytef *)rows;
	stream.avail_in = (uInt)length;
	total = 0;
	in_row = 0;
	status = Z_OK;
	while (status == Z_OK) {
		/* The next slice. */
		stream.next_out = slice;
		stream.avail_out = INTAKE_SLICE;
		status = inflate(&stream, Z_NO_FLUSH);
		if (status != Z_OK && status != Z_STREAM_END)
			break;
		got = INTAKE_SLICE - stream.avail_out;
		if (got > expected - total) {
			status = Z_DATA_ERROR;
			break;
		}

		/* Each row's filter. */
		for (at = 0; at < got; at++) {
			if (in_row == 0 && slice[at] > INTAKE_FILTER_MAX)
				status = Z_DATA_ERROR;
			in_row++;
			if (in_row == row_length)
				in_row = 0;
		}

		/* The slice counted; a slice of nothing before the end is a stream cut short. */
		total += got;
		if (status == Z_OK && got == 0)
			status = Z_BUF_ERROR;
	}

	/* The inflater and the slice go. */
	(void)inflateEnd(&stream);
	free(slice);

	/* Sound: the stream ended with the last row, nothing after it. */
	if (status != Z_STREAM_END || total != expected || stream.avail_in != 0)
		return EINVAL;
	return 0;
}

/*
 * Reads the header chunk: the size, and the components of an 8-bit Gray
 * (1) or RGB (3) PNG without interlace.  Returns 0, EINVAL for a malformed
 * header, E2BIG past the largest side, or ENOTSUP for another kind.
 */
static int
intake_header(
	const unsigned char *chunk,
	uint32_t length,
	size_t *width,
	size_t *height,
	int *components)
{
	/* Its length; a size, the one compression and filter method, an interlace method known. */
	if (length != INTAKE_HEADER_SIZE)
		return EINVAL;
	*width = intake_u32(chunk + 8);
	*height = intake_u32(chunk + 12);
	if (*width == 0 || *height == 0 || chunk[18] != 0 || chunk[19] != 0 || chunk[20] > 1)
		return EINVAL;

	/* Within the largest side. */
	if (*width > INTAKE_SIDE_MAX || *height > INTAKE_SIDE_MAX)
		return E2BIG;

	/* 8 bits of Gray or RGB, not interlaced. */
	if (chunk[16] != 8 || (chunk[17] != 0 && chunk[17] != 2) || chunk[20] != 0)
		return ENOTSUP;
	*components = 1;
	if (chunk[17] == 2)
		*components = 3;
	return 0;
}

/* Reads a big-endian 32-bit number. */
static uint32_t
intake_u32(
	const unsigned char *bytes)
{
	/* The first byte the highest. */
	return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) | ((uint32_t)bytes[2] << 8) | (uint32_t)bytes[3];
}

/* Tells whether a chunk is of a type (its four letters). */
static int
intake_type(
	const unsigned char *chunk,
	const char *type)
{
	/* The letters after the length. */
	return memcmp(chunk + 4, type, 4) == 0;
}
