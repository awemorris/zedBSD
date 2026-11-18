/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The glyphs of terminal: a monospaced TrueType font drawn with
 * libtruetype into an atlas of cell-sized slots.
 *
 * A character is drawn into a free slot the first time the grid shows it
 * and stays there; the renderer samples the slot as the coverage between a
 * cell's background and foreground.  When the atlas is full, a new
 * character shows the replacement glyph's slot instead.
 *
 * A character the grid gives two cells (East Asian wide, or Ambiguous with
 * View > Treat Ambiguous-Width Characters as Wide, ws128-p009) is drawn
 * into two slots side by side, in a frame two cells wide: a glyph made for
 * two cells fills it, a narrower one is centred in it, and box drawing and
 * block elements are drawn twice as wide so their lines still meet the
 * next cell's.  A character the font has no glyph for is drawn from its
 * companion, the monospaced fallback (ws090-p020: the signs and the box
 * drawing Mahora Mono lacks), and one neither has (CJK) from the fallback
 * font when there is one.  The cell's line is fixed (FONT_LINE_*), not the
 * font's.
 */

#include "terminal.h"

#include "userland/desktop/paths.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <truetype/truetype.h>
#include <unistd.h>

/*
 * A cell's line, in thousandths of the size: how far it reaches above the
 * baseline and below it.  It is the line JetBrains Mono gave the terminal
 * before Mahora Mono (ws090-p020), kept whatever fonts are installed, so
 * that the grid keeps its spacing and the fallback's box drawing fills
 * its cells.
 */
#define FONT_LINE_ASCENT	1020U
#define FONT_LINE_DESCENT	300U

/* The largest font file read, and the largest glyph drawn. */
#define FONT_FILE_MAX		(32U * 1024U * 1024U)
#define FONT_GLYPH_MAX		128U

/* The mark of an empty place in the code point table. */
#define FONT_EMPTY		0xffffffffU

/* The bit that makes a code point the key of its glyph drawn two cells wide (no code point has it). */
#define FONT_WIDE_KEY		0x80000000U

/* Box drawing and block elements, which a wide cell draws stretched so their lines join the next cell's. */
#define FONT_STRETCH_FIRST	0x2500U
#define FONT_STRETCH_LAST	0x259fU

/* The replacement character, shown for what cannot be drawn. */
#define FONT_REPLACEMENT	0xfffdU

static int font_read(const char *path, void **data, size_t *size);
static int font_measure(struct terminal_font *font, unsigned pixels);
static unsigned font_lookup(struct terminal_font *font, uint32_t key, unsigned *slot);
static void font_draw(struct terminal_font *font, unsigned slot, uint32_t codepoint, unsigned cells);
static void font_mark(struct terminal_font *font, unsigned x, unsigned y, unsigned width, unsigned height);
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
	int error;

	/* Nothing is held until the file is read. */
	memset(font, 0, sizeof(*font));

	/* The whole file, which the face reads from for as long as it is open. */
	error = font_read(path, &font->data, &font->size);
	if (error != 0)
		return error;

	/* The face over it. */
	error = truetype_open(font->data, font->size, 0U, &font->face);
	if (error != 0) {
		terminal_font_close(font);
		return error;
	}

	/* Its companion, the monospaced fallback, for the signs and the box drawing it lacks (ws090-p020; no bold is drawn). */
	(void)truetype_open_companions(font->face, NULL, KEILAND_FONT_FALLBACK_MONO);

	/* The cell's size at the size every glyph is drawn at. */
	error = font_measure(font, pixels);
	if (error != 0) {
		terminal_font_close(font);
		return error;
	}

	/* Succeeded: the font is open. */
	return 0;
}

/*
 * Changes the size glyphs are drawn at (zooming): the cell is measured
 * again and the atlas starts over with nothing but the cursor's block.
 *
 * Returns 0, or an errno value with the font as it was when the size could
 * not be measured, or ENOMEM when the atlas's table could not be made again.
 */
