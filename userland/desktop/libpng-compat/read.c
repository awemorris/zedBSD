/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Reading PNG with libpng's simplified API (ws071-p010, the read half of
 * ws035-p041).  The file's chunks are checked (their CRCs) and read at the
 * start; the pixels are decompressed (libz-compat), unfiltered and turned
 * into the caller's 8-bit format when the image is finished.
 *
 * Every colour type and bit depth is read, including Adam7 interlacing.
 * 16-bit components keep their high byte.  Alpha is straight
 * (not premultiplied), as libpng gives it.
 */

#include <compat/png/png.h>
#include <compat/zlib/zlib.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The colour types of IHDR. */
#define PNG_TYPE_GRAY		0U
#define PNG_TYPE_RGB		2U
#define PNG_TYPE_PALETTE	3U
#define PNG_TYPE_GRAY_ALPHA	4U
#define PNG_TYPE_RGBA		6U

/* The largest side read, which keeps the sizes worked out below far from overflowing. */
#define PNG_SIDE_MAX		32768U

/* The largest file read into memory. */
#define PNG_FILE_MAX		(256U * 1024U * 1024U)

/* The chunks read, by their types; any other is critical (refused) or ancillary (skipped). */
#define PNG_CHUNK_OTHER		0U
#define PNG_CHUNK_IHDR		1U
#define PNG_CHUNK_PLTE		2U
#define PNG_CHUNK_TRNS		3U
#define PNG_CHUNK_IDAT		4U
#define PNG_CHUNK_IEND		5U

/*
 * An image being read: the file's bytes (owned when read from a file), the
 * header, the palette and its alphas, the transparent colour of a gray or
 * RGB image, and the compressed pixels gathered from the IDAT chunks.
 */
struct png_control {
	unsigned char *file;
	png_uint_32 width;
	png_uint_32 height;
	unsigned depth;
	unsigned type;
	int interlaced;
	unsigned char palette[256][3];
	unsigned palette_count;
	unsigned char palette_alpha[256];
	unsigned transparent[3];
	int has_transparent;
	unsigned char *compressed;
	size_t compressed_length;
};

static int png_begin(png_imagep image, const unsigned char *data, size_t size, unsigned char *owned);
static int png_fail(png_imagep image, const char *message);
static uint32_t png_word(const unsigned char *bytes);
static unsigned png_kind(const unsigned char *type);
static int png_transparency(png_imagep image, struct png_control *control, const unsigned char *data, uint32_t length);
static int png_chunks(png_imagep image, struct png_control *control, const unsigned char *data, size_t size);
static int png_header(png_imagep image, struct png_control *control, const unsigned char *data, uint32_t length);
static int png_append(struct png_control *control, const unsigned char *data, uint32_t length);
static unsigned png_channels(unsigned type);
static int png_unfilter(unsigned char *raw, size_t row_bytes, png_uint_32 height, size_t pixel_bytes);
static unsigned char png_paeth(unsigned char left, unsigned char up, unsigned char corner);
static void png_pixel(const struct png_control *control, const unsigned char *row, png_uint_32 x, unsigned char *rgba);
static unsigned png_sample(const unsigned char *row, png_uint_32 index, unsigned depth);
static void png_store(png_uint_32 format, const unsigned char *rgba, png_const_colorp background, unsigned char *out);
static unsigned char png_luminance(unsigned red, unsigned green, unsigned blue);
static void png_control_free(struct png_control *control);

/*
 * Starts reading a PNG file.
 */
int
png_image_begin_read_from_file(
	png_imagep image,
	const char *file_name)
{
	unsigned char *data;
	FILE *file;
	long size;
	size_t got;
	int seek;

	/* A caller of this version of the structure. */
	if (image == NULL || image->version != PNG_IMAGE_VERSION)
		return 0;

	/* The file. */
	file = fopen(file_name, "rb");
	if (file == NULL)
		return png_fail(image, "cannot open the file");

	/* Its size, within the largest read. */
	seek = fseek(file, 0L, SEEK_END);
	size = ftell(file);
	if (seek != 0 || size <= 0 || (unsigned long)size > PNG_FILE_MAX) {
		fclose(file);
		return png_fail(image, "not a file of a size that can be read");
	}

	/* Back to its start. */
	rewind(file);

	/* All of it in memory. */
	data = malloc((size_t)size);
	if (data == NULL) {
		fclose(file);
		return png_fail(image, "out of memory");
	}

	/* Its bytes. */
	got = fread(data, 1U, (size_t)size, file);
	fclose(file);
	if (got != (size_t)size) {
		free(data);
		return png_fail(image, "cannot read the file");
	}

	/* The chunks, the image keeping the bytes. */
	return png_begin(image, data, (size_t)size, data);
}

