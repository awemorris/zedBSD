/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * ws164 (q875): the preview client's preview_picture for Settings' host
 * tests (host-build.sh).  The real one (userland/desktop/preview/client.c,
 * WS168) runs the confined child, which the host does not have; this one
 * reads the picture in the process with the shared wallpaper decoder and
 * cuts it to the size asked for (cover) by the nearest pixel.  Test code
 * only; the program never has it.
 */

#include "userland/desktop/picture/wallpaper.h"
#include "userland/desktop/preview/client.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int
preview_picture(const char *input, const struct preview_request *request, struct preview_picture *picture)
{
	struct kl_wallpaper_image image;
	unsigned char *data;
	const unsigned char *rgb;
	FILE *file;
	long size;
	double scale;
	double scale_y;
	int x;
	int y;
	int sx;
	int sy;
	int error;

	/* The file, whole. */
	memset(picture, 0, sizeof(*picture));
	file = fopen(input, "rb");
	if (file == NULL)
		return errno;
	(void)fseek(file, 0, SEEK_END);
	size = ftell(file);
	(void)fseek(file, 0, SEEK_SET);
	if (size <= 0) {
		(void)fclose(file);
		return EINVAL;
	}
	data = malloc((size_t)size);
	if (data == NULL) {
		(void)fclose(file);
		return ENOMEM;
	}
	if (fread(data, 1, (size_t)size, file) != (size_t)size) {
		free(data);
		(void)fclose(file);
		return EIO;
	}
	(void)fclose(file);

	/* Decoded. */
	error = kl_wallpaper_decode(data, (size_t)size, &image);
	free(data);
	if (error != 0)
		return error;

	/* Cut to the size asked for: the larger scale covers it. */
	picture->width = request->width;
	picture->height = request->height;
	picture->pixels = malloc((size_t)request->width * (size_t)request->height * sizeof(uint32_t));
	if (picture->pixels == NULL) {
		free(image.rgb);
		return ENOMEM;
	}
	scale = (double)image.width / (double)request->width;
	scale_y = (double)image.height / (double)request->height;
	if (scale_y < scale)
		scale = scale_y;
	for (y = 0; y < request->height; y++) {
		for (x = 0; x < request->width; x++) {
			sx = (int)(((double)x - (double)request->width / 2.0) * scale + (double)image.width / 2.0);
			sy = (int)(((double)y - (double)request->height / 2.0) * scale + (double)image.height / 2.0);
			if (sx < 0)
				sx = 0;
			if (sy < 0)
				sy = 0;
			if (sx >= (int)image.width)
				sx = (int)image.width - 1;
			if (sy >= (int)image.height)
				sy = (int)image.height - 1;
			rgb = image.rgb + ((size_t)sy * image.width + (size_t)sx) * 3U;
			picture->pixels[(size_t)y * (size_t)request->width + (size_t)x] = 0xff000000U | ((uint32_t)rgb[0] << 16) | ((uint32_t)rgb[1] << 8) | (uint32_t)rgb[2];
		}
	}

	/* Succeeded: the decoded picture goes. */
	free(image.rgb);
	return 0;
}

void
preview_picture_release(struct preview_picture *picture)
{
	free(picture->pixels);
	picture->pixels = NULL;
}
