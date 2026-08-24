/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * PC-9800 font
 */

#ifndef ZEDBSD_KERN_PC98_FONT_H
#define ZEDBSD_KERN_PC98_FONT_H

#include <stdint.h>

int
pc98_font_get_glyph(
	uint32_t codepoint,
	uint8_t font[32],
	unsigned *width,
	unsigned *height);

#endif
