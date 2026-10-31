/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The fonts of a page: opening them, picking one for a style, the web
 * fonts the page adds and their families, and the glyph cache that keeps
 * every glyph's advance and coverage bitmap once it has been measured or
 * drawn.
 */

#include "text/text.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <truetype/truetype.h>

/* The smallest and largest pixel size a font is drawn at. */
#define FONT_PIXELS_MIN		1U
#define FONT_PIXELS_MAX		400U

/* The weight from which a font is drawn bold. */
#define FONT_BOLD_WEIGHT	600

/* The size of the glyph cache's table when the first glyph arrives. */
#define FONT_CACHE_MIN		1024U

/*
 * One cached glyph: its key (face, bold, size and code point), what the
 * layout needs, the bitmap once it is drawn, and whether a bold glyph is
 * its face's bold companion's (drawn without the widening).
 */
struct text_glyph_entry {
	uint64_t key;
	struct text_glyph glyph;
	uint8_t *pixels;
	int drawn;
	int bold_face;
};

static uint64_t font_key(const struct text_font *font, uint32_t code_point);
static struct text_glyph_entry **font_slot(struct text_system *system, uint64_t key);
static int font_grow(struct text_system *system);
static int font_measure(struct text_system *system, const struct text_font *font, uint32_t code_point, struct text_glyph_entry *entry);
static int font_draw(struct text_system *system, const struct text_font *font, struct text_glyph_entry *entry);
static int font_open_face(struct text_face *face, const char *path);
static const unsigned char *font_table(const struct wb_buffer *data, const char *tag, size_t length);
static int font_weight_distance(const struct text_family *family, int weight);

/*
 * Opens the fonts of a text system.  The sans font is required; the
 * monospace and the fallback fonts are used when they open.
 */
int
text_system_open(
	struct text_system *system,
	const struct text_font_paths *paths)
{
	int error;

	/* Starts with no faces and an empty cache. */
	memset(system, 0, sizeof(*system));

	/* The sans font must open. */
	error = font_open_face(&system->faces[TEXT_FACE_SANS], paths->sans);
	if (error != 0)
		return error;

	/* Its companions: Mahora Bold for its bold glyphs, the monospaced fallback for the signs it lacks. */
	(void)truetype_open_companions(system->faces[TEXT_FACE_SANS].face, KEILAND_FONT_BOLD, KEILAND_FONT_FALLBACK_MONO);

	/* The others are optional; the monospace font has the same fallback. */
	if (paths->mono != NULL) {
		error = font_open_face(&system->faces[TEXT_FACE_MONO], paths->mono);
		if (error == 0)
			(void)truetype_open_companions(system->faces[TEXT_FACE_MONO].face, NULL, KEILAND_FONT_FALLBACK_MONO);
	}
	if (paths->fallback != NULL)
		font_open_face(&system->faces[TEXT_FACE_FALLBACK], paths->fallback);

	/* Succeeded: the fonts are open. */
	return 0;
}

/*
 * Closes the fonts and frees the glyph cache.
 */
void
text_system_close(
	struct text_system *system)
{
	size_t index;
	int face;

	/* Frees every cached glyph. */
	for (index = 0; index < system->cache_capacity; index++) {
		if (system->cache[index] == NULL)
			continue;

		/* The bitmap and the entry. */
		free(system->cache[index]->pixels);
		free(system->cache[index]);
	}

	/* Then the table. */
	free(system->cache);

	/* Closes the faces and frees their files. */
	for (face = 0; face < TEXT_FACES; face++) {
		if (system->faces[face].open)
			truetype_close(system->faces[face].face);
		wb_buffer_release(&system->faces[face].data);
	}

	/* Nothing is left open. */
	memset(system, 0, sizeof(*system));
}

/*
 * Selects a style's face, logical size and positive rasterizer size.
 * A zero logical size stays distinct from the rasterizer's minimum pixel size.
 */
void
text_select_font(
	const struct text_system *system,
	int monospace,
	float size,
	int weight,
	struct text_font *font)
{
	float rounded;
	int empty;

