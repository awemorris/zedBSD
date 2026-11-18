/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The decoding shared by the programs that read pictures themselves
 * (ws094-p013): Image Viewer (userland/desktop/imageview/image.c) and the
 * file manager's thumbnails (userland/desktop/files/thumb.c).  A JPEG
 * (libjpeg-compat) with its EXIF orientation, one frame of a GIF
 * (libgif-compat) drawn on its screen, and the turning of a picture as an
 * orientation says, all in premultiplied 0xAARRGGBB words.  The source is
 * compiled into each program.
 */

#ifndef KEILAND_PICTURE_H
#define KEILAND_PICTURE_H

#include <compat/gif_lib.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/*
 * A decoded picture: its premultiplied pixels (malloc'd, width by height,
 * no padding), its size and whether any pixel is not opaque.
 */
struct kl_picture {
	uint32_t *pixels;
	int width;
	int height;
	int has_alpha;
};

uint32_t kl_picture_premultiply(unsigned red, unsigned green, unsigned blue, unsigned alpha);
int kl_picture_exif_orientation(const unsigned char *data, size_t size);
int kl_picture_orient(struct kl_picture *picture, int orientation);
int kl_picture_jpeg(FILE *file, const unsigned char *data, size_t size, unsigned max_side, unsigned long max_pixels, struct kl_picture *picture, int *orientation);
int kl_picture_jpeg_reduced(FILE *file, const unsigned char *data, size_t size, unsigned box_side, int cover, unsigned max_side, unsigned long max_pixels, struct kl_picture *picture, int *orientation);
void kl_picture_gif_draw(const GifFileType *gif, int index, int transparent, uint32_t *screen);
int kl_picture_gif_first(GifFileType *gif, unsigned max_side, unsigned long max_pixels, struct kl_picture *picture);

#endif
