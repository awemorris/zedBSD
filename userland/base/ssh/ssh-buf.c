/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The RFC 4251 data types written into a growing buffer and read with bounds checks.
 */

#include <stdlib.h>
#include <string.h>

#include "ssh-buf.h"
#include "ssh-crypto.h"

/*
 * The capacity a buffer first grows to, in bytes, when it has none; later
 * growth doubles the capacity.  The limit of the buffer still wins.
 */
#define BUF_FIRST_CAPACITY	64U

static void reader_fail(struct ssh_reader *reader, size_t start, int error);

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
	/* Starts with no memory, nothing used and no error. */
	buf->data = NULL;
	buf->size = 0;
	buf->capacity = 0;
	buf->limit = limit;
	buf->error = 0;
	buf->secret = secret;
}

/*
 * Frees the memory (wiping it first when secret) and leaves the buffer
 * empty and usable, error cleared.
 */
void
ssh_buf_free(
	struct ssh_buf *buf)
{
	/* Wipes everything the memory may have held before it is given back. */
	if (buf->secret != 0 && buf->data != NULL)
		ssh_wipe(buf->data, buf->capacity);

	/* Gives the memory back and leaves the buffer with none. */
	free(buf->data);
	buf->data = NULL;
	buf->size = 0;
	buf->capacity = 0;
	buf->error = 0;
}

/*
 * Empties the buffer (wiping the used bytes when secret), keeping its
 * memory; clears error.
 */
void
ssh_buf_reset(
	struct ssh_buf *buf)
{
	/* Wipes the bytes that were used. */
	if (buf->secret != 0 && buf->data != NULL)
		ssh_wipe(buf->data, buf->size);

	/* Starts again with nothing used and no error. */
	buf->size = 0;
	buf->error = 0;
}

/*
 * Makes room for size more bytes without writing them.
 *
 * Growing allocates new memory, copies the used bytes into it and frees
 * the old memory (wiped first when secret), never realloc.
 */
int
ssh_buf_reserve(
	struct ssh_buf *buf,
	size_t size)
{
	size_t needed;
	size_t new_capacity;
	uint8_t *new_data;

	/* Does nothing after an earlier failure. */
	if (buf->error != 0)
		return buf->error;

	/* Refuses a size that would take the buffer past its limit. */
	if (buf->size > buf->limit || size > buf->limit - buf->size) {
		buf->error = EMSGSIZE;
		return EMSGSIZE;
	}

	/* The room already there is enough. */
	needed = buf->size + size;
	if (needed <= buf->capacity)
		return 0;

	/* Chooses the new capacity: the first size, then doubling, never past the limit. */
	new_capacity = buf->capacity;
	if (new_capacity == 0) {
		new_capacity = BUF_FIRST_CAPACITY;
		if (new_capacity > buf->limit)
			new_capacity = buf->limit;
	}
	while (new_capacity < needed) {
		if (new_capacity > buf->limit / 2)
			new_capacity = buf->limit;
		else
			new_capacity *= 2;
	}

	/* Allocates the new memory. */
	new_data = malloc(new_capacity);
	if (new_data == NULL) {
		buf->error = ENOMEM;
		return ENOMEM;
	}

	/* Copies the used bytes into the new memory. */
	if (buf->size > 0)
		memcpy(new_data, buf->data, buf->size);

	/* Wipes the old memory when it held a secret, then gives it back. */
	if (buf->secret != 0 && buf->data != NULL)
		ssh_wipe(buf->data, buf->capacity);
	free(buf->data);

	/* Takes the new memory. */
	buf->data = new_data;
	buf->capacity = new_capacity;

	/* Succeeded: the buffer has room for size more bytes. */
	return 0;
}

/*
 * Appends one byte.
 */
void
ssh_buf_put_u8(
	struct ssh_buf *buf,
	uint8_t value)
{
	int error;

	/* Makes room for the byte. */
	error = ssh_buf_reserve(buf, 1);
	if (error != 0)
		return;

	/* Stores the byte after the used ones. */
	buf->data[buf->size] = value;
	buf->size++;
}

/*
 * Appends a boolean as one byte, 1 for any non-zero value.
 */
