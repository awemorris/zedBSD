/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Writes the lines of files in reverse order (as GNU tac does; tac is not
 * in POSIX).
 *
 *	tac [file...]
 *
 * Each file (- or none is standard input) is read whole, and its lines are
 * written from the last to the first.  A last line without a newline is
 * written as it is, so that it joins the line written after it, as GNU tac
 * does.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The contents of one file. */
struct contents {
	char *data;
	size_t length;
	size_t capacity;
};

static int reverse_file(const char *name);
static int read_all(FILE *stream, struct contents *contents);
static void write_reversed(const struct contents *contents);

/*
 * Runs tac.
 */
int
main(
	int argc,
	char **argv)
{
	int index;
	int failed;
	int error;
	int dashes;

	/* Standard input when no file is named. */
	if (argc < 2) {
		error = reverse_file("-");
		if (error != 0)
			return 1;
		return 0;
	}

	/* Each file in turn; -- ends the options. */
	failed = 0;
	index = 1;
	dashes = strcmp(argv[1], "--");
	if (dashes == 0)
		index = 2;
	for (; index < argc; index++) {
		error = reverse_file(argv[index]);
		if (error != 0)
			failed = 1;
	}

	/* Some file could not be read. */
	if (failed)
		return 1;

	/* Succeeded. */
	return 0;
}

/* Writes the lines of one file in reverse order; returns 0 or -1. */
static int
reverse_file(
	const char *name)
{
	struct contents contents;
	FILE *stream;
	int compare;
	int error;

	/* The file; - is standard input. */
	compare = strcmp(name, "-");
	stream = stdin;
	if (compare != 0)
		stream = fopen(name, "r");
	if (stream == NULL) {
		fprintf(stderr, "tac: %s: %s\n", name, strerror(errno));
		return -1;
	}

	/* Its contents. */
	memset(&contents, 0, sizeof(contents));
	error = read_all(stream, &contents);
	if (stream != stdin)
		fclose(stream);
	if (error != 0) {
		fprintf(stderr, "tac: %s: %s\n", name, strerror(errno));
		free(contents.data);
		return -1;
	}

	/* Succeeded: the lines, last first. */
	write_reversed(&contents);
	free(contents.data);
	return 0;
}

/* Reads a stream to its end; returns 0 or -1. */
static int
read_all(
	FILE *stream,
	struct contents *contents)
{
	size_t count;
	char *larger;
	int failed;

	/* Blocks until the end, growing the buffer as needed. */
	for (;;) {
		if (contents->length == contents->capacity) {
			contents->capacity = contents->capacity * 2U + 4096U;
			larger = realloc(contents->data, contents->capacity);
			if (larger == NULL)
				return -1;
			contents->data = larger;
		}

		/* The next block of the stream. */
		count = fread(contents->data + contents->length, 1,
			      contents->capacity - contents->length, stream);
		if (count == 0)
			break;
		contents->length += count;
	}

	/* A read error. */
	failed = ferror(stream);
	if (failed)
		return -1;

	/* Succeeded. */
	return 0;
}

/* Writes the lines from the last to the first. */
static void
write_reversed(
	const struct contents *contents)
{
	size_t end;
	size_t start;

	/* Each line ends after its newline; the last may have none. */
	end = contents->length;
	while (end > 0) {
		start = end - 1U;
		while (start > 0 && contents->data[start - 1U] != '\n')
			start--;
		fwrite(contents->data + start, 1, end - start, stdout);
		end = start;
	}
}
