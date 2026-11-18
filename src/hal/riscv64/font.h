/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The riscv64 early console's font.
 */

#ifndef KERN_HAL_RISCV64_FONT_H
#define KERN_HAL_RISCV64_FONT_H

#include <hal/types.h>

/* The number of glyphs, and the rows of each; a glyph is 8 pixels wide. */
#define RISCV64_FONT_GLYPHS	256U
#define RISCV64_FONT_HEIGHT	16U

/* The glyphs, 16 bytes each, character 0 first: the PC/AT consoles' 8x16 font. */
extern const uint8_t riscv64_font8x16[RISCV64_FONT_GLYPHS * RISCV64_FONT_HEIGHT];

#endif
