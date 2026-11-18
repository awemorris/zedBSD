/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The System Monitor's glyph atlas (design.md section 4.1): every text
 * style's printable ASCII glyphs drawn once on the CPU with libkeiland's
 * text, white with their coverage as alpha, packed in rows of cells.  The
 * values' digits come from the monospaced font so that a number keeps its
 * width as it changes; the labels from the interface font.  A frame draws
 * text as one quad a character from here, so a new value uploads nothing.
 */

#include "app.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The atlas's width, and the margin around each cell for a glyph's overhang. */
#define ATLAS_WIDTH		1024
#define ATLAS_MARGIN		2

/* A style: the font (0 the interface's, 1 the monospaced), its size in logical pixels, and whether it is bold. */
struct atlas_style {
	int mono;
	float pixels;
	int bold;
};

static const struct atlas_style atlas_styles[SM_STYLES] = {
	{ 0, 16.0f, 1 },
	{ 0, 13.0f, 0 },
	{ 0, 12.0f, 0 },
	{ 1, 30.0f, 1 },
	{ 1, 20.0f, 1 },
	{ 0, 22.0f, 1 }
};

/*
 * Draws every style's glyphs at a scale (the window's pixels per logical
 * pixel) into a new atlas; returns 0 or an errno value.
 */
int
sm_atlas_build(
	struct sm_atlas *atlas,
	struct kl_text *sans,
	struct kl_text *mono,
	float scale)
{
	struct kl_text_line line;
	struct kl_canvas canvas;
	struct kl_text *text;
	struct sm_glyph *glyph;
	unsigned pixels[SM_STYLES];
	unsigned style;
	unsigned index;
	unsigned long serial;
	char character;
	int x;
	int y;
	int row;
	int width;
	int error;

	/* The old atlas goes; the serial goes on, so the renderer knows to upload the new one. */
	serial = atlas->serial;
	sm_atlas_release(atlas);
	memset(atlas, 0, sizeof(*atlas));
	atlas->serial = serial + 1U;
	atlas->scale = scale;
	atlas->width = ATLAS_WIDTH;

	/* Each style's cells, packed left to right and down, to find the height. */
	x = 0;
	y = 0;
	row = 0;
	for (style = 0; style < SM_STYLES; style++) {
		/* The style's font and size, and its line. */
		text = sans;
		if (atlas_styles[style].mono)
			text = mono;
		pixels[style] = (unsigned)(atlas_styles[style].pixels * scale + 0.5f);
		if (pixels[style] < 6U)
			pixels[style] = 6U;
		kl_text_metrics(text, pixels[style], &line);
		atlas->ascent[style] = line.ascent;
		atlas->line[style] = line.ascent + line.descent;

		/* Each character's cell. */
		for (index = 0; index < SM_GLYPH_COUNT; index++) {
			character = (char)(SM_GLYPH_FIRST + index);
			width = kl_text_width(text, &character, 1, pixels[style], atlas_styles[style].bold);
			glyph = &atlas->glyphs[style][index];
			glyph->advance = width;
			glyph->width = width + 2 * ATLAS_MARGIN;
			glyph->height = atlas->line[style] + 2 * ATLAS_MARGIN;

			/* A cell that does not fit the row starts the next one. */
			if (x + glyph->width > ATLAS_WIDTH) {
				x = 0;
				y += row;
				row = 0;
			}

			/* The cell's place, and the row's height. */
			glyph->x = x;
			glyph->y = y;
			x += glyph->width;
			if (glyph->height > row)
				row = glyph->height;
		}
	}

	/* The pixels, cleared. */
	atlas->height = y + row;
	atlas->pixels = calloc((size_t)atlas->width * (size_t)atlas->height, sizeof(uint32_t));
	if (atlas->pixels == NULL)
		return ENOMEM;

	/* The canvas over them. */
	error = kl_canvas_init(&canvas, atlas->pixels, (size_t)atlas->width, atlas->width, atlas->height);
	if (error != 0) {
		sm_atlas_release(atlas);
		return error;
	}

	/* Each glyph in white at its cell's baseline. */
	for (style = 0; style < SM_STYLES; style++) {
		text = sans;
		if (atlas_styles[style].mono)
			text = mono;
		for (index = 0; index < SM_GLYPH_COUNT; index++) {
			character = (char)(SM_GLYPH_FIRST + index);
			glyph = &atlas->glyphs[style][index];
			(void)kl_text_draw(text, &canvas, glyph->x + ATLAS_MARGIN, glyph->y + ATLAS_MARGIN + atlas->ascent[style],
					    &character, 1, pixels[style], atlas_styles[style].bold, KL_RGB(0xffffff));
		}
	}

	/* The canvas it was drawn on goes. */
	kl_canvas_release(&canvas);

	/* Succeeded: the atlas is drawn. */
	return 0;
}

/*
 * Frees an atlas's pixels.
 */
void
sm_atlas_release(
	struct sm_atlas *atlas)
{
	/* The pixels. */
	free(atlas->pixels);
	atlas->pixels = NULL;
}

/*
 * Reports how wide a string is in a style, in pixels.
 */
float
sm_atlas_width(
	const struct sm_atlas *atlas,
	enum sm_style style,
	const char *text)
{
	unsigned character;
	float width;

	/* The advances of its characters (others count as a space). */
	width = 0.0f;
	for (; *text != '\0'; text++) {
		character = (unsigned char)*text;
		if (character < SM_GLYPH_FIRST || character > SM_GLYPH_LAST)
			character = ' ';
		width += (float)atlas->glyphs[style][character - SM_GLYPH_FIRST].advance;
	}

	/* Succeeded: the width. */
	return width;
}
