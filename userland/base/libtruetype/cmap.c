/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The character map: which glyph a character is drawn with.
 *
 * A font carries several maps, one per platform and encoding.  Two formats
 * are read here, which together cover every font that maps Unicode:
 *
 *   format 4   the Basic Multilingual Plane, as ranges of characters
 *   format 12  the whole of Unicode, as groups of consecutive characters
 *
 * Format 12 is preferred where a font has both, because format 4 cannot
 * name a character above U+FFFF.
 */

#include "internal.h"

#include <errno.h>

static int subtable_usable(const struct truetype_face *face, uint32_t offset,
			   unsigned *format, uint32_t *length);
static unsigned lookup_format4(const struct truetype_face *face,
			       const uint8_t *table, uint32_t table_size,
			       uint32_t codepoint);
static unsigned lookup_format12(const struct truetype_face *face,
				const uint8_t *table, uint32_t table_size,
				uint32_t codepoint);
static unsigned lookup(const struct truetype_face *face, const uint8_t *table,
		       uint32_t table_size, unsigned format,
		       uint32_t codepoint);

/*
 * Reports whether one subtable is a format this reads and lies inside cmap.
 */
static int
subtable_usable(
	const struct truetype_face *face,
	uint32_t offset,
	unsigned *format,
	uint32_t *length)
{
	const uint8_t *subtable;

	/* Handles a subtable the table does not hold. */
	if (offset > face->cmap_size || face->cmap_size - offset < 4U)
		return 0;
	subtable = face->cmap + offset;
	*format = truetype_u16(subtable);

	/* Handles the selected format. */
	if (*format == 4U) {
		*length = truetype_u16(subtable + 2);
	} else if (*format == 12U) {
		/* Handles a header too short to hold the length. */
		if (face->cmap_size - offset < 16U)
			return 0;
		*length = truetype_u32(subtable + 4);
	} else {
		/* Failed: this reader does not know that format. */
		return 0;
	}

	/* Handles a subtable running past the end of the table. */
	if (*length < 4U || *length > face->cmap_size - offset)
		return 0;

	/* Succeeded. */
	return 1;
}

/*
 * Implements the truetype cmap select operation.
 *
 * Picks the best subtable the font offers.  Platform 3 encoding 10 is
 * Windows full Unicode, 3/1 is Windows BMP, and platform 0 is Unicode
 * proper; any of them may be the only one present.
 */
int
truetype_cmap_select(
	struct truetype_face *face)
{
	const uint8_t *record;
	uint32_t count, offset, length, best_offset, best_length;
	unsigned index, platform, encoding, format, best_format, best_rank, rank;

	/* Handles a cmap too short to hold its own count. */
	if (face->cmap_size < 4U)
		return EINVAL;
	count = truetype_u16(face->cmap + 2);

	/* Handles a record list the table does not hold. */
	if (count > (face->cmap_size - 4U) / 8U)
		return EINVAL;
	best_rank = 0;
	best_offset = 0;
	best_length = 0;
	best_format = 0;

	/* Process each encoding record. */
	for (index = 0; index < count; index++) {
		record = face->cmap + 4U + (size_t)index * 8U;
		platform = truetype_u16(record);
		encoding = truetype_u16(record + 2);
		offset = truetype_u32(record + 4);

		/* Skips a subtable this reader cannot use. */
		if (!subtable_usable(face, offset, &format, &length))
			continue;

		/*
		 * A map of the whole of Unicode beats one of the Basic
		 * Multilingual Plane, and a Unicode map beats a Macintosh one.
		 */
		if (platform == 3U && encoding == 10U)
			rank = 4U;
		else if (platform == 0U && format == 12U)
			rank = 4U;
		else if (platform == 3U && encoding == 1U)
			rank = 3U;
		else if (platform == 0U)
			rank = 2U;
		else
			rank = 1U;

		/* Keeps the first subtable of the best rank seen. */
		if (rank <= best_rank)
			continue;
		best_rank = rank;
		best_offset = offset;
		best_length = length;
		best_format = format;
	}

	/* Handles a font with no map this reader can use. */
	if (best_rank == 0)
		return ENOTSUP;
	face->cmap_subtable = face->cmap + best_offset;
	face->cmap_subtable_size = best_length;
	face->cmap_format = best_format;

	/* Succeeded. */
	return 0;
}

/* Asks one subtable, whichever format it is. */
static unsigned
lookup(
	const struct truetype_face *face,
	const uint8_t *table,
	uint32_t table_size,
	unsigned format,
	uint32_t codepoint)
{
	/* Handles the selected subtable format. */
	if (format == 12U)
		return lookup_format12(face, table, table_size, codepoint);

	/* Returns the computed result. */
	return lookup_format4(face, table, table_size, codepoint);
}