/*
 * Starts reading a PNG in memory (the memory must stay until the image is
 * finished or freed).
 */
int
png_image_begin_read_from_memory(
	png_imagep image,
	png_const_voidp memory,
	size_t size)
{
	/* A caller of this version of the structure, and some bytes. */
	if (image == NULL || image->version != PNG_IMAGE_VERSION)
		return 0;
	if (memory == NULL || size == 0U)
		return png_fail(image, "no memory to read");

	/* The chunks. */
	return png_begin(image, memory, size, NULL);
}

/*
 * Reads the pixels into a buffer in image->format, then frees the image.
 */
int
png_image_finish_read(
	png_imagep image,
	png_const_colorp background,
	void *buffer,
	png_int_32 row_stride,
	void *colormap)
{
	static const png_uint_32 pass_x[] = { 0U, 4U, 0U, 2U, 0U, 1U, 0U };
	static const png_uint_32 pass_y[] = { 0U, 0U, 4U, 0U, 2U, 0U, 1U };
	static const png_uint_32 pass_dx[] = { 8U, 8U, 4U, 4U, 2U, 2U, 1U };
	static const png_uint_32 pass_dy[] = { 8U, 8U, 8U, 4U, 4U, 2U, 2U };
	struct png_control *control;
	unsigned char *raw;
	unsigned char *pass_raw;
	unsigned char *out;
	unsigned char rgba[4];
	uLongf raw_length;
	size_t expected_length;
	size_t row_bytes;
	size_t pixel_bytes;
	size_t stride;
	size_t raw_at;
	unsigned channels;
	unsigned bits;
	unsigned pass;
	unsigned pass_count;
	png_uint_32 start_x;
	png_uint_32 start_y;
	png_uint_32 step_x;
	png_uint_32 step_y;
	png_uint_32 pass_width;
	png_uint_32 pass_height;
	png_uint_32 image_x;
	png_uint_32 image_y;
	png_uint_32 x;
	png_uint_32 y;
	int result;

	/* An image begun, and a format of 8-bit components without a colour map. */
	(void)colormap;
	if (image == NULL || image->opaque == NULL)
		return 0;
	control = image->opaque;
	if ((image->format & (PNG_FORMAT_FLAG_LINEAR | PNG_FORMAT_FLAG_COLORMAP)) != 0U) {
		png_image_free(image);
		return png_fail(image, "only 8-bit formats without a colour map are written");
	}

	/* A buffer to write. */
	if (buffer == NULL) {
		png_image_free(image);
		return png_fail(image, "no buffer");
	}

	/* The samples in a pixel and the bytes the filters look left by. */
	channels = png_channels(control->type);
	bits = channels * control->depth;
	pixel_bytes = (bits + 7U) / 8U;

	/* The decompressed size is one filtered row for the image, or for each nonempty Adam7 pass. */
	pass_count = 1U;
	if (control->interlaced)
		pass_count = 7U;
	expected_length = 0;
	for (pass = 0; pass < pass_count; pass++) {
		start_x = 0U;
		start_y = 0U;
		step_x = 1U;
		step_y = 1U;
		if (control->interlaced) {
			start_x = pass_x[pass];
			start_y = pass_y[pass];
			step_x = pass_dx[pass];
			step_y = pass_dy[pass];
		}

		/* The number of samples in each direction of this pass. */
		pass_width = 0U;
		pass_height = 0U;
		if (control->width > start_x)
			pass_width = (control->width - start_x + step_x - 1U) / step_x;
		if (control->height > start_y)
			pass_height = (control->height - start_y + step_y - 1U) / step_y;
		if (pass_width == 0U || pass_height == 0U)
			continue;
		row_bytes = ((size_t)pass_width * bits + 7U) / 8U;
		expected_length += (row_bytes + 1U) * pass_height;
	}

	/* The pixels decompressed. */
	raw_length = (uLongf)expected_length;
	raw = malloc(expected_length);
	if (raw == NULL) {
		png_image_free(image);
		return png_fail(image, "out of memory");
	}

	/* The pixels, exactly as many bytes as the rows take. */
	result = uncompress(raw, &raw_length, control->compressed, (uLong)control->compressed_length);
	if (result != Z_OK || raw_length != (uLongf)expected_length) {
		free(raw);
		png_image_free(image);
		return png_fail(image, "the pixel data is damaged");
	}

	/* The caller's row stride (a negative stride goes up from the last row). */
	stride = (size_t)PNG_IMAGE_SAMPLE_CHANNELS(image->format) * control->width;
	if (row_stride > 0)
		stride = (size_t)row_stride;
	if (row_stride < 0)
		stride = (size_t)(-(long)row_stride);

	/* Each pass is unfiltered alone, then its pixels are scattered into their full-image rows and columns. */
	raw_at = 0;
	for (pass = 0; pass < pass_count; pass++) {
		start_x = 0U;
		start_y = 0U;
		step_x = 1U;
		step_y = 1U;
		if (control->interlaced) {
			start_x = pass_x[pass];
			start_y = pass_y[pass];
			step_x = pass_dx[pass];
			step_y = pass_dy[pass];
		}

		/* The number of samples in each direction of this pass. */
		pass_width = 0U;
		pass_height = 0U;
		if (control->width > start_x)
			pass_width = (control->width - start_x + step_x - 1U) / step_x;
		if (control->height > start_y)
			pass_height = (control->height - start_y + step_y - 1U) / step_y;
		if (pass_width == 0U || pass_height == 0U)
			continue;
		row_bytes = ((size_t)pass_width * bits + 7U) / 8U;
		pass_raw = raw + raw_at;
		result = png_unfilter(pass_raw, row_bytes, pass_height, pixel_bytes);
		if (result != 0) {
			free(raw);
			png_image_free(image);
			return png_fail(image, "unknown row filter");
		}

		/* The pass's pixels at their Adam7 coordinates. */
		for (y = 0; y < pass_height; y++) {
			image_y = start_y + y * step_y;
			out = (unsigned char *)buffer + (size_t)image_y * stride;
			if (row_stride < 0)
				out = (unsigned char *)buffer + (size_t)(control->height - 1U - image_y) * stride;
			for (x = 0; x < pass_width; x++) {
				image_x = start_x + x * step_x;
				png_pixel(control, pass_raw + (size_t)y * (row_bytes + 1U) + 1U, x, rgba);
				png_store(image->format, rgba, background,
				    out + (size_t)image_x * PNG_IMAGE_SAMPLE_CHANNELS(image->format));
			}
		}

		/* The next pass starts after these filtered rows. */
		raw_at += (row_bytes + 1U) * pass_height;
	}

	/* The raw pixels and the image go. */
	free(raw);
	png_image_free(image);

	/* Succeeded: the buffer holds the picture. */
	return 1;
}

