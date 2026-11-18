/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Merges corresponding or subsequent lines of files (POSIX XCU paste).
 *
 *	paste [-s] [-d list] file...
 *
 * Without -s the files are read side by side: the lines with the same
 * number are joined into one, separated by the delimiters of the list in
 * turn (a tab by default), until every file ends; a file that ended gives
 * empty lines.  With -s each file's lines are joined into one line.  The
 * list may hold \n, \t, \\ and \0 (no delimiter).  - is standard input and
 * may be given more than once; each takes the next line of it.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The delimiter list; an entry of 0 length is \0, no delimiter. */
struct delimiters {
	char characters[256];
	unsigned char empty[256];
	size_t count;
};

/* A line being read. */
struct line {
	char *data;
	size_t length;
	size_t capacity;
};

static int read_options(int argc, char **argv, int *serial, struct delimiters *delimiters);
static void parse_delimiters(const char *list, struct delimiters *delimiters);
static void write_delimiter(const struct delimiters *delimiters, size_t index);
static int paste_parallel(FILE **streams, int count, const struct delimiters *delimiters);
static void paste_serial(FILE *stream, const struct delimiters *delimiters);
static int read_line(FILE *stream, struct line *line);
static void usage(void);

/*
 * Runs paste.
 */
int
main(
	int argc,
	char **argv)
{
	struct delimiters delimiters;
	FILE **streams;
	int serial;
	int first;
	int count;
	int index;
	int compare;
	int status;

	/* The options; at least one file. */
	first = read_options(argc, argv, &serial, &delimiters);
	count = argc - first;
	if (count < 1)
		usage();

	/* The files; - is standard input. */
	streams = calloc((size_t)count, sizeof(*streams));
	if (streams == NULL) {
		fprintf(stderr, "paste: out of memory\n");
		return 1;
	}

	/* Each file; - is standard input. */
	for (index = 0; index < count; index++) {
		compare = strcmp(argv[first + index], "-");
		streams[index] = stdin;
		if (compare != 0)
			streams[index] = fopen(argv[first + index], "r");
		if (streams[index] == NULL) {
			fprintf(stderr, "paste: %s: %s\n", argv[first + index],
				strerror(errno));
			return 1;
		}
	}

	/* Side by side. */
	if (!serial) {
		status = paste_parallel(streams, count, &delimiters);
		return status;
	}

	/* Succeeded: one file after another. */
	for (index = 0; index < count; index++)
		paste_serial(streams[index], &delimiters);
	return 0;
}

/* Reads the options; returns the index of the first file. */
static int
read_options(
	int argc,
	char **argv,
	int *serial,
	struct delimiters *delimiters)
{
	const char *word;
	const char *letter;
	const char *argument;
	int index;

	/* No -s, and a tab between lines. */
	*serial = 0;
	parse_delimiters("\t", delimiters);
	for (index = 1; index < argc; index++) {
		/* A file, or - alone, ends the options; so does --. */
		word = argv[index];
		if (word[0] != '-' || word[1] == '\0')
			break;
		if (word[1] == '-' && word[2] == '\0')
			return index + 1;

		/* Each letter of the word. */
		for (letter = word + 1; *letter != '\0'; letter++) {
			/* -s. */
			if (*letter == 's') {
				*serial = 1;
				continue;
			}

			/* Any other letter but -d is not an option. */
			if (*letter != 'd')
				usage();

			/* -d list: the rest of the word, or the next word. */
			argument = letter + 1;
			if (*argument == '\0') {
				if (index + 1 >= argc)
					usage();
				index++;
				argument = argv[index];
			}

			/* The list. */
			parse_delimiters(argument, delimiters);
			break;
		}
	}

	/* Succeeded: the first file. */
	return index;
}

/* Parses a delimiter list with its escapes. */
static void
parse_delimiters(
	const char *list,
	struct delimiters *delimiters)
{
	char value;
	int empty;

