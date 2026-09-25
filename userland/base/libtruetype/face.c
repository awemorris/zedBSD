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

	/* Handles the hmtx table failure. */
	if (error == 0)
		error = find_table(face, TRUETYPE_TAG('l', 'o', 'c', 'a'),
				   &face->loca, &face->loca_size);

	/* Handles the loca table failure. */
	if (error == 0)
		error = find_table(face, TRUETYPE_TAG('g', 'l', 'y', 'f'),
				   &face->glyf, &face->glyf_size);

	/* Handles the glyf table failure. */
	if (error == 0)
		error = find_table(face, TRUETYPE_TAG('c', 'm', 'a', 'p'),
				   &face->cmap, &face->cmap_size);

	/* Handles the cmap table failure. */
	if (error == 0)
		error = truetype_cmap_select(face);

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
 * Implements the truetype close operation.
 */
void
truetype_close(
	struct truetype_face *face)
{
	/* Handles the face availability. */
	if (face == NULL)
		return;
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

	/* Succeeded. */
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
