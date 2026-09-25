/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The files and commands an awk program reads and writes by name: the
 * output of print and printf redirected with >, >> and |, the input of
 * getline < file and command | getline, and close, fflush and system.
 *
 * A name stays open, and is the same stream, until close names it.  Every
 * output is flushed before a command starts, so that what was written
 * before comes out before what the command writes.
 */

#include "userland/base/awk/awk.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The kinds of stream. */
#define STREAM_OUTPUT_FILE	0	/* > file and >> file */
#define STREAM_OUTPUT_PIPE	1	/* | command */
#define STREAM_INPUT_FILE	2	/* getline < file */
#define STREAM_INPUT_PIPE	3	/* command | getline */

/*
 * A file or a command open by name.  standard is set for the standard
 * streams, which are named but never closed.
 */
struct stream {
	char *name;
	size_t length;
	int kind;
	FILE *file;
	int standard;
	struct stream *next;
};

/*
 * The streams open by name, the most recently opened first.  A stream is
 * added when a name is first used and removed by close; the rest are
 * closed when awk ends.
 */
static struct stream *streams;

static struct stream *find_stream(const char *name, size_t length, int input);
static struct stream *add_stream(const char *name, size_t length, int kind, FILE *file, int standard);
static int close_stream(struct stream *stream);
static void flush_all(void);
static int is_named(const char *name, size_t length, const char *standard_name);

/*
 * Returns the stream print or printf writes to for a redirection, opening
 * it the first time the name is used.
 */
FILE *
io_output(
	int output,
	const char *name,
	size_t length)
{
	struct stream *stream;
	FILE *file;
	int named;

	/* A name already open for output. */
	stream = find_stream(name, length, 0);
	if (stream != NULL)
		return stream->file;

	/* The standard output and the standard error. */
	named = is_named(name, length, "-");
	if (!named)
		named = is_named(name, length, "/dev/stdout");
	if (named) {
		stream = add_stream(name, length, STREAM_OUTPUT_FILE, stdout, 1);
		return stream->file;
	}

	/* The standard error. */
	named = is_named(name, length, "/dev/stderr");
	if (named) {
		stream = add_stream(name, length, STREAM_OUTPUT_FILE, stderr, 1);
		return stream->file;
	}

	/* A command, started after what was written before is out. */
	if (output == OUTPUT_PIPE) {
		flush_all();
		file = popen(name, "w");
		if (file == NULL)
			awk_fatal("can't open pipe to %s (%s)", name, strerror(errno));
		stream = add_stream(name, length, STREAM_OUTPUT_PIPE, file, 0);
		return stream->file;
	}

	/* A file, emptied by > and added to by >>. */
	if (output == OUTPUT_APPEND)
		file = fopen(name, "a");
	else
		file = fopen(name, "w");
	if (file == NULL)
		awk_fatal("can't redirect to %s (%s)", name, strerror(errno));

	/* Succeeded. */
	stream = add_stream(name, length, STREAM_OUTPUT_FILE, file, 0);
	return stream->file;
}

/*
 * Reads a record for getline from a file or a command, opening it the
 * first time the name is used.  Returns 1 for a record, 0 at the end, and
 * -1 when it cannot be opened.
 */
int
io_getline(
	int source,
	const char *name,
	size_t length,
	struct buffer *buffer)
{
	struct stream *stream;
	FILE *file;
	int named;
	int read;

	/* The stream, opened when the name is new. */
	stream = find_stream(name, length, 1);
	if (stream == NULL) {
		if (source == GETLINE_COMMAND) {
			flush_all();
			file = popen(name, "r");
			if (file == NULL)
				return -1;
			stream = add_stream(name, length, STREAM_INPUT_PIPE, file, 0);
		} else {
			named = is_named(name, length, "-");
			if (!named)
				named = is_named(name, length, "/dev/stdin");
			if (named) {
				stream = add_stream(name, length, STREAM_INPUT_FILE, stdin, 1);
			} else {
				file = fopen(name, "r");
				if (file == NULL)
					return -1;
				stream = add_stream(name, length, STREAM_INPUT_FILE, file, 0);
			}
		}
	}

	/* Succeeded: the record, or the end. */
	read = record_read(stream->file, buffer);
	return read;
}

/*
 * Closes every stream of a name.  Returns 0, or -1 when there is none or
 * closing it failed.
 */
