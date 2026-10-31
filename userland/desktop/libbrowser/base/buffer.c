/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The growable buffers: bytes (with a NUL kept past the end) and UTF-16
 * code units.
 */

#include "base/base.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The smallest capacity a buffer grows to, in bytes or units. */
#define BUFFER_MIN_CAPACITY	64U

static size_t buffer_grown(size_t capacity, size_t needed);

/*
 * Prepares an empty byte buffer.
 */
void
wb_buffer_init(
	struct wb_buffer *buffer)
{
	/* An empty buffer owns no memory until the first byte arrives. */
	buffer->data = NULL;
	buffer->length = 0;
	buffer->capacity = 0;
}

/*
 * Makes room for extra more bytes and the terminating NUL.
 */
int
wb_buffer_reserve(
	struct wb_buffer *buffer,
	size_t extra)
{
	unsigned char *data;
	size_t needed;
	size_t capacity;

	/* Refuses a request whose size overflows. */
	if (extra > (size_t)-1 - buffer->length - 1U)
		return ENOMEM;
	needed = buffer->length + extra + 1U;

	/* Nothing to do when the room is already there. */
	if (needed <= buffer->capacity)
		return 0;

	/* Grows the storage geometrically. */
	capacity = buffer_grown(buffer->capacity, needed);
	data = realloc(buffer->data, capacity);
	if (data == NULL)
		return ENOMEM;

	/* Publishes the larger storage. */
	buffer->data = data;
	buffer->capacity = capacity;

	/* Succeeded: extra bytes and a NUL fit. */
	return 0;
}

/*
 * Appends length bytes.
 */
int
wb_buffer_append(
	struct wb_buffer *buffer,
	const void *bytes,
	size_t length)
{
	int error;

	/* Makes room for the bytes. */
	error = wb_buffer_reserve(buffer, length);
	if (error != 0)
		return error;

	/* Copies them and keeps the NUL after them. */
	if (length != 0)
		memcpy(buffer->data + buffer->length, bytes, length);
	buffer->length += length;
	buffer->data[buffer->length] = '\0';

	/* Succeeded: the bytes are at the end. */
	return 0;
}

/*
 * Appends one byte.
 */
int
wb_buffer_append_byte(
	struct wb_buffer *buffer,
	unsigned char byte)
{
	int error;

	/* Appends the byte as a run of one. */
	error = wb_buffer_append(buffer, &byte, 1);
	if (error != 0)
		return error;

	/* Succeeded: the byte is at the end. */
	return 0;
}

/*
 * Appends a NUL-terminated string without its NUL.
 */
int
wb_buffer_append_string(
	struct wb_buffer *buffer,
	const char *string)
{
	size_t length;
	int error;

	/* Appends the string's bytes. */
	length = strlen(string);
	error = wb_buffer_append(buffer, string, length);
	if (error != 0)
		return error;

	/* Succeeded: the string is at the end. */
	return 0;
}

/*
 * Appends one code point in UTF-8.
 */
int
wb_buffer_append_utf8(
	struct wb_buffer *buffer,
	uint32_t code_point)
{
	unsigned char bytes[4];
	size_t length;
	int error;

	/* Encodes the code point and appends its bytes. */
	length = wb_utf8_encode(code_point, bytes);
	error = wb_buffer_append(buffer, bytes, length);
	if (error != 0)
		return error;

	/* Succeeded: the code point is at the end. */
	return 0;
}

/*
 * Appends text formatted as by printf.
 */
int
wb_buffer_printf(
	struct wb_buffer *buffer,
	const char *format,
	...)
{
	va_list arguments;
	int length;
	int error;

	/* Measures the text. */
	va_start(arguments, format);
	length = vsnprintf(NULL, 0, format, arguments);
	va_end(arguments);
	if (length < 0)
		return EINVAL;

	/* Makes room for it and its NUL. */
	error = wb_buffer_reserve(buffer, (size_t)length);
	if (error != 0)
		return error;

	/* Writes it at the end. */
	va_start(arguments, format);
	vsnprintf((char *)buffer->data + buffer->length, (size_t)length + 1U, format, arguments);
	va_end(arguments);
	buffer->length += (size_t)length;

	/* Succeeded: the text is at the end. */
	return 0;
}

/*
 * Empties the buffer and keeps its storage.
 */
void
wb_buffer_clear(
	struct wb_buffer *buffer)
{
	/* Drops the contents; the NUL moves to the start. */
	buffer->length = 0;
	if (buffer->data != NULL)
		buffer->data[0] = '\0';
}

/*
 * Frees the buffer's storage.
 */
