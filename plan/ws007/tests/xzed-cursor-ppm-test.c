/*
 * WS007 GFX-T02: locate Xzed's standard left_ptr in two QEMU PPM captures.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
 */

#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CURSOR_WIDTH 16
#define CURSOR_HEIGHT 16
#define CURSOR_HOT_X 3
#define CURSOR_HOT_Y 1

/* Keep this fixture synchronized with Xzed's public visual cursor contract.
 * Source bits are black; mask-only bits are white. */
static const uint16_t pointer_source[CURSOR_HEIGHT] = {
	0x0000, 0x0008, 0x0018, 0x0038, 0x0078, 0x00f8, 0x01f8, 0x03f8,
	0x07f8, 0x00f8, 0x00d8, 0x0188, 0x0180, 0x0300, 0x0300, 0x0000
};
static const uint16_t pointer_mask[CURSOR_HEIGHT] = {
	0x000c, 0x001c, 0x003c, 0x007c, 0x00fc, 0x01fc, 0x03fc, 0x07fc,
	0x0ffc, 0x0ffc, 0x01fc, 0x03dc, 0x03cc, 0x0780, 0x0780, 0x0300
};

struct ppm {
	unsigned width;
	unsigned height;
	uint8_t *pixels;
};

static void
fail(const char *message, const char *path)
{
	if (path != NULL)
		fprintf(stderr, "%s: %s\n", path, message);
	else
		fprintf(stderr, "%s\n", message);
	exit(1);
}

static int
read_token(FILE *stream, char *token, size_t capacity)
{
	int character;
	size_t used = 0;

	do {
		character = fgetc(stream);
		if (character == '#') {
			do character = fgetc(stream);
			while (character != '\n' && character != EOF);
		}
	} while (character != EOF && isspace((unsigned char)character));
	if (character == EOF)
		return 0;
	do {
		if (used + 1 >= capacity)
			return 0;
		token[used++] = (char)character;
		character = fgetc(stream);
	} while (character != EOF && !isspace((unsigned char)character));
	token[used] = '\0';
	return 1;
}

static unsigned
parse_unsigned(const char *token, const char *path)
{
	char *end;
	unsigned long value;

	errno = 0;
	value = strtoul(token, &end, 10);
	if (errno != 0 || *token == '\0' || *end != '\0' ||
	    value == 0 || value > UINT32_MAX)
		fail("invalid PPM integer", path);
	return (unsigned)value;
}

static struct ppm
read_ppm(const char *path)
{
	struct ppm image = {0};
	char token[64];
	FILE *stream;
	size_t bytes;

	stream = fopen(path, "rb");
	if (stream == NULL)
		fail(strerror(errno), path);
	if (!read_token(stream, token, sizeof(token)) || strcmp(token, "P6") != 0)
		fail("not a binary PPM", path);
	if (!read_token(stream, token, sizeof(token)))
		fail("missing PPM width", path);
	image.width = parse_unsigned(token, path);
	if (!read_token(stream, token, sizeof(token)))
		fail("missing PPM height", path);
	image.height = parse_unsigned(token, path);
	if (!read_token(stream, token, sizeof(token)) ||
	    parse_unsigned(token, path) != 255)
		fail("unsupported PPM sample range", path);
	if (image.width > SIZE_MAX / image.height / 3U)
		fail("PPM extent overflow", path);
	bytes = (size_t)image.width * image.height * 3U;
	image.pixels = malloc(bytes);
	if (image.pixels == NULL)
		fail("out of memory", path);
	if (fread(image.pixels, 1, bytes, stream) != bytes || fgetc(stream) != EOF)
		fail("truncated or trailing PPM data", path);
	if (fclose(stream) != 0)
		fail(strerror(errno), path);
	return image;
}

static int
cursor_matches(const struct ppm *image, unsigned hot_x, unsigned hot_y)
{
	unsigned row, column;

	if (hot_x < CURSOR_HOT_X || hot_y < CURSOR_HOT_Y ||
	    hot_x - CURSOR_HOT_X + CURSOR_WIDTH > image->width ||
	    hot_y - CURSOR_HOT_Y + CURSOR_HEIGHT > image->height)
		return 0;
	for (row = 0; row < CURSOR_HEIGHT; row++) {
		for (column = 0; column < CURSOR_WIDTH; column++) {
			uint16_t bit = (uint16_t)(1U << column);
			unsigned x, y;
			const uint8_t *pixel;
			uint8_t expected;

			if ((pointer_mask[row] & bit) == 0)
				continue;
			x = hot_x - CURSOR_HOT_X + column;
			y = hot_y - CURSOR_HOT_Y + row;
			pixel = image->pixels + ((size_t)y * image->width + x) * 3U;
			expected = (pointer_source[row] & bit) != 0 ? 0 : 255;
			if (pixel[0] != expected || pixel[1] != expected ||
			    pixel[2] != expected)
				return 0;
		}
	}
	return 1;
}

static void
locate_cursor(const struct ppm *image, const char *path,
	unsigned *result_x, unsigned *result_y)
{
	unsigned x, y, found = 0;

	for (y = CURSOR_HOT_Y; y + CURSOR_HEIGHT - CURSOR_HOT_Y <= image->height;
	    y++) {
		for (x = CURSOR_HOT_X;
		    x + CURSOR_WIDTH - CURSOR_HOT_X <= image->width; x++) {
			if (!cursor_matches(image, x, y))
				continue;
			*result_x = x;
			*result_y = y;
			found++;
		}
	}
	if (found != 1) {
		fprintf(stderr, "%s: expected one left_ptr, found %u\n", path, found);
		exit(1);
	}
}

int
main(int argc, char **argv)
{
	struct ppm before, after;
	unsigned before_x = 0, before_y = 0, after_x = 0, after_y = 0;

	if (argc != 2 && argc != 3) {
		fprintf(stderr, "usage: %s BEFORE.ppm [AFTER.ppm]\n", argv[0]);
		return 2;
	}
	before = read_ppm(argv[1]);
	locate_cursor(&before, argv[1], &before_x, &before_y);
	if (argc == 2) {
		printf("GFX-T02 Xzed cursor: (%u,%u)\n", before_x, before_y);
		free(before.pixels);
		return 0;
	}
	after = read_ppm(argv[2]);
	if (before.width != after.width || before.height != after.height)
		fail("capture geometry changed", NULL);
	locate_cursor(&after, argv[2], &after_x, &after_y);
	if (after_x != before_x + 100U || after_y != before_y + 50U) {
		fprintf(stderr, "cursor moved (%u,%u) -> (%u,%u), expected +100,+50\n",
		    before_x, before_y, after_x, after_y);
		return 1;
	}
	printf("GFX-T02 Xzed cursor: (%u,%u) -> (%u,%u): PASS\n",
	    before_x, before_y, after_x, after_y);
	free(before.pixels);
	free(after.pixels);
	return 0;
}
