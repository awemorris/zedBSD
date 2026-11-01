/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The reader and writer of the TLS presentation language.
 */

#include <string.h>

#include "sslc-buf.h"

/* The largest value a length prefix of 3 bytes holds, plus one. */
#define SSLC_BUF_U24_LIMIT	16777216U

/* Reads a big-endian integer of width bytes (1 to 4) and advances. */
static uint32_t sslc_reader_integer(struct sslc_reader *reader, size_t width);

/* Writes the low width bytes (1 to 3) of value, big-endian, at the end of the used part. */
static void sslc_writer_integer(struct sslc_writer *writer, uint32_t value, size_t width);

/* Marks a writer failed with the first error only. */
static void sslc_writer_fail(struct sslc_writer *writer, int error);

/*
 * Starts a reader over size bytes (data may be NULL only when size is 0).
 *
 * A NULL data with a size above 0 is a caller's mistake: the reader is
 * empty and its error is set.
 */
void
sslc_reader_init(
	struct sslc_reader *reader,
	const uint8_t *data,
	size_t size)
{
	/* Starts with no position and no error. */
	reader->data = data;
	reader->size = size;
	reader->position = 0;
	reader->error = 0;

	/* Refuses bytes that do not exist. */
	if (data == NULL && size != 0) {
		reader->size = 0;
		reader->error = EINVAL;
	}
}

/*
 * Returns the bytes left to read (0 after an error).
 */
size_t
sslc_reader_left(
	const struct sslc_reader *reader)
{
	/* After an error nothing is left to read. */
	if (reader->error != 0)
		return 0;

	/* The bytes between the position and the end. */
	return reader->size - reader->position;
}

/*
 * Reads a 1-byte integer (0 and error on the end).
 */
uint32_t
sslc_reader_u8(
	struct sslc_reader *reader)
{
	uint32_t value;

	/* Reads one byte. */
	value = sslc_reader_integer(reader, 1);

	/* The value read, or 0 on an error. */
	return value;
}

/*
 * Reads a 2-byte big-endian integer.
 */
uint32_t
sslc_reader_u16(
	struct sslc_reader *reader)
{
	uint32_t value;

	/* Reads two bytes. */
	value = sslc_reader_integer(reader, 2);

	/* The value read, or 0 on an error. */
	return value;
}

/*
 * Reads a 3-byte big-endian integer.
 */
uint32_t
sslc_reader_u24(
	struct sslc_reader *reader)
{
	uint32_t value;

	/* Reads three bytes. */
	value = sslc_reader_integer(reader, 3);

	/* The value read, or 0 on an error. */
	return value;
}

/*
 * Reads a 4-byte big-endian integer.
 */
uint32_t
sslc_reader_u32(
	struct sslc_reader *reader)
{
	uint32_t value;

	/* Reads four bytes. */
	value = sslc_reader_integer(reader, 4);

	/* The value read, or 0 on an error. */
	return value;
}

/*
 * Gives a view of the next size bytes and advances past them; returns NULL
 * (and sets error) when fewer are left.
 */
const uint8_t *
sslc_reader_bytes(
	struct sslc_reader *reader,
	size_t size)
{
	const uint8_t *view;

	/* A reader that already failed gives nothing. */
	if (reader->error != 0)
		return NULL;

	/* Refuses a view past the end. */
	if (size > reader->size - reader->position) {
		reader->error = EINVAL;
		return NULL;
	}

	/* An empty reader over no memory has no pointer to give. */
	if (reader->data == NULL)
		return NULL;

	/* Takes the view and moves past it. */
	view = reader->data + reader->position;
	reader->position += size;

	/* The view of the bytes. */
	return view;
}

/*
 * Reads a vector: a length prefix of prefix bytes (1, 2 or 3), then that
 * many bytes, given as a new reader over them (3.4: <floor..ceiling>).
 *
 * The length must lie between min and max and fit the bytes left.  On a
 * failure the outer reader's error is set and *inner is an empty reader
 * with its error set.
 */
int
sslc_reader_vector(
	struct sslc_reader *reader,
	unsigned prefix,
	size_t min,
	size_t max,
	struct sslc_reader *inner)
{
	const uint8_t *view;
	uint32_t length;

	/* Starts the inner reader as the empty failed reader of the error returns. */
	inner->data = NULL;
	inner->size = 0;
	inner->position = 0;
	inner->error = EINVAL;