/*
 * Frees what an image holds.
 */
void
png_image_free(
	png_imagep image)
{
	/* Nothing to free. */
	if (image == NULL || image->opaque == NULL)
		return;

	/* The state and what it owns. */
	png_control_free(image->opaque);
	image->opaque = NULL;
}

/* Reads a PNG's chunks into a new state of the image, which fills in its size and format. */
static int
png_begin(
	png_imagep image,
	const unsigned char *data,
	size_t size,
	unsigned char *owned)
{
	static const unsigned char signature[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
	struct png_control *control;
	int differs;
	int result;

	/* A clean image. */
	image->opaque = NULL;
	image->warning_or_error = 0;
	image->message[0] = '\0';
	image->flags = 0;
	image->colormap_entries = 0;

	/* The signature. */
	differs = 1;
	if (size >= 8U)
		differs = memcmp(data, signature, 8U);
	if (differs != 0) {
		free(owned);
		return png_fail(image, "not a PNG file");
	}

	/* The state, which owns the file's bytes when it was read from a file. */
	control = calloc(1, sizeof(*control));
	if (control == NULL) {
		free(owned);
		return png_fail(image, "out of memory");
	}

	/* It owns the file's bytes when it was read from a file. */
	control->file = owned;

	/* The chunks. */
	result = png_chunks(image, control, data + 8, size - 8U);
	if (result == 0) {
		png_control_free(control);
		return 0;
	}

	/* The image's size and the format of what it holds (16-bit components are linear in libpng's terms). */
	image->opaque = control;
	image->width = control->width;
	image->height = control->height;
	image->format = PNG_FORMAT_GRAY;
	if (control->type == PNG_TYPE_RGB || control->type == PNG_TYPE_RGBA || control->type == PNG_TYPE_PALETTE)
		image->format |= PNG_FORMAT_FLAG_COLOR;
	if (control->type == PNG_TYPE_GRAY_ALPHA || control->type == PNG_TYPE_RGBA || control->has_transparent)
		image->format |= PNG_FORMAT_FLAG_ALPHA;
	if (control->depth == 16U)
		image->format |= PNG_FORMAT_FLAG_LINEAR;

	/* Succeeded: the pixels are read by png_image_finish_read. */
	return 1;
}

/* Marks an image as failed with a message; returns 0 for the caller to return. */
static int
png_fail(
	png_imagep image,
	const char *message)
{
	/* The error and why. */
	image->warning_or_error = PNG_IMAGE_ERROR;
	snprintf(image->message, sizeof(image->message), "%s", message);
	return 0;
}

/* Reads a 4-byte big-endian number. */
static uint32_t
png_word(
	const unsigned char *bytes)
{
	/* Most significant byte first. */
	return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) | ((uint32_t)bytes[2] << 8) | (uint32_t)bytes[3];
}

