/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Xzed's glyphs from a TrueType font (WS069 p002): the core font's 8x16
 * cells drawn with libtruetype, in place of /dev/graphics's glyphs (which
 * are the legacy console's).
 */

#ifndef XZED_GLYPHS_H
#define XZED_GLYPHS_H

#include <stdint.h>

/* One glyph as a 1-bit bitmap, rows of stride bytes, the high bit leftmost. */
struct xzed_glyph {
	unsigned width;
	unsigned height;
	unsigned stride;
	unsigned advance;
	uint8_t bitmap[32];
};

int xzed_glyphs_open(const char *path);
int xzed_glyph(uint32_t codepoint, struct xzed_glyph *glyph);
void xzed_glyphs_close(void);

#endif
