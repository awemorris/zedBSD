/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The text of Image Viewer's own interface (the chip, the cards, the
 * messages, the file chooser), drawn with libtruetype from the desktop's
 * font.
 *
 * The strings are short, so each glyph is drawn when it is needed; the
 * string is UTF-8, and a byte sequence that is not UTF-8 is drawn as the
 * font's missing glyph.
 */

#include "imageview.h"

#include "userland/desktop/paths.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <truetype/truetype.h>

/* The largest font file read. */
#define TEXT_FILE_MAX		((size_t)64 * 1024 * 1024)

static uint32_t next_codepoint(const char *string, size_t *index);
static int set_size(struct iv_text *text, unsigned pixels);

/*
 * Opens a TrueType font file.
 *
 * Returns 0, or an errno value.
 */
int
iv_text_open(
	struct iv_text *text,
	const char *path)
{
	FILE *file;
	long size;
	size_t got;
	int status;
	int error;

	/* Nothing is open yet. */
	memset(text, 0, sizeof(*text));

	/* Opens the file. */
	file = fopen(path, "rb");
	if (file == NULL)
		return errno;

	/* Goes to its end, to learn its size. */
	status = fseek(file, 0, SEEK_END);
	if (status != 0) {
		fclose(file);
		return EINVAL;
	}

	/* The size: a file empty, unreadable or larger than a font is refused. */
	size = ftell(file);
	if (size <= 0 || (size_t)size > TEXT_FILE_MAX) {
		fclose(file);
		return EINVAL;
	}

	/* Room for the bytes, read from the start. */
	rewind(file);
	text->data = malloc((size_t)size);
	if (text->data == NULL) {
		fclose(file);
		return ENOMEM;
	}

	/* A file cut short is refused. */
	got = fread(text->data, 1, (size_t)size, file);
	fclose(file);
	if (got != (size_t)size) {
		iv_text_close(text);
		return EIO;
	}

	/* The face reads these bytes. */
	text->size = (size_t)size;

	/* Opens the face over the bytes. */
	error = truetype_open(text->data, text->size, 0, &text->face);
	if (error != 0) {
		text->face = NULL;
		iv_text_close(text);
		return error;
	}

	/* Its companions: Mahora Bold, and the monospaced fallback for the signs it lacks (ws090-p020). */
	(void)truetype_open_companions(text->face, KEILAND_FONT_BOLD, KEILAND_FONT_FALLBACK_MONO);

	/* Succeeded: text can be drawn. */
	return 0;
}

/*
 * Closes the font and frees its bytes.
 */
void
iv_text_close(
	struct iv_text *text)
{
	/* The face before the bytes it reads. */
	if (text->face != NULL)
		truetype_close(text->face);

	/* The bytes and the glyphs' scratch; nothing is open any more. */
	free(text->data);
	free(text->scratch);
	memset(text, 0, sizeof(*text));
}

/*
 * Reports how wide a string is at a size, in pixels (0 without a font).
 */
int
iv_text_width(
	struct iv_text *text,
	const char *string,
	unsigned pixels)
{
	struct truetype_glyph metrics;
	uint32_t codepoint;
	unsigned glyph;
	size_t index;
	int width;
	int error;

	/* Nothing to measure without a font at the size. */
	error = set_size(text, pixels);
	if (error != 0)
		return 0;

	/* Adds each character's advance. */
	width = 0;
	index = 0;
	while (string[index] != '\0') {
		/* The character's glyph and its metrics; a glyph without metrics adds nothing. */
		codepoint = next_codepoint(string, &index);
		glyph = truetype_glyph_index(text->face, codepoint);
		error = truetype_glyph_metrics(text->face, glyph, &metrics);
		if (error == 0)
			width += metrics.advance;
	}

	/* Reports the sum. */
	return width;
}

/*
 * Draws a string with its first character's pen at x on a baseline.
 */
