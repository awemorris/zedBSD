/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A font's /ToUnicode CMap (ws175-p002b, plan/ws175/phase001/design.md
 * section 3.2; PDF 1.7 section 9.10.3): which characters each code of the
 * font's strings stands for.  The CMap's beginbfchar and beginbfrange
 * sections are read (a range's destination may be one string, whose last
 * unit counts up across the range, or an array of strings), each
 * destination a UTF-16BE string of one or more characters (a ligature
 * stands for several).  The code space and the source codes keep their
 * byte lengths, so a 1-byte and a 2-byte code of the same value differ.
 * usecmap is not read.
 *
 * The CMap is not trusted: the entries and the characters of one
 * destination are bounded, and a malformed section ends the reading (what
 * was read before stays).
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <pdf.h>

#include "internal.h"

/* The most entries a CMap may have, and the most characters one code may stand for. */
#define TOUNICODE_ENTRIES_MAX	65536U
#define TOUNICODE_CHARACTERS_MAX	8U

/* The longest source code, in bytes. */
#define TOUNICODE_CODE_MAX	4U

/*
 * One entry: the codes from low to high of one byte length, and what the
 * first stands for (characters, count of them; for a range given by one
 * string the last character counts up with the code), or for a range given
 * by an array each code's own (strings, one per code, in the CMap's
 * arena).
 */
struct tounicode_entry {
	unsigned low;
	unsigned high;
	unsigned length;
	uint32_t characters[TOUNICODE_CHARACTERS_MAX];
	size_t count;
	struct pdf_object *array;
};

/*
 * A CMap's entries, and the memory its arrays' strings live in.
 */
struct pdf_tounicode {
	struct tounicode_entry *entries;
	size_t count;
	size_t capacity;
	struct pdf_arena arena;
};

static int tounicode_section(struct pdf_tounicode *map, struct pdf_lexer *lexer, int ranges);
static int tounicode_add(struct pdf_tounicode *map, const struct tounicode_entry *entry);
static int tounicode_code(const struct pdf_token *token, unsigned *code, unsigned *length);
static size_t tounicode_utf16(const unsigned char *bytes, size_t length, uint32_t *characters, size_t capacity);

/*
 * Reads a /ToUnicode CMap's decoded bytes.  Returns 0 (a CMap without
 * entries is an empty map), ENOMEM, or PDF_EFORMAT for bytes that are not
 * a CMap's tokens at all.
 */
int
pdf_tounicode_parse(
	const unsigned char *data,
	size_t size,
	struct pdf_tounicode **map)
{
	struct pdf_tounicode *made;
	struct pdf_lexer lexer;
	struct pdf_token token;
	int is_chars;
	int is_ranges;
	int error;

	/* Refuses a missing result. */
	if (map == NULL || (data == NULL && size != 0))
		return EINVAL;

	/* The map. */
	made = calloc(1, sizeof(*made));
	if (made == NULL)
		return ENOMEM;

	/* Reads the tokens up to each section, the sections' entries, to the end. */
	memset(&lexer, 0, sizeof(lexer));
	lexer.data = data;
	lexer.size = size;
	lexer.arena = &made->arena;
	error = 0;
	for (;;) {
		/* The next token; a malformed one ends the reading. */
		error = pdf_lexer_next(&lexer, &token);
		if (error != 0 || token.type == PDF_TOKEN_END)
			break;

		/* A section of codes, or of ranges; any other token is the CMap's frame. */
		if (token.type != PDF_TOKEN_KEYWORD)
			continue;
		is_chars = pdf_token_is_keyword(&token, "beginbfchar");
		is_ranges = pdf_token_is_keyword(&token, "beginbfrange");
		if (!is_chars && !is_ranges)
			continue;
		error = tounicode_section(made, &lexer, is_ranges);
		if (error != 0)
			break;
	}

	/* Memory gone is a failure; a malformed CMap keeps what was read. */
	if (error == ENOMEM) {
		pdf_tounicode_free(made);
		return ENOMEM;
	}

	/* Succeeded: the map. */
	*map = made;
	return 0;
}

/*
 * Finds what a code of a byte length stands for: up to capacity
 * characters, count the number of them.  Returns 0, or ENOENT for a code
 * the map does not have.
 */
