/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Base64 (RFC 4648) and PEM certificate blocks (RFC 7468).
 */

#include "cryc-pem.h"

#include <stdlib.h>
#include <string.h>

/*
 * The text that opens a block, the text that closes a label, and the
 * text that opens the closing line (RFC 7468 section 2: "-----BEGIN ",
 * label, "-----"; "-----END ", label, "-----").
 */
#define PEM_BEGIN_PREFIX	"-----BEGIN "
#define PEM_END_PREFIX		"-----END "
#define PEM_DASHES		"-----"

/* The label of the blocks read (RFC 7468 section 5.1). */
#define PEM_CERTIFICATE_LABEL	"CERTIFICATE"

/* The value that base64_value gives a character outside the alphabet. */
#define BASE64_INVALID		0xffU

/* Forward declarations. */
static unsigned base64_value(uint8_t character);
static int base64_is_space(uint8_t character);
static size_t pem_find_begin(const char *text, size_t size, size_t from, size_t *line_end);
static size_t pem_skip_line_end(const char *text, size_t size, size_t position);
static int pem_prefix_matches(const char *text, size_t size, size_t position, const char *prefix);

/*
 * Decodes base64 (RFC 4648 section 4, the standard alphabet of Table 1)
 * from size characters at text into out.
 *
 * The decoder reads groups of four characters.  A group ends the data
 * when it carries "=" padding: one "=" after three characters gives two
 * bytes, two after two characters give one byte, and only whitespace may
 * follow.  When room is too small the decoding goes on without writing so
 * that a malformed text is still reported as EINVAL.
 */
int
cryc_base64_decode(
	const char *text,
	size_t size,
	uint8_t *out,
	size_t room,
	size_t *written)
{
	size_t index;
	size_t used;
	size_t emit;
	unsigned count;
	unsigned padding;
	unsigned value;
	unsigned group[4];
	unsigned bits;
	unsigned long accumulator;
	uint8_t character;
	int overflow;
	int finished;
	int space;

	/* The caller sees nothing written until the decoding succeeds. */
	*written = 0;

	/* The state: no characters of the current group yet, nothing finished. */
	used = 0;
	count = 0;
	padding = 0;
	overflow = 0;
	finished = 0;
	memset(group, 0, sizeof(group));

	/* Reads every character: alphabet, padding or skipped whitespace. */
	for (index = 0; index < size; index++) {
		character = (uint8_t)text[index];

		/* Skips the whitespace RFC 7468 section 3 allows inside the base64. */
		space = base64_is_space(character);
		if (space)
			continue;

		/* Refuses any character after the group that carried padding. */
		if (finished)
			return EINVAL;

		/* Takes a padding character: only the third or fourth of a group may be one. */
		if (character == (uint8_t)'=') {
			if (count < 2U)
				return EINVAL;
			padding++;
			group[count] = 0;
			count++;
		} else {
			/* Takes an alphabet character; one after padding in the group is refused. */
			value = base64_value(character);
			if (value == BASE64_INVALID)
				return EINVAL;
			if (padding != 0)
				return EINVAL;
			group[count] = value;
			count++;
		}

		/* Waits for the rest of the group. */
		if (count < 4U)
			continue;

		/* A complete group makes 3 bytes, or fewer when it carries padding. */
		emit = 3U - padding;
		accumulator = ((unsigned long)group[0] << 18) | ((unsigned long)group[1] << 12) |
		    ((unsigned long)group[2] << 6) | (unsigned long)group[3];

		/* The bits left over by padding must be zero (RFC 4648 3.5). */
		bits = 0;
		if (padding == 1U)
			bits = (unsigned)(accumulator & 0xffUL);
		else if (padding == 2U)
			bits = (unsigned)(accumulator & 0xffffUL);
		if (bits != 0)
			return EINVAL;

		/* Writes the bytes, or only counts them when room runs out. */
		if (used > room || emit > room - used) {
			overflow = 1;
		} else {
			out[used] = (uint8_t)(accumulator >> 16);
			if (emit > 1U)
				out[used + 1U] = (uint8_t)(accumulator >> 8);
			if (emit > 2U)
				out[used + 2U] = (uint8_t)accumulator;
		}

		/* Counts the bytes this group produced. */
		used += emit;

		/* Padding ends the data; an unpadded group starts the next. */
		if (padding != 0)
			finished = 1;
		count = 0;
		padding = 0;
	}

	/* Refuses a text that ends inside a group: padding is required (RFC 7468 section 3). */
	if (count != 0)
		return EINVAL;

	/* Reports that the output buffer was too small. */
	if (overflow)
		return ENOBUFS;

	/* Publishes the number of bytes decoded. */
	*written = used;

	/* Succeeded: every group decoded. */
	return 0;
}

