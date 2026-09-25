/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The read builtin (POSIX XCU read): read [-r] [-d delim] name...
 *
 * A line is read from standard input a byte at a time, so that nothing past
 * it is taken from a shared descriptor.  Without -r a backslash quotes the
 * next character and a backslash-newline joins lines.  The line is split by
 * IFS as field splitting does: a run of IFS whitespace is one separator, and
 * other IFS characters (with the whitespace around them) are one each.  The
 * last name takes the rest of the line, less trailing IFS whitespace.
 */

#include "userland/base/sh/shell.h"
#include "userland/base/sh/vars.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* What a character of the line is to the splitting. */
#define READ_FIELD	0	/* part of a field (or quoted) */
#define READ_WHITE	1	/* IFS whitespace */
#define READ_OTHER	2	/* another IFS character */

/* The line read: its characters, which were quoted, and their classes. */
struct read_line {
	char *text;
	unsigned char *escaped;
	unsigned char *class;
	size_t length;
	size_t capacity;
};

static int read_options(int argc, char **argv, int *index, int *raw, char *delimiter);
static int read_line(struct read_line *line, int raw, char delimiter);
static void read_append(struct read_line *line, char value, int escaped);
static void read_classify(struct read_line *line);
static void read_assign(struct read_line *line, char **names, int count);
static size_t read_skip_white(const struct read_line *line, size_t position);
static size_t read_field_end(const struct read_line *line, size_t position);
static size_t read_separator_end(const struct read_line *line, size_t position);
static size_t read_trim_white(const struct read_line *line, size_t start, size_t end);

/*
 * Implements read.  Returns 0 when a whole line was read, 1 at the end of
 * the input, 2 for a usage error.
 */
int
sh_builtin_read(
	int argc,
	char **argv)
{
	struct read_line line;
	char delimiter;
	int index;
	int raw;
	int valid;
	int name;
	int status;

	/* -r and -d; at least one name. */
	valid = read_options(argc, argv, &index, &raw, &delimiter);
	if (!valid)
		return 2;
	if (index >= argc) {
		fprintf(stderr, "read: arg count\n");
		return 2;
	}

	/* Every name must be a variable name. */
	for (name = index; name < argc; name++) {
		valid = sh_var_name(argv[name]);
		if (!valid) {
			fprintf(stderr, "read: %s: bad variable name\n",
				argv[name]);
			return 2;
		}
	}

	/* The line, split among the names. */
	status = read_line(&line, raw, delimiter);
	read_classify(&line);
	read_assign(&line, argv + index, argc - index);

	/* Succeeded. */
	return status;
}

/*
 * Reads read's options: -r keeps backslashes as they are; -d names the
 * character that ends the line (in the same word or the next).  *index is
 * left at the first name.  Returns 0 (after a message) for a bad option.
 */
static int
read_options(
	int argc,
	char **argv,
	int *index,
	int *raw,
	char *delimiter)
{
	const char *word;
	const char *letter;

	/* Backslashes escape and a newline ends the line, unless an option says otherwise. */
	*raw = 0;
	*delimiter = '\n';
	for (*index = 1; *index < argc; (*index)++) {
		/* A name, or - alone, ends the options. */
		word = argv[*index];
		if (word[0] != '-' || word[1] == '\0')
			break;

		/* -- ends them too, and is not a name. */
		if (word[1] == '-' && word[2] == '\0') {
			(*index)++;
			break;
		}

		/* The letters of the word. */
		for (letter = word + 1; *letter != '\0'; letter++) {
			/* -r. */
			if (*letter == 'r') {
				*raw = 1;
				continue;
			}

			/* Anything but -d is an error. */
			if (*letter != 'd') {
				fprintf(stderr, "read: Illegal option %s\n",
					word);
				return 0;
			}

			/* -d takes the rest of the word, or the next word. */
			if (letter[1] != '\0') {
				*delimiter = letter[1];
			} else if (*index + 1 < argc) {
				(*index)++;
				*delimiter = argv[*index][0];
			} else {
				*delimiter = '\0';
			}

			break;
		}
	}

	/* Succeeded. */
	return 1;
}

/*
 * Reads the line into a temporary buffer: a byte at a time, up to the
 * delimiter.  NUL bytes are dropped.  Returns 0 when the delimiter was
 * read, 1 at the end of the input (or when a trap interrupted the read).
 */