int
terminal_font_resize(
	struct terminal_font *font,
	unsigned pixels)
{
	unsigned old_pixels;
	int error;

	/* The new cell; a size the font cannot give keeps the old one. */
	old_pixels = font->pixels_size;
	error = font_measure(font, pixels);
	if (error != 0) {
		(void)font_measure(font, old_pixels);
		return error;
	}

	/* The table of drawn characters goes; every glyph is drawn again at the new size. */
	free(font->keys);
	free(font->values);
	font->keys = NULL;
	font->values = NULL;
	error = terminal_font_attach(font, font->pixels, font->row_pitch, font->atlas_width, font->atlas_height);
	if (error != 0)
		return error;

	/* Succeeded: the next frame draws at the new size. */
	return 0;
}

/*
 * Opens the fallback font, which draws the characters the font has no
 * glyph for, at the font's size.  It is opened before the atlas is
 * attached, so that every glyph drawn can use it.
 *
 * Returns 0, or an errno value with no fallback when the file cannot be
 * read or is not a TrueType font.
 */
int
terminal_font_fallback(
	struct terminal_font *font,
	const char *path)
{
	int error;

	/* The whole file, which the fallback face reads from for as long as it is open. */
	error = font_read(path, &font->fallback_data, &font->fallback_size);
	if (error != 0)
		return error;

	/* The face over it. */
	error = truetype_open(font->fallback_data, font->fallback_size, 0U, &font->fallback_face);
	if (error != 0) {
		free(font->fallback_data);
		font->fallback_data = NULL;
		font->fallback_face = NULL;
		return error;
	}

	/* Drawn at the font's size. */
	error = truetype_set_pixel_size(font->fallback_face, font->pixels_size);
	if (error != 0) {
		truetype_close(font->fallback_face);
		free(font->fallback_data);
		font->fallback_data = NULL;
		font->fallback_face = NULL;
		return error;
	}

	/* Succeeded: characters missing from the font come from the fallback. */
	return 0;
}

/*
 * Gives the atlas its pixels, draws the cursor's solid block into slot 0
 * and the replacement character two cells wide into slots 1 and 2, and
 * prepares the table of drawn characters.
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
	font->used = 3U;
	font->wide_replacement = 1U;

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

	/*
	 * The first row of slots goes to the GPU's image again; the slots below
	 * it are taken there as they are drawn (no frame samples a slot that
	 * has not been drawn).
	 */
	font->dirty_right = 0U;
	font_mark(font, 0U, 0U, width, font->cell_height);

	/* Slots 1 and 2 are the replacement character two cells wide, for a wide character a full atlas cannot draw. */
	font_draw(font, font->wide_replacement, FONT_REPLACEMENT, 2U);

	/* Succeeded: characters can be drawn into the atlas. */
	return 0;
}

/*
 * Returns the slot a character is drawn in, one cell wide, drawing it
 * first if the atlas has not got it.
 */
unsigned
terminal_font_slot(
	struct terminal_font *font,
	uint32_t codepoint)
{
	unsigned place;
	unsigned slot;
	unsigned found;

	/* A character drawn before keeps its slot. */
	place = font_lookup(font, codepoint, &slot);
	found = font->keys[place];
	if (found == codepoint)
		return slot;

	/* A full atlas draws nothing more: the replacement character's slot, or the block, stands in. */
	if (font->used >= font->slots) {
		if (codepoint == FONT_REPLACEMENT)
			return 0U;
		slot = terminal_font_slot(font, FONT_REPLACEMENT);
		return slot;
	}

	/* A new slot for the character, drawn now and remembered. */
	slot = font->used;
	font->used++;
	font_draw(font, slot, codepoint, 1U);
	font->keys[place] = codepoint;
	font->values[place] = slot;

	/* Succeeded: the character's new slot. */
	return slot;
}

/*
 * Returns the first of the two slots side by side a character is drawn in,
 * two cells wide, drawing it first if the atlas has not got it.
 */
