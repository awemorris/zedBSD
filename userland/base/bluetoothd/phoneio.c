/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The lines of the PHONE requests and events (ws197-p003, see phoneio.h).
 */

#include "userland/base/bluetoothd/phoneio.h"

#include "userland/base/bluetoothd/mapxml.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The start of a PHONE SEND line, and the longest decimal length it gives. */
#define PHONEIO_SEND		"PHONE SEND "
#define PHONEIO_SEND_BYTES	11U
#define PHONEIO_DIGITS_MAX	5U

static int phoneio_hex(char letter);
static int phoneio_lines(struct btd_phoneio_input *input, const struct btd_phoneio_events *events);
static int phoneio_text_begin(struct btd_phoneio_input *input, size_t line_length, size_t text_length, const struct btd_phoneio_events *events);
static void phoneio_text_done(struct btd_phoneio_input *input, const struct btd_phoneio_events *events);

/*
 * Reads the next argument of a request's line from *cursor: its key and
 * its value (a quoted string unescaped).  Returns BTD_PHONEIO_ARGUMENT
 * with *cursor after it, BTD_PHONEIO_END at the line's end, or
 * BTD_PHONEIO_MALFORMED (no '=', a key or value too long, an unended
 * string, a bad escape, a NUL byte in a value).
 */
int
btd_phoneio_next(
	const char **cursor,
	char *key,
	size_t key_size,
	char *value,
	size_t value_size)
{
	const char *text;
	size_t key_length;
	size_t used;
	int high;
	int low;

	/* The spaces before it. */
	text = *cursor;
	while (*text == ' ')
		text++;

	/* The line's end. */
	if (*text == '\0') {
		*cursor = text;
		return BTD_PHONEIO_END;
	}

	/* The key, up to '='. */
	key_length = 0U;
	while (text[key_length] != '\0' && text[key_length] != '=' && text[key_length] != ' ')
		key_length++;
	if (text[key_length] != '=' || key_length == 0U || key_length >= key_size)
		return BTD_PHONEIO_MALFORMED;
	memcpy(key, text, key_length);
	key[key_length] = '\0';
	text += key_length + 1U;

	/* A word: up to the next space. */
	used = 0U;
	if (*text != '"') {
		while (*text != '\0' && *text != ' ') {
			if (used + 1U >= value_size)
				return BTD_PHONEIO_MALFORMED;
			value[used] = *text;
			used++;
			text++;
		}

		/* The word read. */
		value[used] = '\0';
		*cursor = text;
		return BTD_PHONEIO_ARGUMENT;
	}

	/* A string: up to its closing quote, each escape undone. */
	text++;
	while (*text != '"') {
		/* A string must end on its line, and fit. */
		if (*text == '\0' || used + 1U >= value_size)
			return BTD_PHONEIO_MALFORMED;

		/* A plain character. */
		if (*text != '\\') {
			value[used] = *text;
			used++;
			text++;
			continue;
		}

		/* An escaped quote or backslash. */
		if (text[1] == '"' || text[1] == '\\') {
			value[used] = text[1];
			used++;
			text += 2;
			continue;
		}

		/* A byte as two hex digits, never NUL. */
		if (text[1] != 'x')
			return BTD_PHONEIO_MALFORMED;
		high = phoneio_hex(text[2]);
		if (high < 0)
			return BTD_PHONEIO_MALFORMED;
		low = phoneio_hex(text[3]);
		if (low < 0)
			return BTD_PHONEIO_MALFORMED;
		if (high == 0 && low == 0)
			return BTD_PHONEIO_MALFORMED;
		value[used] = (char)(high * 16 + low);
		used++;
		text += 4;
	}

	/* The closing quote must end the argument. */
	text++;
	if (*text != '\0' && *text != ' ')
		return BTD_PHONEIO_MALFORMED;

	/* Succeeded: the argument. */
	value[used] = '\0';
	*cursor = text;
	return BTD_PHONEIO_ARGUMENT;
}

/*
 * Reads the length of a PHONE SEND line (section 4.3): the bytes of text
 * that follow the line.  Returns 0 with *length (1 to BTD_PHONE_SEND_MAX),
 * ENOENT for a line that is not PHONE SEND, or EINVAL for a PHONE SEND
 * line whose arguments are malformed or whose length is missing, given
 * twice or out of range (the stream cannot be read on: the client goes).
 */
