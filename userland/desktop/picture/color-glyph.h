/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A colour glyph (a colour emoji font's PNG, libtruetype's
 * truetype_color_glyph) decoded and scaled to a size, for the programs that
 * draw text themselves: libkeiland's text and the compositor's (ws102-p019).
 * The source is compiled into each of them; they link libpng-compat.
 */

#ifndef KEILAND_COLOR_GLYPH_H
#define KEILAND_COLOR_GLYPH_H

#include <stdint.h>
#include <truetype/truetype.h>

/*
 * A colour glyph at a size: its premultiplied 0xAARRGGBB pixels (malloc'd,
 * width by height), where its top-left corner sits from the pen (left, and
 * top up from the baseline) and how far the pen moves, in pixels.
 */
struct kl_color_image {
	uint32_t *pixels;
	int width;
	int height;
	int left;
	int top;
	int advance;
};

int kl_color_glyph(struct truetype_face *face, unsigned glyph, unsigned pixels, struct kl_color_image *out);
int kl_color_glyph_metrics(struct truetype_face *face, unsigned glyph, unsigned pixels, struct kl_color_image *out);

#endif
