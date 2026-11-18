/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Colour glyphs stored as bitmaps (ws102-p019): the CBLC table indexes,
 * for each size ("strike") the font has, where each glyph's image lies in
 * the CBDT table, and CBDT holds the images as PNG files with their
 * metrics.  Colour emoji fonts (Noto Color Emoji) are made this way.
 *
 * This finds a glyph's PNG and its metrics at the strike nearest the size
 * asked for; decoding and scaling the image is the caller's (it has a PNG
 * reader, this library has none).  The index formats 1 to 5 and the image
 * formats 17, 18 and 19 (PNG with small, big and shared metrics) are read.
 */

#include "internal.h"

#include <errno.h>
#include <string.h>

/* The sizes of a strike's record, an index array's entry, and the two kinds of metrics. */
#define COLOR_STRIKE_BYTES	48U
#define COLOR_ARRAY_BYTES	8U
#define COLOR_SMALL_METRICS	5U
#define COLOR_BIG_METRICS	8U

static int color_strike(const struct truetype_face *face, unsigned glyph, const uint8_t **strike);
static int color_locate(const struct truetype_face *face, const uint8_t *strike, unsigned glyph, uint32_t *offset, uint32_t *length, unsigned *format, const uint8_t **metrics);
static int color_image(const struct truetype_face *face, uint32_t offset, uint32_t length, unsigned format, const uint8_t *shared, unsigned ppem, struct truetype_color_glyph *out);
static void color_big_metrics(const uint8_t *metrics, struct truetype_color_glyph *out);

/*
 * Finds a glyph's colour image: its PNG bytes (inside the font's memory),
 * its size and place at the strike's size, and that size.  The strike is
 * the smallest at least as large as the face's pixel size, else the
 * largest.  Returns 0, ENOENT for a face or a glyph without a colour
 * image, or EINVAL for a damaged table.
 */
int
truetype_color_glyph(
	const struct truetype_face *face,
	unsigned glyph,
	struct truetype_color_glyph *out)
{
	const uint8_t *strike;
	const uint8_t *shared;
	uint32_t offset;
	uint32_t length;
	unsigned format;
	int error;

	/* A face without colour bitmaps. */
	if (face == NULL || out == NULL)
		return EINVAL;
	memset(out, 0, sizeof(*out));

	/* The face among its companions that has the glyph (companion.c). */
	face = truetype_resolve_const(face, &glyph);
	if (face->cblc == NULL || face->cbdt == NULL)
		return ENOENT;

	/* The strike that has the glyph, nearest the size. */
	error = color_strike(face, glyph, &strike);
	if (error != 0)
		return error;

	/* Where its image is in CBDT, in which format. */
	error = color_locate(face, strike, glyph, &offset, &length, &format, &shared);
	if (error != 0)
		return error;

	/* Succeeded when the image is a PNG this reads. */
	error = color_image(face, offset, length, format, shared, strike[45], out);
	return error;
}

/* Chooses the strike for the face's size among those whose glyph range has the glyph; ENOENT for none. */
static int
color_strike(
	const struct truetype_face *face,
	unsigned glyph,
	const uint8_t **strike)
{
	const uint8_t *record;
	const uint8_t *best;
	uint32_t count;
	uint32_t index;
	unsigned ppem;
	unsigned best_ppem;
	unsigned first;
	unsigned last;

	/* The header: the version and the number of strikes, which must fit the table. */
	if (face->cblc_size < 8U)
		return EINVAL;
	count = truetype_u32(face->cblc + 4);
	if (count > (face->cblc_size - 8U) / COLOR_STRIKE_BYTES)
		return EINVAL;

	/* Each strike that covers the glyph: the smallest at least the size, else the largest. */
	best = NULL;
	best_ppem = 0;
	for (index = 0; index < count; index++) {
		record = face->cblc + 8U + (size_t)index * COLOR_STRIKE_BYTES;
		first = truetype_u16(record + 40);
		last = truetype_u16(record + 42);
		ppem = record[45];
		if (glyph < first || glyph > last || ppem == 0U)
			continue;

		/* The first found, a closer one at or above the size, or a larger one while none reaches it. */
		if (best == NULL ||
		    (ppem >= face->pixels && (best_ppem < face->pixels || ppem < best_ppem)) ||
		    (best_ppem < face->pixels && ppem > best_ppem)) {
			best = record;
			best_ppem = ppem;
		}
	}

	/* No strike has the glyph. */
	if (best == NULL)
		return ENOENT;

	/* Succeeded: the strike. */
	*strike = best;
	return 0;
}

/*
 * Finds a glyph's image in a strike's index subtables: its offset and
 * length in CBDT, its image format, and the metrics the index keeps for
 * every glyph of a subtable (formats 2 and 5), or NULL.
 */