void
ssh_buf_put_bool(
	struct ssh_buf *buf,
	int value)
{
	/* Writes 1 for TRUE and 0 for FALSE (RFC 4251 5, boolean). */
	if (value != 0)
		ssh_buf_put_u8(buf, 1);
	else
		ssh_buf_put_u8(buf, 0);
}

/*
 * Appends a uint32, big-endian.
 */
void
ssh_buf_put_u32(
	struct ssh_buf *buf,
	uint32_t value)
{
	int error;

	/* Makes room for the four bytes. */
	error = ssh_buf_reserve(buf, 4);
	if (error != 0)
		return;

	/* Stores the integer after the used bytes. */
	ssh_store_be32(buf->data + buf->size, value);
	buf->size += 4;
}

/*
 * Appends a uint64, big-endian.
 */
void
ssh_buf_put_u64(
	struct ssh_buf *buf,
	uint64_t value)
{
	int error;

	/* Makes room for the eight bytes. */
	error = ssh_buf_reserve(buf, 8);
	if (error != 0)
		return;

	/* Stores the integer after the used bytes. */
	ssh_store_be64(buf->data + buf->size, value);
	buf->size += 8;
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
	int error;

	/* Refuses bytes that are missing. */
	if (data == NULL && size != 0) {
		if (buf->error == 0)
			buf->error = EINVAL;
		return;
	}

	/* Makes room for the bytes. */
	error = ssh_buf_reserve(buf, size);
	if (error != 0)
		return;

	/* Copies the bytes after the used ones. */
	if (size > 0) {
		memcpy(buf->data + buf->size, data, size);
		buf->size += size;
	}
}

/*
 * Appends a string: the uint32 length, then size bytes.
 *
 * The room for the length and the bytes together is made first, so a
 * string that does not fit leaves nothing of itself in the buffer.
 */
void
ssh_buf_put_string(
	struct ssh_buf *buf,
	const void *data,
	size_t size)
{
	int error;

	/* Refuses bytes that are missing. */
	if (data == NULL && size != 0) {
		if (buf->error == 0)
			buf->error = EINVAL;
		return;
	}

	/* Refuses a length that a uint32 cannot hold, or a total that would wrap. */
	if ((uint64_t)size > 0xffffffffU || size > (size_t)-1 - 4) {
		if (buf->error == 0)
			buf->error = EMSGSIZE;
		return;
	}

	/* Makes room for the length and the bytes. */
	error = ssh_buf_reserve(buf, size + 4);
	if (error != 0)
		return;

	/* Stores the length, then the bytes. */
	ssh_buf_put_u32(buf, (uint32_t)size);
	ssh_buf_put_bytes(buf, data, size);
}

/*
 * Appends a NUL-terminated C string as a string (without the NUL).
 */
void
ssh_buf_put_cstring(
	struct ssh_buf *buf,
	const char *text)
{
	size_t length;

	/* Refuses a text that is missing. */
	if (text == NULL) {
		if (buf->error == 0)
			buf->error = EINVAL;
		return;
	}

	/* Writes the text as a string. */
	length = strlen(text);
	ssh_buf_put_string(buf, text, length);
}

/*
 * Reserves room for size bytes at the end, counts them as used, and
 * returns where they start, for a caller that fills them itself (an
 * encryption in place).
 *
 * For size 0 the pointer is still not NULL on success: the memory is
 * allocated if there is none.
 */
uint8_t *
ssh_buf_put_space(
	struct ssh_buf *buf,
	size_t size)
{
	uint8_t *space;
	int error;

	/* Makes room for the bytes; a buffer with no memory gets some even for none, so success has a pointer. */
	error = ssh_buf_reserve(buf, size);
	if (error != 0)
		return NULL;
	if (buf->data == NULL) {
		error = ssh_buf_reserve(buf, 1);
		if (error != 0)
			return NULL;
	}

	/* Counts the bytes as used and reports where they start. */
	space = buf->data + buf->size;
	buf->size += size;

	/* Reports the start of the bytes. */
	return space;
}

/*
 * Removes size bytes from the front (bytes already sent, or input
 * already parsed), moving the rest down.
 *
 * It works whether or not an error is recorded: it is not a write.
 */
