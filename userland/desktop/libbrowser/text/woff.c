/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Web font files (ws074-p070): a WOFF 1.0 file unpacked into the sfnt
 * (TrueType) file it wraps, each table inflated with libz-compat, or a
 * TrueType file taken as it is.  WOFF 2.0 (Brotli) and fonts with CFF
 * outlines ("OTTO") are not read in this pass.
 */

#include "text/text.h"

#include <compat/zlib/zlib.h>
#include <errno.h>
#include <string.h>

/* The sizes of a WOFF header and of one of its table entries, and of an sfnt's header and table record. */
#define WOFF_HEADER		44U
#define WOFF_ENTRY		20U
#define WOFF_SFNT_HEADER	12U
#define WOFF_SFNT_RECORD	16U

/* The largest font file unpacked (a larger one is refused rather than allocated). */
#define WOFF_SIZE_MAX		(32U * 1024U * 1024U)

static uint32_t woff_get32(const unsigned char *bytes);
static uint16_t woff_get16(const unsigned char *bytes);
static void woff_put32(unsigned char *bytes, uint32_t value);
static void woff_put16(unsigned char *bytes, uint16_t value);
static int woff_unpack(const unsigned char *bytes, size_t length, struct wb_buffer *sfnt);

/*
 * Turns a font file's bytes into an sfnt file the text system opens: a
 * WOFF file unpacked, a TrueType file copied.  Returns ENOTSUP for the
 * formats this pass does not read and EINVAL for a damaged file.
 */
int
text_font_file(
	const unsigned char *bytes,
	size_t length,
	struct wb_buffer *sfnt)
{
	uint32_t signature;
	int error;

	/* The first four bytes say what the file is. */
	if (length < 4U)
		return EINVAL;
	signature = woff_get32(bytes);

	/* A WOFF file is unpacked. */
	if (signature == 0x774f4646U) {
		error = woff_unpack(bytes, length, sfnt);
		return error;
	}

	/* A TrueType file (version 1.0, or Apple's "true") is taken as it is. */
	if (signature == 0x00010000U || signature == 0x74727565U) {
		error = wb_buffer_append(sfnt, bytes, length);
		return error;
	}

	/* WOFF 2.0, CFF outlines and anything else are not read. */
	return ENOTSUP;
}

/*
 * Unpacks a WOFF file: the sfnt header for its tables, each table's
 * record, and each table's bytes, inflated when they are compressed and
 * padded to four bytes.
 */
static int
woff_unpack(
	const unsigned char *bytes,
	size_t length,
	struct wb_buffer *sfnt)
{
	const unsigned char *entry;
	unsigned char *out;
	unsigned char *record;
	uint32_t flavor;
	uint32_t offset;
	uint32_t compressed;
	uint32_t original;
	uLongf inflated;
	size_t count;
	size_t total;
	size_t place;
	size_t index;
	uint16_t selector;
	uint16_t range;
	int status;
	int error;

	/* The header: the flavor of the sfnt inside and how many tables it has. */
	if (length < WOFF_HEADER)
		return EINVAL;
	flavor = woff_get32(bytes + 4);
	count = woff_get16(bytes + 12);
	if (count == 0 || length < WOFF_HEADER + count * WOFF_ENTRY)
		return EINVAL;

	/* Only TrueType outlines are read in this pass. */
	if (flavor != 0x00010000U && flavor != 0x74727565U)
		return ENOTSUP;

	/* The sfnt's size: its header, records, and every table padded to four bytes. */
	total = WOFF_SFNT_HEADER + count * WOFF_SFNT_RECORD;
	for (index = 0; index < count; index++) {
		entry = bytes + WOFF_HEADER + index * WOFF_ENTRY;
		original = woff_get32(entry + 12);
		if (original > WOFF_SIZE_MAX)
			return EINVAL;
		total += ((size_t)original + 3U) & ~(size_t)3U;
		if (total > WOFF_SIZE_MAX)
			return EINVAL;
	}

	/* The sfnt's bytes, zeroed so that the padding is. */
	error = wb_buffer_reserve(sfnt, total);
	if (error != 0)
		return error;
	out = (unsigned char *)sfnt->data;
	memset(out, 0, total);

	/* The header: the flavor, the count, and the binary search's fields. */
	selector = 0;
	while ((2U << selector) <= count)
		selector++;
	range = (uint16_t)((1U << selector) * WOFF_SFNT_RECORD);
	woff_put32(out, flavor);
	woff_put16(out + 4, (uint16_t)count);
	woff_put16(out + 6, range);
	woff_put16(out + 8, selector);
	woff_put16(out + 10, (uint16_t)(count * WOFF_SFNT_RECORD - range));

	/* Each table: its record (in the WOFF's order, which is the tags'), then its bytes. */
	place = WOFF_SFNT_HEADER + count * WOFF_SFNT_RECORD;
	for (index = 0; index < count; index++) {
		entry = bytes + WOFF_HEADER + index * WOFF_ENTRY;
		offset = woff_get32(entry + 4);
		compressed = woff_get32(entry + 8);
		original = woff_get32(entry + 12);
		if (offset > length || compressed > length - offset || compressed > original)
			return EINVAL;

		/* The record: the tag, the checksum, where the table is and how long. */
		record = out + WOFF_SFNT_HEADER + index * WOFF_SFNT_RECORD;
		memcpy(record, entry, 4U);
		memcpy(record + 4, entry + 16, 4U);
		woff_put32(record + 8, (uint32_t)place);
		woff_put32(record + 12, original);

		/* A table stored as it is, or one inflated to exactly its size. */
		if (compressed == original) {
			memcpy(out + place, bytes + offset, original);
		} else {
			inflated = original;
			status = uncompress(out + place, &inflated, bytes + offset, compressed);
			if (status != Z_OK || inflated != original)
				return EINVAL;
		}

		/* The next table starts on four bytes. */
		place += ((size_t)original + 3U) & ~(size_t)3U;
	}

	/* Succeeded: the sfnt is unpacked. */
	sfnt->length = total;
	return 0;
}

/* Reads a big-endian 32-bit number. */
static uint32_t
woff_get32(
	const unsigned char *bytes)
{
	/* The four bytes, the first the highest. */
	return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) | ((uint32_t)bytes[2] << 8) | (uint32_t)bytes[3];
}

/* Reads a big-endian 16-bit number. */
static uint16_t
woff_get16(
	const unsigned char *bytes)
{
	/* The two bytes, the first the higher. */
	return (uint16_t)(((unsigned)bytes[0] << 8) | (unsigned)bytes[1]);
}

/* Writes a big-endian 32-bit number. */
static void
woff_put32(
	unsigned char *bytes,
	uint32_t value)
{
	/* The highest byte first. */
	bytes[0] = (unsigned char)(value >> 24);
	bytes[1] = (unsigned char)(value >> 16);
	bytes[2] = (unsigned char)(value >> 8);
	bytes[3] = (unsigned char)value;
}

/* Writes a big-endian 16-bit number. */
static void
woff_put16(
	unsigned char *bytes,
	uint16_t value)
{
	/* The higher byte first. */
	bytes[0] = (unsigned char)(value >> 8);
	bytes[1] = (unsigned char)value;
}
