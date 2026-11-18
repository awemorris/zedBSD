/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The MIPS boards' console font (a copy of the PC/AT 8x16 VGA font).
 */

#ifndef KERN_HAL_MIPS_FONT_H
#define KERN_HAL_MIPS_FONT_H

#include <hal/types.h>

/* The number of glyphs, and the rows of each; a glyph is 8 pixels wide. */
#define MIPS_FONT_GLYPHS	256U
#define MIPS_FONT_HEIGHT	16U

extern const uint8_t mips_font8x16[MIPS_FONT_GLYPHS * MIPS_FONT_HEIGHT];

#endif