void
ssh_buf_consume(
	struct ssh_buf *buf,
	size_t size)
{
	size_t remaining;

	/* Removing nothing changes nothing. */
	if (size == 0)
		return;

	/* Removing everything (or more) empties the buffer. */
	if (size >= buf->size) {
		if (buf->secret != 0 && buf->data != NULL)
			ssh_wipe(buf->data, buf->size);
		buf->size = 0;
		return;
	}

	/* Moves the bytes that stay down to the front. */
	remaining = buf->size - size;
	memmove(buf->data, buf->data + size, remaining);

	/* Wipes the stale copy of the moved bytes at the end. */
	if (buf->secret != 0)
		ssh_wipe(buf->data + remaining, size);
	buf->size = remaining;
}

/*
 * Returns the error recorded (0 while none).
 */
int
ssh_buf_error(
	const struct ssh_buf *buf)
{
	/* Reports the first failure. */
	return buf->error;
}

/*
 * Starts a reader over size bytes at data (data may be NULL only when
 * size is 0).
 *
 * A missing data with a size above 0 records EINVAL and leaves a reader
 * of no bytes.
 */
void
ssh_reader_init(
	struct ssh_reader *reader,
	const uint8_t *data,
	size_t size)
{
	/* Starts at the first byte with no error. */
	reader->data = data;
	reader->size = size;
	reader->offset = 0;
	reader->error = 0;

	/* Refuses bytes that are missing. */
	if (data == NULL && size != 0) {
		reader->size = 0;
		reader->error = EINVAL;
	}
}

/*
 * Reads one byte.
 */
uint8_t
ssh_reader_get_u8(
	struct ssh_reader *reader)
{
	uint8_t value;

	/* Reads nothing after an earlier failure. */
	if (reader->error != 0)
		return 0;

	/* Refuses a read past the end. */
	if (reader->size - reader->offset < 1) {
		reader->error = EINVAL;
		return 0;
	}

	/* Takes the byte. */
	value = reader->data[reader->offset];
	reader->offset++;

	/* Reports the byte. */
	return value;
}

/*
 * Reads a boolean: 0 for a zero byte, 1 for any other (RFC 4251 5).
 */
int
ssh_reader_get_bool(
	struct ssh_reader *reader)
{
	uint8_t value;

	/* Reads the byte. */
	value = ssh_reader_get_u8(reader);

	/* Any non-zero value is TRUE. */
	if (value != 0)
		return 1;

	/* A zero byte is FALSE. */
	return 0;
}

/*
 * Reads a big-endian uint32.
 */
uint32_t
ssh_reader_get_u32(
	struct ssh_reader *reader)
{
	uint32_t value;

	/* Reads nothing after an earlier failure. */
	if (reader->error != 0)
		return 0;

	/* Refuses a read past the end. */
	if (reader->size - reader->offset < 4) {
		reader->error = EINVAL;
		return 0;
	}

	/* Takes the four bytes. */
	value = ssh_load_be32(reader->data + reader->offset);
	reader->offset += 4;

	/* Reports the integer. */
	return value;
}

/*
 * Reads a big-endian uint64.
 */
uint64_t
ssh_reader_get_u64(
	struct ssh_reader *reader)
{
	uint64_t value;

	/* Reads nothing after an earlier failure. */
	if (reader->error != 0)
		return 0;

	/* Refuses a read past the end. */
	if (reader->size - reader->offset < 8) {
		reader->error = EINVAL;
		return 0;
	}

	/* Takes the eight bytes. */
	value = ssh_load_be64(reader->data + reader->offset);
	reader->offset += 8;

	/* Reports the integer. */
	return value;
}

/*
 * Reads size raw bytes and returns where they start inside the reader's
 * data (NULL with the error recorded when fewer remain).
 *
 * Reading 0 bytes succeeds and returns where the next byte would be,
 * which is NULL only when the reader was started over no data at all.
 */
const uint8_t *
ssh_reader_get_bytes(
	struct ssh_reader *reader,
	size_t size)
{
	const uint8_t *bytes;

	/* Reads nothing after an earlier failure. */
	if (reader->error != 0)
		return NULL;

	/* Refuses a read past the end. */
	if (size > reader->size - reader->offset) {
		reader->error = EINVAL;
		return NULL;
	}

	/* Takes the bytes. */
	bytes = NULL;
	if (reader->data != NULL)
		bytes = reader->data + reader->offset;
	reader->offset += size;

	/* Reports where the bytes start. */
	return bytes;
}

