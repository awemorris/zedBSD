/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Opening a TrueType face and finding its tables.
 *
 * A TrueType file starts with a directory naming every table and where it
 * lies.  Only five are needed to draw text: head for the units per em and
 * the index format, maxp for the glyph count, hhea and hmtx for the advance
 * widths, loca and glyf for the outlines, and cmap for the character map.
 *
 * Every table is range-checked once here, so the readers below may index
 * within a table knowing it is inside the file.  A font is a file from
 * somewhere else; nothing in it is trusted.
 */

#include "internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static int find_table(struct truetype_face *face, uint32_t tag,
		      const uint8_t **table, uint32_t *size);
static int read_head(struct truetype_face *face);
static int read_maxp(struct truetype_face *face);
static int read_hhea(struct truetype_face *face);
static int open_face(const void *data, size_t size, unsigned index, int map_required, struct truetype_face **result);
static int read_map(struct truetype_face *face, int map_required);
static void read_color(struct truetype_face *face);

/*
 * Reads one big-endian value.  A font is big-endian whatever the machine is.
 */
uint16_t
truetype_u16(
	const uint8_t *bytes)
{
	return (uint16_t)(((uint16_t)bytes[0] << 8) | bytes[1]);
}

int16_t
truetype_s16(
	const uint8_t *bytes)
{
	return (int16_t)truetype_u16(bytes);
}

uint32_t
truetype_u32(
	const uint8_t *bytes)
{
	return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) |
		((uint32_t)bytes[2] << 8) | bytes[3];
}

/*
 * Finds one table in the directory and checks that it lies inside the file.
 */
static int
find_table(
	struct truetype_face *face,
	uint32_t tag,
	const uint8_t **table,
	uint32_t *size)
{
	const uint8_t *entry;
	uint32_t offset, length;
	unsigned index;

	/* Process each table directory entry. */
	for (index = 0; index < face->table_count; index++) {
		entry = face->directory + (size_t)index * 16U;

		/* Skips an entry naming another table. */
		if (truetype_u32(entry) != tag)
			continue;
		offset = truetype_u32(entry + 8);
		length = truetype_u32(entry + 12);

		/* Handles a table the file does not hold. */
		if (offset > face->size || length > face->size - offset)
			return EINVAL;
		*table = face->data + offset;
		*size = length;

		/* Succeeded: the table lies inside the file. */
		return 0;
	}

	/* Failed: this font has no such table. */
	return ENOENT;
}

/* Reads the units per em and the index-to-location format. */
static int
read_head(
	struct truetype_face *face)
{
	const uint8_t *head;
	uint32_t size;
	int error;

	error = find_table(face, TRUETYPE_TAG('h', 'e', 'a', 'd'), &head, &size);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Handles a head table too short to hold what is read from it. */
	if (size < 54U)
		return EINVAL;
	face->units_per_em = truetype_u16(head + 18);

	/* Handles the scale that every measurement is divided by. */
	if (face->units_per_em == 0)
		return EINVAL;
	face->long_loca = truetype_u16(head + 50) != 0;

	/* Succeeded. */
	return 0;
}

/* Reads how many glyphs the font has. */
static int
read_maxp(
	struct truetype_face *face)
{
	const uint8_t *maxp;
	uint32_t size;
	int error;

	error = find_table(face, TRUETYPE_TAG('m', 'a', 'x', 'p'), &maxp, &size);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Handles a maxp table too short to hold the glyph count. */
	if (size < 6U)
		return EINVAL;
	face->glyph_count = truetype_u16(maxp + 4);

	/* Succeeded. */
	return 0;
}

/* Reads the vertical measurements and the advance-width count. */
static int
read_hhea(
	struct truetype_face *face)
{
	const uint8_t *hhea;
	uint32_t size;
	int error;

	error = find_table(face, TRUETYPE_TAG('h', 'h', 'e', 'a'), &hhea, &size);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Handles an hhea table too short to hold what is read from it. */
	if (size < 36U)
		return EINVAL;
	face->ascent = truetype_s16(hhea + 4);
	face->descent = truetype_s16(hhea + 6);
	face->line_gap = truetype_s16(hhea + 8);
	face->hmetric_count = truetype_u16(hhea + 34);

	/* Succeeded. */
	return 0;
}

/*
 * Implements the truetype open operation.
 */
int
truetype_open(
	const void *data,
	size_t size,
	unsigned index,
	struct truetype_face **result)
{
	int error;

	/* Opens the face, which must map characters to its glyphs. */
	error = open_face(data, size, index, 1, result);
	if (error != 0)
		return error;