int
btd_phoneio_send_length(
	const char *line,
	size_t *length)
{
	char key[BTD_PHONEIO_KEY_MAX];
	char value[BTD_PHONEIO_VALUE_MAX];
	const char *cursor;
	size_t digits;
	size_t index;
	size_t number;
	int found;
	int same;
	int got;

	/* A PHONE SEND line. */
	same = strncmp(line, PHONEIO_SEND, PHONEIO_SEND_BYTES);
	if (same != 0)
		return ENOENT;

	/* Each argument, the length once among them. */
	cursor = line + PHONEIO_SEND_BYTES;
	found = 0;
	number = 0U;
	for (;;) {
		got = btd_phoneio_next(&cursor, key, sizeof(key), value, sizeof(value));
		if (got == BTD_PHONEIO_END)
			break;
		if (got != BTD_PHONEIO_ARGUMENT)
			return EINVAL;

		/* Another argument is the request's to read. */
		same = strcmp(key, "length");
		if (same != 0)
			continue;

		/* The length, once, in decimal digits alone. */
		digits = strlen(value);
		if (found || digits == 0U || digits > PHONEIO_DIGITS_MAX)
			return EINVAL;
		for (index = 0U; index < digits; index++) {
			if (value[index] < '0' || value[index] > '9')
				return EINVAL;
			number = number * 10U + (size_t)(value[index] - '0');
		}

		/* Read once. */
		found = 1;
	}

	/* A length within the range. */
	if (!found || number == 0U || number > BTD_PHONE_SEND_MAX)
		return EINVAL;

	/* Succeeded: the length of the text that follows. */
	*length = number;
	return 0;
}

/*
 * Adds a value as a string in double quotes to a line at *used: the text
 * cut to limit bytes without splitting a character, each '"' and '\\'
 * escaped with a backslash, each byte below 0x20 and 0x7f as \xHH.
 * Returns 0, or ENOSPC when the line has no room (the line is left as it
 * was).
 */
int
btd_phoneio_quote(
	char *line,
	size_t size,
	size_t *used,
	const char *text,
	size_t limit)
{
	static const char digits[] = "0123456789abcdef";
	size_t length;
	size_t index;
	size_t at;
	unsigned char byte;

	/* The text as far as it is kept. */
	length = strlen(text);
	length = btd_mapxml_utf8_cut(text, length, limit);

	/* The opening quote. */
	at = *used;
	if (at + 1U >= size)
		return ENOSPC;
	line[at] = '"';
	at++;

	/* Each byte, escaped where it must be (room for four bytes, the quote and the NUL kept). */
	for (index = 0U; index < length; index++) {
		if (at + 6U >= size)
			return ENOSPC;
		byte = (unsigned char)text[index];

		/* A quote or a backslash after a backslash. */
		if (byte == '"' || byte == '\\') {
			line[at] = '\\';
			line[at + 1U] = (char)byte;
			at += 2U;
			continue;
		}

		/* A control byte as two hex digits. */
		if (byte < 0x20U || byte == 0x7fU) {
			line[at] = '\\';
			line[at + 1U] = 'x';
			line[at + 2U] = digits[byte >> 4];
			line[at + 3U] = digits[byte & 0x0fU];
			at += 4U;
			continue;
		}

		/* Any other byte as it is. */
		line[at] = (char)byte;
		at++;
	}

	/* The closing quote. */
	if (at + 1U >= size)
		return ENOSPC;
	line[at] = '"';
	at++;

	/* Succeeded: the line ends after it. */
	line[at] = '\0';
	*used = at;
	return 0;
}

/* Prepares a client's input: no line, no text. */
void
btd_phoneio_input_init(
	struct btd_phoneio_input *input)
{
	/* Nothing read. */
	memset(input, 0, sizeof(*input));
}

/*
 * Gives where the client's next bytes go and how many fit: the text being
 * read, or the line (its NUL's room kept).
 */
void
btd_phoneio_input_room(
	struct btd_phoneio_input *input,
	uint8_t **room,
	size_t *size)
{
	/* The text being read. */
	if (input->text != NULL) {
		*room = input->text + input->text_used;
		*size = input->text_length - input->text_used;
		return;
	}

	/* The line. */
	*room = (uint8_t *)input->line + input->used;
	*size = sizeof(input->line) - 1U - input->used;
}

/*
 * Takes count bytes the client wrote where btd_phoneio_input_room said:
 * a text's bytes until it is whole (then its request), or the line's
 * bytes and each whole line (a request, or a PHONE SEND line that starts
 * its text).  Returns 0, EINVAL for a malformed PHONE SEND line, EMSGSIZE
 * for a line that fills the room without ending, ENOMEM when a text
 * cannot be kept (each closes the client), or ECANCELED when a request's
 * line stopped the reading (the client went: the input is not touched
 * again).
 */
