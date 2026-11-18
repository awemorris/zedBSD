/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The board's console font (a copy of the PC/AT 8x16 VGA font).
 */

#ifndef KERN_HAL_PPC64_PSERIES_FONT_H
#define KERN_HAL_PPC64_PSERIES_FONT_H

#include <hal/types.h>

/* The number of glyphs, and the rows of each; a glyph is 8 pixels wide. */
#define VIRT_FONT_GLYPHS	256U
#define VIRT_FONT_HEIGHT	16U

extern const uint8_t virt_font8x16[VIRT_FONT_GLYPHS * VIRT_FONT_HEIGHT];

#endif