	/* Succeeded: the caller owns the face. */
	return 0;
}

/*
 * Opens a face embedded in a document, whose character map is optional.
 *
 * A font a document carries is often cut down to the glyphs the document
 * uses and addressed by glyph number, and such a font may have no cmap or
 * only a map this reader does not choose.  The face opens all the same;
 * truetype_glyph_index() then finds no glyph and truetype_cmap_lookup()
 * reads whatever maps there are.
 */
int
truetype_open_embedded(
	const void *data,
	size_t size,
	struct truetype_face **result)
{
	int error;

	/* Opens the first face without requiring a character map. */
	error = open_face(data, size, 0, 0, result);
	if (error != 0)
		return error;

	/* Succeeded: the caller owns the face. */
	return 0;
}

/*
 * Opens one face of a font file; map_required refuses a face without a
 * Unicode character map this reader uses.
 */
static int
open_face(
	const void *data,
	size_t size,
	unsigned index,
	int map_required,
	struct truetype_face **result)
{
	struct truetype_face *face;
	const uint8_t *bytes;
	uint32_t version, directory, count;
	int error;

	/* Validates the arguments. */
	if (data == NULL || result == NULL)
		return EINVAL;
	bytes = data;
	*result = NULL;

	/* Handles a file too short to hold even the offset table. */
	if (size < 12U)
		return EINVAL;
	version = truetype_u32(bytes);
	directory = 12U;

	/*
	 * A collection holds several faces, each with its own directory; an
	 * ordinary font is one face and index must be zero for it.
	 */
	if (version == TRUETYPE_TAG('t', 't', 'c', 'f')) {
		/* Handles a header too short to hold the face count. */
		if (size < 16U)
			return EINVAL;
		count = truetype_u32(bytes + 8);

		/* Handles a face this collection does not hold. */
		if (index >= count || count > (size - 12U) / 4U)
			return EINVAL;
		directory = truetype_u32(bytes + 12 + (size_t)index * 4U);

		/* Handles a directory the file does not hold. */
		if (directory > size - 12U)
			return EINVAL;
		version = truetype_u32(bytes + directory);
		directory += 12U;
	} else if (index != 0) {
		/* Failed: an ordinary font has one face. */
		return EINVAL;
	}

	/*
	 * 0x00010000 is the original outline format and "true" is the same
	 * thing as Apple spelled it.  "OTTO" holds CFF outlines, which this
	 * does not read.
	 */
	if (version != 0x00010000U && version != TRUETYPE_TAG('t', 'r', 'u', 'e'))
		return ENOTSUP;
	count = truetype_u16(bytes + directory - 8);

	/* Handles a directory the file does not hold. */
	if (count > (size - directory) / 16U)
		return EINVAL;
	face = calloc(1, sizeof(*face));

	/* Handles the allocation failure. */
	if (face == NULL)
		return ENOMEM;
	face->data = bytes;
	face->size = size;
	face->directory = bytes + directory;
	face->table_count = count;
	error = read_head(face);

	/* Handles the head table failure. */
	if (error == 0)
		error = read_maxp(face);

	/* Handles the maxp table failure. */
	if (error == 0)
		error = read_hhea(face);

	/* Handles the hhea table failure. */
	if (error == 0)
		error = find_table(face, TRUETYPE_TAG('h', 'm', 't', 'x'),
				   &face->hmtx, &face->hmtx_size);

	/* The colour bitmaps, when the face has both of their tables (ws102-p019). */
	if (error == 0)
		read_color(face);

	/* Handles the hmtx table failure. */
	if (error == 0) {
		error = find_table(face, TRUETYPE_TAG('l', 'o', 'c', 'a'),
				   &face->loca, &face->loca_size);

		/* Handles the loca table failure. */
		if (error == 0)
			error = find_table(face, TRUETYPE_TAG('g', 'l', 'y', 'f'),
					   &face->glyf, &face->glyf_size);

		/* A face of colour bitmaps alone has no outlines: its glyphs are only its colour ones. */
		if (error != 0 && face->cblc != NULL) {
			face->loca = NULL;
			face->loca_size = 0;
			face->glyf = NULL;
			face->glyf_size = 0;
			error = 0;
		}
	}

	/* Handles the glyf table failure. */
	if (error == 0)
		error = read_map(face, map_required);

	/* Reports the failure. */
	if (error != 0) {
		free(face);
		return error;
	}

	/* The size a caller did not choose still has to draw something. */
	face->pixels = 16U;

	/* Succeeded: the face reads every table it needs. */
	*result = face;
	return 0;
}

