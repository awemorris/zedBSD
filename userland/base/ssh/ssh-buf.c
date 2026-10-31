/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The RFC 4251 data types written into a growing buffer and read with bounds checks.
 *
 * Skeleton: every function answers ENOSYS (or does nothing) until it is
 * implemented.
 */

#include "ssh-buf.h"

/*
 * Starts an empty buffer that may grow to limit bytes (1 or more);
 * secret as in struct ssh_buf.
 */
void
ssh_buf_init(
	struct ssh_buf *buf,
	size_t limit,
	int secret)
{
	/* Marks the arguments used until the function is implemented. */
	(void)buf;
	(void)limit;
	(void)secret;
}

/*
 * Frees the memory (wiping it first when secret) and leaves the buffer
 * empty and usable, error cleared.
 */
void
ssh_buf_free(
	struct ssh_buf *buf)
{
	/* Marks the arguments used until the function is implemented. */
	(void)buf;
}

/*
 * Empties the buffer (wiping the used bytes when secret), keeping its
 * memory; clears error.
 */
void
ssh_buf_reset(
	struct ssh_buf *buf)
{
	/* Marks the arguments used until the function is implemented. */
	(void)buf;
}

/*
 * Makes room for size more bytes without writing them.
 */
int
ssh_buf_reserve(
	struct ssh_buf *buf,
	size_t size)
{
	/* Marks the arguments used until the function is implemented. */
	(void)buf;
	(void)size;

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Appends one byte.
 */
void
ssh_buf_put_u8(
	struct ssh_buf *buf,
	uint8_t value)
{
	/* Marks the arguments used until the function is implemented. */
	(void)buf;
	(void)value;
}

/*
 * Appends a boolean as one byte, 1 for any non-zero value.
 */
void
ssh_buf_put_bool(
	struct ssh_buf *buf,
	int value)
{
	/* Marks the arguments used until the function is implemented. */
	(void)buf;
	(void)value;
}

/*
 * Appends a uint32, big-endian.
 */
void
ssh_buf_put_u32(
	struct ssh_buf *buf,
	uint32_t value)
{
	/* Marks the arguments used until the function is implemented. */
	(void)buf;
	(void)value;
}

/*
 * Appends a uint64, big-endian.
 */
void
ssh_buf_put_u64(
	struct ssh_buf *buf,
	uint64_t value)
{
	/* Marks the arguments used until the function is implemented. */
	(void)buf;
	(void)value;
}

/*
 * Appends size raw bytes (no length); data may be NULL only when size is
 * 0.
 */
void
ssh_buf_put_bytes(
	struct ssh_buf *buf,
	const void *data,
	size_t size)
{
	/* Marks the arguments used until the function is implemented. */
	(void)buf;
	(void)data;
	(void)size;
}

/*
 * Appends a string: the uint32 length, then size bytes.
 */
void
ssh_buf_put_string(
	struct ssh_buf *buf,
	const void *data,
	size_t size)
{
	/* Marks the arguments used until the function is implemented. */
	(void)buf;
	(void)data;
	(void)size;
}

/*
 * Appends a NUL-terminated C string as a string (without the NUL).
 */
void
ssh_buf_put_cstring(
	struct ssh_buf *buf,
	const char *text)
{
	/* Marks the arguments used until the function is implemented. */
	(void)buf;
	(void)text;
}

/*
 * Reserves room for size bytes at the end, counts them as used, and
 * returns where they start, for a caller that fills them itself (an
 * encryption in place).
 */
uint8_t *
ssh_buf_put_space(
	struct ssh_buf *buf,
	size_t size)
{
	/* Marks the arguments used until the function is implemented. */
	(void)buf;
	(void)size;

	/* Reports that the function is not implemented yet. */
	return NULL;
}

/*
 * Removes size bytes from the front (bytes already sent, or input
 * already parsed), moving the rest down.
 */
void
ssh_buf_consume(
	struct ssh_buf *buf,
	size_t size)
{
	/* Marks the arguments used until the function is implemented. */
	(void)buf;
	(void)size;
}

/*
 * Returns the error recorded (0 while none).
 */
int
ssh_buf_error(
	const struct ssh_buf *buf)
{
	/* Marks the arguments used until the function is implemented. */
	(void)buf;

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Starts a reader over size bytes at data (data may be NULL only when
 * size is 0).
 */
void
ssh_reader_init(
	struct ssh_reader *reader,
	const uint8_t *data,
	size_t size)
{
	/* Marks the arguments used until the function is implemented. */
	(void)reader;
	(void)data;
	(void)size;
}

/*
 * Reads one byte.
 */
uint8_t
ssh_reader_get_u8(
	struct ssh_reader *reader)
{
	/* Marks the arguments used until the function is implemented. */
	(void)reader;

	/* Reports that the function is not implemented yet. */
	return 0;
}

/*
 * Reads a boolean: 0 for a zero byte, 1 for any other (RFC 4251 5).
 */
int
ssh_reader_get_bool(
	struct ssh_reader *reader)
{
	/* Marks the arguments used until the function is implemented. */
	(void)reader;

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Reads a big-endian uint32.
 */
uint32_t
ssh_reader_get_u32(
	struct ssh_reader *reader)
{
	/* Marks the arguments used until the function is implemented. */
	(void)reader;

	/* Reports that the function is not implemented yet. */
	return 0;
}

/*
 * Reads a big-endian uint64.
 */
uint64_t
ssh_reader_get_u64(
	struct ssh_reader *reader)
{
	/* Marks the arguments used until the function is implemented. */
	(void)reader;

	/* Reports that the function is not implemented yet. */
	return 0;
}

/*
 * Reads size raw bytes and returns where they start inside the reader's
 * data (NULL with the error recorded when fewer remain).
 */
const uint8_t *
ssh_reader_get_bytes(
	struct ssh_reader *reader,
	size_t size)
{
	/* Marks the arguments used until the function is implemented. */
	(void)reader;
	(void)size;

	/* Reports that the function is not implemented yet. */
	return NULL;
}

/*
 * Reads a string: *data points to its bytes inside the reader's data and
 * size receives its length.
 */
void
ssh_reader_get_string(
	struct ssh_reader *reader,
	const uint8_t **data,
	size_t *size)
{
	/* Marks the arguments used until the function is implemented. */
	(void)reader;
	(void)data;
	(void)size;
}

/*
 * Reads a string into out as a NUL-terminated C string.
 */
void
ssh_reader_get_cstring(
	struct ssh_reader *reader,
	char *out,
	size_t capacity)
{
	/* Marks the arguments used until the function is implemented. */
	(void)reader;
	(void)out;
	(void)capacity;
}

/*
 * Reads a string that must be a name-list (ssh_namelist_check): *list
 * and size as in ssh_reader_get_string.
 */
void
ssh_reader_get_namelist(
	struct ssh_reader *reader,
	const char **list,
	size_t *size)
{
	/* Marks the arguments used until the function is implemented. */
	(void)reader;
	(void)list;
	(void)size;
}

/*
 * Returns the number of bytes not yet read (0 after an error).
 */
size_t
ssh_reader_left(
	const struct ssh_reader *reader)
{
	/* Marks the arguments used until the function is implemented. */
	(void)reader;

	/* Reports that the function is not implemented yet. */
	return 0;
}

/*
 * Returns the error recorded, or EINVAL when no error is recorded but
 * bytes remain unread (a message with trailing bytes), or 0 when the
 * reader ended exactly at its end.
 */
int
ssh_reader_finish(
	const struct ssh_reader *reader)
{
	/* Marks the arguments used until the function is implemented. */
	(void)reader;

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Returns the error recorded (0 while none), whether or not bytes
 * remain.
 */
int
ssh_reader_error(
	const struct ssh_reader *reader)
{
	/* Marks the arguments used until the function is implemented. */
	(void)reader;

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}
