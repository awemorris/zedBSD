/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Macintosh console font (a copy of the PC/AT 8x16 VGA font).
 */

#ifndef KERN_HAL_M68K_MAC68K_FONT_H
#define KERN_HAL_M68K_MAC68K_FONT_H

#include <hal/types.h>

/* The number of glyphs, and the rows of each; a glyph is 8 pixels wide. */
#define MAC68K_FONT_GLYPHS	256U
#define MAC68K_FONT_HEIGHT	16U

extern const uint8_t mac68k_font8x16[MAC68K_FONT_GLYPHS * MAC68K_FONT_HEIGHT];

#endif