/*
 * Reads the chunks after the signature, each with its CRC checked: IHDR
 * first, PLTE, tRNS and the IDATs kept, IEND last; other critical chunks
 * refused, ancillary ones skipped.  Returns 1, or 0 with the image failed.
 */
static int
png_chunks(
	png_imagep image,
	struct png_control *control,
	const unsigned char *data,
	size_t size)
{
	const unsigned char *body;
	uint32_t length;
	uint32_t crc;
	uint32_t stored;
	unsigned kind;
	size_t at;
	int seen_header;
	int seen_end;
	int result;

	/* Chunk after chunk until IEND. */
	at = 0;
	seen_header = 0;
	seen_end = 0;
	while (!seen_end) {
		/* Its length and type, its body and its CRC must be there. */
		if (size - at < 12U)
			return png_fail(image, "the file ends inside a chunk");
		length = png_word(data + at);
		if (length > size - at - 12U)
			return png_fail(image, "the file ends inside a chunk");
		body = data + at + 8U;

		/* The CRC over the type and the body. */
		crc = (uint32_t)crc32(crc32(0L, NULL, 0), data + at + 4U, length + 4U);
		stored = png_word(body + length);
		if (crc != stored)
			return png_fail(image, "a chunk's CRC is wrong");

		/* The header comes first. */
		kind = png_kind(data + at + 4U);
		if (!seen_header && kind != PNG_CHUNK_IHDR)
			return png_fail(image, "the header chunk is not first");

		/* Each chunk by its type. */
		if (kind == PNG_CHUNK_IHDR) {
			result = png_header(image, control, body, length);
			if (result == 0)
				return 0;
			seen_header = 1;
		} else if (kind == PNG_CHUNK_PLTE) {
			if (length % 3U != 0U || length / 3U > 256U || length == 0U)
				return png_fail(image, "a palette of a wrong size");
			control->palette_count = length / 3U;
			memcpy(control->palette, body, length);
			memset(control->palette_alpha, 0xff, sizeof(control->palette_alpha));
		} else if (kind == PNG_CHUNK_TRNS) {
			result = png_transparency(image, control, body, length);
			if (result == 0)
				return 0;
		} else if (kind == PNG_CHUNK_IDAT) {
			result = png_append(control, body, length);
			if (result != 0)
				return png_fail(image, "out of memory");
		} else if (kind == PNG_CHUNK_IEND) {
			seen_end = 1;
		} else if ((data[at + 4U] & 0x20U) == 0U) {
			return png_fail(image, "an unknown critical chunk");
		}

		/* The next chunk. */
		at += (size_t)length + 12U;
	}

	/* A palette image needs its palette, and every image some pixels. */
	if (control->type == PNG_TYPE_PALETTE && control->palette_count == 0U)
		return png_fail(image, "a palette image without a palette");
	if (control->compressed_length == 0U)
		return png_fail(image, "no pixel data");

	/* Succeeded. */
	return 1;
}