int
btd_phoneio_input_got(
	struct btd_phoneio_input *input,
	size_t count,
	const struct btd_phoneio_events *events)
{
	int error;

	/* A text's bytes: its request once it is whole. */
	if (input->text != NULL) {
		input->text_used += count;
		if (input->text_used == input->text_length)
			phoneio_text_done(input, events);
		return 0;
	}

	/* The line's bytes, then each whole line. */
	input->used += count;
	input->line[input->used] = '\0';
	error = phoneio_lines(input, events);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Empties a client's input, a text being read freed. */
void
btd_phoneio_input_clear(
	struct btd_phoneio_input *input)
{
	/* The text freed. */
	free(input->text);

	/* Succeeded: nothing read. */
	memset(input, 0, sizeof(*input));
}

/*
 * Carries out each whole line the input holds, until a text is read:
 * a PHONE SEND line starts its text, any other line is a request.
 * Returns as btd_phoneio_input_got.
 */
static int
phoneio_lines(
	struct btd_phoneio_input *input,
	const struct btd_phoneio_events *events)
{
	size_t text_length;
	size_t length;
	char *end;
	int sending;
	int stop;
	int error;

	/* Each whole line, while no text is being read. */
	while (input->text == NULL) {
		end = memchr(input->line, '\n', input->used);
		if (end == NULL)
			break;
		*end = '\0';
		length = (size_t)(end + 1 - input->line);

		/* A PHONE SEND line: a malformed one stops the stream; a good one starts its text. */
		sending = btd_phoneio_send_length(input->line, &text_length);
		if (sending == EINVAL)
			return EINVAL;
		if (sending == 0) {
			error = phoneio_text_begin(input, length, text_length, events);
			if (error != 0)
				return error;
			continue;
		}

		/* Any other line is a request; one that stops the reading leaves the input alone. */
		stop = events->line(events->context, input->line);
		if (stop)
			return ECANCELED;

		/* The line leaves the input. */
		memmove(input->line, end + 1, input->used - length + 1U);
		input->used -= length;
	}

	/* A line that fills the room without ending is not one. */
	if (input->text == NULL && input->used + 1U >= sizeof(input->line))
		return EMSGSIZE;

	/* Succeeded. */
	return 0;
}

/*
 * Starts the text of a PHONE SEND line: the line kept, the room made, and
 * what the input holds after the line taken into the text first.
 * Returns 0, or ENOMEM.
 */
static int
phoneio_text_begin(
	struct btd_phoneio_input *input,
	size_t line_length,
	size_t text_length,
	const struct btd_phoneio_events *events)
{
	size_t have;
	size_t taken;

	/* The line, kept for the request. */
	memcpy(input->send_line, input->line, line_length);

	/* The room for the text. */
	input->text = malloc(text_length);
	if (input->text == NULL)
		return ENOMEM;
	input->text_length = text_length;

	/* The bytes after the line, as far as the text goes. */
	have = input->used - line_length;
	taken = have;
	if (taken > text_length)
		taken = text_length;
	memcpy(input->text, input->line + line_length, taken);
	input->text_used = taken;

	/* The line and those bytes leave the input. */
	memmove(input->line, input->line + line_length + taken, have - taken + 1U);
	input->used = have - taken;

	/* The text whole already: its request. */
	if (input->text_used == input->text_length)
		phoneio_text_done(input, events);

	/* Succeeded. */
	return 0;
}

/* Gives a whole text with its line to the daemon, then frees it. */
static void
phoneio_text_done(
	struct btd_phoneio_input *input,
	const struct btd_phoneio_events *events)
{
	uint8_t *text;
	size_t length;

	/* The text, out of the input (the request may close the client). */
	text = input->text;
	length = input->text_length;
	input->text = NULL;
	input->text_length = 0U;
	input->text_used = 0U;

	/* Succeeded: the request, then the text freed. */
	events->text(events->context, input->send_line, text, length);
	free(text);
}

/* Gives a hex digit's value, or -1 for another character. */
static int
phoneio_hex(
	char letter)
{
	/* A decimal digit. */
	if (letter >= '0' && letter <= '9')
		return letter - '0';

	/* A lower-case letter. */
	if (letter >= 'a' && letter <= 'f')
		return letter - 'a' + 10;

	/* An upper-case letter. */
	if (letter >= 'A' && letter <= 'F')
		return letter - 'A' + 10;

	/* Not a hex digit. */
	return -1;
}
