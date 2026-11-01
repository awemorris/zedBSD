/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A host tool of the Windows build: writes OmniGrammaton.ico, the browser's
 * picture on its tile as the Keiland desktop draws it (the compositor's
 * icons.c, built into this tool: kwl_icon_tile of the "browser"
 * application), at 16, 20, 24, 32, 40, 48, 64 and 256 pixels.  The 256
 * pixel image is a PNG (png-write.c), the others are DIB images of straight
 * alpha (ico.c).
 *
 *   make-ico OUTPUT.ico
 */

#include "apps/browser-win32/tools/ico.h"
#include "userland/desktop/picture/png-write.h"
#include "userland/desktop/wayland/icons.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The sizes the icon holds, the last one a PNG. */
static const uint32_t make_ico_sizes[] = { 16U, 20U, 24U, 32U, 40U, 48U, 64U, 256U };

#define MAKE_ICO_COUNT	(sizeof(make_ico_sizes) / sizeof(make_ico_sizes[0]))

static void make_ico_straight(uint32_t *pixels, size_t count);
static int make_ico_save(const char *path, const unsigned char *bytes, size_t length);

int
main(
	int argc,
	char **argv)
{
	struct ico_image images[MAKE_ICO_COUNT];
	uint32_t *pixels[MAKE_ICO_COUNT];
	unsigned char *png;
	unsigned char *file;
	size_t png_length;
	size_t length;
	unsigned index;
	uint32_t side;
	int icon;
	int error;

	/* One argument: the output. */
	if (argc != 2) {
		fprintf(stderr, "usage: make-ico OUTPUT.ico\n");
		return 2;
	}

	/* The browser's picture. */
	icon = kwl_icon_for_app_id("browser");
	if (icon < 0) {
		fprintf(stderr, "make-ico: no picture for the browser\n");
		return 1;
	}

	/* Each size: the tile, premultiplied as the desktop draws it. */
	png = NULL;
	png_length = 0U;
	memset(images, 0, sizeof(images));
	for (index = 0U; index < MAKE_ICO_COUNT; index++) {
		side = make_ico_sizes[index];
		pixels[index] = calloc((size_t)side * side, sizeof(uint32_t));
		if (pixels[index] == NULL) {
			fprintf(stderr, "make-ico: out of memory\n");
			return 1;
		}
		kwl_icon_tile((unsigned)icon, side, pixels[index], side);
		images[index].width = side;
		images[index].height = side;

		/* The largest as a PNG (which takes premultiplied pixels), the others as straight-alpha DIBs. */
		if (side == ICO_SIZE_MAX) {
			error = kl_picture_png(pixels[index], (int)side, (int)side, side, &png, &png_length);
			if (error != 0) {
				fprintf(stderr, "make-ico: cannot write the PNG (%d)\n", error);
				return 1;
			}
			images[index].png = png;
			images[index].png_length = png_length;
		} else {
			make_ico_straight(pixels[index], (size_t)side * side);
			images[index].pixels = pixels[index];
			images[index].stride = side;
		}
	}

	/* The icon file. */
	error = ico_write(images, (unsigned)MAKE_ICO_COUNT, &file, &length);
	if (error != 0) {
		fprintf(stderr, "make-ico: cannot write the icon (%d)\n", error);
		return 1;
	}
	error = make_ico_save(argv[1], file, length);
	free(file);
	free(png);
	for (index = 0U; index < MAKE_ICO_COUNT; index++)
		free(pixels[index]);
	if (error != 0) {
		fprintf(stderr, "make-ico: cannot write %s (%d)\n", argv[1], error);
		return 1;
	}

	/* Succeeded. */
	return 0;
}

/* Divides each pixel's colours by its alpha (premultiplied to straight). */
static void
make_ico_straight(
	uint32_t *pixels,
	size_t count)
{
	uint32_t alpha;
	uint32_t red;
	uint32_t green;
	uint32_t blue;
	size_t index;

	/* Each pixel with some alpha. */
	for (index = 0U; index < count; index++) {
		alpha = pixels[index] >> 24;
		if (alpha == 0U || alpha == 255U)
			continue;
		red = ((pixels[index] >> 16) & 0xffU) * 255U / alpha;
		green = ((pixels[index] >> 8) & 0xffU) * 255U / alpha;
		blue = (pixels[index] & 0xffU) * 255U / alpha;
		if (red > 255U)
			red = 255U;
		if (green > 255U)
			green = 255U;
		if (blue > 255U)
			blue = 255U;
		pixels[index] = (alpha << 24) | (red << 16) | (green << 8) | blue;
	}
}

/* Writes bytes to a file; 0 or an errno value. */
static int
make_ico_save(
	const char *path,
	const unsigned char *bytes,
	size_t length)
{
	FILE *out;
	size_t written;
	int closed;

	/* The file, written whole. */
	out = fopen(path, "wb");
	if (out == NULL)
		return errno;
	written = fwrite(bytes, 1U, length, out);
	closed = fclose(out);
	if (written != length || closed != 0)
		return EIO;

	/* Succeeded. */
	return 0;
}