static int
read_line(
	struct read_line *line,
	int raw,
	char delimiter)
{
	ssize_t count;
	char value;
	int error;

	/* An empty line to start. */
	line->capacity = 128;
	line->length = 0;
	line->text = sh_temp_own(sh_malloc(line->capacity));
	line->escaped = sh_temp_own(sh_malloc(line->capacity));
	line->class = NULL;

	/* Reads a byte at a time, so that nothing past the line is taken from the input. */
	for (;;) {
		/* The next byte; a signal with a trap ends the read. */
		count = read(0, &value, 1);
		if (count < 0) {
			error = errno;
			if (error == EINTR && !sh_trap_pending_any)
				continue;
			break;
		}

		/* The end of the input ends the line. */
		if (count == 0)
			break;

		/* The delimiter ends the line. */
		if (value == delimiter) {
			line->text[line->length] = '\0';
			return 0;
		}

		/* A NUL byte is dropped. */
		if (value == '\0')
			continue;

		/* An ordinary character, or a backslash kept by -r. */
		if (value != '\\' || raw) {
			read_append(line, value, 0);
			continue;
		}

		/* A backslash quotes the next character, and joins lines. */
		count = read(0, &value, 1);
		if (count <= 0)
			break;
		if (value == '\n')
			continue;
		read_append(line, value, 1);
	}

	/* The end of the input. */
	line->text[line->length] = '\0';
	return 1;
}

/* Appends a character to the line, with whether it was quoted. */
static void
read_append(
	struct read_line *line,
	char value,
	int escaped)
{
	size_t capacity;

	/* Room for it and the terminating NUL. */
	if (line->length + 2U > line->capacity) {
		capacity = line->capacity * 2U;
		line->text = sh_temp_grow(line->text, line->length, capacity);
		line->escaped = sh_temp_grow(line->escaped, line->length,
					     capacity);
		line->capacity = capacity;
	}

	/* The character. */
	line->text[line->length] = value;
	line->escaped[line->length] = (unsigned char)escaped;
	line->length++;
}

/* Classifies each character of the line by IFS. */
static void
read_classify(
	struct read_line *line)
{
	const char *ifs;
	const char *found;
	size_t position;
	char value;

	/* IFS, or its default when it is unset. */
	ifs = sh_var_get("IFS");
	if (ifs == NULL)
		ifs = " \t\n";

	/* Each character: quoted ones and those not in IFS are field text. */
	line->class = sh_temp_own(sh_malloc(line->length + 1U));
	for (position = 0; position < line->length; position++) {
		value = line->text[position];
		line->class[position] = READ_FIELD;
		if (line->escaped[position])
			continue;
		found = strchr(ifs, value);
		if (found == NULL)
			continue;
		if (value == ' ' || value == '\t' || value == '\n')
			line->class[position] = READ_WHITE;
		else
			line->class[position] = READ_OTHER;
	}
}

/* Assigns the fields of the line to the names; the last takes the rest. */
static void
read_assign(
	struct read_line *line,
	char **names,
	int count)
{
	char *field;
	size_t position;
	size_t start;
	size_t end;
	int name;
	int error;

	/* Each name takes a field from the start of the line on. */
	position = 0;
	for (name = 0; name < count; name++) {
		/* Leading IFS whitespace is skipped. */
		position = read_skip_white(line, position);
		start = position;

		/* The last name takes the rest of the line; any other one field. */
		if (name == count - 1) {
			/* The last name: the rest, less trailing whitespace. */
			end = read_trim_white(line, start, line->length);
			position = line->length;
		} else {
			/* Another name: one field, then its separator. */
			end = read_field_end(line, position);
			position = read_separator_end(line, end);
		}

		/* The field is assigned. */
		field = sh_temp_own(sh_strndup(line->text + start,
					       end - start));
		error = sh_var_set(names[name], field, 0);
		if (error != 0)
			sh_error("read: %s: is read only", names[name]);
	}
}

/* Returns the position after a run of IFS whitespace. */
static size_t
read_skip_white(
	const struct read_line *line,
	size_t position)
{
	/* The whitespace. */
	while (position < line->length && line->class[position] == READ_WHITE)
		position++;

	/* Succeeded: the first other character. */
	return position;
}

/* Returns the end of a field: the first IFS character. */
static size_t
read_field_end(
	const struct read_line *line,
	size_t position)
{
	/* The field's characters. */
	while (position < line->length && line->class[position] == READ_FIELD)
		position++;

	/* Succeeded: the separator, or the end. */
	return position;
}

/*
 * Returns the end of a separator: IFS whitespace, then at most one other IFS
 * character.  (The whitespace after it is skipped before the next field.)
 */
static size_t
read_separator_end(
	const struct read_line *line,
	size_t position)
{
	/* The whitespace. */
	position = read_skip_white(line, position);

	/* One other IFS character. */
	if (position < line->length && line->class[position] == READ_OTHER)
		position++;

	/* Succeeded: the start of what follows. */
	return position;
}

/* Returns where trailing IFS whitespace starts between start and end. */
static size_t
read_trim_white(
	const struct read_line *line,
	size_t start,
	size_t end)
{
	/* The whitespace, from the end. */
	while (end > start && line->class[end - 1] == READ_WHITE)
		end--;

	/* Succeeded: the end of the text. */
	return end;
}