	/* Monospace text uses its actual open face before later family selection. */
	font->face = TEXT_FACE_SANS;
	if (monospace && system->faces[TEXT_FACE_MONO].open)
		font->face = TEXT_FACE_MONO;

	/* Logical zero must not become a one-pixel advance when rasterization is clamped. */
	empty = 0;
	if (size == 0.0f)
		empty = 1;

	/* Positive and fractional inputs retain their existing minimum-size behavior. */
	if (size < (float)FONT_PIXELS_MIN)
		size = (float)FONT_PIXELS_MIN;

	/* The rasterizer's existing upper bound also remains the positive logical bound. */
	if (size > (float)FONT_PIXELS_MAX)
		size = (float)FONT_PIXELS_MAX;

	/* Layout and metrics observe true zero independently of the safe rasterizer size. */
	font->size = size;
	if (empty)
		font->size = 0.0f;

	/* Rounds the rasterizer's request while keeping its minimum strictly positive. */
	rounded = size + 0.5f;
	if (rounded < (float)FONT_PIXELS_MIN)
		rounded = (float)FONT_PIXELS_MIN;

	/* Rounded upper bounds cannot exceed the supported raster size. */
	if (rounded > (float)FONT_PIXELS_MAX)
		rounded = (float)FONT_PIXELS_MAX;

	/* Publishes the positive draw size separately from logical zero. */
	font->pixels = (unsigned)rounded;

	/* Synthetic bold remains a style choice even when the selected font has no ink. */
	font->bold = 0;
	if (weight >= FONT_BOLD_WEIGHT)
		font->bold = 1;

	/* Succeeded: the selected face and independent logical/raster sizes are ready. */
	return;
}

/*
 * Adds a web font's face under its family, weights and style;
 * the face takes the font file's bytes from data (which is left empty).
 * Returns ENOSPC when the system holds as many web faces as it can.
 */
int
text_add_face(
	struct text_system *system,
	const void *family,
	int weight_min,
	int weight_max,
	int italic,
	struct wb_buffer *data)
{
	struct text_face *face;
	struct text_family *entry;
	int index;
	int error;

	/* The next web face's place. */
	if (system->family_count >= TEXT_WEB_FACES)
		return ENOSPC;
	index = TEXT_FACE_WEB + system->family_count;
	face = &system->faces[index];

	/* The face takes the bytes and opens its first face. */
	face->data = *data;
	wb_buffer_init(data);
	error = truetype_open(face->data.data, face->data.length, 0, &face->face);
	if (error != 0) {
		wb_buffer_release(&face->data);
		return error;
	}

	/* The face is open. */
	face->open = 1;

	/* Its family entry. */
	entry = &system->families[system->family_count];
	entry->family = family;
	entry->weight_min = weight_min;
	entry->weight_max = weight_max;
	entry->italic = italic;
	entry->face = index;
	system->family_count++;

	/* Succeeded: the face is the family's. */
	return 0;
}

/*
 * Picks a web font's face for a family, a weight and a style,
 * as CSS font matching does in short: the faces of the style asked for (or
 * of the other style when the family has none), the weight nearest the one
 * asked for (a face covering it, else a heavier one for bold weights and a
 * lighter one for the others).  The font's face becomes it, drawn bold when
 * a bold weight is asked of a face that is not.  Reports whether the family
 * has a face.
 */
int
text_select_family(
	const struct text_system *system,
	const void *family,
	int weight,
	int italic,
	struct text_font *font)
{
	const struct text_family *entry;
	const struct text_family *best;
	int best_style;
	int best_distance;
	int style_matches;
	int distance;
	int index;

	/* The family's faces, the best by style, then by weight. */
	best = NULL;
	best_style = 0;
	best_distance = 0;
	for (index = 0; index < system->family_count; index++) {
		entry = &system->families[index];
		if (entry->family != family)
			continue;

		/* How well the face fits. */
		style_matches = 0;
		if (entry->italic == italic)
			style_matches = 1;
		distance = font_weight_distance(entry, weight);

		/* A face of the right style wins; among those of the same style, the nearer weight. */
		if (best == NULL) {
			best = entry;
		} else if (style_matches > best_style) {
			best = entry;
		} else if (style_matches == best_style && distance < best_distance) {
			best = entry;
		}

		/* The best so far. */
		if (best == entry) {
			best_style = style_matches;
			best_distance = distance;
		}
	}

