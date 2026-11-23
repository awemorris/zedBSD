/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The core font's glyphs, from a monospaced TrueType font.
 *
 * The core font the server offers (zed-unicode) has 8x16 cells, the text
 * drawn above the baseline the client gives.  The TrueType font is drawn
 * at 13 pixels onto a baseline at row 12 of the cell and turned into one
 * bit a pixel (coverage of a half or more).  The printable ASCII glyphs
 * are kept once drawn.
 */

#include "userland/desktop/xserver/internal.h"

#include <string.h>
#include <truetype/truetype.h>

/* The cell, the size the font is drawn at, and where its baseline is in the cell. */
#define GLYPHS_CELL_WIDTH	8U
#define GLYPHS_CELL_HEIGHT	16U
#define GLYPHS_PIXELS		13U
#define GLYPHS_BASELINE		12

/* How much coverage sets a pixel. */
#define GLYPHS_COVERED		128U

static void glyphs_draw(struct x11_glyphs *glyphs, uint32_t codepoint, struct x11_glyph *glyph);

/*
 * Opens the core font over a monospaced TrueType file the caller keeps in
 * memory, with the file of the characters it lacks (NULL: none), which
 * the font library reads by its path.  Returns 0, or an errno value
 * (there is then no text).
 */
int
x11_glyphs_open(
	struct x11_glyphs *glyphs,
	const uint8_t *bytes,
	size_t size,
	const char *fallback_path)
{
	int error;

	/* The face over the bytes. */
	memset(glyphs, 0, sizeof(*glyphs));
	error = truetype_open(bytes, size, 0U, &glyphs->face);
	if (error != 0) {
		x11_glyphs_close(glyphs);
		return error;
	}

	/* Its companion, the monospaced fallback, for the signs it lacks. */
	if (fallback_path != NULL)
		(void)truetype_open_companions(glyphs->face, NULL, fallback_path);

	/* At the cell's size. */
	error = truetype_set_pixel_size(glyphs->face, GLYPHS_PIXELS);
	if (error != 0) {
		x11_glyphs_close(glyphs);
		return error;
	}

	/* Succeeded: glyphs can be drawn. */
	return 0;
}

/*
 * Gives a character's glyph (a kept one, or drawn now).  Returns 0, or -1
 * without a font.
 */
int
x11_glyph(
	struct x11_glyphs *glyphs,
	uint32_t codepoint,
	struct x11_glyph *glyph)
{
	/* Without a font there are no glyphs. */
	if (glyphs->face == NULL)
		return -1;

	/* A kept ASCII glyph. */
	if (codepoint < X11_GLYPHS_CACHED && glyphs->cached[codepoint]) {
		*glyph = glyphs->cache[codepoint];
		return 0;
	}

	/* A new one, kept when it is ASCII. */
	glyphs_draw(glyphs, codepoint, glyph);
	if (codepoint < X11_GLYPHS_CACHED) {
		glyphs->cache[codepoint] = *glyph;
		glyphs->cached[codepoint] = 1U;
	}

	/* Succeeded: the glyph. */
	return 0;
}

/*
 * Releases the font (the file's bytes stay the caller's).
 */
void
x11_glyphs_close(
	struct x11_glyphs *glyphs)
{
	/* The face. */
	if (glyphs->face != NULL)
		truetype_close(glyphs->face);

	/* No font, and no glyph kept. */
	memset(glyphs, 0, sizeof(*glyphs));
}

/* Draws a glyph onto the cell's baseline as one bit a pixel (a glyph that cannot be drawn is an empty cell). */
static void
glyphs_draw(
	struct x11_glyphs *glyphs,
	uint32_t codepoint,
	struct x11_glyph *glyph)
{
	struct truetype_glyph metrics;
	unsigned index;
	unsigned x;
	unsigned y;
	int cell_x;
	int cell_y;
	int error;

	/* An empty cell of the core font's size. */
	memset(glyph, 0, sizeof(*glyph));
	glyph->width = GLYPHS_CELL_WIDTH;
	glyph->height = GLYPHS_CELL_HEIGHT;
	glyph->stride = 1U;
	glyph->advance = GLYPHS_CELL_WIDTH;

	/* The glyph's coverage. */
	index = truetype_glyph_index(glyphs->face, codepoint);
	error = truetype_render_glyph(glyphs->face, index, &metrics, glyphs->coverage, X11_GLYPH_DRAWN_MAX, sizeof(glyphs->coverage));
	if (error != 0)
		return;

	/* Each covered pixel that lands in the cell sets its bit (the top counts up from the baseline). */
	for (y = 0U; y < metrics.height; y++) {
		cell_y = GLYPHS_BASELINE - metrics.top + (int)y;
		if (cell_y < 0 || cell_y >= (int)GLYPHS_CELL_HEIGHT)
			continue;
		for (x = 0U; x < metrics.width; x++) {
			cell_x = metrics.left + (int)x;
			if (cell_x < 0 || cell_x >= (int)GLYPHS_CELL_WIDTH)
				continue;
			if (glyphs->coverage[y * X11_GLYPH_DRAWN_MAX + x] >= GLYPHS_COVERED)
				glyph->bitmap[cell_y] = (uint8_t)(glyph->bitmap[cell_y] | (0x80U >> cell_x));
		}
	}
}
