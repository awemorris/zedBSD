/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Checks one fixture against dwebp's output with libwebp-compat's inner
 * modules alone (before the interface of decode.c exists), for the
 * decoders' Phases:
 *
 *   fixture-check lossless FILE.webp EXPECT.pam      VP8L ARGB = the PAM's RGBA
 *   fixture-check lossy FILE.webp EXPECT.yuv [nofilter]
 *                                                   VP8 planes = dwebp -yuv's Y, U, V
 *   fixture-check alpha FILE.webp EXPECT.pam         ALPH values = the PAM's alpha
 *
 * Prints one line, "ok NAME" or "mismatch NAME: <what>", and exits 0 for
 * ok, 1 for a mismatch or an error.
 */

#include "riff.h"
#include "vp8l.h"
#include "vp8.h"
#include "alpha.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *check_read(const char *path, size_t *size);
static const uint8_t *check_pam_pixels(const uint8_t *pam, size_t size, unsigned width, unsigned height);
static int check_lossless(const char *name, const struct webpc_container *container, const uint8_t *expect,
	size_t expect_size);
static int check_lossy(const char *name, const struct webpc_container *container, const uint8_t *expect,
	size_t expect_size, int nofilter);
static int check_alpha(const char *name, const struct webpc_container *container, const uint8_t *expect,
	size_t expect_size);

/*
 * Checks one fixture as the command line says.
 */
int
main(
	int argc,
	char **argv)
{
	struct webpc_container container;
	uint8_t *file;
	uint8_t *expect;
	size_t file_size;
	size_t expect_size;
	int error;
	int result;

	/* The arguments and the two files. */
	if (argc < 4) {
		fprintf(stderr, "usage: fixture-check lossless|lossy|alpha FILE.webp EXPECT [nofilter]\n");
		return 2;
	}
	file = check_read(argv[2], &file_size);
	expect = check_read(argv[3], &expect_size);
	if (file == NULL || expect == NULL) {
		printf("mismatch %s: unreadable input\n", argv[2]);
		return 1;
	}

	/* The container, as a decoding reads it. */
	error = webpc_riff_parse(file, file_size, 1, &container);
	if (error != 0 || container.bitstream == NULL) {
		printf("mismatch %s: container error %d\n", argv[2], error);
		return 1;
	}

	/* The check the mode names. */
	if (strcmp(argv[1], "lossless") == 0)
		result = check_lossless(argv[2], &container, expect, expect_size);
	else if (strcmp(argv[1], "lossy") == 0)
		result = check_lossy(argv[2], &container, expect, expect_size, argc > 4 && strcmp(argv[4], "nofilter") == 0);
	else if (strcmp(argv[1], "alpha") == 0)
		result = check_alpha(argv[2], &container, expect, expect_size);
	else
		result = 2;
	free(file);
	free(expect);
	return result;
}

/* Reads a whole file into a new buffer. */
static uint8_t *
check_read(
	const char *path,
	size_t *size)
{
	uint8_t *bytes;
	FILE *file;
	long length;

	file = fopen(path, "rb");
	if (file == NULL)
		return NULL;
	if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) < 0 || fseek(file, 0, SEEK_SET) != 0) {
		fclose(file);
		return NULL;
	}
	bytes = malloc(length > 0 ? (size_t)length : 1U);
	if (bytes == NULL || fread(bytes, 1, (size_t)length, file) != (size_t)length) {
		fclose(file);
		free(bytes);
		return NULL;
	}
	fclose(file);
	*size = (size_t)length;
	return bytes;
}

/* Finds the RGBA pixels of a PAM of the given size (dwebp -pam), or NULL. */
static const uint8_t *
check_pam_pixels(
	const uint8_t *pam,
	size_t size,
	unsigned width,
	unsigned height)
{
	char text[256];
	const char *end;
	const char *field;
	unsigned pam_width;
	unsigned pam_height;
	size_t header;
	size_t length;

	/* The header up to ENDHDR (copied and ended, the file's bytes are not), and the size it gives. */
	length = size < sizeof(text) - 1U ? size : sizeof(text) - 1U;
	memcpy(text, pam, length);
	text[length] = '\0';
	end = strstr(text, "ENDHDR\n");
	if (end == NULL)
		return NULL;
	header = (size_t)(end - text) + 7U;
	field = strstr(text, "WIDTH");
	if (field == NULL || sscanf(field, "WIDTH %u", &pam_width) != 1)
		return NULL;
	field = strstr(text, "HEIGHT");
	if (field == NULL || sscanf(field, "HEIGHT %u", &pam_height) != 1)
		return NULL;
	if (pam_width != width || pam_height != height || size < header + (size_t)width * height * 4U)
		return NULL;
	return pam + header;
}