	/* A family without faces is not the font. */
	if (best == NULL)
		return 0;

	/* The face; bold is drawn only when the face is lighter than the weight asked for. */
	font->face = best->face;
	font->bold = 0;
	if (weight >= FONT_BOLD_WEIGHT && best->weight_max < FONT_BOLD_WEIGHT)
		font->bold = 1;

	/* The family has the face. */
	return 1;
}

/*
 * Measures how far a face's weights are from a weight asked for, the way
 * CSS prefers: none inside its range; for a bold weight, heavier faces
 * before lighter ones; otherwise lighter faces before heavier ones.
 */
static int
font_weight_distance(
	const struct text_family *family,
	int weight)
{
	/* A face covering the weight fits exactly. */
	if (weight >= family->weight_min && weight <= family->weight_max)
		return 0;

	/* A bold weight: heavier faces by how much heavier, then the lighter ones after them all. */
	if (weight >= FONT_BOLD_WEIGHT) {
		if (family->weight_min > weight)
			return family->weight_min - weight;
		return 1000 + weight - family->weight_max;
	}

	/* Another weight: lighter faces first, then the heavier ones. */
	if (family->weight_max < weight)
		return weight - family->weight_max;
	return 1000 + family->weight_min - weight;
}

/*
 * Reports a font's ascent, descent and line height in pixels.
 */
int
text_font_metrics(
	struct text_system *system,
	const struct text_font *font,
	struct text_metrics *metrics)
{
	struct truetype_design_metrics design;
	struct text_face *face;
	float scale;
	int error;

	/* The face's design measures. */
	face = &system->faces[font->face];
	error = truetype_design_metrics(face->face, &design);
	if (error != 0)
		return error;
	if (design.units_per_em == 0)
		return EINVAL;

	/*
	 * Scales each to the fractional size and rounds each to whole pixels, the
	 * way Chromium's font code does, so lines are as tall as in Chromium.
	 */
	scale = font->size / (float)design.units_per_em;
	metrics->ascent = (int)((float)design.ascent * scale + 0.5f);
	metrics->descent = (int)((float)(-design.descent) * scale + 0.5f);
	metrics->line_height = metrics->ascent + metrics->descent + (int)((float)design.line_gap * scale + 0.5f);

	/* Succeeded: the measures are filled. */
	return 0;
}

/*
 * Reports a font's average and largest character widths in whole pixels,
 * as Chromium's font code takes them: the OS/2 table's xAvgCharWidth and
 * the width of the head table's bounding box, scaled to the font's size
 * and rounded.  A form control's natural width is measured with them.
 * Returns ENOENT when the face lacks either table.
 */
int
text_font_char_widths(
	struct text_system *system,
	const struct text_font *font,
	int *average,
	int *maximum)
{
	const unsigned char *head;
	const unsigned char *os2;
	struct text_face *face;
	unsigned units_per_em;
	float scale;
	int average_units;
	int x_min;
	int x_max;

	/* The two tables of the face's file. */
	face = &system->faces[font->face];
	head = font_table(&face->data, "head", 44U);
	os2 = font_table(&face->data, "OS/2", 4U);
	if (head == NULL || os2 == NULL)
		return ENOENT;

	/* unitsPerEm and the bounding box's left and right from head, xAvgCharWidth from OS/2 (big-endian). */
	units_per_em = ((unsigned)head[18] << 8) | (unsigned)head[19];
	x_min = (int)(int16_t)(((unsigned)head[36] << 8) | (unsigned)head[37]);
	x_max = (int)(int16_t)(((unsigned)head[40] << 8) | (unsigned)head[41]);
	average_units = (int)(int16_t)(((unsigned)os2[2] << 8) | (unsigned)os2[3]);
	if (units_per_em == 0 || average_units <= 0)
		return ENOENT;

	/* Scaled to the size and rounded to whole pixels. */
	scale = font->size / (float)units_per_em;
	*average = (int)((float)average_units * scale + 0.5f);
	*maximum = (int)((float)(x_max - x_min) * scale + 0.5f);

	/* Succeeded: the widths are reported. */
	return 0;
}

