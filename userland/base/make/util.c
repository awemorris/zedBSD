/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The allocation, the messages and the growable buffer of make.
 *
 * Running out of memory ends make with status 2, as any other fatal
 * error does, so the rest of the program never checks an allocation.
 * Messages carry the program name as GNU make words it: "make:" at the
 * top and "make[N]:" in a recursive make.
 */

#include "make.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

static void out_of_memory(void);

/*
 * Allocates memory, ending make when there is none.
 */
void *
make_malloc(
	size_t size)
{
	void *memory;

	/* At least one byte, so that NULL always means failure. */
	if (size == 0)
		size = 1;
	memory = malloc(size);
	if (memory == NULL)
		out_of_memory();

	/* Succeeded. */
	return memory;
}

/*
 * Resizes memory, ending make when there is none.
 */
void *
make_realloc(
	void *memory,
	size_t size)
{
	void *resized;

	/* At least one byte, so that NULL always means failure. */
	if (size == 0)
		size = 1;
	resized = realloc(memory, size);
	if (resized == NULL)
		out_of_memory();

	/* Succeeded. */
	return resized;
}

/*
 * Returns an allocated copy of a string.
 */
char *
make_strdup(
	const char *text)
{
	size_t length;
	char *copy;

	/* The text and its terminating null. */
	length = strlen(text);
	copy = make_strndup(text, length);

	/* Succeeded. */
	return copy;
}

/*
 * Returns an allocated copy of the first length bytes of a text.
 */
char *
make_strndup(
	const char *text,
	size_t length)
{
	char *copy;

	/* The bytes, then a terminating null. */
	copy = make_malloc(length + 1U);
	memcpy(copy, text, length);
	copy[length] = '\0';

	/* Succeeded. */
	return copy;
}

/*
 * Returns the name messages start with: "make", or "make[N]" in a
 * recursive make.
 */
const char *
make_program(
	void)
{
	static char name[32];

	/* The level is shown below the top. */
	if (make_level == 0)
		return "make";

	/* Succeeded: the name with the level. */
	snprintf(name, sizeof(name), "make[%d]", make_level);
	return name;
}

/*
 * Writes a message to standard error, after the program name.
 */
void
make_message(
	const char *format,
	...)
{
	va_list arguments;

	/* What make has written so far goes out first, in order. */
	fflush(stdout);

	/* The program name, the message and a newline. */
	va_start(arguments, format);
	fprintf(stderr, "%s: ", make_program());
	vfprintf(stderr, format, arguments);
	fputc('\n', stderr);
	va_end(arguments);
}

/*
 * Reports an error that stops make ("make: *** ...  Stop.") and ends it
 * with status 2.
 */
void
make_fatal(
	const char *format,
	...)
{
	va_list arguments;

	/* What make has written so far goes out first, in order. */
	fflush(stdout);

	/* The message in GNU make's words. */
	va_start(arguments, format);
	fprintf(stderr, "%s: *** ", make_program());
	vfprintf(stderr, format, arguments);
	fputs(".  Stop.\n", stderr);
	va_end(arguments);
	exit(2);
}

/*
 * Reports an error at a line of a makefile ("file:line: *** ...  Stop.")
 * and ends make with status 2.
 */
void
make_file_fatal(
	const char *file,
	long line,
	const char *format,
	...)
{
	va_list arguments;

	/* What make has written so far goes out first, in order. */
	fflush(stdout);

	/* The place, then the message in GNU make's words. */
	va_start(arguments, format);
	fprintf(stderr, "%s:%ld: *** ", file, line);
	vfprintf(stderr, format, arguments);
	fputs(".  Stop.\n", stderr);
	va_end(arguments);
	exit(2);
}

/*
 * Appends bytes to a buffer, keeping it terminated.
 */
void
buffer_add(
	struct buffer *buffer,
	const char *text,
	size_t length)
{
	size_t capacity;

	/* Room for the bytes and the terminator, doubling the size. */
	if (buffer->length + length + 1U > buffer->capacity) {
		capacity = buffer->capacity;
		if (capacity == 0)
			capacity = 64U;
		while (buffer->length + length + 1U > capacity)
			capacity *= 2U;
		buffer->text = make_realloc(buffer->text, capacity);
		buffer->capacity = capacity;
	}

	/* The bytes, and the terminator after them. */
	memcpy(buffer->text + buffer->length, text, length);
	buffer->length += length;
	buffer->text[buffer->length] = '\0';
}

/*
 * Appends a string to a buffer.
 */
void
buffer_add_string(
	struct buffer *buffer,
	const char *text)
{
	size_t length;

	/* The whole string. */
	length = strlen(text);
	buffer_add(buffer, text, length);
}

/*
 * Appends one character to a buffer.
 */
void
buffer_add_char(
	struct buffer *buffer,
	char character)
{
	/* A text of one byte. */
	buffer_add(buffer, &character, 1);
}

/*
 * Returns the text of a buffer, which the caller now owns, and empties
 * the buffer.  An empty buffer gives an empty string.
 */
char *
buffer_finish(
	struct buffer *buffer)
{
	char *text;

	/* Nothing was added: an empty string. */
	if (buffer->text == NULL)
		return make_strdup("");

	/* Succeeded: the text, and the buffer starts again. */
	text = buffer->text;
	buffer->text = NULL;
	buffer->length = 0;
	buffer->capacity = 0;
	return text;
}

/*
 * Returns the text of a buffer without taking it: an empty string when
 * nothing was added.
 */
const char *
buffer_text(
	const struct buffer *buffer)
{
	/* Nothing was added. */
	if (buffer->text == NULL)
		return "";

	/* Succeeded. */
	return buffer->text;
}

/*
 * Reports whether a character is a blank (a space or a tab), which is
 * what separates words in make.
 */
int
make_is_blank(
	char character)
{
	/* A space or a tab. */
	if (character == ' ' || character == '\t')
		return 1;

	/* Anything else, newline included. */
	return 0;
}

/*
 * Returns the next word of a text at *cursor and its length, moving the
 * cursor past it; returns NULL when no word is left.  Words are separated
 * by blanks and newlines.
 */
const char *
make_next_word(
	const char **cursor,
	size_t *length)
{
	const char *start;
	const char *end;

	/* Past the separators before the word. */
	start = *cursor;
	while (*start == ' ' || *start == '\t' || *start == '\n')
		start++;
	if (*start == '\0') {
		*cursor = start;
		return NULL;
	}

	/* To the end of the word. */
	end = start;
	while (*end != '\0' && *end != ' ' && *end != '\t' && *end != '\n')
		end++;

	/* Succeeded: the word, and the cursor after it. */
	*length = (size_t)(end - start);
	*cursor = end;
	return start;
}

/* Reports that memory ran out and ends make. */
static void
out_of_memory(
	void)
{
	/* The message, then the status of any other fatal error. */
	fflush(stdout);
	fprintf(stderr, "%s: *** virtual memory exhausted.  Stop.\n", make_program());
	exit(2);
}