/*
 * Finds the next CERTIFICATE block in a PEM text, starting at *position,
 * and decodes it.
 *
 * A BEGIN line is "-----BEGIN ", a label and "-----" at the start of a
 * line, followed by nothing but spaces and tabs; text that only looks like
 * one is explanatory text.  Blocks with another label are passed over.
 * The END line is the first "-" after the BEGIN line, which must be
 * "-----END CERTIFICATE-----"; base64 never contains "-".
 */
int
cryc_pem_next_certificate(
	const char *text,
	size_t size,
	size_t *position,
	uint8_t **der,
	size_t *der_size)
{
	size_t from;
	size_t begin;
	size_t line_end;
	size_t body_start;
	size_t body_end;
	size_t end_marker;
	size_t end_length;
	size_t room;
	size_t decoded;
	uint8_t *buffer;
	int matched;
	int error;

	/* The caller sees nothing of a failed read. */
	*der = NULL;
	*der_size = 0;

	/* A position at or past the end has nothing left. */
	from = *position;
	if (from >= size) {
		*position = size;
		return ENOENT;
	}

	/* Looks for a BEGIN line with the label CERTIFICATE, passing over other labels. */
	for (;;) {
		begin = pem_find_begin(text, size, from, &line_end);
		if (begin == size) {
			/* No further block: the search ends at the end of the text. */
			*position = size;
			return ENOENT;
		}

		/* Takes the block when its label is CERTIFICATE. */
		matched = pem_prefix_matches(text, size, begin, PEM_BEGIN_PREFIX PEM_CERTIFICATE_LABEL PEM_DASHES);
		if (matched)
			break;

		/* Another label: the search goes on after this line. */
		from = line_end;
	}

	/* Whatever happens next, a caller that continues starts after the BEGIN line. */
	body_start = pem_skip_line_end(text, size, line_end);
	*position = body_start;

	/* The END line is the first "-" after the BEGIN line. */
	end_marker = body_start;
	while (end_marker < size && text[end_marker] != '-')
		end_marker++;

	/* Refuses a block with no END line. */
	if (end_marker == size)
		return EINVAL;

	/* Refuses an END line that is not "-----END CERTIFICATE-----" (a different label). */
	end_length = strlen(PEM_END_PREFIX PEM_CERTIFICATE_LABEL PEM_DASHES);
	matched = pem_prefix_matches(text, size, end_marker, PEM_END_PREFIX PEM_CERTIFICATE_LABEL PEM_DASHES);
	if (!matched)
		return EINVAL;

	/* The base64 lies between the BEGIN line and the END line. */
	body_end = end_marker;
	room = (body_end - body_start) / 4U * 3U + 3U;

	/* Allocates the buffer for the decoded bytes (never zero in size). */
	buffer = malloc(room + 1U);
	if (buffer == NULL)
		return ENOMEM;

	/* Decodes the base64. */
	decoded = 0;
	error = cryc_base64_decode(text + body_start, body_end - body_start, buffer, room, &decoded);
	if (error != 0) {
		free(buffer);
		return EINVAL;
	}

	/* Refuses a block with no content: no certificate is empty. */
	if (decoded == 0) {
		free(buffer);
		return EINVAL;
	}

	/* Moves past the END line, its trailing spaces and its line end. */
	*position = pem_skip_line_end(text, size, end_marker + end_length);

	/* Publishes the DER bytes; the caller frees them. */
	*der = buffer;
	*der_size = decoded;

	/* Succeeded: one certificate was read. */
	return 0;
}

/*
 * Gives the value of a base64 alphabet character (RFC 4648 Table 1: A-Z
 * are 0-25, a-z are 26-51, 0-9 are 52-61, "+" is 62, "/" is 63), or
 * BASE64_INVALID.
 */