/* Names a chunk by its type (four letters), PNG_CHUNK_OTHER for one not read. */
static unsigned
png_kind(
	const unsigned char *type)
{
	static const char *const names[] = { "IHDR", "PLTE", "tRNS", "IDAT", "IEND" };
	unsigned index;
	int differs;

	/* Each type read, in PNG_CHUNK_* order from 1. */
	for (index = 0; index < 5U; index++) {
		differs = memcmp(type, names[index], 4U);
		if (differs == 0)
			return index + 1U;
	}

	/* Not one of them. */
	return PNG_CHUNK_OTHER;
}

/*
 * Reads tRNS: the palette's alphas, or the transparent gray or RGB colour
 * (at the file's depth).  Returns 1, or 0 with the image failed.
 */
static int
png_transparency(
	png_imagep image,
	struct png_control *control,
	const unsigned char *data,
	uint32_t length)
{
	uint32_t index;

	/* A palette's alphas, one a colour from the first. */
	if (control->type == PNG_TYPE_PALETTE) {
		if (length > 256U)
			return png_fail(image, "a transparency of a wrong size");
		memcpy(control->palette_alpha, data, length);
		control->has_transparent = 1;
		return 1;
	}

	/* A gray image's transparent gray. */
	if (control->type == PNG_TYPE_GRAY && length == 2U) {
		control->transparent[0] = ((unsigned)data[0] << 8) | data[1];
		control->has_transparent = 1;
		return 1;
	}

	/* An RGB image's transparent colour. */
	if (control->type == PNG_TYPE_RGB && length == 6U) {
		for (index = 0; index < 3U; index++)
			control->transparent[index] = ((unsigned)data[index * 2U] << 8) | data[index * 2U + 1U];
		control->has_transparent = 1;
	}

	/* Succeeded (a transparency of another kind is ignored). */
	return 1;
}

/* Reads IHDR: the size, a bit depth that goes with the colour type, and the interlace method. */
static int
png_header(
	png_imagep image,
	struct png_control *control,
	const unsigned char *data,
	uint32_t length)
{
	unsigned depth;
	unsigned type;
	int allowed;

	/* Thirteen bytes. */
	if (length != 13U)
		return png_fail(image, "a header of a wrong size");
	control->width = png_word(data);
	control->height = png_word(data + 4U);
	depth = data[8];
	type = data[9];

	/* A size that can be read. */
	if (control->width == 0U || control->height == 0U || control->width > PNG_SIDE_MAX || control->height > PNG_SIDE_MAX)
		return png_fail(image, "an image of a size that is not read");

	/* The depths each colour type allows. */
	allowed = 0;
	if (type == PNG_TYPE_GRAY && (depth == 1U || depth == 2U || depth == 4U || depth == 8U || depth == 16U))
		allowed = 1;
	if (type == PNG_TYPE_PALETTE && (depth == 1U || depth == 2U || depth == 4U || depth == 8U))
		allowed = 1;
	if ((type == PNG_TYPE_RGB || type == PNG_TYPE_GRAY_ALPHA || type == PNG_TYPE_RGBA) && (depth == 8U || depth == 16U))
		allowed = 1;
	if (!allowed)
		return png_fail(image, "a colour type and bit depth that do not go together");

	/* Deflate, the adaptive filters, and either plain rows or Adam7. */
	if (data[10] != 0U || data[11] != 0U)
		return png_fail(image, "an unknown compression or filter method");
	if (data[12] > 1U)
		return png_fail(image, "an unknown interlace method");

	/* Succeeded. */
	control->depth = depth;
	control->type = type;
	control->interlaced = data[12] == 1U;
	return 1;
}

/* Adds an IDAT chunk's bytes to the compressed pixels; returns 0, or 1 without memory. */
static int
png_append(
	struct png_control *control,
	const unsigned char *data,
	uint32_t length)
{
	unsigned char *grown;

	/* Room for them. */
	grown = realloc(control->compressed, control->compressed_length + length + 1U);
	if (grown == NULL)
		return 1;
	control->compressed = grown;

	/* The bytes after those before. */
	memcpy(control->compressed + control->compressed_length, data, length);
	control->compressed_length += length;
	return 0;
}

