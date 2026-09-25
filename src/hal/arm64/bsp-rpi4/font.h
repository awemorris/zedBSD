/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Raspberry Pi 4 console font (a copy of the PC/AT 8x16 VGA font).
 */

#ifndef KERN_HAL_ARM64_BSP_RPI4_FONT_H
#define KERN_HAL_ARM64_BSP_RPI4_FONT_H

#include <hal/types.h>

/* The number of glyphs, and the rows of each; a glyph is 8 pixels wide. */
#define RPI4_FONT_GLYPHS	256U
#define RPI4_FONT_HEIGHT	16U

extern const uint8_t rpi4_font8x16[RPI4_FONT_GLYPHS * RPI4_FONT_HEIGHT];

#endif
