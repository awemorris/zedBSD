/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The text of Text Editor (Files' text.c): UTF-8 drawn from TrueType fonts
 * (libtruetype) onto the canvas.
 *
 * A main font draws what it can, with its companions (ws090-p020:
 * Mahora Bold for the interface's bold glyphs, the monospaced fallback for
 * the signs and letters it lacks), and a fallback font (optional) the
 * characters they lack, such as Japanese.  Every glyph is drawn once per
 * size into a cache of coverage bitmaps; a bold glyph without a bold face
 * is the regular one's outline widened as a vector by libtruetype
 * (truetype_set_bold, BUG-205).
 * There is no kerning and no shaping: characters are set one after
 * another, and the document's text one character to its cell.
 *
 * A character neither font has is looked for in the colour emoji font
 * (TE_TEXT_EMOJI, opened the first time one is drawn, ws102-p019) and
 * drawn in its colours (userland/desktop/picture/color-glyph.c).
 */

#include "textedit.h"

#include "../picture/color-glyph.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <truetype/truetype.h>
#include <unistd.h>

/* How many glyphs the cache holds before it is emptied. */
#define TEXT_CACHE_SIZE		4096U

/* The largest font file read, in bytes. */
#define TEXT_FONT_MAX		(64U * 1024U * 1024U)

/* The ellipsis put where a string is cut, in UTF-8. */
#define TEXT_ELLIPSIS		"\xe2\x80\xa6"

/* The code point drawn for bytes that are not UTF-8. */
#define TEXT_REPLACEMENT	0xfffdU

static int text_face_open(struct te_text_face *face, const char *path);
static const struct te_glyph *text_glyph(struct te_text *text, uint32_t codepoint, unsigned pixels, int bold);
static struct te_glyph *text_slot(struct te_text *text, uint32_t key);
static int text_render(struct te_text *text, int face_index, unsigned glyph_index, unsigned pixels, int bold, struct te_glyph *glyph);
static int text_set_size(struct te_text_face *face, unsigned pixels);
static void text_clear(struct te_text *text);
static int text_render_color(struct te_text *text, unsigned glyph_index, unsigned pixels, struct te_glyph *glyph);

/*
 * Opens the main font and, when a path is given and readable, the fallback.
 *
 * Returns 0, or an errno value when the main font cannot be used.
 */
int
te_text_open(
	struct te_text *text,
	const char *primary,
	const char *fallback)
{
	int error;

	/* Nothing is open yet. */
	memset(text, 0, sizeof(*text));

	/* The glyph cache. */
	text->cache = calloc(TEXT_CACHE_SIZE, sizeof(text->cache[0]));
	if (text->cache == NULL)
		return ENOMEM;
	text->cache_size = TEXT_CACHE_SIZE;

	/* The main font, which every string needs. */
	error = text_face_open(&text->faces[0], primary);
	if (error != 0) {
		te_text_close(text);
		return error;
	}

	/* Its companions: Mahora Bold for its bold glyphs, the monospaced fallback for the signs it lacks (ws090-p020). */
	(void)truetype_open_companions(text->faces[0].face, KEILAND_FONT_BOLD, KEILAND_FONT_FALLBACK_MONO);

	/* One face so far. */
	text->face_count = 1;

	/* The fallback, used only when it opens. */
	if (fallback != NULL) {
		error = text_face_open(&text->faces[1], fallback);
		if (error == 0)
			text->face_count = 2;
	}

	/* Succeeded: text can be measured and drawn. */
	return 0;
}

/*
 * Closes the fonts and frees the glyph cache.
 */
void
te_text_close(
	struct te_text *text)
{
	int index;

	/* The cached glyphs and the cache. */
	if (text->cache != NULL)
		text_clear(text);
	free(text->cache);
	free(text->scratch);

	/* The faces, then the bytes they read. */
	for (index = 0; index < TE_TEXT_FACES; index++) {
		if (text->faces[index].face != NULL)
			truetype_close(text->faces[index].face);
		free(text->faces[index].data);
	}

	/* Nothing is open any more. */
	memset(text, 0, sizeof(*text));
}

/*
 * Reports the ascent, descent and line height of the main font at a size.
 */
void
te_text_metrics(
	struct te_text *text,
	unsigned pixels,
	struct te_text_line *line)
{
	struct truetype_metrics metrics;
	int error;

	/* A guess from the size, when the font cannot say. */
	line->ascent = (int)(pixels * 4U / 5U);
	line->descent = (int)(pixels / 4U);
	line->height = (int)(pixels * 5U / 4U);

	/* The main font at that size. */
	error = text_set_size(&text->faces[0], pixels);
	if (error != 0)
		return;

	/* Its own measurements. */
	error = truetype_metrics(text->faces[0].face, &metrics);
	if (error != 0)
		return;

	/* Succeeded: the font's values (the font's descent is below zero; here it is a depth). */
	line->ascent = metrics.ascent;
	line->descent = -metrics.descent;
	line->height = metrics.line_height;
}

/*
 * Reports the baseline that centres capital letters of a size in a band
 * from top of a height (the eye centres text by its capitals, not by the
 * font's ascent, which leaves room for accents).
 */
int
te_text_center(
	unsigned pixels,
	int top,
	int height)
{
	int capitals;

	/* A capital letter is about 0.72 of the size in the fonts used. */
	capitals = (int)((pixels * 72U + 50U) / 100U);

	/* Reports the baseline that puts the capitals in the middle. */
	return top + (height + capitals) / 2;
}

/*
 * Reports how wide a string (of a byte length) is at a size, in pixels.
 */
int
te_text_width(
	struct te_text *text,
	const char *string,
	size_t length,
	unsigned pixels,
	int bold)
{
	const struct te_glyph *glyph;
	uint32_t codepoint;
	size_t index;
	int width;

	/* Each character's advance. */
	width = 0;
	index = 0;
	while (index < length) {
		codepoint = te_utf8_next(string, length, &index);
		glyph = text_glyph(text, codepoint, pixels, bold);
		if (glyph != NULL)
			width += glyph->advance;
	}

	/* Reports the sum of the advances. */
	return width;
}

/*
 * Draws a string with its baseline at a height, and reports how far the
 * pen moved.
 */
int
te_text_draw(
	struct te_text *text,
	struct te_canvas *canvas,
	int x,
	int baseline,
	const char *string,
	size_t length,
	unsigned pixels,
	int bold,
	uint32_t color)
{
	const struct te_glyph *glyph;
	uint32_t codepoint;
	size_t index;
	int pen;

	/* Each character at the pen, which then moves by its advance. */
	pen = x;
	index = 0;
	while (index < length) {
		codepoint = te_utf8_next(string, length, &index);
		glyph = text_glyph(text, codepoint, pixels, bold);
		if (glyph == NULL)
			continue;

		/* The glyph's coverage, in the color; a colour glyph in its own colours. */
		if (glyph->bitmap != NULL)
			te_canvas_mask(canvas, pen + glyph->left, baseline - glyph->top, glyph->bitmap, glyph->width, glyph->height, (size_t)glyph->width, color);
		if (glyph->pixels != NULL)
			te_canvas_pixels(canvas, pen + glyph->left, baseline - glyph->top, glyph->pixels, glyph->width, glyph->height);

		/* The pen moves past it. */
		pen += glyph->advance;
	}

	/* Reports how far the pen moved. */
	return pen - x;
}

/*
 * Draws a string cut to a width (an ellipsis at the cut), and reports how
 * wide what was drawn is.
 */
int
te_text_draw_fit(
	struct te_text *text,
	struct te_canvas *canvas,
	int x,
	int baseline,
	const char *string,
	unsigned pixels,
	int bold,
	int width,
	uint32_t color)
{
	char fitted[1024];
	size_t length;
	int drawn;

	/* The string as much of it as fits. */
	length = te_text_fit(text, string, pixels, bold, width, fitted, sizeof(fitted));

	/* Draws it. */
	drawn = te_text_draw(text, canvas, x, baseline, fitted, length, pixels, bold, color);

	/* Reports its width. */
	return drawn;
}

/*
 * Copies as much of a string as fits a width into a buffer, with an
 * ellipsis when it was cut, and returns the copy's byte length.
 */
size_t
te_text_fit(
	struct te_text *text,
	const char *string,
	unsigned pixels,
	int bold,
	int width,
	char *out,
	size_t size)
{
	const struct te_glyph *glyph;
	uint32_t codepoint;
	size_t length;
	size_t index;
	size_t kept;
	int ellipsis;
	int used;
	int total;

	/* The string's length and whole width. */
	length = strlen(string);
	total = te_text_width(text, string, length, pixels, bold);

	/* A string that fits (and fits the buffer) is copied as it is. */
	if (total <= width && length < size) {
		memcpy(out, string, length);
		out[length] = '\0';
		return length;
	}

	/* The characters that fit together with the ellipsis. */
	ellipsis = te_text_width(text, TEXT_ELLIPSIS, sizeof(TEXT_ELLIPSIS) - 1U, pixels, bold);
	used = 0;
	kept = 0;
	index = 0;
	while (index < length) {
		codepoint = te_utf8_next(string, length, &index);
		glyph = text_glyph(text, codepoint, pixels, bold);
		if (glyph != NULL)
			used += glyph->advance;

		/* The character that would cross the width is where the cut goes. */
		if (used + ellipsis > width)
			break;

		/* A character that does not fit the buffer with the ellipsis also ends it. */
		if (index + sizeof(TEXT_ELLIPSIS) > size)
			break;
		kept = index;
	}

	/* The kept characters and the ellipsis. */
	memcpy(out, string, kept);
	memcpy(out + kept, TEXT_ELLIPSIS, sizeof(TEXT_ELLIPSIS));

	/* Reports the length of the cut string. */
	return kept + sizeof(TEXT_ELLIPSIS) - 1U;
}

/*
 * Reports how far a character moves the pen at a size (0 when no font
 * draws it).
 */
int
te_text_advance(
	struct te_text *text,
	uint32_t codepoint,
	unsigned pixels)
{
	const struct te_glyph *glyph;

	/* The glyph, from the cache or drawn now. */
	glyph = text_glyph(text, codepoint, pixels, 0);
	if (glyph == NULL)
		return 0;

	/* Succeeded: its advance. */
	return glyph->advance;
}

/*
 * Draws one character with its pen at x and its baseline at a height.
 */
void
te_text_draw_char(
	struct te_text *text,
	struct te_canvas *canvas,
	int x,
	int baseline,
	uint32_t codepoint,
	unsigned pixels,
	uint32_t color)
{
	const struct te_glyph *glyph;

	/* The glyph, from the cache or drawn now; a blank one draws nothing, a colour one its colours. */
	glyph = text_glyph(text, codepoint, pixels, 0);
	if (glyph == NULL)
		return;
	if (glyph->pixels != NULL) {
		te_canvas_pixels(canvas, x + glyph->left, baseline - glyph->top, glyph->pixels, glyph->width, glyph->height);
		return;
	}

	/* A blank glyph. */
	if (glyph->bitmap == NULL)
		return;

	/* Its coverage, in the colour. */
	te_canvas_mask(canvas, x + glyph->left, baseline - glyph->top, glyph->bitmap, glyph->width, glyph->height, (size_t)glyph->width, color);
}

/*
 * Decodes the UTF-8 character at *index and moves *index past it.
 *
 * A byte that does not start a well-formed character is U+FFFD and is
 * skipped alone, so a broken file name still draws.
 */
uint32_t
te_utf8_next(
	const char *string,
	size_t length,
	size_t *index)
{
	const unsigned char *bytes;
	uint32_t value;
	uint32_t minimum;
	unsigned first;
	unsigned count;
	unsigned position;

	/* The lead byte decides how many bytes follow. */
	bytes = (const unsigned char *)string;
	first = bytes[*index];
	if (first < 0x80U) {
		*index += 1U;
		return first;
	} else if (first >= 0xc2U && first < 0xe0U) {
		value = first & 0x1fU;
		count = 1U;
		minimum = 0x80U;
	} else if (first >= 0xe0U && first < 0xf0U) {
		value = first & 0x0fU;
		count = 2U;
		minimum = 0x800U;
	} else if (first >= 0xf0U && first < 0xf5U) {
		value = first & 0x07U;
		count = 3U;
		minimum = 0x10000U;
	} else {
		*index += 1U;
		return TEXT_REPLACEMENT;
	}

	/* A character cut off by the end of the string is not one. */
	if (*index + count >= length + 1U) {
		*index += 1U;
		return TEXT_REPLACEMENT;
	}

	/* Each continuation byte adds six bits. */
	for (position = 1U; position <= count; position++) {
		if ((bytes[*index + position] & 0xc0U) != 0x80U) {
			*index += 1U;
			return TEXT_REPLACEMENT;
		}

		/* The byte's six bits after those so far. */
		value = (value << 6) | (bytes[*index + position] & 0x3fU);
	}

	/* An overlong form or a surrogate is not a character. */
	if (value < minimum ||
	    (value >= 0xd800U &&
	     value < 0xe000U)) {
		*index += 1U;
		return TEXT_REPLACEMENT;
	}

	/* Succeeded: the character, and the index past it. */
	*index += count + 1U;
	return value;
}

/* Reads a font file into memory and opens its face. */
static int
text_face_open(
	struct te_text_face *face,
	const char *path)
{
	struct stat status;
	ssize_t count;
	size_t done;
	int descriptor;
	int error;

	/* The file. */
	descriptor = open(path, O_RDONLY);
	if (descriptor < 0)
		return errno;

	/* Its size, which must be sane for a font. */
	error = fstat(descriptor, &status);
	if (error != 0 ||
	    status.st_size <= 0 ||
	    (size_t)status.st_size > TEXT_FONT_MAX) {
		close(descriptor);
		return EINVAL;
	}

	/* A buffer for all of it, which the face keeps using. */
	face->size = (size_t)status.st_size;
	face->data = malloc(face->size);
	if (face->data == NULL) {
		close(descriptor);
		return ENOMEM;
	}

	/* Reads the whole file. */
	done = 0;
	while (done < face->size) {
		count = read(descriptor, (char *)face->data + done, face->size - done);
		if (count <= 0)
			break;
		done += (size_t)count;
	}

	/* The file is not needed once read. */
	close(descriptor);

	/* A short read leaves no usable font. */
	if (done != face->size) {
		free(face->data);
		face->data = NULL;
		return EIO;
	}

	/* The face over the bytes. */
	error = truetype_open(face->data, face->size, 0U, &face->face);
	if (error != 0) {
		free(face->data);
		face->data = NULL;
		face->face = NULL;
		return error;
	}

	/* Succeeded: no size is set yet. */
	face->pixels = 0U;
	return 0;
}

/* Finds a character's glyph at a size in the cache, drawing it the first time; NULL when it cannot be drawn. */
static const struct te_glyph *
text_glyph(
	struct te_text *text,
	uint32_t codepoint,
	unsigned pixels,
	int bold)
{
	struct te_glyph *glyph;
	unsigned glyph_index;
	int face_index;
	uint32_t weight;
	uint32_t key;
	int error;

	/* Sizes are kept within what the key holds. */
	if (pixels == 0U || pixels > 127U)
		return NULL;

	/* The first face with a glyph for the character, or the main face's missing glyph. */
	face_index = 0;
	glyph_index = truetype_glyph_index(text->faces[0].face, codepoint);
	if (glyph_index == 0U && text->face_count > 1) {
		glyph_index = truetype_glyph_index(text->faces[1].face, codepoint);
		if (glyph_index != 0U)
			face_index = 1;
	}

	/* Neither has it: the emoji font, opened the first time (a program without one draws the main font's box). */
	if (glyph_index == 0U && !text->emoji_tried) {
		text->emoji_tried = 1;
		(void)text_face_open(&text->faces[2], TE_TEXT_EMOJI);
	}

	/* The emoji font's glyph, when it has the character. */
	if (glyph_index == 0U && text->faces[2].face != NULL) {
		glyph_index = truetype_glyph_index(text->faces[2].face, codepoint);
		if (glyph_index != 0U)
			face_index = 2;
	}

	/* The key: glyph, size, face and weight (never zero, which marks an empty slot). */
	weight = 0U;
	if (bold != 0)
		weight = 1U;
	key = (glyph_index & 0xffffU) | ((uint32_t)pixels << 16) | ((uint32_t)face_index << 23) | (weight << 25) | 0x80000000U;

	/* A glyph drawn before is in its slot. */
	glyph = text_slot(text, key);
	if (glyph->key == key)
		return glyph;

	/* A full cache is emptied first (the glyphs are drawn again as they are needed). */
	if (text->cache_used * 4U >= text->cache_size * 3U) {
		text_clear(text);
		glyph = text_slot(text, key);
	}

	/* Draws the glyph into the free slot. */
	error = text_render(text, face_index, glyph_index, pixels, bold, glyph);
	if (error != 0)
		return NULL;

	/* Succeeded: the slot holds the glyph from now on. */
	glyph->key = key;
	text->cache_used++;
	return glyph;
}

/* Finds the slot of a key: the one holding it, or the empty one where it would go. */
static struct te_glyph *
text_slot(
	struct te_text *text,
	uint32_t key)
{
	unsigned slot;
	uint32_t hash;

	/*
	 * The key's hash, every bit of the key mixed into the low ones (as
	 * libkeiland's text.c, ws210-p014): a multiply alone left the glyphs
	 * of one size and face on a few home slots in long runs.
	 */
	hash = key;
	hash ^= hash >> 16;
	hash *= 0x7feb352dU;
	hash ^= hash >> 15;
	hash *= 0x846ca68bU;
	hash ^= hash >> 16;

	/* Open addressing from the key's hash. */
	slot = hash % text->cache_size;
	while (text->cache[slot].key != 0U && text->cache[slot].key != key)
		slot = (slot + 1U) % text->cache_size;

	/* Reports that slot. */
	return &text->cache[slot];
}

/* Draws a glyph of a face at a size into a cache slot (from a widened outline when bold). */
static int
text_render(
	struct te_text *text,
	int face_index,
	unsigned glyph_index,
	unsigned pixels,
	int bold,
	struct te_glyph *glyph)
{
	struct te_text_face *face;
	struct truetype_glyph metrics;
	size_t needed;
	uint8_t *grown;
	int error;
	int x;
	int y;
	int width;

	/* A colour glyph of the emoji font. */
	if (face_index == 2) {
		error = text_render_color(text, glyph_index, pixels, glyph);
		return error;
	}

	/* The face at the size. */
	face = &text->faces[face_index];
	error = text_set_size(face, pixels);
	if (error != 0)
		return error;

	/* The weight: a bold glyph is drawn from its outline widened. */
	error = truetype_set_bold(face->face, bold);
	if (error != 0)
		return error;

	/* How large the glyph is. */
	error = truetype_glyph_metrics(face->face, glyph_index, &metrics);
	if (error != 0)
		return error;

	/* A scratch bitmap large enough for it. */
	needed = (size_t)metrics.width * (size_t)metrics.height;
	if (needed > text->scratch_size) {
		grown = realloc(text->scratch, needed);
		if (grown == NULL)
			return ENOMEM;
		text->scratch = grown;
		text->scratch_size = needed;
	}

	/* The glyph's coverage, when it has any pixels. */
	if (needed != 0U) {
		error = truetype_render_glyph(face->face, glyph_index, &metrics, text->scratch, (size_t)metrics.width, needed);
		if (error != 0)
			return error;
	}

	/* The slot's measurements (a bold glyph's are its widened outline's). */
	width = (int)metrics.width;
	glyph->width = width;
	glyph->height = (int)metrics.height;
	glyph->left = metrics.left;
	glyph->top = metrics.top;
	glyph->advance = metrics.advance;
	glyph->bitmap = NULL;
	glyph->pixels = NULL;

	/* A blank glyph (a space) needs no bitmap. */
	if (needed == 0U)
		return 0;

	/* The slot's own bitmap. */
	glyph->bitmap = calloc((size_t)width * metrics.height, 1U);
	if (glyph->bitmap == NULL)
		return ENOMEM;

	/* The coverage copied. */
	for (y = 0; y < (int)metrics.height; y++) {
		for (x = 0; x < (int)metrics.width; x++) {
			glyph->bitmap[(size_t)y * (size_t)width + (size_t)x] = text->scratch[(size_t)y * metrics.width + (size_t)x];
		}
	}

	/* Succeeded: the glyph is in the slot. */
	return 0;
}

/* Sets a face's size when it is not set already. */
static int
text_set_size(
	struct te_text_face *face,
	unsigned pixels)
{
	int error;

	/* The size set last is still in force. */
	if (face->pixels == pixels)
		return 0;

	/* The new size. */
	error = truetype_set_pixel_size(face->face, pixels);
	if (error != 0)
		return error;

	/* Succeeded: the face draws at this size. */
	face->pixels = pixels;
	return 0;
}

/* Empties the glyph cache. */
static void
text_clear(
	struct te_text *text)
{
	unsigned slot;

	/* Every slot's bitmap goes and the slot is empty. */
	for (slot = 0; slot < text->cache_size; slot++) {
		free(text->cache[slot].bitmap);
		free(text->cache[slot].pixels);
		memset(&text->cache[slot], 0, sizeof(text->cache[slot]));
	}

	/* No slot is used. */
	text->cache_used = 0;
}

/* Draws a colour glyph of the emoji font at a size into a cache slot (bold draws it as it is). */
static int
text_render_color(
	struct te_text *text,
	unsigned glyph_index,
	unsigned pixels,
	struct te_glyph *glyph)
{
	struct kl_color_image image;
	int error;

	/* The glyph's colours at the size (the face's size changes with it). */
	error = kl_color_glyph(text->faces[2].face, glyph_index, pixels, &image);
	text->faces[2].pixels = pixels;
	if (error != 0)
		return error;

	/* Succeeded: the slot holds the colours. */
	glyph->bitmap = NULL;
	glyph->pixels = image.pixels;
	glyph->width = image.width;
	glyph->height = image.height;
	glyph->left = image.left;
	glyph->top = image.top;
	glyph->advance = image.advance;
	return 0;
}