unsigned
terminal_font_wide_slot(
	struct terminal_font *font,
	uint32_t codepoint)
{
	unsigned per_row;
	unsigned place;
	unsigned slot;
	unsigned found;
	unsigned key;

	/* A character drawn wide before keeps its slots. */
	key = codepoint | FONT_WIDE_KEY;
	place = font_lookup(font, key, &slot);
	found = font->keys[place];
	if (found == key)
		return slot;

	/* Two slots side by side must be on one row of the atlas: a row's last slot is passed over. */
	per_row = font->atlas_width / font->cell_width;
	if (font->used % per_row == per_row - 1U)
		font->used++;

	/* A full atlas draws nothing more: the wide replacement character stands in. */
	if (font->used + 2U > font->slots)
		return font->wide_replacement;

	/* Two new slots for the character, drawn now and remembered. */
	slot = font->used;
	font->used += 2U;
	font_draw(font, slot, codepoint, 2U);
	font->keys[place] = key;
	font->values[place] = slot;

	/* Succeeded: the first of the character's new slots. */
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
	/* The faces read from the files, so they go first. */
	if (font->face != NULL)
		truetype_close(font->face);

	/* The fallback face, when one was opened. */
	if (font->fallback_face != NULL)
		truetype_close(font->fallback_face);

	/* The files and the table. */
	free(font->data);
	free(font->fallback_data);
	free(font->keys);
	free(font->values);
	memset(font, 0, sizeof(*font));
}

/* Reads a whole font file into memory a face keeps using. */
static int
font_read(
	const char *path,
	void **data,
	size_t *size)
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
	*size = (size_t)status.st_size;
	*data = malloc(*size);
	if (*data == NULL) {
		close(descriptor);
		return ENOMEM;
	}

	/* Reads it to the end. */
	done = 0U;
	while (done < *size) {
		count = read(descriptor, (char *)*data + done, *size - done);
		if (count <= 0) {
			error = EIO;
			if (count < 0)
				error = errno;
			close(descriptor);
			free(*data);
			*data = NULL;
			return error;
		}

		/* What was read counts towards the whole. */
		done += (size_t)count;
	}

	/* Succeeded: the font is in memory. */
	close(descriptor);
	return 0;
}

/* Sets the size glyphs are drawn at and measures the cell and the baseline at it. */
static int
font_measure(
	struct terminal_font *font,
	unsigned pixels)
{
	struct truetype_glyph glyph;
	unsigned ascent;
	unsigned descent;
	unsigned index;
	int error;

	/* The size every glyph is drawn at. */
	error = truetype_set_pixel_size(font->face, pixels);
	if (error != 0)
		return error;

	/* The line's reach above and below the baseline at the size, in whole pixels rounded out. */
	ascent = (pixels * FONT_LINE_ASCENT + 999U) / 1000U;
	descent = (pixels * FONT_LINE_DESCENT + 999U) / 1000U;

	/* The cell is as wide as an M advances. */
	index = truetype_glyph_index(font->face, 'M');
	error = truetype_glyph_metrics(font->face, index, &glyph);
	if (error != 0)
		return error;

	/* A font with no width is not usable for a grid. */
	if (glyph.advance <= 0)
		return EINVAL;

	/* The fallback is drawn at the same size; one that cannot be leaves its glyphs at the old size. */
	if (font->fallback_face != NULL) {
		error = truetype_set_pixel_size(font->fallback_face, pixels);
		if (error != 0)
			printf("ZTERM FONT fallback-size=%u error=%d\n", pixels, error);
	}

	/* Succeeded: the cell's size and baseline. */
	font->pixels_size = pixels;
	font->cell_width = (unsigned)glyph.advance;
	font->cell_height = ascent + descent;
	font->baseline = (int)ascent;
	return 0;
}

/*
 * Looks a key up in the table of drawn characters, probing from its hash
 * until it or an empty place is found.  Returns the place, and the key's
 * slot in *slot when the place holds the key.
 */
static unsigned
font_lookup(
	struct terminal_font *font,
	uint32_t key,
	unsigned *slot)
{
	unsigned place;

	/* Nothing found yet. */
	*slot = 0U;

	/* The probe starts at the key's hash and stops at the key or at an empty place. */
	place = (key * 2654435761U) % font->table_size;
	while (font->keys[place] != FONT_EMPTY) {
		/* The key's own place. */
		if (font->keys[place] == key) {
			*slot = font->values[place];
			return place;
		}

		/* Another key's place: the probe goes on to the next one. */
		place = (place + 1U) % font->table_size;
	}

	/* The empty place where the key would go. */
	return place;
}

/*
 * Draws a character's glyph into a frame of one cell or of two (two slots
 * side by side), on the baseline, as white coverage.
 */
