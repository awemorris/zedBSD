/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The subset of a TrueType font a document embeds (ws175-p005, plan/
 * ws175/phase001/design.md section 4.3): the glyphs a document uses and
 * the parts their composite glyphs draw, the other glyphs left empty.
 *
 * The glyphs keep their numbers (the other glyphs' outlines are empty, as
 * fontTools' "retain glyph IDs" makes them), so that the shown strings
 * name glyphs by the font's own numbers (CIDs, CIDToGIDMap /Identity) and
 * do not wait for the subset; loca, made long, and hmtx keep a slot a
 * glyph, which compress to little.  The tables a PDF reader draws with are
 * kept -- head (its checkSumAdjustment again), hhea, maxp, loca, glyf,
 * hmtx, cvt, fpgm, prep (the hinting as it is), name (the copyright and
 * license records), OS/2 (fsType) and post (format 3) --, the others left
 * out: cmap (the strings name glyphs), the layout tables, the vertical
 * ones and a variable font's (the default instance stays).
 *
 * The font is not trusted: every table and glyph is checked against the
 * file's length, and a composite glyph's parts are followed to a depth of
 * 8.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <pdf.h>

#include "internal.h"

/* The tables kept, in the order they are written (tags in ASCII order, as a table directory wants them). */
static const char *const subset_tables[] = {
	"OS/2", "cvt ", "fpgm", "glyf", "head", "hhea", "hmtx", "loca", "maxp", "name", "post", "prep"
};
#define SUBSET_TABLES	(sizeof(subset_tables) / sizeof(subset_tables[0]))

/* Their places in that list. */
#define SUBSET_OS2	0U
#define SUBSET_GLYF	3U
#define SUBSET_HEAD	4U
#define SUBSET_HHEA	5U
#define SUBSET_HMTX	6U
#define SUBSET_LOCA	7U
#define SUBSET_MAXP	8U
#define SUBSET_POST	10U

/* The deepest composite glyph followed, and the most glyphs a font has. */
#define SUBSET_DEPTH_MAX	8
#define SUBSET_GLYPHS_MAX	65535U

/* A composite glyph's component flags (the TrueType specification's). */
#define SUBSET_ARGS_ARE_WORDS	0x0001U
#define SUBSET_HAVE_SCALE	0x0008U
#define SUBSET_MORE		0x0020U
#define SUBSET_HAVE_XY_SCALE	0x0040U
#define SUBSET_HAVE_TWO_BY_TWO	0x0080U

/* The checksum the whole font must sum to, by the head's checkSumAdjustment. */
#define SUBSET_MAGIC		0xb1b0afbaUL

/* One table of the font: its place and length in the file (0 length: the font has none). */
struct subset_table {
	size_t offset;
	size_t length;
};

/* A font being read: its bytes, its tables, its glyphs' count and loca format. */
struct subset_font {
	const unsigned char *data;
	size_t size;
	struct subset_table tables[SUBSET_TABLES];
	unsigned glyphs;
	int long_loca;
};

static int subset_open(const unsigned char *data, size_t size, struct subset_font *font);
static int subset_glyph(const struct subset_font *font, unsigned glyph, size_t *offset, size_t *length);
static int subset_mark(const struct subset_font *font, unsigned glyph, unsigned char *keep, int depth);
static unsigned subset_u16(const unsigned char *bytes);
static uint32_t subset_u32(const unsigned char *bytes);
static void subset_put16(unsigned char *bytes, unsigned value);
static void subset_put32(unsigned char *bytes, uint32_t value);
static uint32_t subset_checksum(const unsigned char *bytes, size_t length);

/*
 * Reads a TrueType font's embedding permission (its OS/2 fsType, design.md
 * section 4.4): *allowed is 0 for a font whose license restricts embedding
 * (0x0002) or allows only bitmaps (0x0200), and *whole 1 when it may not
 * be subset (0x0100).  A font without OS/2 may be embedded.  Returns 0 or
 * PDF_EFORMAT for bytes that are not a TrueType font.
 */
int
pdf_truetype_permission(
	const unsigned char *data,
	size_t size,
	int *allowed,
	int *whole)
{
	struct subset_font font;
	unsigned type;
	int error;

	/* The font's tables. */
	error = subset_open(data, size, &font);
	if (error != 0)
		return error;

	/* Without OS/2 (index 0), nothing is restricted. */
	*allowed = 1;
	*whole = 0;
	if (font.tables[SUBSET_OS2].length < 10U)
		return 0;

	/* fsType, at 8 in OS/2. */
	type = subset_u16(data + font.tables[SUBSET_OS2].offset + 8U);
	if ((type & 0x000fU) == 0x0002U || (type & 0x0200U) != 0U)
		*allowed = 0;
	if ((type & 0x0100U) != 0U)
		*whole = 1;
	return 0;
}

/*
 * Makes the subset of a TrueType font that draws the glyphs used marks
 * (one byte a glyph, glyph_count of them; glyph 0 is always kept), with
 * the parts of their composite glyphs, into a new buffer the caller frees.
 * Returns 0, EINVAL, PDF_EFORMAT for a font that is not sound, or ENOMEM.
 */
int
pdf_truetype_subset(
	const unsigned char *data,
	size_t size,
	const unsigned char *used,
	size_t glyph_count,
	unsigned char **out,
	size_t *out_size)
{
	struct subset_font font;
	unsigned char *keep;
	unsigned char *made;
	unsigned char *table;
	unsigned char *loca;
	size_t offsets[SUBSET_TABLES];
	size_t lengths[SUBSET_TABLES];
	size_t glyph_offset;
	size_t glyph_length;
	size_t written;
	size_t total;
	size_t position;
	size_t at;
	unsigned glyph;
	unsigned count;
	unsigned power;
	unsigned selector;
	uint32_t sum;
	int present;
	int error;

	/* A font, and the marks. */
	if (data == NULL || used == NULL || out == NULL || out_size == NULL)
		return EINVAL;
	error = subset_open(data, size, &font);
	if (error != 0)
		return error;

	/* The glyphs kept: glyph 0, those used (within the font), and their parts. */
	keep = calloc(font.glyphs, 1U);
	if (keep == NULL)
		return ENOMEM;
	error = subset_mark(&font, 0, keep, 0);
	for (glyph = 1; glyph < font.glyphs && error == 0; glyph++) {
		if (glyph < glyph_count && used[glyph] != 0U)
			error = subset_mark(&font, glyph, keep, 0);
	}

	/* The new glyf's length: each kept glyph's bytes, padded to four. */
	written = 0;
	for (glyph = 0; glyph < font.glyphs && error == 0; glyph++) {
		if (keep[glyph] == 0U)
			continue;
		error = subset_glyph(&font, glyph, &glyph_offset, &glyph_length);
		written += (glyph_length + 3U) & ~(size_t)3U;
	}

	/* A glyph that is not sound fails the subset. */
	if (error != 0) {
		free(keep);
		return error;
	}

	/* Each table's new length: glyf and loca made again, post's header alone, the others as they are. */
	count = 0;
	for (at = 0; at < SUBSET_TABLES; at++) {
		lengths[at] = font.tables[at].length;
		if (at == SUBSET_GLYF)
			lengths[at] = written;
		if (at == SUBSET_LOCA)
			lengths[at] = ((size_t)font.glyphs + 1U) * 4U;
		if (at == SUBSET_POST && lengths[at] > 0)
			lengths[at] = 32U;
		present = lengths[at] > 0 || at == SUBSET_GLYF;
		if (present)
			count++;
	}

	/* The file: the directory, then the tables, each on four bytes. */
	total = 12U + 16U * count;
	for (at = 0; at < SUBSET_TABLES; at++) {
		offsets[at] = total;
		total += (lengths[at] + 3U) & ~(size_t)3U;
	}

	/* Its bytes, zero to start with. */
	made = calloc(total, 1U);
	if (made == NULL) {
		free(keep);
		return ENOMEM;
	}

	/* The offset table: TrueType outlines, the tables' count and the binary search's numbers. */
	power = 1;
	selector = 0;
	while (power * 2U <= count) {
		power *= 2U;
		selector++;
	}

	/* The offset table's numbers. */
	subset_put32(made, 0x00010000UL);
	subset_put16(made + 4, count);
	subset_put16(made + 6, power * 16U);
	subset_put16(made + 8, selector);
	subset_put16(made + 10, count * 16U - power * 16U);

	/* The tables copied as they are, and post's header as format 3 (no glyph names). */
	for (at = 0; at < SUBSET_TABLES; at++) {
		if (at == SUBSET_GLYF || at == SUBSET_LOCA || lengths[at] == 0)
			continue;
		table = made + offsets[at];
		memcpy(table, data + font.tables[at].offset, lengths[at]);
		if (at == SUBSET_POST)
			subset_put32(table, 0x00030000UL);
	}

	/* glyf and its loca: the kept glyphs, each where the long loca says, the others empty. */
	table = made + offsets[SUBSET_GLYF];
	loca = made + offsets[SUBSET_LOCA];
	position = 0;
	for (glyph = 0; glyph < font.glyphs; glyph++) {
		subset_put32(loca + (size_t)glyph * 4U, (uint32_t)position);
		if (keep[glyph] == 0U)
			continue;
		(void)subset_glyph(&font, glyph, &glyph_offset, &glyph_length);
		memcpy(table + position, data + glyph_offset, glyph_length);
		position += (glyph_length + 3U) & ~(size_t)3U;
	}

	/* The end of the last glyph. */
	subset_put32(loca + (size_t)font.glyphs * 4U, (uint32_t)position);
	free(keep);

	/* head: the long loca, the adjustment zeroed until the whole is summed. */
	table = made + offsets[SUBSET_HEAD];
	subset_put16(table + 50, 1U);
	subset_put32(table + 8, 0UL);

	/* The directory: each table's tag, checksum, place and length. */
	position = 12;
	for (at = 0; at < SUBSET_TABLES; at++) {
		present = lengths[at] > 0 || at == SUBSET_GLYF;
		if (!present)
			continue;
		memcpy(made + position, subset_tables[at], 4);
		subset_put32(made + position + 4, subset_checksum(made + offsets[at], lengths[at]));
		subset_put32(made + position + 8, (uint32_t)offsets[at]);
		subset_put32(made + position + 12, (uint32_t)lengths[at]);
		position += 16U;
	}

	/* head's checkSumAdjustment: the whole font sums to the magic number. */
	sum = subset_checksum(made, total);
	subset_put32(made + offsets[SUBSET_HEAD] + 8, (uint32_t)((SUBSET_MAGIC - sum) & 0xffffffffUL));

	/* Succeeded: the subset. */
	*out = made;
	*out_size = total;
	return 0;
}

/*
 * Reads what a font descriptor says of a TrueType font (ws175-p005): its
 * em, its box (head), its ascent and descent (hhea), its capital height
 * (OS/2 version 2 and later; seven tenths of the ascent otherwise) and
 * whether it is monospaced (post), and the advance of each glyph through
 * pdf_truetype_advance.  Returns 0 or PDF_EFORMAT.
 */
int
pdf_truetype_metrics(
	const unsigned char *data,
	size_t size,
	struct pdf_truetype_metrics *metrics)
{
	struct subset_font font;
	const unsigned char *head;
	const unsigned char *hhea;
	const unsigned char *os2;
	unsigned version;
	int error;

	/* The tables. */
	error = subset_open(data, size, &font);
	if (error != 0)
		return error;
	if (font.tables[SUBSET_HHEA].length < 36U)
		return PDF_EFORMAT;
	head = data + font.tables[SUBSET_HEAD].offset;
	hhea = data + font.tables[SUBSET_HHEA].offset;

	/* The em and the box, the ascent and the descent. */
	memset(metrics, 0, sizeof(*metrics));
	metrics->units_per_em = subset_u16(head + 18);
	if (metrics->units_per_em == 0)
		return PDF_EFORMAT;
	metrics->box[0] = (short)subset_u16(head + 36);
	metrics->box[1] = (short)subset_u16(head + 38);
	metrics->box[2] = (short)subset_u16(head + 40);
	metrics->box[3] = (short)subset_u16(head + 42);
	metrics->ascent = (short)subset_u16(hhea + 4);
	metrics->descent = (short)subset_u16(hhea + 6);
	metrics->metrics_count = subset_u16(hhea + 34);
	metrics->glyphs = font.glyphs;

	/* The capital height. */
	metrics->cap_height = metrics->ascent * 7 / 10;
	if (font.tables[SUBSET_OS2].length >= 90U) {
		os2 = data + font.tables[SUBSET_OS2].offset;
		version = subset_u16(os2);
		if (version >= 2U)
			metrics->cap_height = (short)subset_u16(os2 + 88);
	}

	/* Monospaced, by post's isFixedPitch. */
	if (font.tables[SUBSET_POST].length >= 16U)
		metrics->fixed = subset_u32(data + font.tables[SUBSET_POST].offset + 12U) != 0U;
	return 0;
}

/*
 * Gives a glyph's advance in the font's units (hmtx; a glyph past the
 * metrics' count has the last's).  Returns 0 or PDF_EFORMAT.
 */
int
pdf_truetype_advance(
	const unsigned char *data,
	size_t size,
	unsigned glyph,
	unsigned *advance)
{
	struct subset_font font;
	unsigned count;
	unsigned at;
	int error;

	/* The tables, and the count of full metrics. */
	error = subset_open(data, size, &font);
	if (error != 0)
		return error;
	if (font.tables[SUBSET_HHEA].length < 36U || glyph >= font.glyphs)
		return PDF_EFORMAT;
	count = subset_u16(data + font.tables[SUBSET_HHEA].offset + 34U);
	if (count == 0 || (size_t)count * 4U > font.tables[SUBSET_HMTX].length)
		return PDF_EFORMAT;

	/* The glyph's, or the last full one's. */
	at = glyph;
	if (at >= count)
		at = count - 1U;
	*advance = subset_u16(data + font.tables[SUBSET_HMTX].offset + (size_t)at * 4U);
	return 0;
}

/*
 * Reads a font's table directory into the tables kept, and its glyphs'
 * count and loca format.  Returns 0 or PDF_EFORMAT.
 */
static int
subset_open(
	const unsigned char *data,
	size_t size,
	struct subset_font *font)
{
	const unsigned char *record;
	uint32_t version;
	unsigned tables;
	unsigned at;
	size_t kept;
	size_t offset;
	size_t length;
	int same;

	/* TrueType outlines (1.0 or "true") and the directory within the file. */
	memset(font, 0, sizeof(*font));
	if (data == NULL || size < 12U)
		return PDF_EFORMAT;
	version = subset_u32(data);
	if (version != 0x00010000UL && version != 0x74727565UL)
		return PDF_EFORMAT;
	tables = subset_u16(data + 4);
	if ((size_t)tables * 16U > size - 12U)
		return PDF_EFORMAT;

	/* Each record of a kept table, its bytes within the file. */
	for (at = 0; at < tables; at++) {
		record = data + 12U + (size_t)at * 16U;
		offset = subset_u32(record + 8);
		length = subset_u32(record + 12);
		if (offset > size || length > size - offset)
			return PDF_EFORMAT;
		for (kept = 0; kept < SUBSET_TABLES; kept++) {
			same = memcmp(record, subset_tables[kept], 4) == 0;
			if (!same)
				continue;
			font->tables[kept].offset = offset;
			font->tables[kept].length = length;
		}
	}

	/* head, maxp, loca and glyf must be there; the glyphs' count and the loca's format. */
	if (font->tables[SUBSET_HEAD].length < 54U || font->tables[SUBSET_MAXP].length < 6U || font->tables[SUBSET_LOCA].length == 0)
		return PDF_EFORMAT;
	font->data = data;
	font->size = size;
	font->glyphs = subset_u16(data + font->tables[SUBSET_MAXP].offset + 4U);
	font->long_loca = subset_u16(data + font->tables[SUBSET_HEAD].offset + 50U) != 0U;
	if (font->glyphs == 0 || font->glyphs > SUBSET_GLYPHS_MAX)
		return PDF_EFORMAT;

	/* The loca holds an offset a glyph and one more. */
	length = ((size_t)font->glyphs + 1U) * 2U;
	if (font->long_loca)
		length *= 2U;
	if (font->tables[SUBSET_LOCA].length < length)
		return PDF_EFORMAT;
	return 0;
}

/* Finds a glyph's bytes in the file.  Returns 0 or PDF_EFORMAT. */
static int
subset_glyph(
	const struct subset_font *font,
	unsigned glyph,
	size_t *offset,
	size_t *length)
{
	const unsigned char *loca;
	size_t start;
	size_t end;

	/* The glyph's start and the next one's, from loca. */
	loca = font->data + font->tables[SUBSET_LOCA].offset;
	if (font->long_loca) {
		start = subset_u32(loca + (size_t)glyph * 4U);
		end = subset_u32(loca + (size_t)glyph * 4U + 4U);
	} else {
		start = (size_t)subset_u16(loca + (size_t)glyph * 2U) * 2U;
		end = (size_t)subset_u16(loca + (size_t)glyph * 2U + 2U) * 2U;
	}

	/* Within glyf. */
	if (start > end || end > font->tables[SUBSET_GLYF].length)
		return PDF_EFORMAT;

	/* Succeeded: its bytes. */
	*offset = font->tables[SUBSET_GLYF].offset + start;
	*length = end - start;
	return 0;
}

/*
 * Keeps a glyph and, for a composite one, its parts (to SUBSET_DEPTH_MAX).
 * Returns 0 or PDF_EFORMAT.
 */
static int
subset_mark(
	const struct subset_font *font,
	unsigned glyph,
	unsigned char *keep,
	int depth)
{
	const unsigned char *bytes;
	size_t offset;
	size_t length;
	size_t position;
	unsigned flags;
	unsigned part;
	int contours;
	int error;

	/* A glyph of the font, kept. */
	if (glyph >= font->glyphs || depth > SUBSET_DEPTH_MAX)
		return PDF_EFORMAT;
	keep[glyph] = 1U;
	error = subset_glyph(font, glyph, &offset, &length);
	if (error != 0)
		return error;

	/* A simple glyph (or an empty one) has no parts. */
	if (length < 10U)
		return 0;
	bytes = font->data + offset;
	contours = (int)(short)subset_u16(bytes);
	if (contours >= 0)
		return 0;

	/* Each component: its flags, its glyph, its arguments and scale. */
	position = 10;
	do {
		if (position + 4U > length)
			return PDF_EFORMAT;
		flags = subset_u16(bytes + position);
		part = subset_u16(bytes + position + 2U);
		position += 4U;
		error = subset_mark(font, part, keep, depth + 1);
		if (error != 0)
			return error;

		/* Past its arguments and its scale. */
		position += 2U;
		if ((flags & SUBSET_ARGS_ARE_WORDS) != 0U)
			position += 2U;
		if ((flags & SUBSET_HAVE_SCALE) != 0U)
			position += 2U;
		else if ((flags & SUBSET_HAVE_XY_SCALE) != 0U)
			position += 4U;
		else if ((flags & SUBSET_HAVE_TWO_BY_TWO) != 0U)
			position += 8U;
	} while ((flags & SUBSET_MORE) != 0U);

	/* Succeeded: the glyph and its parts. */
	return 0;
}

/* Reads a big-endian 16-bit number. */
static unsigned
subset_u16(
	const unsigned char *bytes)
{
	/* The high byte first. */
	return ((unsigned)bytes[0] << 8) | bytes[1];
}

/* Reads a big-endian 32-bit number. */
static uint32_t
subset_u32(
	const unsigned char *bytes)
{
	/* The highest byte first. */
	return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) | ((uint32_t)bytes[2] << 8) | (uint32_t)bytes[3];
}

