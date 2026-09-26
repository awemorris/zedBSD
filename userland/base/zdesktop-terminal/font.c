/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The glyphs of zdesktop-terminal: a monospaced TrueType font drawn with
 * libtruetype into an atlas of cell-sized slots.
 *
 * A character is drawn into a free slot the first time the grid shows it
 * and stays there; the renderer samples the slot as the coverage between a
 * cell's background and foreground.  When the atlas is full, a new
 * character shows the replacement glyph's slot instead.
 */

#include "terminal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <truetype.h>
#include <unistd.h>

/* The largest font file read, and the largest glyph drawn. */
#define FONT_FILE_MAX		(32U * 1024U * 1024U)
#define FONT_GLYPH_MAX		128U

/* The mark of an empty place in the code point table. */
#define FONT_EMPTY		0xffffffffU

static int font_read(struct terminal_font *font, const char *path);
static void font_draw(struct terminal_font *font, unsigned slot, uint32_t codepoint);
static void font_slot_origin(const struct terminal_font *font, unsigned slot, unsigned *x, unsigned *y);

/*
 * Reads a font file and measures its cells at a size in pixels.
 *
 * Returns 0, or an errno value when the file cannot be read or is not a
 * TrueType font.
 */
int
terminal_font_open(
	struct terminal_font *font,
	const char *path,
	unsigned pixels)
{
	struct truetype_metrics metrics;
	struct truetype_glyph glyph;
	unsigned index;
	int error;

	/* Nothing is held until the file is read. */
	memset(font, 0, sizeof(*font));

	/* The whole file, which the face reads from for as long as it is open. */
	error = font_read(font, path);
	if (error != 0)
		return error;

	/* The face over it. */
	error = truetype_open(font->data, font->size, 0U, &font->face);
	if (error != 0) {
		terminal_font_close(font);
		return error;
	}

	/* The size every glyph is drawn at. */
	error = truetype_set_pixel_size(font->face, pixels);
	if (error != 0) {
		terminal_font_close(font);
		return error;
	}

	/* The line's height and where its baseline is. */
	error = truetype_metrics(font->face, &metrics);
	if (error != 0) {
		terminal_font_close(font);
		return error;
	}

	/* The cell is as wide as an M advances and as tall as the font's line. */
	index = truetype_glyph_index(font->face, 'M');
	error = truetype_glyph_metrics(font->face, index, &glyph);
	if (error != 0) {
		terminal_font_close(font);
		return error;
	}

	/* A font with no width or height is not usable for a grid. */
	if (glyph.advance <= 0 || metrics.line_height <= 0) {
		terminal_font_close(font);
		return EINVAL;
	}

	/* Succeeded: the cell's size and baseline. */
	font->cell_width = (unsigned)glyph.advance;
	font->cell_height = (unsigned)metrics.line_height;
	font->baseline = metrics.ascent;
	return 0;
}

/*
 * Gives the atlas its pixels, draws the cursor's solid block into slot 0
 * and prepares the table of drawn characters.
 *
 * Returns 0, or ENOMEM.
 */
int
terminal_font_attach(
	struct terminal_font *font,
	unsigned char *pixels,
	size_t row_pitch,
	unsigned width,
	unsigned height)
{
	uint32_t *row;
	unsigned index;
	unsigned x;
	unsigned y;

	/* The image the renderer made for the atlas, and how many cells fit in it. */
	font->pixels = pixels;
	font->row_pitch = row_pitch;
	font->atlas_width = width;
	font->atlas_height = height;
	font->slots = (width / font->cell_width) * (height / font->cell_height);
	font->used = 1U;

	/* The table of drawn characters, twice as large as the slots so that it never fills. */
	font->table_size = font->slots * 2U;
	font->keys = malloc((size_t)font->table_size * sizeof(font->keys[0]));
	if (font->keys == NULL)
		return ENOMEM;

	/* And the slot each of those characters is in. */
	font->values = malloc((size_t)font->table_size * sizeof(font->values[0]));
	if (font->values == NULL)
		return ENOMEM;

	/* Every place of the table empty. */
	for (index = 0U; index < font->table_size; index++)
		font->keys[index] = FONT_EMPTY;

	/* The atlas starts with no coverage anywhere. */
	for (y = 0U; y < height; y++)
		memset(pixels + (size_t)y * row_pitch, 0, (size_t)width * 4U);

	/* Slot 0 is full coverage: the cursor is a cell drawn in the foreground. */
	for (y = 0U; y < font->cell_height; y++) {
		row = (uint32_t *)(pixels + (size_t)y * row_pitch);
		for (x = 0U; x < font->cell_width; x++)
			row[x] = 0xffffffffU;
	}

	/* Succeeded: characters can be drawn into the atlas. */
	return 0;
}

/*
 * Returns the slot a character is drawn in, drawing it first if the atlas
 * has not got it.
 */