/*
 * Finds the glyph of a code point in a font (or, when the font lacks it,
 * in the fallback), with its advance and, when asked, its bitmap.
 *
 * The glyph stays valid until the text system is closed.
 */
int
text_glyph(
	struct text_system *system,
	const struct text_font *font,
	uint32_t code_point,
	int with_bitmap,
	struct text_glyph *glyph)
{
	struct text_glyph_entry **slot;
	struct text_glyph_entry *entry;
	uint64_t key;
	int error;

	/* Zero logical size has neither advance nor coverage, including bold and fallback glyphs. */
	if (font->size == 0.0f) {
		memset(glyph, 0, sizeof(*glyph));
		glyph->face = font->face;

		/* Succeeded: callers receive the ordinary empty-ink glyph contract. */
		return 0;
	}

	/* Grows the cache before publishing an entry that would exceed its load bound. */
	if ((system->cache_count + 1U) * 2U > system->cache_capacity) {
		error = font_grow(system);
		if (error != 0)
			return error;
	}

	/* Looks up the exact selected face, size, weight and code point. */
	key = font_key(font, code_point);
	slot = font_slot(system, key);
	entry = *slot;

	/* A missing key must be fully measured before the cache can own it. */
	if (entry == NULL) {
		/* Allocates one unpublished glyph record. */
		entry = calloc(1, sizeof(*entry));
		if (entry == NULL)
			return ENOMEM;

		/* Measures the complete record before any shared cache slot points to it. */
		entry->key = key;
		error = font_measure(system, font, code_point, entry);
		if (error != 0) {
			free(entry);
			return error;
		}

		/* The cache owns this glyph until system close; its count preserves the load bound. */
		*slot = entry;
		system->cache_count++;
	}

	/* Draws requested coverage once; empty logical glyphs already returned above. */
	if (with_bitmap && !entry->drawn) {
		error = font_draw(system, font, entry);
		if (error != 0)
			return error;
	}

	/* Succeeded: the ordinary positive-size glyph borrows the cache's stable storage. */
	*glyph = entry->glyph;
	return 0;
}

/*
 * Reports the advance of a code point's glyph in a font, in pixels.
 */
int
text_glyph_advance(
	struct text_system *system,
	const struct text_font *font,
	uint32_t code_point,
	int *advance)
{
	struct text_glyph glyph;
	int error;

	/* Measures the glyph without drawing it. */
	error = text_glyph(system, font, code_point, 0, &glyph);
	if (error != 0)
		return error;

	/* Succeeded: reports its advance. */
	*advance = glyph.advance;
	return 0;
}

/* Packs a font and a code point into a cache key. */
static uint64_t
font_key(
	const struct text_font *font,
	uint32_t code_point)
{
	uint64_t key;

	/* The code point, the size in 1/64 pixels, the bold flag and the face, in separate bits. */
	key = code_point;
	key |= (uint64_t)(uint32_t)(font->size * 64.0f + 0.5f) << 21;
	key |= (uint64_t)(font->bold & 1) << 40;
	key |= (uint64_t)font->face << 41;

	/* Reports the key. */
	return key;
}

/* Finds the cache slot of a key: the one holding it, or the empty one where it goes. */
static struct text_glyph_entry **
font_slot(
	struct text_system *system,
	uint64_t key)
{
	size_t slot;

	/* Probes linearly from the key's hash. */
	slot = (size_t)((key * 0x9e3779b97f4a7c15ULL) >> 20) & (system->cache_capacity - 1U);
	while (system->cache[slot] != NULL) {
		if (system->cache[slot]->key == key)
			return &system->cache[slot];

		/* The next slot of the probe. */
		slot = (slot + 1U) & (system->cache_capacity - 1U);
	}

	/* The empty slot where the key goes. */
	return &system->cache[slot];
}

/* Doubles the cache's table and moves every entry. */
static int
font_grow(
	struct text_system *system)
{
	struct text_glyph_entry **old;
	struct text_glyph_entry **slot;
	size_t old_capacity;
	size_t index;