void
iv_text_draw(
	struct iv_text *text,
	struct iv_canvas *canvas,
	int x,
	int baseline,
	const char *string,
	unsigned pixels,
	uint32_t color)
{
	struct truetype_glyph metrics;
	unsigned char *grown;
	uint32_t codepoint;
	unsigned glyph;
	size_t index;
	size_t needed;
	int error;

	/* Nothing to draw without a font at the size. */
	error = set_size(text, pixels);
	if (error != 0)
		return;

	/* Draws each character and moves the pen past it. */
	index = 0;
	while (string[index] != '\0') {
		/* The character's glyph and its metrics; a glyph without metrics is passed over. */
		codepoint = next_codepoint(string, &index);
		glyph = truetype_glyph_index(text->face, codepoint);
		error = truetype_glyph_metrics(text->face, glyph, &metrics);
		if (error != 0)
			continue;

		/* Grows the scratch bitmap to the glyph's size. */
		needed = (size_t)metrics.width * (size_t)metrics.height;
		if (needed > text->scratch_size) {
			grown = realloc(text->scratch, needed);
			if (grown == NULL)
				return;

			/* The grown bitmap is the scratch from now on. */
			text->scratch = grown;
			text->scratch_size = needed;
		}

		/* Draws the glyph's coverage and blends it in the colour. */
		if (needed > 0) {
			/* The coverage, blended only when the glyph could be drawn. */
			error = truetype_render_glyph(text->face, glyph, &metrics, text->scratch, metrics.width, text->scratch_size);
			if (error == 0)
				iv_canvas_mask(canvas, x + metrics.left, baseline - metrics.top, text->scratch, (int)metrics.width, (int)metrics.height, color);
		}

		/* The pen moves by the glyph's advance. */
		x += metrics.advance;
	}
}

/* Reads one UTF-8 character and moves past it; a malformed byte reads as U+FFFD. */
static uint32_t
next_codepoint(
	const char *string,
	size_t *index)
{
	const unsigned char *bytes;
	uint32_t codepoint;
	size_t length;
	size_t part;

	/* The lead byte decides the length. */
	bytes = (const unsigned char *)string + *index;
	if (bytes[0] < 0x80U) {
		*index += 1;
		return bytes[0];
	}

	/* Two, three or four bytes; anything else is one byte of U+FFFD. */
	if ((bytes[0] & 0xe0U) == 0xc0U) {
		codepoint = bytes[0] & 0x1fU;
		length = 2;
	} else if ((bytes[0] & 0xf0U) == 0xe0U) {
		codepoint = bytes[0] & 0x0fU;
		length = 3;
	} else if ((bytes[0] & 0xf8U) == 0xf0U) {
		codepoint = bytes[0] & 0x07U;
		length = 4;
	} else {
		*index += 1;
		return 0xfffdU;
	}

	/* The continuation bytes; a missing one ends the character early. */
	for (part = 1; part < length; part++) {
		/* A byte that does not continue the character ends it as U+FFFD. */
		if ((bytes[part] & 0xc0U) != 0x80U) {
			*index += part;
			return 0xfffdU;
		}

		/* The byte's six bits of the character. */
		codepoint = (codepoint << 6) | (bytes[part] & 0x3fU);
	}

	/* The string moves on past the character. */
	*index += length;

	/* Succeeded: the character. */
	return codepoint;
}

/* Sets the face's size when it is not set already. */
static int
set_size(
	struct iv_text *text,
	unsigned pixels)
{
	int error;

	/* Without a font there are no words to measure. */
	if (text->face == NULL)
		return ENOENT;

	/* The size set last. */
	if (text->pixels == pixels)
		return 0;

	/* The new size. */
	error = truetype_set_pixel_size(text->face, pixels);
	if (error != 0)
		return error;

	/* The size the face is set to now. */
	text->pixels = pixels;

	/* Succeeded: the face is at the size. */
	return 0;
}