/*
 * Looks one character up in a format 4 subtable.
 *
 * The map is a list of character ranges in increasing order, each with
 * either a constant to add to the character or an index into a glyph array.
 */
static unsigned
lookup_format4(
	const struct truetype_face *face,
	const uint8_t *table_base,
	uint32_t table_size,
	uint32_t codepoint)
{
	const uint8_t *table, *ends, *starts, *deltas, *ranges;
	uint32_t segments, index, start, end, offset, position;
	unsigned glyph;

	/* Handles a character this format cannot name. */
	if (codepoint > 0xffffU)
		return 0;
	table = table_base;

	/* Handles a subtable too short to hold its own header. */
	if (table_size < 14U)
		return 0;
	segments = truetype_u16(table + 6) / 2U;

	/* Handles a segment list the subtable does not hold. */
	if (segments == 0 ||
	    (uint32_t)segments * 8U + 16U > table_size)
		return 0;
	ends = table + 14;
	starts = ends + (size_t)segments * 2U + 2U;
	deltas = starts + (size_t)segments * 2U;
	ranges = deltas + (size_t)segments * 2U;

	/* Process each segment in order. */
	for (index = 0; index < segments; index++) {
		end = truetype_u16(ends + (size_t)index * 2U);

		/* Skips a segment ending before this character. */
		if (end < codepoint)
			continue;
		start = truetype_u16(starts + (size_t)index * 2U);

		/* Handles a character in the gap before this segment. */
		if (start > codepoint)
			return 0;
		offset = truetype_u16(ranges + (size_t)index * 2U);

		/* Handles the segment that names its glyphs by adding. */
		if (offset == 0) {
			glyph = (unsigned)((codepoint +
				truetype_u16(deltas + (size_t)index * 2U)) &
				0xffffU);

			/* Returns the computed result. */
			return glyph < face->glyph_count ? glyph : 0;
		}

		/*
		 * Otherwise the offset is counted from the entry itself into
		 * a glyph array that follows the segments.
		 */
		position = (uint32_t)((ranges + (size_t)index * 2U) -
			   table_base) + offset + (codepoint - start) * 2U;

		/* Handles an entry the subtable does not hold. */
		if (position + 2U > table_size)
			return 0;
		glyph = truetype_u16(table_base + position);

		/* Handles the absent glyph, which is not shifted by the delta. */
		if (glyph == 0)
			return 0;
		glyph = (unsigned)((glyph +
			truetype_u16(deltas + (size_t)index * 2U)) & 0xffffU);

		/* Returns the computed result. */
		return glyph < face->glyph_count ? glyph : 0;
	}

	/* Failed: this character is past the last segment. */
	return 0;
}

/*
 * Looks one character up in a format 12 subtable.
 *
 * The map is groups of consecutive characters in increasing order, each
 * naming the glyph its first character uses.
 */
static unsigned
lookup_format12(
	const struct truetype_face *face,
	const uint8_t *table,
	uint32_t table_size,
	uint32_t codepoint)
{
	const uint8_t *group;
	uint32_t count, low, high, middle, start, end, first;
	unsigned glyph;

	/* Handles a subtable too short to hold its own count. */
	if (table_size < 16U)
		return 0;
	count = truetype_u32(table + 12);

	/* Handles a group list the subtable does not hold. */
	if (count > (table_size - 16U) / 12U)
		return 0;
	low = 0;
	high = count;

	/* The groups are ordered, so the search halves the range each time. */
	while (low < high) {
		middle = low + (high - low) / 2U;
		group = table + 16U + (size_t)middle * 12U;
		start = truetype_u32(group);
		end = truetype_u32(group + 4);

		/* Handles a group ending before this character. */
		if (codepoint > end) {
			low = middle + 1U;
			continue;
		}

		/* Handles a group starting after this character. */
		if (codepoint < start) {
			high = middle;
			continue;
		}
		first = truetype_u32(group + 8);
		glyph = (unsigned)(first + (codepoint - start));

		/* Returns the computed result. */
		return glyph < face->glyph_count ? glyph : 0;
	}

	/* Failed: no group holds this character. */
	return 0;
}

/*
 * Implements the truetype glyph index operation.
 */
unsigned
truetype_glyph_index(
	const struct truetype_face *face,
	uint32_t codepoint)
{
	/* Validates the arguments. */
	if (face == NULL || face->cmap_subtable == NULL)
		return 0;

	/* Returns the computed result. */
	return lookup(face, face->cmap_subtable, face->cmap_subtable_size,
		      face->cmap_format, codepoint);
}