/*
 * Finds the character map and chooses its Unicode subtable.
 *
 * Without map_required, a face that has no cmap, or none this reader
 * chooses, is left without a chosen subtable instead of refused.
 */
static int
read_map(
	struct truetype_face *face,
	int map_required)
{
	int error;

	/* Finds the cmap table. */
	error = find_table(face, TRUETYPE_TAG('c', 'm', 'a', 'p'),
			   &face->cmap, &face->cmap_size);
	if (error == ENOENT && !map_required) {
		/* An embedded face without a map is addressed by glyph number. */
		face->cmap = NULL;
		face->cmap_size = 0;
		return 0;
	}

	/* Reports any other failure. */
	if (error != 0)
		return error;

	/* Chooses the Unicode subtable. */
	error = truetype_cmap_select(face);
	if (error != 0 && !map_required) {
		/* The other subtables stay readable through truetype_cmap_lookup(). */
		face->cmap_subtable = NULL;
		return 0;
	}

	/* A face that must map characters and cannot is refused. */
	if (error != 0)
		return error;

	/* Succeeded: the face maps characters to glyphs. */
	return 0;
}

/*
 * Implements the truetype close operation.
 */
void
truetype_close(
	struct truetype_face *face)
{
	/* Nothing to close. */
	if (face == NULL)
		return;

	/* The companions first (companion.c), then the bytes the face read itself, then the face. */
	truetype_close(face->bold_face);
	truetype_close(face->next);
	free(face->owned);
	free(face);
}

/*
 * Implements the truetype set pixel size operation.
 */
int
truetype_set_pixel_size(
	struct truetype_face *face,
	unsigned pixels)
{
	/* Validates the arguments. */
	if (face == NULL || pixels == 0 || pixels > TRUETYPE_PIXELS_MAX)
		return EINVAL;
	face->pixels = pixels;

	/* The companions draw at the face's size (companion.c). */
	if (face->bold_face != NULL)
		face->bold_face->pixels = pixels;
	if (face->next != NULL)
		(void)truetype_set_pixel_size(face->next, pixels);

	/* Succeeded. */
	return 0;
}

/*
 * Makes the face's later glyphs bold or regular.
 */
int
truetype_set_bold(
	struct truetype_face *face,
	int bold)
{
	/* Validates the arguments. */
	if (face == NULL)
		return EINVAL;

	/* The weight every later glyph is measured and drawn at. */
	face->bold = 0U;
	if (bold != 0)
		face->bold = 1U;

	/* The next companion draws at the face's weight; the bold one is bold already (companion.c). */
	if (face->next != NULL)
		(void)truetype_set_bold(face->next, bold);

	/* Succeeded: the weight is chosen. */
	return 0;
}

/*
 * Implements the truetype metrics operation.
 */
int
truetype_metrics(
	const struct truetype_face *face,
	struct truetype_metrics *metrics)
{
	/* Validates the arguments. */
	if (face == NULL || metrics == NULL)
		return EINVAL;

	/*
	 * The font measures in its own units; a size in pixels per em is the
	 * scale between them.  Rounding away from zero keeps a descent of a
	 * fraction of a pixel from becoming none at all.
	 */
	metrics->ascent = truetype_scale_up(face, face->ascent);
	metrics->descent = truetype_scale_down(face, face->descent);
	metrics->line_height = truetype_scale_up(
		face, face->ascent - face->descent + face->line_gap);

	/*
	 * Ascent and descent are each rounded outward, so together they can
	 * reach further than the rounded sum.  A caller stacking lines by
	 * line_height would then overlap the boxes this very call reported,
	 * which is why the spacing is never less than they need.
	 */
	if (metrics->line_height < metrics->ascent - metrics->descent)
		metrics->line_height = metrics->ascent - metrics->descent;

	/* Succeeded. */
	return 0;
}

/*
 * Finds the colour bitmaps' two tables (CBLC and CBDT, ws102-p019); a face
 * without both keeps neither.
 */
static void
read_color(
	struct truetype_face *face)
{
	int error;

	/* The index. */
	error = find_table(face, TRUETYPE_TAG('C', 'B', 'L', 'C'), &face->cblc, &face->cblc_size);
	if (error != 0) {
		face->cblc = NULL;
		face->cblc_size = 0;
		return;
	}

	/* The images, without which the index is of no use. */
	error = find_table(face, TRUETYPE_TAG('C', 'B', 'D', 'T'), &face->cbdt, &face->cbdt_size);
	if (error != 0) {
		face->cblc = NULL;
		face->cblc_size = 0;
		face->cbdt = NULL;
		face->cbdt_size = 0;
	}
}