int
io_close(
	const char *name,
	size_t length)
{
	struct stream **link;
	struct stream *stream;
	int closed;
	int result;
	int found;
	int named;
	int compare;

	/* Each stream of the name, out of the list. */
	result = 0;
	found = 0;
	link = &streams;
	while (*link != NULL) {
		stream = *link;
		named = 0;
		if (stream->length == length) {
			compare = memcmp(stream->name, name, length);
			if (compare == 0)
				named = 1;
		}

		/* Another name stays. */
		if (!named) {
			link = &stream->next;
			continue;
		}

		/* The name's stream is closed. */
		*link = stream->next;
		found = 1;
		closed = close_stream(stream);
		if (closed != 0)
			result = -1;
	}

	/* A name that is not open. */
	if (!found)
		return -1;

	/* Succeeded: whether every stream closed. */
	return result;
}

/*
 * Flushes the output of a name, or all output when name is NULL.  Returns
 * 0, or -1 when the name is not open for output.
 */
int
io_flush(
	const char *name,
	size_t length)
{
	struct stream *stream;
	int named;

	/* Everything. */
	if (name == NULL) {
		flush_all();
		return 0;
	}

	/* The standard output by its names. */
	named = is_named(name, length, "/dev/stdout");
	if (named) {
		fflush(stdout);
		return 0;
	}

	/* A name open for output. */
	stream = find_stream(name, length, 0);
	if (stream == NULL)
		return -1;

	/* Succeeded. */
	fflush(stream->file);
	return 0;
}

/*
 * Runs a command with the shell after flushing all output, and returns
 * its wait status as it is (as gawk does in POSIX mode), or -1.
 */
int
io_system(
	const char *command)
{
	int status;

	/* What was written before comes out before the command's output. */
	flush_all();

	/* Succeeded: the status. */
	status = system(command);
	return status;
}

/*
 * Closes every stream still open, waiting for the commands, as awk ends.
 */
void
io_close_all(
	void)
{
	struct stream *stream;

	/* Each stream, off the list. */
	while (streams != NULL) {
		stream = streams;
		streams = stream->next;
		close_stream(stream);
	}
}

/* Finds a stream of a name, for input or for output. */
static struct stream *
find_stream(
	const char *name,
	size_t length,
	int input)
{
	struct stream *stream;
	int stream_input;
	int compare;

	/* The streams of the same direction and name. */
	for (stream = streams; stream != NULL; stream = stream->next) {
		stream_input = 0;
		if (stream->kind == STREAM_INPUT_FILE || stream->kind == STREAM_INPUT_PIPE)
			stream_input = 1;
		if (stream_input != input || stream->length != length)
			continue;
		compare = memcmp(stream->name, name, length);
		if (compare == 0)
			return stream;
	}

	/* None. */
	return NULL;
}

/* Adds an open stream to the list. */
static struct stream *
add_stream(
	const char *name,
	size_t length,
	int kind,
	FILE *file,
	int standard)
{
	struct stream *stream;

	/* The stream, at the head of the list. */
	stream = awk_allocate(sizeof(*stream));
	stream->name = awk_copy(name, length);
	stream->length = length;
	stream->kind = kind;
	stream->file = file;
	stream->standard = standard;
	stream->next = streams;
	streams = stream;

	/* Succeeded. */
	return stream;
}

/* Closes a stream that is off the list and frees it; returns 0 or -1. */
static int
close_stream(
	struct stream *stream)
{
	int status;
	int result;

	/* A standard stream is only flushed; a command is waited for. */
	result = 0;
	if (stream->standard) {
		fflush(stream->file);
	} else if (stream->kind == STREAM_OUTPUT_PIPE || stream->kind == STREAM_INPUT_PIPE) {
		status = pclose(stream->file);
		if (status == -1)
			result = -1;
	} else {
		status = fclose(stream->file);
		if (status != 0)
			result = -1;
	}

	/* Succeeded: the stream goes. */
	free(stream->name);
	free(stream);
	return result;
}

/* Flushes the standard output, the standard error and every output. */
static void
flush_all(
	void)
{
	struct stream *stream;

	/* The standard streams, then the named ones. */
	fflush(stdout);
	fflush(stderr);
	for (stream = streams; stream != NULL; stream = stream->next) {
		if (stream->kind == STREAM_OUTPUT_FILE || stream->kind == STREAM_OUTPUT_PIPE)
			fflush(stream->file);
	}
}

/* Returns whether a name is a given name. */
static int
is_named(
	const char *name,
	size_t length,
	const char *standard_name)
{
	size_t standard_length;
	int compare;

	/* The same length and the same bytes. */
	standard_length = strlen(standard_name);
	if (standard_length != length)
		return 0;
	compare = memcmp(name, standard_name, length);
	if (compare != 0)
		return 0;

	/* Succeeded: the same. */
	return 1;
}
