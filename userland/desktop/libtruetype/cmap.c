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
static int subtable_any(const struct truetype_face *face, uint32_t offset, unsigned *format, uint32_t *length);
static unsigned lookup_format0(const struct truetype_face *face, const uint8_t *table, uint32_t table_size, uint32_t code);
static unsigned lookup_format6(const struct truetype_face *face, const uint8_t *table, uint32_t table_size, uint32_t code);

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
	if (format == 0U)
		return lookup_format0(face, table, table_size, codepoint);
	if (format == 6U)
		return lookup_format6(face, table, table_size, codepoint);

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
	unsigned glyph;
	unsigned other;

	/* Validates the arguments. */
	if (face == NULL || face->cmap_subtable == NULL)
		return 0;

	/* The face's own glyph. */
	glyph = lookup(face, face->cmap_subtable, face->cmap_subtable_size,
		       face->cmap_format, codepoint);
	if (glyph != 0U || face->next == NULL)
		return glyph;

	/* The next companion's, numbered after the face's own (companion.c). */
	other = truetype_glyph_index(face->next, codepoint);
	if (other == 0U)
		return 0;

	/* Succeeded: the companion's glyph. */
	return (unsigned)face->glyph_count + other;
}

/*
 * Looks a code up in the subtable of one platform and encoding.
 *
 * A document's font is often addressed through a map other than the
 * Unicode one truetype_glyph_index() reads: the Windows symbol map (3, 0)
 * or the Macintosh Roman map (1, 0), in the byte formats 0 and 6 as well as
 * 4 and 12.  ENOENT means the font has no such subtable in a format this
 * reads; otherwise *glyph is the glyph, 0 when the code is not mapped.
 */
int
truetype_cmap_lookup(
	const struct truetype_face *face,
	unsigned platform,
	unsigned encoding,
	uint32_t code,
	unsigned *glyph)
{
	const uint8_t *record;
	uint32_t count;
	uint32_t offset;
	uint32_t length;
	unsigned index;
	unsigned format;
	unsigned record_platform;
	unsigned record_encoding;
	int usable;

	/* Refuses a missing face or answer. */
	if (face == NULL)
		return EINVAL;
	if (glyph == NULL)
		return EINVAL;
	*glyph = 0;

	/* A face without a map has no subtable. */
	if (face->cmap == NULL)
		return ENOENT;
	if (face->cmap_size < 4U)
		return ENOENT;
	count = truetype_u16(face->cmap + 2);

	/* Refuses a record list the table does not hold. */
	if (count > (face->cmap_size - 4U) / 8U)
		return ENOENT;

	/* Finds the first usable subtable of the platform and encoding. */
	for (index = 0; index < count; index++) {
		record = face->cmap + 4U + (size_t)index * 8U;

		/* Skips a record of another platform or encoding. */
		record_platform = truetype_u16(record);
		if (record_platform != platform)
			continue;
		record_encoding = truetype_u16(record + 2);
		if (record_encoding != encoding)
			continue;

		/* Skips a subtable this reader cannot use. */
		offset = truetype_u32(record + 4);
		usable = subtable_any(face, offset, &format, &length);
		if (!usable)
			continue;

		/* Succeeded: the subtable's answer. */
		*glyph = lookup(face, face->cmap + offset, length, format, code);
		return 0;
	}

	/* The font has no such subtable. */
	return ENOENT;
}

/*
 * Reports whether a subtable is in any format this reads (0, 4, 6, 12) and
 * lies inside cmap.
 */
static int
subtable_any(
	const struct truetype_face *face,
	uint32_t offset,
	unsigned *format,
	uint32_t *length)
{
	const uint8_t *subtable;
	int usable;

	/* Formats 4 and 12 are the ones the Unicode choice reads. */
	usable = subtable_usable(face, offset, format, length);
	if (usable)
		return 1;

	/* Refuses a subtable the table does not hold. */
	if (offset > face->cmap_size || face->cmap_size - offset < 6U)
		return 0;
	subtable = face->cmap + offset;
	*format = truetype_u16(subtable);

	/* Formats 0 and 6 keep a 16-bit length after the format. */
	if (*format != 0U && *format != 6U)
		return 0;
	*length = truetype_u16(subtable + 2);

	/* Refuses a subtable running past the end of the table. */
	if (*length < 6U || *length > face->cmap_size - offset)
		return 0;

	/* Succeeded: the subtable can be read. */
	return 1;
}

/*
 * Looks one byte code up in a format 0 subtable: 256 glyph numbers of one
 * byte each.
 */
static unsigned
lookup_format0(
	const struct truetype_face *face,
	const uint8_t *table,
	uint32_t table_size,
	uint32_t code)
{
	unsigned glyph;

	/* A code past one byte, or past the array, is not mapped. */
	if (code > 255U)
		return 0;
	if (6U + code >= table_size)
		return 0;
	glyph = table[6U + code];

	/* A glyph the face does not have is not mapped. */
	if (glyph >= face->glyph_count)
		return 0;

	/* Succeeded: the code's glyph. */
	return glyph;
}

/*
 * Looks one code up in a format 6 subtable: a run of 16-bit glyph numbers
 * for consecutive codes from a first code.
 */
static unsigned
lookup_format6(
	const struct truetype_face *face,
	const uint8_t *table,
	uint32_t table_size,
	uint32_t code)
{
	uint32_t first;
	uint32_t count;
	uint32_t position;
	unsigned glyph;

	/* Refuses a subtable too short for its header. */
	if (table_size < 10U)
		return 0;
	first = truetype_u16(table + 6);
	count = truetype_u16(table + 8);

	/* A code outside the run is not mapped. */
	if (code < first)
		return 0;
	if (code - first >= count)
		return 0;

	/* Refuses an entry the subtable does not hold. */
	position = 10U + (code - first) * 2U;
	if (position + 2U > table_size)
		return 0;
	glyph = truetype_u16(table + position);

	/* A glyph the face does not have is not mapped. */
	if (glyph >= face->glyph_count)
		return 0;

	/* Succeeded: the code's glyph. */
	return glyph;
}
