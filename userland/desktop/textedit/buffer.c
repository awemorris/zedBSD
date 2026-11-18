/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The document of Text Editor: its UTF-8 bytes in a gap buffer, and a
 * table of where each line starts (plan/ws092/design.md section 5).
 *
 * An edit moves the gap to where it happens, so typing in one place moves
 * no bytes.  The table of line starts is kept up to date by each edit:
 * the starts after the edit move by its length, and the newlines it added
 * or removed add or remove starts.  A byte that does not begin a
 * well-formed UTF-8 character counts as one character of its own.
 */

#include "textedit.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The room a gap is given beyond what an edit needs. */
#define BUFFER_SLACK		(64U * 1024U)

/* The first room of the table of line starts. */
#define BUFFER_LINES		256U

/* The code point a malformed byte stands for. */
#define BUFFER_REPLACEMENT	0xfffdU

static void buffer_move_gap(struct te_buffer *buffer, size_t position);
static int buffer_grow(struct te_buffer *buffer, size_t needed);
static int buffer_lines_room(struct te_buffer *buffer, size_t extra);
static int buffer_count_lines(struct te_buffer *buffer);

/*
 * Makes a document holding a text (which is copied).
 *
 * Returns 0, or ENOMEM.
 */
int
te_buffer_init(
	struct te_buffer *buffer,
	const char *text,
	size_t length)
{
	int error;

	/* The bytes with a gap after them. */
	memset(buffer, 0, sizeof(*buffer));
	buffer->capacity = length + BUFFER_SLACK;
	buffer->data = malloc(buffer->capacity);
	if (buffer->data == NULL)
		return ENOMEM;

	/* The text before the gap, the gap to the end. */
	if (length != 0U)
		memcpy(buffer->data, text, length);
	buffer->gap_start = length;
	buffer->gap_end = buffer->capacity;

	/* Where each line starts. */
	error = buffer_count_lines(buffer);
	if (error != 0) {
		te_buffer_free(buffer);
		return error;
	}

	/* Succeeded: the document holds the text. */
	return 0;
}

/*
 * Frees a document's memory.
 */
void
te_buffer_free(
	struct te_buffer *buffer)
{
	/* The bytes and the table. */
	free(buffer->data);
	free(buffer->lines);
	memset(buffer, 0, sizeof(*buffer));
}

/*
 * Reports how many bytes the document holds.
 */
size_t
te_buffer_length(
	const struct te_buffer *buffer)
{
	/* Everything but the gap. */
	return buffer->capacity - (buffer->gap_end - buffer->gap_start);
}

/*
 * Inserts bytes at a position.
 *
 * Returns 0, or ENOMEM (the document is then unchanged).
 */
int
te_buffer_insert(
	struct te_buffer *buffer,
	size_t position,
	const char *bytes,
	size_t count)
{
	size_t line;
	size_t newlines;
	size_t index;
	size_t at;
	int error;

	/* Nothing to insert. */
	if (count == 0U)
		return 0;

	/* Room in the gap, and in the table for the newlines the bytes bring. */
	error = buffer_grow(buffer, count);
	if (error != 0)
		return error;
	newlines = 0;
	for (index = 0; index < count; index++) {
		if (bytes[index] == '\n')
			newlines++;
	}

	/* The table must hold the new lines. */
	error = buffer_lines_room(buffer, newlines);
	if (error != 0)
		return error;

	/* The bytes go in at the start of the gap, moved to the position. */
	line = te_buffer_line_of(buffer, position);
	buffer_move_gap(buffer, position);
	memcpy(buffer->data + buffer->gap_start, bytes, count);
	buffer->gap_start += count;

	/* The lines after the position start later by the count. */
	for (index = line + 1U; index < buffer->line_count; index++)
		buffer->lines[index] += count;

	/* The new lines' starts go in after the position's line, in order. */
	memmove(buffer->lines + line + 1U + newlines, buffer->lines + line + 1U, (buffer->line_count - line - 1U) * sizeof(buffer->lines[0]));
	at = line + 1U;
	for (index = 0; index < count; index++) {
		if (bytes[index] != '\n')
			continue;
		buffer->lines[at] = position + index + 1U;
		at++;
	}

	/* Succeeded: the table holds the new lines too. */
	buffer->line_count += newlines;
	return 0;
}

/*
 * Deletes the bytes from start up to end.
 */