	/* A prefix size other than 1, 2 or 3 is a caller's mistake. */
	if (prefix < 1U || prefix > 3U) {
		reader->error = EINVAL;
		return EINVAL;
	}

	/* Reads the length prefix. */
	length = sslc_reader_integer(reader, prefix);
	if (reader->error != 0)
		return EINVAL;

	/* Refuses a length below the floor. */
	if ((size_t)length < min) {
		reader->error = EINVAL;
		return EINVAL;
	}

	/* Refuses a length above the ceiling. */
	if ((size_t)length > max) {
		reader->error = EINVAL;
		return EINVAL;
	}

	/* Takes the vector's bytes; a length past the end sets the error. */
	view = sslc_reader_bytes(reader, (size_t)length);
	if (reader->error != 0)
		return EINVAL;

	/* Hands the bytes to the inner reader. */
	inner->data = view;
	inner->size = (size_t)length;
	inner->error = 0;

	/* Succeeded: the inner reader covers the vector. */
	return 0;
}

/*
 * Requires the reader to be at its end without an error: returns 0, or
 * EINVAL when bytes are left or an error was set (a message with trailing
 * bytes is malformed, 4.1.2 and the like).
 */
int
sslc_reader_finish(
	const struct sslc_reader *reader)
{
	/* A reader that failed did not read a whole message. */
	if (reader->error != 0)
		return EINVAL;

	/* Bytes left over are trailing bytes. */
	if (reader->position != reader->size)
		return EINVAL;

	/* Succeeded: the reader ended exactly at its end. */
	return 0;
}

/*
 * Starts a writer into room bytes at data.
 *
 * A NULL data with a room above 0 is a caller's mistake: the writer has no
 * room and its error is set.
 */
void
sslc_writer_init(
	struct sslc_writer *writer,
	uint8_t *data,
	size_t room)
{
	unsigned index;

	/* Starts empty with no vector open and no error. */
	writer->data = data;
	writer->room = room;
	writer->used = 0;
	writer->depth = 0;
	writer->error = 0;

	/* Clears the open vectors. */
	for (index = 0;
	     index < SSLC_WRITER_DEPTH;
	     index++) {
		writer->open[index] = 0;
		writer->prefix[index] = 0;
	}

	/* Refuses room that does not exist. */
	if (data == NULL && room != 0) {
		writer->room = 0;
		writer->error = EINVAL;
	}
}

/*
 * Writes a 1-byte integer (the low 8 bits of value).
 */
void
sslc_writer_u8(
	struct sslc_writer *writer,
	uint32_t value)
{
	/* Writes one byte. */
	sslc_writer_integer(writer, value, 1);
}

/*
 * Writes a 2-byte big-endian integer (the low 16 bits).
 */
void
sslc_writer_u16(
	struct sslc_writer *writer,
	uint32_t value)
{
	/* Writes two bytes. */
	sslc_writer_integer(writer, value, 2);
}

/*
 * Writes a 3-byte big-endian integer (the low 24 bits).
 */
void
sslc_writer_u24(
	struct sslc_writer *writer,
	uint32_t value)
{
	/* Writes three bytes. */
	sslc_writer_integer(writer, value, 3);
}

/*
 * Writes size bytes (data may be NULL only when size is 0).
 */
void
sslc_writer_bytes(
	struct sslc_writer *writer,
	const uint8_t *data,
	size_t size)
{
	/* A writer that failed does nothing more. */
	if (writer->error != 0)
		return;

	/* Bytes that do not exist cannot be copied. */
	if (data == NULL && size != 0) {
		sslc_writer_fail(writer, EINVAL);
		return;
	}

	/* Refuses bytes that do not fit the room left; nothing is written. */
	if (size > writer->room - writer->used) {
		sslc_writer_fail(writer, ENOBUFS);
		return;
	}

	/* Nothing to copy (and possibly no pointers to copy with). */
	if (size == 0)
		return;

	/* Copies the bytes after those already written. */
	memcpy(writer->data + writer->used, data, size);
	writer->used += size;
}

/*
 * Begins a vector with a length prefix of prefix bytes (1, 2 or 3): the
 * prefix's room is written as zeros and filled by sslc_writer_end.
 */
