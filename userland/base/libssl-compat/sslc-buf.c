/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The reader and writer of the TLS presentation language.
 */

#include "sslc-buf.h"

/*
 * Starts a reader over size bytes (data may be NULL only when size is 0).
 */
void
sslc_reader_init(
	struct sslc_reader *reader,
	const uint8_t *data,
	size_t size)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(reader);
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(size);
}

/*
 * Returns the bytes left to read (0 after an error).
 */
size_t
sslc_reader_left(
	const struct sslc_reader *reader)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(reader);

	/* Not implemented: the failure value. */
	return 0;
}

/*
 * Reads a 1-byte integer (0 and error on the end).
 */
uint32_t
sslc_reader_u8(
	struct sslc_reader *reader)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(reader);

	/* Not implemented: the failure value. */
	return 0;
}

/*
 * Reads a 2-byte big-endian integer.
 */
uint32_t
sslc_reader_u16(
	struct sslc_reader *reader)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(reader);

	/* Not implemented: the failure value. */
	return 0;
}

/*
 * Reads a 3-byte big-endian integer.
 */
uint32_t
sslc_reader_u24(
	struct sslc_reader *reader)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(reader);

	/* Not implemented: the failure value. */
	return 0;
}

/*
 * Reads a 4-byte big-endian integer.
 */
uint32_t
sslc_reader_u32(
	struct sslc_reader *reader)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(reader);

	/* Not implemented: the failure value. */
	return 0;
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
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(reader);
	UNUSED_PARAMETER(size);

	/* Not implemented: the failure value. */
	return NULL;
}

/*
 * Reads a vector: a length prefix of prefix bytes (1, 2 or 3), then that
 * many bytes, given as a new reader over them (3.4: <floor..ceiling>).
 */
int
sslc_reader_vector(
	struct sslc_reader *reader,
	unsigned prefix,
	size_t min,
	size_t max,
	struct sslc_reader *inner)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(reader);
	UNUSED_PARAMETER(prefix);
	UNUSED_PARAMETER(min);
	UNUSED_PARAMETER(max);
	UNUSED_PARAMETER(inner);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
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
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(reader);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}

/*
 * Starts a writer into room bytes at data.
 */
void
sslc_writer_init(
	struct sslc_writer *writer,
	uint8_t *data,
	size_t room)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(writer);
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(room);
}

/*
 * Writes a 1-byte integer (the low 8 bits of value).
 */
void
sslc_writer_u8(
	struct sslc_writer *writer,
	uint32_t value)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(writer);
	UNUSED_PARAMETER(value);
}

/*
 * Writes a 2-byte big-endian integer (the low 16 bits).
 */
void
sslc_writer_u16(
	struct sslc_writer *writer,
	uint32_t value)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(writer);
	UNUSED_PARAMETER(value);
}

/*
 * Writes a 3-byte big-endian integer (the low 24 bits).
 */
void
sslc_writer_u24(
	struct sslc_writer *writer,
	uint32_t value)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(writer);
	UNUSED_PARAMETER(value);
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
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(writer);
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(size);
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
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(writer);
	UNUSED_PARAMETER(prefix);
}

/*
 * Ends the innermost vector: writes its length into its prefix.
 */
void
sslc_writer_end(
	struct sslc_writer *writer)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(writer);
}

/*
 * Returns 0 when no error was set and no vector is left open, else EINVAL
 * (a vector left open, a too-long vector) or ENOBUFS (room exhausted).
 */
int
sslc_writer_finish(
	const struct sslc_writer *writer)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(writer);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}