static void
font_draw(
	struct terminal_font *font,
	unsigned slot,
	uint32_t codepoint,
	unsigned cells)
{
	static uint8_t bitmap[FONT_GLYPH_MAX * FONT_GLYPH_MAX];
	struct truetype_face *face;
	struct truetype_glyph glyph;
	uint32_t *row;
	uint32_t value;
	unsigned origin_x;
	unsigned origin_y;
	unsigned index;
	unsigned fallback_index;
	unsigned frame_width;
	unsigned copies;
	unsigned copy;
	unsigned x;
	unsigned y;
	int shift;
	int stretch;
	int target_x;
	int target_y;
	int error;

	/* The slot's cells go to the GPU's image, drawn or left blank. */
	font_slot_origin(font, slot, &origin_x, &origin_y);
	font_mark(font, origin_x, origin_y, cells * font->cell_width, font->cell_height);

	/* The font's glyph, or the fallback's when the font has none and the fallback has one. */
	face = font->face;
	index = truetype_glyph_index(font->face, codepoint);
	if (index == 0U && font->fallback_face != NULL) {
		/* The fallback's glyph for the character, used when it has one. */
		fallback_index = truetype_glyph_index(font->fallback_face, codepoint);
		if (fallback_index != 0U) {
			face = font->fallback_face;
			index = fallback_index;
		}
	}

	/* The glyph and how big it is; a glyph too large for the buffer is left blank. */
	error = truetype_render_glyph(face, index, &glyph, bitmap, FONT_GLYPH_MAX, sizeof(bitmap));
	if (error != 0)
		return;

	/*
	 * Where the glyph goes across the frame.  One cell draws it as the font
	 * places it.  Two cells draw box drawing and block elements twice as
	 * wide, each pixel twice, so a line runs from edge to edge; any other
	 * glyph narrower than the frame is centred in it.
	 */
	frame_width = cells * font->cell_width;
	shift = 0;
	stretch = 0;
	if (cells == 2U &&
	    codepoint >= FONT_STRETCH_FIRST &&
	    codepoint <= FONT_STRETCH_LAST) {
		/* A line or a block that must meet the next cell's. */
		stretch = 1;
	} else if (cells == 2U && glyph.advance < (int)frame_width) {
		/* A glyph made for one cell, in the middle of two. */
		shift = ((int)frame_width - glyph.advance) / 2;
	}

	/* A stretched pixel is written twice. */
	copies = 1U;
	if (stretch)
		copies = 2U;

	/* Copies the coverage into the slot, clipped to the frame. */
	font_slot_origin(font, slot, &origin_x, &origin_y);
	for (y = 0U; y < glyph.height; y++) {
		/* The row of the cell this glyph row lands on, if any (top counts up from the baseline). */
		target_y = font->baseline - glyph.top + (int)y;
		if (target_y < 0 || target_y >= (int)font->cell_height)
			continue;
		row = (uint32_t *)(font->pixels + (size_t)(origin_y + (unsigned)target_y) * font->row_pitch);

		/* Each pixel of the glyph row, once or twice, where it lands in the frame. */
		for (x = 0U; x < glyph.width; x++) {
			/* The glyph pixel's coverage. */
			value = bitmap[y * FONT_GLYPH_MAX + x];

			/* Writes it once, or twice side by side for a stretched glyph, where it lands in the frame. */
			for (copy = 0U; copy < copies; copy++) {
				target_x = shift + (glyph.left + (int)x) * (int)copies + (int)copy;
				if (target_x < 0 || target_x >= (int)frame_width)
					continue;
				row[origin_x + (unsigned)target_x] = (value << 24) | (value << 16) | (value << 8) | value;
			}
		}
	}
}

/* Adds a box of the atlas's pixels to the box the renderer copies to the GPU's image. */
static void
font_mark(
	struct terminal_font *font,
	unsigned x,
	unsigned y,
	unsigned width,
	unsigned height)
{
	/* An empty box becomes this one. */
	if (font->dirty_right == 0U) {
		font->dirty_left = x;
		font->dirty_top = y;
		font->dirty_right = x + width;
		font->dirty_bottom = y + height;
		return;
	}

	/* Otherwise the box grows to hold both. */
	if (x < font->dirty_left)
		font->dirty_left = x;
	if (y < font->dirty_top)
		font->dirty_top = y;
	if (x + width > font->dirty_right)
		font->dirty_right = x + width;
	if (y + height > font->dirty_bottom)
		font->dirty_bottom = y + height;
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