	/* Each character of the list. */
	memset(delimiters, 0, sizeof(*delimiters));
	while (*list != '\0' && delimiters->count < sizeof(delimiters->characters)) {
		/* A character, or an escape. */
		value = *list;
		empty = 0;
		if (value == '\\' && list[1] != '\0') {
			list++;
			value = *list;
			if (value == 'n')
				value = '\n';
			else if (value == 't')
				value = '\t';
			else if (value == '0')
				empty = 1;
		}

		/* The delimiter, added. */
		delimiters->characters[delimiters->count] = value;
		delimiters->empty[delimiters->count] = (unsigned char)empty;
		delimiters->count++;
		list++;
	}

	/* An empty list is no delimiter at all. */
	if (delimiters->count == 0) {
		delimiters->empty[0] = 1;
		delimiters->count = 1;
	}
}

/* Writes the delimiter at a place in the list, which it goes round. */
static void
write_delimiter(
	const struct delimiters *delimiters,
	size_t index)
{
	size_t at;

	/* The entry, unless it is \0. */
	at = index % delimiters->count;
	if (!delimiters->empty[at])
		putchar(delimiters->characters[at]);
}

/*
 * Joins the lines with the same number from every file, until every file
 * has ended.
 */
static int
paste_parallel(
	FILE **streams,
	int count,
	const struct delimiters *delimiters)
{
	struct line *lines;
	unsigned char *have;
	int remaining;
	int index;
	int read;

	/* A line buffer and a flag for each file. */
	lines = calloc((size_t)count, sizeof(*lines));
	have = calloc((size_t)count, 1);
	if (lines == NULL || have == NULL) {
		fprintf(stderr, "paste: out of memory\n");
		return 1;
	}

	/* Until every file has ended. */
	for (;;) {
		/* A line from each file that has not ended. */
		remaining = 0;
		for (index = 0; index < count; index++) {
			read = 0;
			if (streams[index] != NULL)
				read = read_line(streams[index], &lines[index]);
			if (!read)
				streams[index] = NULL;
			have[index] = (unsigned char)read;
			if (read)
				remaining = 1;
		}

		/* Every file has ended. */
		if (!remaining)
			break;

		/* The lines, with the delimiters between them. */
		for (index = 0; index < count; index++) {
			if (index > 0)
				write_delimiter(delimiters, (size_t)index - 1U);
			if (have[index] && lines[index].length > 0) {
				fwrite(lines[index].data, 1, lines[index].length,
				       stdout);
			}
		}

		/* The joined line ends. */
		putchar('\n');
	}

	/* The buffers go. */
	for (index = 0; index < count; index++)
		free(lines[index].data);
	free(lines);
	free(have);
	return 0;
}

/* Joins every line of one file into one line (-s). */
static void
paste_serial(
	FILE *stream,
	const struct delimiters *delimiters)
{
	struct line line;
	size_t index;
	int read;

	/* Each line of the file. */
	memset(&line, 0, sizeof(line));
	for (index = 0;; index++) {
		/* The next line, after the delimiter. */
		read = read_line(stream, &line);
		if (!read)
			break;
		if (index > 0)
			write_delimiter(delimiters, index - 1U);
		if (line.length > 0)
			fwrite(line.data, 1, line.length, stdout);
	}

	/* Succeeded: the line ends. */
	putchar('\n');
	free(line.data);
}

/* Reads one line, without its newline.  Returns 0 at the end. */
static int
read_line(
	FILE *stream,
	struct line *line)
{
	int value;
	int newline;

	/* Bytes up to the newline. */
	line->length = 0;
	newline = 0;
	for (;;) {
		/* The next byte. */
		value = getc(stream);
		if (value == EOF)
			break;
		if (value == '\n') {
			newline = 1;
			break;
		}

		/* Room for it. */
		if (line->length + 1U > line->capacity) {
			line->capacity = line->capacity * 2U + 128U;
			line->data = realloc(line->data, line->capacity);
			if (line->data == NULL) {
				fprintf(stderr, "paste: out of memory\n");
				exit(1);
			}
		}

		/* The byte. */
		line->data[line->length] = (char)value;
		line->length++;
	}

	/* The end of the input. */
	if (line->length == 0 && !newline)
		return 0;
	return 1;
}

/* Reports the usage and ends paste. */
static void
usage(
	void)
{
	/* The form. */
	fprintf(stderr, "usage: paste [-s] [-d list] file...\n");
	exit(1);
}
