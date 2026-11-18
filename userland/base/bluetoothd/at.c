/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The AT lines of HFP (ws197-p006): cutting the audio gateway's bytes into
 * lines and reading the parts of a result.  HFP 1.8 section 4.33 gives the
 * form: results end in CR LF, fields are separated by commas, strings are
 * in double quotes without escapes.
 */

#include "userland/base/bluetoothd/at.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static const char *at_field_start(const char *text, unsigned index);
static const char *at_field_end(const char *text);

/*
 * Starts the cutting of lines with nothing read.
 */
void
btd_at_input_init(
	struct btd_at_input *input)
{
	/* No line yet. */
	memset(input, 0, sizeof(*input));
}

/*
 * Takes the bytes the audio gateway sent: each line ended by CR or LF is
 * given to line (empty ones skipped); a line past BTD_AT_LINE_MAX bytes is
 * dropped whole and counted; a NUL byte ends nothing and is dropped.
 */
void
btd_at_input(
	struct btd_at_input *input,
	const uint8_t *data,
	size_t length,
	btd_at_line_fn line,
	void *context)
{
	size_t index;
	uint8_t byte;

	/* Each byte. */
	for (index = 0U; index < length; index++) {
		byte = data[index];

		/* A line's end: the line given, unless it was empty or too long. */
		if (byte == '\r' || byte == '\n') {
			if (input->overlong) {
				input->dropped++;
			} else if (input->used != 0U) {
				input->line[input->used] = '\0';
				line(context, input->line);
			}

			/* The next line starts empty. */
			input->used = 0U;
			input->overlong = 0;
			continue;
		}

		/* A NUL means nothing in a line of text. */
		if (byte == '\0')
			continue;

		/* A line past the room is dropped at its end. */
		if (input->used >= BTD_AT_LINE_MAX) {
			input->overlong = 1;
			continue;
		}

		/* The byte kept. */
		input->line[input->used] = (char)byte;
		input->used++;
	}
}

/*
 * Tells whether a line is the result name ("+CIND") followed by a colon;
 * *rest is then what follows the colon and its spaces.  Returns 1 or 0.
 */
int
btd_at_result(
	const char *line,
	const char *name,
	const char **rest)
{
	size_t length;
	int same;

	/* The name, then the colon. */
	length = strlen(name);
	same = strncmp(line, name, length);
	if (same != 0)
		return 0;
	if (line[length] != ':')
		return 0;

	/* What follows, past its spaces. */
	line += length + 1U;
	while (*line == ' ')
		line++;
	*rest = line;

	/* It is the result. */
	return 1;
}

/*
 * Copies a result's field (0 the first) without the spaces around it and,
 * when it is quoted, without its quotes.  Returns 0, ENOENT when there is
 * no such field, ENOSPC when it does not fit (out is then empty).
 */
int
btd_at_field(
	const char *text,
	unsigned index,
	char *out,
	size_t size)
{
	const char *start;
	const char *end;
	size_t length;

	/* Nothing copied yet. */
	if (size == 0U)
		return ENOSPC;
	out[0] = '\0';

	/* The field's start. */
	start = at_field_start(text, index);
	if (start == NULL)
		return ENOENT;

	/* Its end, the spaces around it left out. */
	end = at_field_end(start);
	while (start < end && *start == ' ')
		start++;
	while (end > start && end[-1] == ' ')
		end--;

	/* A quoted field without its quotes. */
	if (end - start >= 2 && *start == '"' && end[-1] == '"') {
		start++;
		end--;
	}

	/* Copied when it fits. */
	length = (size_t)(end - start);
	if (length >= size)
		return ENOSPC;
	memcpy(out, start, length);
	out[length] = '\0';

	/* Succeeded: the field. */
	return 0;
}

/*
 * Reads a result's field (0 the first) as a decimal number.  Returns 0,
 * ENOENT when there is no such field, EINVAL when it is not a number.
 */
int
btd_at_number(
	const char *text,
	unsigned index,
	long *value)
{
	char field[24];
	char *end;
	long number;
	int error;

	/* The field. */
	error = btd_at_field(text, index, field, sizeof(field));
	if (error == ENOSPC)
		return EINVAL;
	if (error != 0)
		return error;

	/* Digits alone, with a sign at most. */
	if (field[0] == '\0')
		return EINVAL;
	number = strtol(field, &end, 10);
	if (*end != '\0')
		return EINVAL;

	/* Succeeded: the number. */
	*value = number;
	return 0;
}

/*
 * Reads the indicators' names of +CIND=? ("(\"call\",(0,1)),(\"callsetup\",(0-3)),...")
 * in their order: each group's quoted name, lowercased, cut to
 * BTD_AT_NAME_MAX - 1 bytes.  Returns 0 with *count of them, or EINVAL for
 * a group without a name or more groups than capacity.
 */
int
btd_at_indicators(
	const char *text,
	char names[][BTD_AT_NAME_MAX],
	size_t capacity,
	size_t *count)
{
	const char *at;
	const char *quote;
	size_t length;
	size_t letter;
	size_t found;
	unsigned depth;

	/* Each top-level group, its first quoted string its name. */
	found = 0U;
	depth = 0U;
	for (at = text; *at != '\0'; at++) {
		/* Into a group: a top-level one starts an indicator. */
		if (*at == '(') {
			depth++;
			if (depth != 1U)
				continue;

			/* Its name, the quoted string at its start. */
			quote = at + 1;
			while (*quote == ' ')
				quote++;
			if (*quote != '"')
				return EINVAL;
			quote++;
			length = strcspn(quote, "\"");
			if (quote[length] != '"')
				return EINVAL;

			/* Room for it. */
			if (found >= capacity)
				return EINVAL;

			/* Kept lowercased, cut to the room. */
			if (length >= BTD_AT_NAME_MAX)
				length = BTD_AT_NAME_MAX - 1U;
			for (letter = 0U; letter < length; letter++) {
				names[found][letter] = quote[letter];
				if (quote[letter] >= 'A' && quote[letter] <= 'Z')
					names[found][letter] = (char)(quote[letter] - 'A' + 'a');
			}

			/* Ended, and counted. */
			names[found][length] = '\0';
			found++;
			continue;
		}

		/* Out of a group. */
		if (*at == ')' && depth != 0U)
			depth--;
	}

	/* Succeeded: the names in order. */
	*count = found;
	return 0;
}

/* Finds where a result's field starts (0 the first), past the commas outside quotes and parentheses; NULL when there is none. */
static const char *
at_field_start(
	const char *text,
	unsigned index)
{
	const char *at;
	unsigned field;

	/* Each field's end in turn. */
	at = text;
	for (field = 0U; field < index; field++) {
		at = at_field_end(at);
		if (*at != ',')
			return NULL;
		at++;
	}

	/* Succeeded: the field's start. */
	return at;
}

/* Finds the end of the field that starts at text: its comma (outside quotes and parentheses) or the text's end. */
static const char *
at_field_end(
	const char *text)
{
	const char *at;
	unsigned depth;
	int quoted;

	/* Up to a comma that is neither quoted nor in parentheses. */
	depth = 0U;
	quoted = 0;
	for (at = text; *at != '\0'; at++) {
		if (*at == '"') {
			quoted = !quoted;
		} else if (quoted) {
			continue;
		} else if (*at == '(') {
			depth++;
		} else if (*at == ')' && depth != 0U) {
			depth--;
		} else if (*at == ',' && depth == 0U) {
			break;
		}
	}

	/* Succeeded: the comma, or the end. */
	return at;
}