void
wb_buffer_release(
	struct wb_buffer *buffer)
{
	/* Frees the bytes and leaves an empty buffer. */
	free(buffer->data);
	wb_buffer_init(buffer);
}

/*
 * Reads the buffer as a C string ("" when it owns no memory).
 */
const char *
wb_buffer_string(
	const struct wb_buffer *buffer)
{
	/* An empty buffer without storage reads as the empty string. */
	if (buffer->data == NULL)
		return "";

	/* Reports the bytes, which a NUL always follows. */
	return (const char *)buffer->data;
}

/*
 * Finds the first place a run of bytes appears in others: haystack itself
 * for an empty run, NULL when it does not appear.
 */
const void *
wb_memmem(
	const void *haystack,
	size_t length,
	const void *needle,
	size_t needle_length)
{
	const unsigned char *bytes;
	size_t index;
	int differs;

	/* An empty run is found where the bytes start; a longer run than the bytes nowhere. */
	bytes = haystack;
	if (needle_length == 0)
		return bytes;
	if (length < needle_length)
		return NULL;

	/* Each place the run could start. */
	for (index = 0; index + needle_length <= length; index++) {
		differs = memcmp(bytes + index, needle, needle_length);
		if (differs == 0)
			return bytes + index;
	}

	/* Not there. */
	return NULL;
}

/*
 * Prepares an empty UTF-16 buffer.
 */
void
wb_units_init(
	struct wb_units *units)
{
	/* An empty buffer owns no memory until the first unit arrives. */
	units->data = NULL;
	units->length = 0;
	units->capacity = 0;
}

/*
 * Makes room for extra more code units.
 */
int
wb_units_reserve(
	struct wb_units *units,
	size_t extra)
{
	uint16_t *data;
	size_t needed;
	size_t capacity;

	/* Refuses a request whose size overflows. */
	if (extra > ((size_t)-1) / 2U - units->length)
		return ENOMEM;
	needed = units->length + extra;

	/* Nothing to do when the room is already there. */
	if (needed <= units->capacity)
		return 0;

	/*
	 * Grows the storage geometrically.  A doubling whose byte count would
	 * not fit in size_t falls back to the need itself, which fits because
	 * the need is at most half of SIZE_MAX (BUG-133).
	 */
	capacity = buffer_grown(units->capacity, needed);
	if (capacity > ((size_t)-1) / sizeof(*data))
		capacity = needed;
	data = realloc(units->data, capacity * sizeof(*data));
	if (data == NULL)
		return ENOMEM;

	/* Publishes the larger storage. */
	units->data = data;
	units->capacity = capacity;

	/* Succeeded: extra units fit. */
	return 0;
}

/*
 * Appends length code units.
 */
int
wb_units_append(
	struct wb_units *units,
	const uint16_t *source,
	size_t length)
{
	int error;

	/* Makes room for the units. */
	error = wb_units_reserve(units, length);
	if (error != 0)
		return error;

	/* Copies them to the end. */
	if (length != 0)
		memcpy(units->data + units->length, source, length * sizeof(*source));
	units->length += length;

	/* Succeeded: the units are at the end. */
	return 0;
}

/*
 * Appends one code point as one or two UTF-16 code units.
 */
int
wb_units_append_code_point(
	struct wb_units *units,
	uint32_t code_point)
{
	uint16_t encoded[2];
	size_t length;
	int error;

	/* Encodes the code point and appends its units. */
	length = wb_utf16_encode(code_point, encoded);
	error = wb_units_append(units, encoded, length);
	if (error != 0)
		return error;

	/* Succeeded: the code point is at the end. */
	return 0;
}

/*
 * Empties the UTF-16 buffer and keeps its storage.
 */
void
wb_units_clear(
	struct wb_units *units)
{
	/* Drops the contents. */
	units->length = 0;
}

/*
 * Frees the UTF-16 buffer's storage.
 */
void
wb_units_release(
	struct wb_units *units)
{
	/* Frees the units and leaves an empty buffer. */
	free(units->data);
	wb_units_init(units);
}

/* Picks the capacity a buffer grows to so that it holds at least needed items. */
static size_t
buffer_grown(
	size_t capacity,
	size_t needed)
{
	size_t grown;

	/* Doubles from the current size (or the minimum) until the need fits. */
	grown = capacity;
	if (grown < BUFFER_MIN_CAPACITY)
		grown = BUFFER_MIN_CAPACITY;
	while (grown < needed) {
		/* Stops doubling where doubling would overflow. */
		if (grown > ((size_t)-1) / 2U) {
			grown = needed;
			break;
		}

		/* Doubles the capacity. */
		grown *= 2U;
	}

	/* Reports the new capacity. */
	return grown;
}