void
te_buffer_delete(
	struct te_buffer *buffer,
	size_t start,
	size_t end)
{
	size_t length;
	size_t first;
	size_t last;
	size_t count;
	size_t index;

	/* Nothing past the end is deleted, and nothing empty. */
	length = te_buffer_length(buffer);
	if (end > length)
		end = length;
	if (end <= start)
		return;

	/* The lines whose starts fall in the deleted text (after its first byte) go. */
	first = te_buffer_line_of(buffer, start);
	last = te_buffer_line_of(buffer, end);
	count = last - first;
	memmove(buffer->lines + first + 1U, buffer->lines + last + 1U, (buffer->line_count - last - 1U) * sizeof(buffer->lines[0]));
	buffer->line_count -= count;

	/* The lines after it start earlier by its length. */
	for (index = first + 1U; index < buffer->line_count; index++)
		buffer->lines[index] -= end - start;

	/* The bytes join the gap. */
	buffer_move_gap(buffer, start);
	buffer->gap_end += end - start;
}

/*
 * Reports the byte at a position (0 past the end).
 */
unsigned char
te_buffer_byte(
	const struct te_buffer *buffer,
	size_t position)
{
	/* Before the gap the byte is where it is; after it, past the gap. */
	if (position < buffer->gap_start)
		return (unsigned char)buffer->data[position];

	/* Past the end there is no byte. */
	position += buffer->gap_end - buffer->gap_start;
	if (position >= buffer->capacity)
		return 0U;

	/* Succeeded: the byte after the gap. */
	return (unsigned char)buffer->data[position];
}

/*
 * Copies the bytes from start up to end into out (which has room for them).
 */
void
te_buffer_copy(
	const struct te_buffer *buffer,
	size_t start,
	size_t end,
	char *out)
{
	size_t before;
	size_t gap;

	/* Nothing to copy. */
	if (end <= start)
		return;

	/* The part before the gap. */
	before = 0;
	if (start < buffer->gap_start) {
		before = buffer->gap_start - start;
		if (before > end - start)
			before = end - start;
		memcpy(out, buffer->data + start, before);
	}

	/* The part after it. */
	gap = buffer->gap_end - buffer->gap_start;
	if (before < end - start)
		memcpy(out + before, buffer->data + start + before + gap, end - start - before);
}

/*
 * Gives the document's bytes as the two runs around the gap.
 */
void
te_buffer_segments(
	const struct te_buffer *buffer,
	const char **first,
	size_t *first_length,
	const char **second,
	size_t *second_length)
{
	/* The bytes before the gap, then after it. */
	*first = buffer->data;
	*first_length = buffer->gap_start;
	*second = buffer->data + buffer->gap_end;
	*second_length = buffer->capacity - buffer->gap_end;
}

/*
 * Reports the line a position is on.
 */
size_t
te_buffer_line_of(
	const struct te_buffer *buffer,
	size_t position)
{
	size_t low;
	size_t high;
	size_t middle;

	/* The last line whose start is at or before the position. */
	low = 0;
	high = buffer->line_count;
	while (high - low > 1U) {
		middle = low + (high - low) / 2U;
		if (buffer->lines[middle] <= position) {
			low = middle;
		} else {
			high = middle;
		}
	}

	/* Succeeded: the line. */
	return low;
}

/*
 * Reports where a line starts (the end of the document past the last).
 */
size_t
te_buffer_line_start(
	const struct te_buffer *buffer,
	size_t line)
{
	size_t length;

	/* A line past the last starts at the end. */
	if (line >= buffer->line_count) {
		length = te_buffer_length(buffer);
		return length;
	}

	/* Succeeded: the line's start. */
	return buffer->lines[line];
}

/*
 * Reports where a line ends: its newline, or the end of the document.
 */
size_t
te_buffer_line_end(
	const struct te_buffer *buffer,
	size_t line)
{
	size_t length;

	/* The last line ends at the end of the document. */
	if (line + 1U >= buffer->line_count) {
		length = te_buffer_length(buffer);
		return length;
	}

	/* Succeeded: just before the next line's start, where the newline is. */
	return buffer->lines[line + 1U] - 1U;
}

/*
 * Decodes the character at a position and gives where the next one
 * starts; a malformed byte is U+FFFD and one byte long.
 */
uint32_t
te_buffer_char(
	const struct te_buffer *buffer,
	size_t position,
	size_t *next)
{
	unsigned char bytes[4];
	uint32_t codepoint;
	size_t length;
	size_t count;
	size_t index;

	/* The next four bytes at most. */
	length = te_buffer_length(buffer);
	count = length - position;
	if (count > 4U)
		count = 4U;
	for (index = 0; index < count; index++)
		bytes[index] = te_buffer_byte(buffer, position + index);

	/* Nothing past the end. */
	if (count == 0U) {
		*next = position;
		return 0U;
	}

	/* Decoded as a string of those bytes. */
	index = 0;
	codepoint = te_utf8_next((const char *)bytes, count, &index);
	*next = position + index;

	/* Succeeded: the character. */
	return codepoint;
}

/*
 * Reports where the character after the one at a position starts.
 */
size_t
te_buffer_next_char(
	const struct te_buffer *buffer,
	size_t position)
{
	size_t next;
	size_t length;

	/* Nothing past the end. */
	length = te_buffer_length(buffer);
	if (position >= length)
		return length;

	/* Succeeded: the character's end. */
	(void)te_buffer_char(buffer, position, &next);
	return next;
}