/* Returns how many samples a pixel of a colour type has. */
static unsigned
png_channels(
	unsigned type)
{
	/* Gray and a palette index: one; gray and alpha: two; RGB: three; RGBA: four. */
	if (type == PNG_TYPE_GRAY_ALPHA)
		return 2U;
	if (type == PNG_TYPE_RGB)
		return 3U;
	if (type == PNG_TYPE_RGBA)
		return 4U;
	return 1U;
}

/*
 * Undoes each row's filter in place (None, Sub, Up, Average, Paeth), the
 * byte before a row's pixels being its filter.  Returns 0, or 1 for an
 * unknown filter.
 */
static int
png_unfilter(
	unsigned char *raw,
	size_t row_bytes,
	png_uint_32 height,
	size_t pixel_bytes)
{
	unsigned char *row;
	const unsigned char *above;
	unsigned left;
	unsigned up;
	unsigned corner;
	png_uint_32 y;
	size_t index;

	/* Row by row, each after the one above is done. */
	for (y = 0; y < height; y++) {
		row = raw + (size_t)y * (row_bytes + 1U) + 1U;
		above = NULL;
		if (y > 0U)
			above = row - (row_bytes + 1U);

		/* Each byte from the ones to its left and above (zero past the edges). */
		for (index = 0; index < row_bytes; index++) {
			left = 0;
			if (index >= pixel_bytes)
				left = row[index - pixel_bytes];
			up = 0;
			if (above != NULL)
				up = above[index];
			corner = 0;
			if (above != NULL && index >= pixel_bytes)
				corner = above[index - pixel_bytes];

			/* The row's filter. */
			switch (row[-1]) {
			case 0:
				break;
			case 1:
				row[index] = (unsigned char)(row[index] + left);
				break;
			case 2:
				row[index] = (unsigned char)(row[index] + up);
				break;
			case 3:
				row[index] = (unsigned char)(row[index] + ((left + up) >> 1));
				break;
			case 4:
				row[index] = (unsigned char)(row[index] + png_paeth((unsigned char)left, (unsigned char)up, (unsigned char)corner));
				break;
			default:
				return 1;
			}
		}
	}

	/* Succeeded. */
	return 0;
}

/* The Paeth predictor: of the left, upper and upper-left bytes, the one nearest to left + up - corner. */
static unsigned char
png_paeth(
	unsigned char left,
	unsigned char up,
	unsigned char corner)
{
	int estimate;
	int to_left;
	int to_up;
	int to_corner;

	/* The distances to the estimate. */
	estimate = (int)left + (int)up - (int)corner;
	to_left = abs(estimate - (int)left);
	to_up = abs(estimate - (int)up);
	to_corner = abs(estimate - (int)corner);

	/* The nearest, left first on a tie, then up. */
	if (to_left <= to_up && to_left <= to_corner)
		return left;
	if (to_up <= to_corner)
		return up;
	return corner;
}

/* Reads a pixel as 8-bit straight RGBA. */
static void
png_pixel(
	const struct png_control *control,
	const unsigned char *row,
	png_uint_32 x,
	unsigned char *rgba)
{
	unsigned samples[4];
	unsigned channels;
	unsigned index;
	unsigned maximum;

	/* The pixel's samples at the file's depth. */
	channels = png_channels(control->type);
	for (index = 0; index < channels; index++)
		samples[index] = png_sample(row, x * channels + index, control->depth);

	/* A palette index: its colour and its alpha. */
	if (control->type == PNG_TYPE_PALETTE) {
		index = samples[0];
		if (index >= control->palette_count)
			index = 0;
		rgba[0] = control->palette[index][0];
		rgba[1] = control->palette[index][1];
		rgba[2] = control->palette[index][2];
		rgba[3] = control->palette_alpha[index];
		return;
	}

	/* Opaque, unless the transparent colour (at the file's depth) or an alpha sample says otherwise. */
	rgba[3] = 255;
	if (control->has_transparent) {
		if (control->type == PNG_TYPE_GRAY && samples[0] == control->transparent[0])
			rgba[3] = 0;
		if (control->type == PNG_TYPE_RGB &&
		    samples[0] == control->transparent[0] &&
		    samples[1] == control->transparent[1] &&
		    samples[2] == control->transparent[2])
			rgba[3] = 0;
	}

	/* Each sample to 8 bits: the high byte of 16, or a low depth stretched over 0 to 255. */
	maximum = (1U << control->depth) - 1U;
	for (index = 0; index < channels; index++) {
		if (control->depth == 16U) {
			samples[index] >>= 8;
		} else if (control->depth < 8U) {
			samples[index] = samples[index] * 255U / maximum;
		}
	}

	/* Gray (with alpha) or colour (with alpha). */
	if (control->type == PNG_TYPE_GRAY || control->type == PNG_TYPE_GRAY_ALPHA) {
		rgba[0] = (unsigned char)samples[0];
		rgba[1] = (unsigned char)samples[0];
		rgba[2] = (unsigned char)samples[0];
		if (control->type == PNG_TYPE_GRAY_ALPHA)
			rgba[3] = (unsigned char)samples[1];
		return;
	}

	/* Colour. */
	rgba[0] = (unsigned char)samples[0];
	rgba[1] = (unsigned char)samples[1];
	rgba[2] = (unsigned char)samples[2];
	if (control->type == PNG_TYPE_RGBA)
		rgba[3] = (unsigned char)samples[3];
}