static unsigned
base64_value(
	uint8_t character)
{
	/* Upper-case letters. */
	if (character >= (uint8_t)'A' && character <= (uint8_t)'Z')
		return (unsigned)(character - (uint8_t)'A');

	/* Lower-case letters. */
	if (character >= (uint8_t)'a' && character <= (uint8_t)'z')
		return 26U + (unsigned)(character - (uint8_t)'a');

	/* Digits. */
	if (character >= (uint8_t)'0' && character <= (uint8_t)'9')
		return 52U + (unsigned)(character - (uint8_t)'0');

	/* The two symbols. */
	if (character == (uint8_t)'+')
		return 62U;
	if (character == (uint8_t)'/')
		return 63U;

	/* Not in the alphabet. */
	return BASE64_INVALID;
}

/*
 * Tells whether a character is whitespace the decoder skips: space, tab,
 * carriage return and line feed.
 */
static int
base64_is_space(
	uint8_t character)
{
	/* The four characters of RFC 7468 section 3's whitespace. */
	if (character == (uint8_t)' ')
		return 1;
	if (character == (uint8_t)'\t')
		return 1;
	if (character == (uint8_t)'\r')
		return 1;
	if (character == (uint8_t)'\n')
		return 1;

	/* Not whitespace. */
	return 0;
}

/*
 * Finds the next BEGIN line at or after from: "-----BEGIN ", a label
 * without line breaks, "-----", then only spaces and tabs up to the line
 * end.  Gives its start (size when there is none) and, in *line_end, the
 * position of the line break that ends it (or size).
 */
static size_t
pem_find_begin(
	const char *text,
	size_t size,
	size_t from,
	size_t *line_end)
{
	size_t start;
	size_t scan;
	size_t prefix_length;
	int at_line_start;
	int matched;

	/* Examines every position that starts a line. */
	prefix_length = strlen(PEM_BEGIN_PREFIX);
	for (start = from; start < size; start++) {
		/* A BEGIN line starts the text or follows a line break. */
		at_line_start = 0;
		if (start == 0 || text[start - 1U] == '\n' || text[start - 1U] == '\r')
			at_line_start = 1;
		if (!at_line_start)
			continue;

		/* The line must open with the BEGIN prefix. */
		matched = pem_prefix_matches(text, size, start, PEM_BEGIN_PREFIX);
		if (!matched)
			continue;

		/* The label runs to the next "-----" on the same line. */
		scan = start + prefix_length;
		while (scan < size && text[scan] != '\r' && text[scan] != '\n') {
			matched = pem_prefix_matches(text, size, scan, PEM_DASHES);
			if (matched)
				break;
			scan++;
		}

		/* Skips a line that is not a complete closing line. */
		if (scan >= size || text[scan] == '\r' || text[scan] == '\n')
			continue;

		/* Only spaces and tabs may follow the closing dashes. */
		scan += strlen(PEM_DASHES);
		while (scan < size && (text[scan] == ' ' || text[scan] == '\t'))
			scan++;
		if (scan < size && text[scan] != '\r' && text[scan] != '\n')
			continue;

		/* Found: publishes the line end. */
		*line_end = scan;
		return start;
	}

	/* No further BEGIN line. */
	*line_end = size;
	return size;
}

/*
 * Moves past trailing spaces and tabs and one line break (LF, CR or CR LF)
 * from position, and gives the new position.
 */
static size_t
pem_skip_line_end(
	const char *text,
	size_t size,
	size_t position)
{
	/* Skips the spaces and tabs that end the line. */
	while (position < size && (text[position] == ' ' || text[position] == '\t'))
		position++;

	/* Skips CR, LF or CR LF. */
	if (position < size && text[position] == '\r')
		position++;
	if (position < size && text[position] == '\n')
		position++;

	/* The position of the next line. */
	return position;
}

/*
 * Tells whether the NUL-terminated prefix occurs at position in the text.
 */
static int
pem_prefix_matches(
	const char *text,
	size_t size,
	size_t position,
	const char *prefix)
{
	size_t length;
	int different;

	/* A prefix that runs past the end does not match. */
	length = strlen(prefix);
	if (position > size || length > size - position)
		return 0;

	/* Compares the bytes. */
	different = memcmp(text + position, prefix, length);
	if (different != 0)
		return 0;

	/* Matched. */
	return 1;
}
