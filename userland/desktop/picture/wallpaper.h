/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The decoding of a wallpaper (ws138-p001), shared by the compositor
 * (userland/desktop/wayland/glass.c) and Settings' Wallpaper page
 * (userland/desktop/settings/look.c): a PNG (libpng-compat; its transparent
 * parts over black) or a JPEG (libjpeg-compat) into RGB pixels, three
 * bytes each, the rows packed (since ws138-p002 nothing else: a PPM is
 * refused like any other file).  The source is compiled into each program;
 * it keeps no state, so any thread may call it.
 */

#ifndef KEILAND_WALLPAPER_H
#define KEILAND_WALLPAPER_H

#include <stddef.h>
#include <stdint.h>

/* The longest side and the most pixels a wallpaper may have (the file manager's THUMB_PIXELS_MAX). */
#define KL_WALLPAPER_SIDE_MAX	8192U
#define KL_WALLPAPER_PIXELS_MAX	(16UL * 1024UL * 1024UL)

/*
 * A decoded wallpaper: its RGB pixels (malloc'd, three bytes each, width
 * by height without padding) and its size.  The caller frees rgb.
 */
struct kl_wallpaper_image {
	unsigned char *rgb;
	uint32_t width;
	uint32_t height;
};

int kl_wallpaper_decode(const unsigned char *data, size_t size, struct kl_wallpaper_image *image);

#endif