int
pdf_tounicode_lookup(
	const struct pdf_tounicode *map,
	unsigned code,
	unsigned length,
	uint32_t *characters,
	size_t capacity,
	size_t *count)
{
	const struct tounicode_entry *entry;
	const struct pdf_object *string;
	size_t at;
	size_t copied;

	/* Refuses a missing map or result. */
	if (map == NULL || characters == NULL || count == NULL || capacity == 0)
		return EINVAL;

	/* The last entry that holds the code wins (a later entry in a CMap overrides an earlier one). */
	for (at = map->count; at > 0; at--) {
		entry = &map->entries[at - 1];
		if (entry->length != length || code < entry->low || code > entry->high)
			continue;

		/* A range of an array: the code's own string. */
		if (entry->array != NULL) {
			string = entry->array->values[code - entry->low];
			*count = tounicode_utf16(string->bytes, string->length, characters, capacity);
			return 0;
		}

		/* One string: the last character counts up with the code. */
		copied = entry->count;
		if (copied > capacity)
			copied = capacity;
		memcpy(characters, entry->characters, copied * sizeof(*characters));
		if (copied > 0)
			characters[copied - 1] += code - entry->low;
		*count = copied;
		return 0;
	}

	/* Not in the map. */
	return ENOENT;
}

/*
 * Finds a code that stands for one character, the CMap read backward
 * (ws175-p004): a code of an entry whose destination is that one
 * character (a range of one string counts up its last character, a range
 * of an array has each code's own), checked forward so that a later entry
 * that overrides it is not taken.  Returns 0 with the code and its byte
 * length, EINVAL, or ENOENT when the map has none.
 */
int
pdf_tounicode_reverse(
	const struct pdf_tounicode *map,
	uint32_t character,
	unsigned *code,
	unsigned *length)
{
	const struct tounicode_entry *entry;
	const struct pdf_object *string;
	uint32_t found[TOUNICODE_CHARACTERS_MAX];
	uint32_t decoded[TOUNICODE_CHARACTERS_MAX];
	unsigned candidate;
	size_t count;
	size_t at;
	size_t item;
	int error;

	/* Refuses a missing map or result. */
	if (map == NULL || code == NULL || length == NULL)
		return EINVAL;

	/* The entries, the last first. */
	for (at = map->count; at > 0; at--) {
		entry = &map->entries[at - 1];
		candidate = 0U;
		count = 0;

		/* A range of an array: the code whose string is the character. */
		if (entry->array != NULL) {
			for (item = 0; item < entry->array->count && count == 0; item++) {
				string = entry->array->values[item];
				count = tounicode_utf16(string->bytes, string->length, decoded, TOUNICODE_CHARACTERS_MAX);
				if (count != 1 || decoded[0] != character)
					count = 0;
				candidate = entry->low + (unsigned)item;
			}
		} else if (entry->count == 1 && character >= entry->characters[0] && character - entry->characters[0] <= entry->high - entry->low) {
			/* One character counting up: the code as far from the low one. */
			candidate = entry->low + (unsigned)(character - entry->characters[0]);
			count = 1;
		}

		/* Not here. */
		if (count == 0)
			continue;

		/* The code must still stand for the character (no later entry overrides it). */
		error = pdf_tounicode_lookup(map, candidate, entry->length, found, TOUNICODE_CHARACTERS_MAX, &count);
		if (error != 0 || count != 1 || found[0] != character)
			continue;

		/* Succeeded: the code and its length. */
		*code = candidate;
		*length = entry->length;
		return 0;
	}

	/* No code. */
	return ENOENT;
}

/*
 * Frees a map.
 */
void
pdf_tounicode_free(
	struct pdf_tounicode *map)
{
	/* Nothing to free. */
	if (map == NULL)
		return;

	/* The entries, the strings, the map. */
	free(map->entries);
	pdf_arena_free(&map->arena);
	free(map);
}

/*
 * Reads one section's entries up to its end (endbfchar or endbfrange):
 * pairs of a code and a string, or triples of two codes and a string or an
 * array of strings.  Returns 0, ENOMEM, or PDF_EFORMAT for a malformed
 * entry (the section ends there).
 */