static int
color_locate(
	const struct truetype_face *face,
	const uint8_t *strike,
	unsigned glyph,
	uint32_t *offset,
	uint32_t *length,
	unsigned *format,
	const uint8_t **metrics)
{
	const uint8_t *array;
	const uint8_t *table;
	uint32_t array_offset;
	uint32_t tables;
	uint32_t table_offset;
	uint32_t image_offset;
	uint32_t size;
	uint32_t count;
	uint32_t index;
	uint32_t start;
	uint32_t end;
	unsigned first;
	unsigned last;
	unsigned index_format;
	unsigned listed;
	unsigned entry;

	/* The strike's array of index subtables, which must lie inside CBLC. */
	array_offset = truetype_u32(strike);
	tables = truetype_u32(strike + 8);
	if (array_offset > face->cblc_size || tables > (face->cblc_size - array_offset) / COLOR_ARRAY_BYTES)
		return EINVAL;
	array = face->cblc + array_offset;

	/* The subtable whose range has the glyph. */
	for (index = 0; index < tables; index++) {
		first = truetype_u16(array + (size_t)index * COLOR_ARRAY_BYTES);
		last = truetype_u16(array + (size_t)index * COLOR_ARRAY_BYTES + 2U);
		if (glyph >= first && glyph <= last)
			break;
	}

	/* No subtable has it. */
	if (index == tables)
		return ENOENT;

	/* The subtable's header: its format, the images' format and where they start in CBDT. */
	table_offset = array_offset + truetype_u32(array + (size_t)index * COLOR_ARRAY_BYTES + 4U);
	if (table_offset > face->cblc_size || face->cblc_size - table_offset < 8U)
		return EINVAL;
	table = face->cblc + table_offset;
	size = face->cblc_size - table_offset;
	index_format = truetype_u16(table);
	*format = truetype_u16(table + 2);
	image_offset = truetype_u32(table + 4);
	*metrics = NULL;

	/* Each index format. */
	switch (index_format) {
	case 1:
	case 3:
		/* Offsets for first .. last + 1: four bytes each (1), or two (3). */
		count = last - first + 2U;
		entry = 2U;
		if (index_format == 1U)
			entry = 4U;
		if (size < 8U + count * entry)
			return EINVAL;

		/* The glyph's offset and the next one's. */
		if (index_format == 1U) {
			start = truetype_u32(table + 8U + (glyph - first) * 4U);
			end = truetype_u32(table + 8U + (glyph - first + 1U) * 4U);
		} else {
			start = truetype_u16(table + 8U + (glyph - first) * 2U);
			end = truetype_u16(table + 8U + (glyph - first + 1U) * 2U);
		}

		/* The range is found. */
		break;
	case 2:
		/* One size and one set of metrics for every glyph. */
		if (size < 8U + 4U + COLOR_BIG_METRICS)
			return EINVAL;
		count = truetype_u32(table + 8);
		start = count * (glyph - first);
		end = start + count;
		*metrics = table + 12;
		break;
	case 4:
		/* A list of the glyphs present with their offsets (two bytes each), the last closing the one before it. */
		if (size < 12U)
			return EINVAL;
		count = truetype_u32(table + 8);
		if (count > (size - 12U) / 4U - 1U)
			return EINVAL;
		for (index = 0; index < count; index++) {
			listed = truetype_u16(table + 12U + index * 4U);
			if (listed == glyph)
				break;
		}

		/* Not in the list. */
		if (index == count)
			return ENOENT;
		start = truetype_u16(table + 12U + index * 4U + 2U);
		end = truetype_u16(table + 12U + (index + 1U) * 4U + 2U);
		break;
	case 5:
		/* One size and metrics for the glyphs of a list. */
		if (size < 8U + 4U + COLOR_BIG_METRICS + 4U)
			return EINVAL;
		count = truetype_u32(table + 12U + COLOR_BIG_METRICS);
		if (count > (size - 16U - COLOR_BIG_METRICS) / 2U)
			return EINVAL;
		for (index = 0; index < count; index++) {
			listed = truetype_u16(table + 16U + COLOR_BIG_METRICS + index * 2U);
			if (listed == glyph)
				break;
		}

		/* Not in the list. */
		if (index == count)
			return ENOENT;
		start = truetype_u32(table + 8) * index;
		end = start + truetype_u32(table + 8);
		*metrics = table + 12;
		break;
	default:
		return ENOENT;
	}

	/* An empty entry is a glyph without an image. */
	if (end <= start)
		return ENOENT;

	/* Succeeded: the image's place in CBDT. */
	*offset = image_offset + start;
	*length = end - start;
	return 0;
}

/* Reads an image record of CBDT: its metrics and its PNG (formats 17, 18 and 19). */
static int
color_image(
	const struct truetype_face *face,
	uint32_t offset,
	uint32_t length,
	unsigned format,
	const uint8_t *shared,
	unsigned ppem,
	struct truetype_color_glyph *out)
{
	const uint8_t *record;
	uint32_t header;
	uint32_t data;

	/* The record inside CBDT. */
	if (offset > face->cbdt_size || length > face->cbdt_size - offset)
		return EINVAL;
	record = face->cbdt + offset;

	/* Its metrics, then the PNG's length and bytes. */
	switch (format) {
	case 17:
		/* Small metrics: height, width, bearing x and y, advance. */
		header = COLOR_SMALL_METRICS;
		if (length < header + 4U)
			return EINVAL;
		out->height = record[0];
		out->width = record[1];
		out->left = (int8_t)record[2];
		out->top = (int8_t)record[3];
		out->advance = record[4];
		break;
	case 18:
		/* Big metrics, of which the horizontal ones are used. */
		header = COLOR_BIG_METRICS;
		if (length < header + 4U)
			return EINVAL;
		color_big_metrics(record, out);
		break;
	case 19:
		/* The metrics are the index's. */
		header = 0;
		if (shared == NULL || length < 4U)
			return EINVAL;
		color_big_metrics(shared, out);
		break;
	default:
		return ENOENT;
	}

	/* The PNG, which must fit the record. */
	data = truetype_u32(record + header);
	if (data > length - header - 4U || data == 0U)
		return EINVAL;
	out->png = record + header + 4U;
	out->png_size = data;
	out->ppem = ppem;

	/* Succeeded: the image and where it sits. */
	return 0;
}

/* Reads big glyph metrics' horizontal part: height, width, bearing x and y, advance. */
static void
color_big_metrics(
	const uint8_t *metrics,
	struct truetype_color_glyph *out)
{
	out->height = metrics[0];
	out->width = metrics[1];
	out->left = (int8_t)metrics[2];
	out->top = (int8_t)metrics[3];
	out->advance = metrics[4];
}
