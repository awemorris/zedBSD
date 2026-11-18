/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The sun4u board's console font (a copy of the PC/AT 8x16 VGA font).
 */

#ifndef KERN_HAL_SPARCV9_BSP_SUN4U_FONT_H
#define KERN_HAL_SPARCV9_BSP_SUN4U_FONT_H

#include <hal/types.h>

/* The number of glyphs, and the rows of each; a glyph is 8 pixels wide. */
#define SUN4U_FONT_GLYPHS	256U
#define SUN4U_FONT_HEIGHT	16U

extern const uint8_t sun4u_font8x16[SUN4U_FONT_GLYPHS * SUN4U_FONT_HEIGHT];

#endif