/* Reads the index'th sample of a row at a bit depth (big-endian samples, high bits first below 8). */
static unsigned
png_sample(
	const unsigned char *row,
	png_uint_32 index,
	unsigned depth)
{
	size_t bit;
	unsigned shift;

	/* Two bytes, one byte, or a few bits of a byte. */
	if (depth == 16U)
		return ((unsigned)row[(size_t)index * 2U] << 8) | row[(size_t)index * 2U + 1U];
	if (depth == 8U)
		return row[index];
	bit = (size_t)index * depth;
	shift = 8U - depth - (unsigned)(bit % 8U);
	return (row[bit / 8U] >> shift) & ((1U << depth) - 1U);
}

/* Writes a straight RGBA pixel in a format: gray or colour, in its order, with or without alpha. */
static void
png_store(
	png_uint_32 format,
	const unsigned char *rgba,
	png_const_colorp background,
	unsigned char *out)
{
	unsigned colour[3];
	unsigned alpha;
	unsigned index;
	unsigned at;

	/* The colour, laid on the background when the format drops a pixel's alpha. */
	alpha = rgba[3];
	colour[0] = rgba[0];
	colour[1] = rgba[1];
	colour[2] = rgba[2];
	if ((format & PNG_FORMAT_FLAG_ALPHA) == 0U && alpha < 255U && background != NULL) {
		colour[0] = (colour[0] * alpha + background->red * (255U - alpha) + 127U) / 255U;
		colour[1] = (colour[1] * alpha + background->green * (255U - alpha) + 127U) / 255U;
		colour[2] = (colour[2] * alpha + background->blue * (255U - alpha) + 127U) / 255U;
	}

	/* Alpha first, when the format puts it there. */
	at = 0;
	if ((format & PNG_FORMAT_FLAG_ALPHA) != 0U && (format & PNG_FORMAT_FLAG_AFIRST) != 0U) {
		out[at] = (unsigned char)alpha;
		at++;
	}

	/* Gray, or the three colours in the format's order. */
	if ((format & PNG_FORMAT_FLAG_COLOR) == 0U) {
		out[at] = png_luminance(colour[0], colour[1], colour[2]);
		at++;
	} else {
		for (index = 0; index < 3U; index++) {
			out[at] = (unsigned char)colour[index];
			if ((format & PNG_FORMAT_FLAG_BGR) != 0U)
				out[at] = (unsigned char)colour[2U - index];
			at++;
		}
	}

	/* Alpha last, when the format has it there. */
	if ((format & PNG_FORMAT_FLAG_ALPHA) != 0U && (format & PNG_FORMAT_FLAG_AFIRST) == 0U)
		out[at] = (unsigned char)alpha;
}

/* The gray of a colour (Rec. 709 weights, as libpng uses for sRGB, in fixed point). */
static unsigned char
png_luminance(
	unsigned red,
	unsigned green,
	unsigned blue)
{
	unsigned gray;

	/* The weighted sum, rounded. */
	gray = (red * 6968U + green * 23434U + blue * 2366U + 16384U) >> 15;
	if (gray > 255U)
		gray = 255U;
	return (unsigned char)gray;
}

/* Frees an image's state and what it owns. */
static void
png_control_free(
	struct png_control *control)
{
	/* The compressed pixels, the file's bytes, the state. */
	free(control->compressed);
	free(control->file);
	free(control);
}