/* Compares the lossless decoder's ARGB with the PAM. */
static int
check_lossless(
	const char *name,
	const struct webpc_container *container,
	const uint8_t *expect,
	size_t expect_size)
{
	const uint8_t *rgba;
	uint32_t *pixels;
	unsigned width;
	unsigned height;
	size_t index;
	size_t count;
	size_t wrong;
	uint32_t want;
	int error;

	error = webpc_vp8l_decode(container->bitstream, container->bitstream_size, &pixels, &width, &height);
	if (error != 0) {
		printf("mismatch %s: webpc_vp8l_decode %d\n", name, error);
		return 1;
	}
	rgba = check_pam_pixels(expect, expect_size, width, height);
	if (rgba == NULL) {
		printf("mismatch %s: size %ux%u differs from the PAM\n", name, width, height);
		free(pixels);
		return 1;
	}
	count = (size_t)width * height;
	wrong = 0;
	for (index = 0; index < count; index++) {
		want = ((uint32_t)rgba[index * 4U + 3U] << 24) | ((uint32_t)rgba[index * 4U] << 16) |
			((uint32_t)rgba[index * 4U + 1U] << 8) | (uint32_t)rgba[index * 4U + 2U];
		if (pixels[index] != want) {
			if (wrong == 0)
				printf("first difference at (%lu, %lu): %08lx, want %08lx\n", (unsigned long)(index % width),
					(unsigned long)(index / width), (unsigned long)pixels[index], (unsigned long)want);
			wrong++;
		}
	}
	free(pixels);
	if (wrong != 0) {
		printf("mismatch %s: %lu of %lu pixels\n", name, (unsigned long)wrong, (unsigned long)count);
		return 1;
	}
	printf("ok %s\n", name);
	return 0;
}

/* Compares the lossy decoder's planes with dwebp -yuv's. */
static int
check_lossy(
	const char *name,
	const struct webpc_container *container,
	const uint8_t *expect,
	size_t expect_size,
	int nofilter)
{
	struct webpc_vp8_picture picture;
	const uint8_t *planes[3];
	ptrdiff_t strides[3];
	const uint8_t *want;
	unsigned widths[3];
	unsigned heights[3];
	unsigned plane;
	unsigned row;
	unsigned column;
	size_t offset;
	size_t wrong;
	int error;

	error = webpc_vp8_decode(container->bitstream, container->bitstream_size, nofilter, &picture);
	if (error != 0) {
		printf("mismatch %s: webpc_vp8_decode %d\n", name, error);
		return 1;
	}
	planes[0] = picture.y;
	planes[1] = picture.u;
	planes[2] = picture.v;
	strides[0] = picture.y_stride;
	strides[1] = picture.uv_stride;
	strides[2] = picture.uv_stride;
	widths[0] = picture.width;
	heights[0] = picture.height;
	widths[1] = widths[2] = (picture.width + 1U) / 2U;
	heights[1] = heights[2] = (picture.height + 1U) / 2U;
	if (expect_size < (size_t)widths[0] * heights[0] + 2U * (size_t)widths[1] * heights[1]) {
		printf("mismatch %s: the expected planes are shorter than %ux%u\n", name, picture.width, picture.height);
		webpc_vp8_picture_release(&picture);
		return 1;
	}
	offset = 0;
	wrong = 0;
	for (plane = 0; plane < 3U; plane++) {
		for (row = 0; row < heights[plane]; row++) {
			want = expect + offset + (size_t)row * widths[plane];
			for (column = 0; column < widths[plane]; column++) {
				if (planes[plane][(ptrdiff_t)row * strides[plane] + column] != want[column]) {
					if (wrong == 0)
						printf("first difference in plane %u at (%u, %u): %u, want %u\n", plane, column, row,
							planes[plane][(ptrdiff_t)row * strides[plane] + column], want[column]);
					wrong++;
				}
			}
		}
		offset += (size_t)widths[plane] * heights[plane];
	}
	webpc_vp8_picture_release(&picture);
	if (wrong != 0) {
		printf("mismatch %s: %lu samples\n", name, (unsigned long)wrong);
		return 1;
	}
	printf("ok %s\n", name);
	return 0;
}

/* Compares the alpha chunk's decoded values with the PAM's alpha. */
static int
check_alpha(
	const char *name,
	const struct webpc_container *container,
	const uint8_t *expect,
	size_t expect_size)
{
	const uint8_t *rgba;
	uint8_t *alpha;
	size_t index;
	size_t count;
	size_t wrong;
	int error;

	if (container->alpha == NULL) {
		printf("mismatch %s: no ALPH chunk\n", name);
		return 1;
	}
	rgba = check_pam_pixels(expect, expect_size, container->width, container->height);
	if (rgba == NULL) {
		printf("mismatch %s: size differs from the PAM\n", name);
		return 1;
	}
	count = (size_t)container->width * container->height;
	alpha = malloc(count);
	if (alpha == NULL)
		return 1;
	error = webpc_alpha_decode(container->alpha, container->alpha_size, container->width, container->height, alpha);
	if (error != 0) {
		printf("mismatch %s: webpc_alpha_decode %d\n", name, error);
		free(alpha);
		return 1;
	}
	wrong = 0;
	for (index = 0; index < count; index++) {
		if (alpha[index] != rgba[index * 4U + 3U]) {
			if (wrong == 0)
				printf("first difference at (%lu, %lu): %u, want %u\n", (unsigned long)(index % container->width),
					(unsigned long)(index / container->width), alpha[index], rgba[index * 4U + 3U]);
			wrong++;
		}
	}
	free(alpha);
	if (wrong != 0) {
		printf("mismatch %s: %lu of %lu values\n", name, (unsigned long)wrong, (unsigned long)count);
		return 1;
	}
	printf("ok %s\n", name);
	return 0;
}