	/* Takes the larger, empty table. */
	old = system->cache;
	old_capacity = system->cache_capacity;
	system->cache_capacity = old_capacity * 2U;
	if (system->cache_capacity < FONT_CACHE_MIN)
		system->cache_capacity = FONT_CACHE_MIN;
	system->cache = calloc(system->cache_capacity, sizeof(*system->cache));
	if (system->cache == NULL) {
		system->cache = old;
		system->cache_capacity = old_capacity;
		return ENOMEM;
	}

	/* Moves the entries. */
	for (index = 0; index < old_capacity; index++) {
		if (old[index] == NULL)
			continue;

		/* Into the slot its key probes to. */
		slot = font_slot(system, old[index]->key);
		*slot = old[index];
	}

	/* The old table is no longer needed. */
	free(old);

	/* Succeeded: the table has room. */
	return 0;
}

/* Finds the face and index of a code point's glyph and measures its advance and box. */
static int
font_measure(
	struct text_system *system,
	const struct text_font *font,
	uint32_t code_point,
	struct text_glyph_entry *entry)
{
	struct truetype_design_metrics design;
	struct truetype_glyph measured;
	struct text_face *face;
	unsigned index;
	int design_advance;
	int face_index;
	int widened;
	int error;

	/* The font's own face, or the fallback when it lacks the glyph. */
	face_index = font->face;
	index = truetype_glyph_index(system->faces[face_index].face, code_point);

	/* A no-break space a face lacks is its space (a fallback face would change the line's height). */
	if (index == 0 && code_point == 0xa0U)
		index = truetype_glyph_index(system->faces[face_index].face, 0x20U);
	if (index == 0 && system->faces[TEXT_FACE_FALLBACK].open) {
		index = truetype_glyph_index(system->faces[TEXT_FACE_FALLBACK].face, code_point);
		if (index != 0)
			face_index = TEXT_FACE_FALLBACK;
	}

	/* Signs neither face has (the ellipsis, arrows) come from the monospaced face when it has them. */
	if (index == 0 && face_index != TEXT_FACE_MONO && system->faces[TEXT_FACE_MONO].open) {
		index = truetype_glyph_index(system->faces[TEXT_FACE_MONO].face, code_point);
		if (index != 0)
			face_index = TEXT_FACE_MONO;
	}

	/*
	 * A bold glyph its face's bold companion has (Mahora Bold)
	 * is drawn from it as it is; any other is widened by a pixel here.
	 */
	face = &system->faces[face_index];
	entry->bold_face = 0;
	if (font->bold)
		entry->bold_face = truetype_glyph_bold_face(face->face, index);
	error = truetype_set_bold(face->face, entry->bold_face);
	if (error != 0)
		return error;

	/* Measures the glyph at the size (the missing glyph when neither face has it). */
	error = truetype_set_pixel_size(face->face, font->pixels);
	if (error != 0)
		return error;
	error = truetype_glyph_metrics(face->face, index, &measured);
	if (error != 0)
		return error;

	/* The advance at the fractional size, from the design units. */
	error = truetype_design_metrics(face->face, &design);
	if (error != 0)
		return error;
	error = truetype_glyph_design_advance(face->face, index, &design_advance);
	if (error != 0)
		return error;
	entry->glyph.advance_units = (int32_t)((float)design_advance * font->size * 64.0f / (float)design.units_per_em + 0.5f);
	widened = 0;
	if (font->bold && !entry->bold_face)
		widened = 1;
	entry->glyph.advance_units += widened * 64;

	/* Records it; a widened bold glyph is a pixel wider. */
	entry->glyph.face = face_index;
	entry->glyph.index = index;
	entry->glyph.advance = measured.advance + widened;
	entry->glyph.width = (int)measured.width;
	entry->glyph.height = (int)measured.height;
	entry->glyph.left = measured.left;
	entry->glyph.top = measured.top;
	entry->glyph.bitmap = NULL;

	/* Succeeded: the entry is measured. */
	return 0;
}

