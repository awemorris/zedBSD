/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The data types of RFC 4251 section 5, written into a growing buffer and
 * read from a byte string with bounds checks.
 *
 * Both directions keep a sticky error: the first failure is recorded in
 * the structure, every later call does nothing, and the caller checks the
 * error once after a run of calls.  A reader never reads past its end;
 * a writer never grows past its limit.
 *
 * The types (RFC 4251 5): byte; boolean (one byte, 0 is FALSE and any
 * other value TRUE when read, 1 when written); uint32 and uint64
 * (big-endian); string (uint32 length, then the bytes); name-list (a
 * string holding comma-separated names).  mpint is not used by any
 * algorithm these programs support and is not provided.
 */

#ifndef SSH_SSH_BUF_H
#define SSH_SSH_BUF_H

#include "ssh-common.h"

/*
 * A byte buffer that grows as it is written.
 *
 * data is malloc'd (NULL while capacity is 0); size bytes are used.
 * limit is the largest size the buffer may reach: a write that would pass
 * it fails with EMSGSIZE.  error is the first failure (0 while none);
 * once it is set, writes do nothing.  secret is 1 when the contents are a
 * secret: ssh_buf_free and ssh_buf_reset then wipe the bytes, and growing
 * copies into the new memory and wipes the old (no realloc that could
 * leave a copy behind).  The owner calls ssh_buf_free.
 */
struct ssh_buf {
	uint8_t *data;
	size_t size;
	size_t capacity;
	size_t limit;
	int error;
	int secret;
};

/*
 * A cursor over size bytes at data, which the reader does not own.
 *
 * offset is the next byte to read (0 to size).  error is the first
 * failure (EINVAL when a field runs past the end or breaks its type's
 * rules); once set, every read returns a zero value or an empty string
 * and does not move offset.
 */
struct ssh_reader {
	const uint8_t *data;
	size_t size;
	size_t offset;
	int error;
};

/*
 * Starts an empty buffer that may grow to limit bytes (1 or more); secret
 * as in struct ssh_buf.  Allocates nothing.
 */
void ssh_buf_init(struct ssh_buf *buf, size_t limit, int secret);

/* Frees the memory (wiping it first when secret) and leaves the buffer empty and usable, error cleared. */
void ssh_buf_free(struct ssh_buf *buf);

/* Empties the buffer (wiping the used bytes when secret), keeping its memory; clears error. */
void ssh_buf_reset(struct ssh_buf *buf);

/*
 * Makes room for size more bytes without writing them.
 *
 * Returns 0, EMSGSIZE when the size would pass the limit, ENOMEM, or the
 * error already recorded; a failure is recorded in buf->error.
 */
int ssh_buf_reserve(struct ssh_buf *buf, size_t size);

/* Appends one byte. */
void ssh_buf_put_u8(struct ssh_buf *buf, uint8_t value);

/* Appends a boolean as one byte, 1 for any non-zero value. */
void ssh_buf_put_bool(struct ssh_buf *buf, int value);

/* Appends a uint32, big-endian. */
void ssh_buf_put_u32(struct ssh_buf *buf, uint32_t value);

/* Appends a uint64, big-endian. */
void ssh_buf_put_u64(struct ssh_buf *buf, uint64_t value);

/* Appends size raw bytes (no length); data may be NULL only when size is 0. */
void ssh_buf_put_bytes(struct ssh_buf *buf, const void *data, size_t size);

/*
 * Appends a string: the uint32 length, then size bytes.  size above
 * 0xffffffff records EMSGSIZE.  data may be NULL only when size is 0.
 */
void ssh_buf_put_string(struct ssh_buf *buf, const void *data, size_t size);

/* Appends a NUL-terminated C string as a string (without the NUL). */
void ssh_buf_put_cstring(struct ssh_buf *buf, const char *text);

/*
 * Reserves room for size bytes at the end, counts them as used, and
 * returns where they start, for a caller that fills them itself (an
 * encryption in place).  Returns NULL on a failure (recorded).  The
 * pointer is valid only until the next call that writes to buf (a write
 * may move the memory).
 */
uint8_t *ssh_buf_put_space(struct ssh_buf *buf, size_t size);

/*
 * Removes size bytes from the front (bytes already sent, or input already
 * parsed), moving the rest down.  size larger than buf->size empties the
 * buffer.  The removed bytes are wiped when secret.
 */
void ssh_buf_consume(struct ssh_buf *buf, size_t size);

/* Returns the error recorded (0 while none). */
int ssh_buf_error(const struct ssh_buf *buf);

/* Starts a reader over size bytes at data (data may be NULL only when size is 0). */
void ssh_reader_init(struct ssh_reader *reader, const uint8_t *data, size_t size);

/* Reads one byte. */
uint8_t ssh_reader_get_u8(struct ssh_reader *reader);

/* Reads a boolean: 0 for a zero byte, 1 for any other (RFC 4251 5). */
int ssh_reader_get_bool(struct ssh_reader *reader);

/* Reads a big-endian uint32. */
uint32_t ssh_reader_get_u32(struct ssh_reader *reader);

/* Reads a big-endian uint64. */
uint64_t ssh_reader_get_u64(struct ssh_reader *reader);

/*
 * Reads size raw bytes and returns where they start inside the reader's
 * data (NULL with the error recorded when fewer remain).
 */
const uint8_t *ssh_reader_get_bytes(struct ssh_reader *reader, size_t size);

/*
 * Reads a string: *data points to its bytes inside the reader's data and
 * *size receives its length.
 *
 * A length that runs past the end records EINVAL; on an error *data is
 * NULL and *size 0.
 */
void ssh_reader_get_string(struct ssh_reader *reader, const uint8_t **data, size_t *size);

/*
 * Reads a string into out as a NUL-terminated C string.
 *
 * capacity is the room at out including the NUL.  A string with a NUL
 * byte inside records EINVAL; a string of capacity bytes or more records
 * EMSGSIZE.  On an error out is set to the empty string.
 */
void ssh_reader_get_cstring(struct ssh_reader *reader, char *out, size_t capacity);

/*
 * Reads a string that must be a name-list (ssh_namelist_check): *list and
 * *size as in ssh_reader_get_string.  An invalid list records its error.
 */
void ssh_reader_get_namelist(struct ssh_reader *reader, const char **list, size_t *size);

/* Returns the number of bytes not yet read (0 after an error). */
size_t ssh_reader_left(const struct ssh_reader *reader);

/*
 * Returns the error recorded, or EINVAL when no error is recorded but
 * bytes remain unread (a message with trailing bytes), or 0 when the
 * reader ended exactly at its end.
 */
int ssh_reader_finish(const struct ssh_reader *reader);

/* Returns the error recorded (0 while none), whether or not bytes remain. */
int ssh_reader_error(const struct ssh_reader *reader);

#endif