/*
 * Reports where the character before a position starts.
 */
size_t
te_buffer_prev_char(
	const struct te_buffer *buffer,
	size_t position)
{
	size_t start;
	size_t next;
	unsigned back;
	unsigned char byte;

	/* Nothing before the start. */
	if (position == 0U)
		return 0U;

	/* Back over up to three continuation bytes to what may be the character's first byte. */
	start = position - 1U;
	for (back = 1U; back < 4U && start > 0U; back++) {
		byte = te_buffer_byte(buffer, start);
		if ((byte & 0xc0U) != 0x80U)
			break;
		start--;
	}

	/* A character starting there that ends exactly at the position is the one before. */
	(void)te_buffer_char(buffer, start, &next);
	if (next == position)
		return start;

	/* Otherwise the byte just before is a character of its own. */
	return position - 1U;
}

/* Moves the gap so that it starts at a position. */
static void
buffer_move_gap(
	struct te_buffer *buffer,
	size_t position)
{
	size_t count;

	/* Bytes after the position move to after the gap. */
	if (position < buffer->gap_start) {
		count = buffer->gap_start - position;
		memmove(buffer->data + buffer->gap_end - count, buffer->data + position, count);
		buffer->gap_start -= count;
		buffer->gap_end -= count;
		return;
	}

	/* Bytes before the position move to before the gap. */
	if (position > buffer->gap_start) {
		count = position - buffer->gap_start;
		memmove(buffer->data + buffer->gap_start, buffer->data + buffer->gap_end, count);
		buffer->gap_start += count;
		buffer->gap_end += count;
	}
}

/* Makes the gap hold at least a count of bytes; returns 0 or ENOMEM. */
static int
buffer_grow(
	struct te_buffer *buffer,
	size_t needed)
{
	size_t capacity;
	size_t after;
	size_t length;
	char *larger;

	/* A gap that is big enough stays. */
	if (buffer->gap_end - buffer->gap_start >= needed)
		return 0;

	/* The text may not grow past its limit. */
	length = te_buffer_length(buffer);
	if (length + needed > TE_TEXT_MAX)
		return ENOMEM;

	/* Twice the size, or enough for the bytes and some slack. */
	capacity = buffer->capacity * 2U;
	if (capacity < buffer->capacity + needed + BUFFER_SLACK)
		capacity = buffer->capacity + needed + BUFFER_SLACK;

	/* The larger block. */
	larger = realloc(buffer->data, capacity);
	if (larger == NULL)
		return ENOMEM;

	/* The bytes after the gap move to the new end. */
	after = buffer->capacity - buffer->gap_end;
	memmove(larger + capacity - after, larger + buffer->gap_end, after);
	buffer->data = larger;
	buffer->gap_end = capacity - after;
	buffer->capacity = capacity;

	/* Succeeded: the gap has the room. */
	return 0;
}

/* Makes the table of line starts hold extra lines more; returns 0 or ENOMEM. */
static int
buffer_lines_room(
	struct te_buffer *buffer,
	size_t extra)
{
	size_t capacity;
	size_t *larger;

	/* A table with the room stays. */
	if (buffer->line_count + extra <= buffer->line_capacity)
		return 0;

	/* Doubled until it fits. */
	capacity = buffer->line_capacity;
	if (capacity == 0U)
		capacity = BUFFER_LINES;
	while (capacity < buffer->line_count + extra)
		capacity *= 2U;

	/* The larger table. */
	larger = realloc(buffer->lines, capacity * sizeof(larger[0]));
	if (larger == NULL)
		return ENOMEM;

	/* Succeeded: the table has the room. */
	buffer->lines = larger;
	buffer->line_capacity = capacity;
	return 0;
}

/* Builds the table of line starts from the text; returns 0 or ENOMEM. */
static int
buffer_count_lines(
	struct te_buffer *buffer)
{
	size_t length;
	size_t index;
	size_t count;
	unsigned char byte;
	int error;

	/* How many lines: one, and one more after each newline. */
	length = te_buffer_length(buffer);
	count = 1U;
	for (index = 0; index < length; index++) {
		byte = te_buffer_byte(buffer, index);
		if (byte == '\n')
			count++;
	}

	/* The table's room. */
	buffer->line_count = 0;
	error = buffer_lines_room(buffer, count);
	if (error != 0)
		return error;

	/* Each start: 0, then after each newline. */
	buffer->lines[0] = 0;
	buffer->line_count = 1;
	for (index = 0; index < length; index++) {
		byte = te_buffer_byte(buffer, index);
		if (byte != '\n')
			continue;
		buffer->lines[buffer->line_count] = index + 1U;
		buffer->line_count++;
	}

	/* Succeeded: the table is built. */
	return 0;
}