/* Writes a big-endian 16-bit number. */
static void
subset_put16(
	unsigned char *bytes,
	unsigned value)
{
	/* The high byte first. */
	bytes[0] = (unsigned char)(value >> 8);
	bytes[1] = (unsigned char)value;
}

/* Writes a big-endian 32-bit number. */
static void
subset_put32(
	unsigned char *bytes,
	uint32_t value)
{
	/* The highest byte first. */
	bytes[0] = (unsigned char)(value >> 24);
	bytes[1] = (unsigned char)(value >> 16);
	bytes[2] = (unsigned char)(value >> 8);
	bytes[3] = (unsigned char)value;
}

/* Sums bytes as big-endian 32-bit numbers, the last padded with zeros. */
static uint32_t
subset_checksum(
	const unsigned char *bytes,
	size_t length)
{
	unsigned char last[4];
	uint32_t sum;
	size_t at;

	/* Each whole word. */
	sum = 0;
	for (at = 0; at + 4U <= length; at += 4U)
		sum = (sum + subset_u32(bytes + at)) & 0xffffffffUL;

	/* The last one, padded. */
	if (at < length) {
		memset(last, 0, sizeof(last));
		memcpy(last, bytes + at, length - at);
		sum = (sum + subset_u32(last)) & 0xffffffffUL;
	}

	/* The sum. */
	return sum;
}