static int
tounicode_section(
	struct pdf_tounicode *map,
	struct pdf_lexer *lexer,
	int ranges)
{
	struct tounicode_entry entry;
	struct pdf_token token;
	struct pdf_object *array;
	unsigned high_length;
	size_t at;
	size_t start;
	int ended;
	int error;

	/* Entry after entry. */
	for (;;) {
		/* The first code, or the section's end. */
		error = pdf_lexer_next(lexer, &token);
		if (error != 0)
			return error;
		ended = pdf_token_is_keyword(&token, "endbfchar");
		if (!ended)
			ended = pdf_token_is_keyword(&token, "endbfrange");
		if (ended || token.type == PDF_TOKEN_END)
			return 0;
		memset(&entry, 0, sizeof(entry));
		error = tounicode_code(&token, &entry.low, &entry.length);
		if (error != 0)
			return error;
		entry.high = entry.low;

		/* A range's last code, of the same length. */
		if (ranges) {
			error = pdf_lexer_next(lexer, &token);
			if (error != 0)
				return error;
			error = tounicode_code(&token, &entry.high, &high_length);
			if (error != 0 || high_length != entry.length || entry.high < entry.low)
				return PDF_EFORMAT;
		}

		/* The destination: a string, or (a range's) an array of strings, one per code. */
		start = lexer->position;
		error = pdf_lexer_next(lexer, &token);
		if (error != 0)
			return error;
		if (token.type == PDF_TOKEN_STRING) {
			entry.count = tounicode_utf16(token.bytes, token.length, entry.characters, TOUNICODE_CHARACTERS_MAX);
		} else if (ranges && token.type == PDF_TOKEN_ARRAY_OPEN) {
			/* The array, each of its items a string, as many as the range has codes. */
			lexer->position = start;
			error = pdf_parse_object(lexer, 0, &array);
			if (error != 0)
				return error;
			if (array->type != PDF_OBJECT_ARRAY || array->count != (size_t)(entry.high - entry.low) + 1U)
				return PDF_EFORMAT;
			for (at = 0; at < array->count; at++) {
				if (array->values[at]->type != PDF_OBJECT_STRING)
					return PDF_EFORMAT;
			}

			/* The range's strings. */
			entry.array = array;
		} else {
			return PDF_EFORMAT;
		}

		/* The entry. */
		error = tounicode_add(map, &entry);
		if (error != 0)
			return error;
	}
}

/* Adds an entry.  Returns 0, ENOMEM, or ERANGE past the limit of entries. */
static int
tounicode_add(
	struct pdf_tounicode *map,
	const struct tounicode_entry *entry)
{
	struct tounicode_entry *grown;
	size_t capacity;

	/* Room, within the limit. */
	if (map->count == TOUNICODE_ENTRIES_MAX)
		return ERANGE;
	if (map->count == map->capacity) {
		capacity = map->capacity + map->capacity / 2 + 16;
		grown = realloc(map->entries, capacity * sizeof(*grown));
		if (grown == NULL)
			return ENOMEM;
		map->entries = grown;
		map->capacity = capacity;
	}

	/* Succeeded: the entry. */
	map->entries[map->count] = *entry;
	map->count++;
	return 0;
}

/* Reads a source code: a string of one to four bytes, big-endian.  Returns 0 or PDF_EFORMAT. */
static int
tounicode_code(
	const struct pdf_token *token,
	unsigned *code,
	unsigned *length)
{
	size_t at;

	/* A string of a code's length. */
	if (token->type != PDF_TOKEN_STRING || token->length == 0 || token->length > TOUNICODE_CODE_MAX)
		return PDF_EFORMAT;

	/* Its bytes, the first the highest. */
	*code = 0U;
	for (at = 0; at < token->length; at++)
		*code = (*code << 8) | token->bytes[at];
	*length = (unsigned)token->length;
	return 0;
}

/*
 * Decodes a UTF-16BE string into characters (a surrogate pair is one; an
 * unpaired surrogate is U+FFFD), up to capacity.  Returns how many.
 */
static size_t
tounicode_utf16(
	const unsigned char *bytes,
	size_t length,
	uint32_t *characters,
	size_t capacity)
{
	uint32_t unit;
	uint32_t low;
	size_t count;
	size_t at;

	/* Unit after unit (an odd byte at the end is left out). */
	count = 0;
	at = 0;
	while (at + 1U < length && count < capacity) {
		unit = ((uint32_t)bytes[at] << 8) | bytes[at + 1U];
		at += 2U;

		/* A high surrogate followed by a low one is one character. */
		if (unit >= 0xd800U && unit <= 0xdbffU && at + 1U < length) {
			low = ((uint32_t)bytes[at] << 8) | bytes[at + 1U];
			if (low >= 0xdc00U && low <= 0xdfffU) {
				unit = 0x10000U + ((unit - 0xd800U) << 10) + (low - 0xdc00U);
				at += 2U;
			}
		}

		/* An unpaired surrogate stands for nothing known. */
		if (unit >= 0xd800U && unit <= 0xdfffU)
			unit = 0xfffdU;
		characters[count] = unit;
		count++;
	}

	/* The characters decoded. */
	return count;
}