void
sslc_writer_begin(
	struct sslc_writer *writer,
	unsigned prefix)
{
	size_t index;

	/* A writer that failed does nothing more. */
	if (writer->error != 0)
		return;

	/* A prefix size other than 1, 2 or 3 is a caller's mistake. */
	if (prefix < 1U || prefix > 3U) {
		sslc_writer_fail(writer, EINVAL);
		return;
	}

	/* Refuses a vector nested deeper than the writer keeps. */
	if (writer->depth >= SSLC_WRITER_DEPTH) {
		sslc_writer_fail(writer, EINVAL);
		return;
	}

	/* Refuses a prefix that does not fit the room left. */
	if ((size_t)prefix > writer->room - writer->used) {
		sslc_writer_fail(writer, ENOBUFS);
		return;
	}

	/* Remembers where the prefix is and how wide it is. */
	writer->open[writer->depth] = writer->used;
	writer->prefix[writer->depth] = prefix;
	writer->depth++;

	/* Reserves the prefix as zeros, to be filled when the vector ends. */
	for (index = 0;
	     index < (size_t)prefix;
	     index++)
		writer->data[writer->used + index] = 0;
	writer->used += (size_t)prefix;
}

/*
 * Ends the innermost vector: writes its length into its prefix.
 */
void
sslc_writer_end(
	struct sslc_writer *writer)
{
	size_t start;
	size_t width;
	size_t length;
	size_t index;

	/* A writer that failed does nothing more. */
	if (writer->error != 0)
		return;

	/* An end with no vector begun is a caller's mistake. */
	if (writer->depth == 0) {
		sslc_writer_fail(writer, EINVAL);
		return;
	}

	/* Takes the innermost vector off the stack and measures its contents. */
	writer->depth--;
	start = writer->open[writer->depth];
	width = (size_t)writer->prefix[writer->depth];
	length = writer->used - start - width;

	/* Refuses contents longer than the prefix can count. */
	if (width == 1U && length >= 256U) {
		sslc_writer_fail(writer, EINVAL);
		return;
	}
	if (width == 2U && length >= 65536U) {
		sslc_writer_fail(writer, EINVAL);
		return;
	}
	if (width == 3U && length >= SSLC_BUF_U24_LIMIT) {
		sslc_writer_fail(writer, EINVAL);
		return;
	}

	/* Writes the length into the prefix, most significant byte first. */
	for (index = 0;
	     index < width;
	     index++) {
		writer->data[start + index] =
		    (uint8_t)((length >> (8U * (width - 1U - index))) & 0xffU);
	}
}

/*
 * Returns 0 when no error was set and no vector is left open, else EINVAL
 * (a vector left open, a too-long vector) or ENOBUFS (room exhausted).
 */
int
sslc_writer_finish(
	const struct sslc_writer *writer)
{
	/* Reports the first error the writer met. */
	if (writer->error != 0)
		return writer->error;

	/* A vector left open has no length yet. */
	if (writer->depth != 0)
		return EINVAL;

	/* Succeeded: everything written and every vector closed. */
	return 0;
}

/*
 * Reads a big-endian integer of width bytes (1 to 4) and advances; 0 and
 * the error on a short input.
 */
static uint32_t
sslc_reader_integer(
	struct sslc_reader *reader,
	size_t width)
{
	uint32_t value;
	size_t index;

	/* A reader that already failed reads zeros. */
	if (reader->error != 0)
		return 0;

	/* Refuses an integer that runs past the end. */
	if (width > reader->size - reader->position) {
		reader->error = EINVAL;
		return 0;
	}

	/* Accumulates the bytes, most significant first. */
	value = 0;
	for (index = 0;
	     index < width;
	     index++) {
		value = (value << 8) | (uint32_t)reader->data[reader->position + index];
	}
	reader->position += width;

	/* The integer read. */
	return value;
}

/*
 * Writes the low width bytes (1 to 3) of value, big-endian.
 */
static void
sslc_writer_integer(
	struct sslc_writer *writer,
	uint32_t value,
	size_t width)
{
	size_t index;

	/* A writer that failed does nothing more. */
	if (writer->error != 0)
		return;

	/* Refuses an integer that does not fit the room left; nothing is written. */
	if (width > writer->room - writer->used) {
		sslc_writer_fail(writer, ENOBUFS);
		return;
	}

	/* Stores the bytes, most significant first. */
	for (index = 0;
	     index < width;
	     index++) {
		writer->data[writer->used + index] =
		    (uint8_t)((value >> (8U * (width - 1U - index))) & 0xffU);
	}
	writer->used += width;
}

/*
 * Marks a writer failed with the first error only.
 */
static void
sslc_writer_fail(
	struct sslc_writer *writer,
	int error)
{
	/* Keeps the first error: later ones follow from it. */
	if (writer->error == 0)
		writer->error = error;
}