/*
 * Reads a string: *data points to its bytes inside the reader's data and
 * *size receives its length.
 *
 * A failed read leaves the offset where the string began.
 */
void
ssh_reader_get_string(
	struct ssh_reader *reader,
	const uint8_t **data,
	size_t *size)
{
	size_t start;
	uint32_t length;

	/* Starts with nothing, which is what an error reports. */
	*data = NULL;
	*size = 0;

	/* Reads nothing after an earlier failure. */
	if (reader->error != 0)
		return;

	/* Reads the length. */
	start = reader->offset;
	length = ssh_reader_get_u32(reader);
	if (reader->error != 0)
		return;

	/* Refuses a length that runs past the end. */
	if (length > reader->size - reader->offset) {
		reader_fail(reader, start, EINVAL);
		return;
	}

	/* Takes the bytes of the string. */
	*data = reader->data + reader->offset;
	*size = length;
	reader->offset += length;
}

/*
 * Reads a string into out as a NUL-terminated C string.
 *
 * A capacity of 0 leaves no room even for the NUL: it records EINVAL and
 * writes nothing.
 */
void
ssh_reader_get_cstring(
	struct ssh_reader *reader,
	char *out,
	size_t capacity)
{
	const uint8_t *bytes;
	const void *found;
	size_t length;
	size_t start;

	/* A caller that gives no room for the NUL has a bug; nothing can be written. */
	if (capacity == 0) {
		if (reader->error == 0)
			reader->error = EINVAL;
		return;
	}

	/* Starts with the empty string, which is what an error leaves. */
	out[0] = '\0';

	/* Reads the string. */
	start = reader->offset;
	ssh_reader_get_string(reader, &bytes, &length);
	if (reader->error != 0)
		return;

	/* Refuses a NUL inside the string, which a C string cannot hold. */
	found = NULL;
	if (length > 0)
		found = memchr(bytes, 0, length);
	if (found != NULL) {
		reader_fail(reader, start, EINVAL);
		return;
	}

	/* Refuses a string that does not fit with its NUL. */
	if (length >= capacity) {
		reader_fail(reader, start, EMSGSIZE);
		return;
	}

	/* Copies the characters and ends them. */
	if (length > 0)
		memcpy(out, bytes, length);
	out[length] = '\0';
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
	const uint8_t *bytes;
	size_t length;
	size_t start;
	int error;

	/* Starts with nothing, which is what an error reports. */
	*list = NULL;
	*size = 0;

	/* Reads the string. */
	start = reader->offset;
	ssh_reader_get_string(reader, &bytes, &length);
	if (reader->error != 0)
		return;

	/* Checks that the string is a well-formed name-list. */
	error = ssh_namelist_check((const char *)bytes, length);
	if (error != 0) {
		reader_fail(reader, start, error);
		return;
	}

	/* Hands out the name-list. */
	*list = (const char *)bytes;
	*size = length;
}

/*
 * Returns the number of bytes not yet read (0 after an error).
 */
size_t
ssh_reader_left(
	const struct ssh_reader *reader)
{
	/* After an error nothing is left to read. */
	if (reader->error != 0)
		return 0;

	/* Reports the bytes between the offset and the end. */
	return reader->size - reader->offset;
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
	/* Reports an error recorded earlier. */
	if (reader->error != 0)
		return reader->error;

	/* Trailing bytes are a malformed message. */
	if (reader->offset < reader->size)
		return EINVAL;

	/* Succeeded: the reader ended exactly at its end. */
	return 0;
}

/*
 * Returns the error recorded (0 while none), whether or not bytes
 * remain.
 */
int
ssh_reader_error(
	const struct ssh_reader *reader)
{
	/* Reports the first failure. */
	return reader->error;
}

/*
 * Records a failure of a read and puts the offset back where the read began.
 *
 * It is called only while no error is recorded, so the error it records is
 * the first.
 */
static void
reader_fail(
	struct ssh_reader *reader,
	size_t start,
	int error)
{
	/* Records the failure and does not consume the field that failed. */
	reader->error = error;
	reader->offset = start;
}
