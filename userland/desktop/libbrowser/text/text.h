/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Text (plan/ws074/design.md §7): the fonts, the glyphs of a code point at
 * a size, their advances and coverage bitmaps, and where a line may break.
 *
 * The first pass uses the fonts of the compositor image through libtruetype
 * at whole pixel sizes: a sans font (Inter), a monospace font (JetBrains
 * Mono) and a fallback for the characters the others lack (Droid Sans
 * Fallback, for Japanese).  Serif maps to the sans font until the image has
 * a serif font.  Bold is the regular glyph widened by a pixel; there is no
 * kerning or shaping yet.
 *
 * A page adds the faces its @font-face rules load (ws074-p070) after
 * those: each under its family, weights and style, which a style's
 * font-family finds before the sans font.
 */

#ifndef KEILAND_BROWSER_TEXT_H
#define KEILAND_BROWSER_TEXT_H

#include "base/base.h"
#include "userland/desktop/paths.h"

/* The faces a text system holds: sans, monospace, the fallback, then the page's web fonts. */
#define TEXT_FACE_SANS		0
#define TEXT_FACE_MONO		1
#define TEXT_FACE_FALLBACK	2
#define TEXT_FACE_WEB		3
#define TEXT_WEB_FACES		24
#define TEXT_FACES		(TEXT_FACE_WEB + TEXT_WEB_FACES)

/* The fonts the compositor image installs. */
#define TEXT_DEFAULT_SANS	KEILAND_DATADIR "/fonts/keiland.ttf"
#define TEXT_DEFAULT_MONO	KEILAND_DATADIR "/fonts/keiland-mono.ttf"
#define TEXT_DEFAULT_FALLBACK	KEILAND_DATADIR "/fonts/keiland-fallback.ttf"

struct truetype_face;
struct text_glyph_entry;

/*
 * The paths of the fonts to open (NULL leaves a face out; the sans face is
 * required).
 */
struct text_font_paths {
	const char *sans;
	const char *mono;
	const char *fallback;
};

/*
 * One font file and its face.
 */
struct text_face {
	struct wb_buffer data;
	struct truetype_face *face;
	int open;
};

/*
 * A web font's face in its family (ws074-p070): the family's key (an
 * opaque pointer the caller matches by identity, the page's atom of the
 * name), the weights the face covers, whether it is italic, and the face.
 */
struct text_family {
	const void *family;
	int weight_min;
	int weight_max;
	int italic;
	int face;
};

/*
 * A font as a style selects it: a face, its size (in pixels, fractional:
 * the layout measures with it), the whole pixel size its glyphs are drawn
 * at, and whether it is drawn bold.
 */
struct text_font {
	int face;
	float size;
	unsigned pixels;
	int bold;
};

/*
 * The vertical measures of a font, in pixels.
 */
struct text_metrics {
	int ascent;
	int descent;
	int line_height;
};

/*
 * One glyph as the layout and the painting use it: the face that has it,
 * its index, its advance (in whole pixels, and in 1/64 pixels from the
 * font's design units at the font's fractional size) and, once drawn, its
 * coverage bitmap (one byte a pixel, width by height, placed left and top
 * from the pen and baseline).
 */
struct text_glyph {
	int face;
	unsigned index;
	int advance;
	int32_t advance_units;
	int width;
	int height;
	int left;
	int top;
	const uint8_t *bitmap;
};

/*
 * The fonts of a page and the cache of the glyphs drawn from them.
 */
struct text_system {
	struct text_face faces[TEXT_FACES];
	struct text_family families[TEXT_WEB_FACES];
	int family_count;
	struct text_glyph_entry **cache;
	size_t cache_capacity;
	size_t cache_count;
};

/* Fonts (font.c). */
int text_system_open(struct text_system *system, const struct text_font_paths *paths);
void text_system_close(struct text_system *system);
void text_select_font(const struct text_system *system, int monospace, float size, int weight, struct text_font *font);
int text_add_face(struct text_system *system, const void *family, int weight_min, int weight_max, int italic, struct wb_buffer *data);
int text_select_family(const struct text_system *system, const void *family, int weight, int italic, struct text_font *font);

/* Web font files (woff.c). */
int text_font_file(const unsigned char *bytes, size_t length, struct wb_buffer *sfnt);
int text_font_metrics(struct text_system *system, const struct text_font *font, struct text_metrics *metrics);
int text_glyph(struct text_system *system, const struct text_font *font, uint32_t code_point, int with_bitmap, struct text_glyph *glyph);
int text_glyph_advance(struct text_system *system, const struct text_font *font, uint32_t code_point, int *advance);
int text_font_char_widths(struct text_system *system, const struct text_font *font, int *average, int *maximum);

/* Line breaking (linebreak.c). */
int text_is_space(uint32_t code_point);
int text_break_between(uint32_t before, uint32_t after);

#endif