/* Draws a measured glyph's coverage bitmap, widened by a pixel when bold. */
static int
font_draw(
	struct text_system *system,
	const struct text_font *font,
	struct text_glyph_entry *entry)
{
	struct truetype_glyph measured;
	struct text_face *face;
	uint8_t *pixels;
	size_t size;
	int width;
	int height;
	int x;
	int y;
	int error;

	/* An empty glyph (a space) has no pixels. */
	entry->drawn = 1;
	width = entry->glyph.width;
	height = entry->glyph.height;
	if (width <= 0 || height <= 0)
		return 0;

	/* Draws the glyph into a buffer one pixel wider than it, for the bold widening. */
	size = (size_t)(width + 1) * (size_t)height;
	pixels = calloc(size, 1);
	if (pixels == NULL)
		return ENOMEM;
	face = &system->faces[entry->glyph.face];
	error = truetype_set_bold(face->face, entry->bold_face);
	if (error == 0)
		error = truetype_set_pixel_size(face->face, font->pixels);
	if (error == 0)
		error = truetype_render_glyph(face->face, entry->glyph.index, &measured, pixels, (size_t)(width + 1), size);
	if (error != 0) {
		free(pixels);
		return error;
	}

	/* Bold widens every row by a pixel (not a glyph of the bold face): each pixel is the stronger of itself and its left neighbour. */
	if (font->bold && !entry->bold_face) {
		for (y = 0; y < height; y++) {
			for (x = width; x > 0; x--) {
				if (pixels[(size_t)y * (size_t)(width + 1) + (size_t)(x - 1)] > pixels[(size_t)y * (size_t)(width + 1) + (size_t)x])
					pixels[(size_t)y * (size_t)(width + 1) + (size_t)x] = pixels[(size_t)y * (size_t)(width + 1) + (size_t)(x - 1)];
			}
		}
	}

	/* Publishes the bitmap: the stride is the width plus one, which the glyph's width now is. */
	entry->pixels = pixels;
	entry->glyph.width = width + 1;
	entry->glyph.bitmap = pixels;

	/* Succeeded: the glyph has its bitmap. */
	return 0;
}

/*
 * Finds a table of a font file by its tag in the table directory, when it
 * lies inside the file with at least length bytes; NULL otherwise.
 */
static const unsigned char *
font_table(
	const struct wb_buffer *data,
	const char *tag,
	size_t length)
{
	const unsigned char *bytes;
	const unsigned char *record;
	size_t count;
	size_t index;
	size_t offset;
	size_t size;
	int differs;

	/* The directory's count of tables, after the four byte version. */
	bytes = data->data;
	if (data->length < 12U)
		return NULL;
	count = ((size_t)bytes[4] << 8) | (size_t)bytes[5];

	/* Each record: the tag, a checksum, the offset and the length. */
	for (index = 0; index < count; index++) {
		record = bytes + 12U + index * 16U;
		if (12U + (index + 1U) * 16U > data->length)
			return NULL;
		differs = memcmp(record, tag, 4U);
		if (differs != 0)
			continue;

		/* The table, when it is inside the file and long enough. */
		offset = ((size_t)record[8] << 24) | ((size_t)record[9] << 16) | ((size_t)record[10] << 8) | (size_t)record[11];
		size = ((size_t)record[12] << 24) | ((size_t)record[13] << 16) | ((size_t)record[14] << 8) | (size_t)record[15];
		if (size < length)
			return NULL;
		if (offset > data->length)
			return NULL;
		if (data->length - offset < length)
			return NULL;

		/* The table's first byte. */
		return bytes + offset;
	}

	/* The file has no such table. */
	return NULL;
}

/* Reads a font file and opens its first face. */
static int
font_open_face(
	struct text_face *face,
	const char *path)
{
	int error;

	/* Reads the file. */
	wb_buffer_init(&face->data);
	error = wb_file_read(path, &face->data);
	if (error != 0) {
		wb_buffer_release(&face->data);
		return error;
	}

	/* Opens the face; the data stays with it. */
	error = truetype_open(face->data.data, face->data.length, 0, &face->face);
	if (error != 0) {
		wb_buffer_release(&face->data);
		return error;
	}

	/* Succeeded: the face is open. */
	face->open = 1;
	return 0;
}