unsigned
terminal_font_slot(
	struct terminal_font *font,
	uint32_t codepoint)
{
	unsigned place;
	unsigned slot;

	/* Looks the character up, probing from its hash until it or an empty place is found. */
	place = (codepoint * 2654435761U) % font->table_size;
	while (font->keys[place] != FONT_EMPTY) {
		/* A character drawn before keeps its slot. */
		if (font->keys[place] == codepoint)
			return font->values[place];
		place = (place + 1U) % font->table_size;
	}

	/* A full atlas draws nothing more: the replacement character's slot, or the block, stands in. */
	if (font->used >= font->slots) {
		if (codepoint == 0xfffdU)
			return 0U;
		slot = terminal_font_slot(font, 0xfffdU);
		return slot;
	}

	/* A new slot for the character, drawn now and remembered. */
	slot = font->used;
	font->used++;
	font_draw(font, slot, codepoint);
	font->keys[place] = codepoint;
	font->values[place] = slot;

	/* Succeeded: the character's new slot. */
	return slot;
}

/*
 * Releases the face, the file and the table (not the atlas's pixels, which
 * are the renderer's).
 */
void
terminal_font_close(
	struct terminal_font *font)
{
	/* The face reads from the file, so it goes first. */
	if (font->face != NULL)
		truetype_close(font->face);

	/* The file and the table. */
	free(font->data);
	free(font->keys);
	free(font->values);
	memset(font, 0, sizeof(*font));
}

/* Reads the whole font file into memory the face keeps using. */
static int
font_read(
	struct terminal_font *font,
	const char *path)
{
	struct stat status;
	ssize_t count;
	size_t done;
	int descriptor;
	int error;

	/* Opens the file. */
	descriptor = open(path, O_RDONLY | O_CLOEXEC);
	if (descriptor < 0)
		return errno;

	/* Its size, which must be sensible for a font. */
	error = fstat(descriptor, &status);
	if (error != 0) {
		error = errno;
		close(descriptor);
		return error;
	}

	/* A file too short or too long to be a font is refused. */
	if (status.st_size <= 0 || (unsigned long)status.st_size > FONT_FILE_MAX) {
		close(descriptor);
		return EFBIG;
	}

	/* The memory for all of it. */
	font->size = (size_t)status.st_size;
	font->data = malloc(font->size);
	if (font->data == NULL) {
		close(descriptor);
		return ENOMEM;
	}

	/* Reads it to the end. */
	done = 0U;
	while (done < font->size) {
		count = read(descriptor, (char *)font->data + done, font->size - done);
		if (count <= 0) {
			error = EIO;
			if (count < 0)
				error = errno;
			close(descriptor);
			return error;
		}

		/* What was read counts towards the whole. */
		done += (size_t)count;
	}

	/* Succeeded: the font is in memory. */
	close(descriptor);
	return 0;
}

/* Draws a character's glyph into a slot, on the baseline, as white coverage. */
static void
font_draw(
	struct terminal_font *font,
	unsigned slot,
	uint32_t codepoint)
{
	static uint8_t bitmap[FONT_GLYPH_MAX * FONT_GLYPH_MAX];
	struct truetype_glyph glyph;
	uint32_t *row;
	uint32_t value;
	unsigned origin_x;
	unsigned origin_y;
	unsigned index;
	unsigned x;
	unsigned y;
	int target_x;
	int target_y;
	int error;

	/* The glyph and how big it is; a glyph too large for the buffer is left blank. */
	index = truetype_glyph_index(font->face, codepoint);
	error = truetype_render_glyph(font->face, index, &glyph, bitmap, FONT_GLYPH_MAX, sizeof(bitmap));
	if (error != 0)
		return;

	/* Copies the coverage into the slot, clipped to the cell. */
	font_slot_origin(font, slot, &origin_x, &origin_y);
	for (y = 0U; y < glyph.height; y++) {
		/* The row of the cell this glyph row lands on, if any (top counts up from the baseline). */
		target_y = font->baseline - glyph.top + (int)y;
		if (target_y < 0 || target_y >= (int)font->cell_height)
			continue;
		row = (uint32_t *)(font->pixels + (size_t)(origin_y + (unsigned)target_y) * font->row_pitch);

		/* Each pixel of the glyph row that lands in the cell. */
		for (x = 0U; x < glyph.width; x++) {
			target_x = glyph.left + (int)x;
			if (target_x < 0 || target_x >= (int)font->cell_width)
				continue;
			value = bitmap[y * FONT_GLYPH_MAX + x];
			row[origin_x + (unsigned)target_x] = (value << 24) | (value << 16) | (value << 8) | value;
		}
	}
}

/* Finds where a slot's cell starts in the atlas. */
static void
font_slot_origin(
	const struct terminal_font *font,
	unsigned slot,
	unsigned *x,
	unsigned *y)
{
	unsigned per_row;

	/* The slots fill the atlas row by row. */
	per_row = font->atlas_width / font->cell_width;
	*x = (slot % per_row) * font->cell_width;
	*y = (slot / per_row) * font->cell_height;
}
