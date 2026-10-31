/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A reader and a writer of the TLS presentation language (RFC 8446
 * section 3): big-endian integers of 1, 2, 3 and 4 bytes, opaque bytes
 * of a fixed length, and vectors with a length prefix of 1, 2 or 3 bytes
 * (3.4).
 *
 * Errors are sticky, as in a stream: the first read past the end, or the
 * first write past the room, sets error; every later operation does
 * nothing and returns zeros.  A caller checks error once after a group of
 * operations and before it uses any value read as a length or an index.
 * The reader never reads outside its bytes; the writer never writes
 * outside its room.  The input is untrusted.
 */

#ifndef LIBSSL_COMPAT_SSLC_BUF_H
#define LIBSSL_COMPAT_SSLC_BUF_H

#include "../libcrypto-compat/cryc.h"

/*
 * The deepest nesting of open vectors a writer keeps: a handshake
 * message, its extensions block, one extension's data, a list inside it
 * (server_name_list, client_shares), an entry of the list (HostName,
 * key_exchange) is 5.
 */
#define SSLC_WRITER_DEPTH	6U

/*
 * A position in borrowed bytes.  data and size are the bytes; position
 * is the next byte to read; error is set by the first failed read.
 */
struct sslc_reader {
	const uint8_t *data;
	size_t size;
	size_t position;
	int error;
};

/*
 * A writer into a borrowed buffer of room bytes.  used counts the bytes
 * written.  open[] holds, for each vector begun and not yet ended, the
 * offset of its length prefix, and prefix[] the prefix's size; depth
 * counts them.  error is set by the first failed write (room exhausted,
 * a vector longer than its prefix allows, too many open vectors, or an
 * end without a begin).
 */
struct sslc_writer {
	uint8_t *data;
	size_t room;
	size_t used;
	size_t open[SSLC_WRITER_DEPTH];
	unsigned prefix[SSLC_WRITER_DEPTH];
	unsigned depth;
	int error;
};

/* Starts a reader over size bytes (data may be NULL only when size is 0). */
void sslc_reader_init(struct sslc_reader *reader, const uint8_t *data, size_t size);

/* Returns the bytes left to read (0 after an error). */
size_t sslc_reader_left(const struct sslc_reader *reader);

/* Reads a 1-byte integer (0 and error on the end). */
uint32_t sslc_reader_u8(struct sslc_reader *reader);

/* Reads a 2-byte big-endian integer. */
uint32_t sslc_reader_u16(struct sslc_reader *reader);

/* Reads a 3-byte big-endian integer. */
uint32_t sslc_reader_u24(struct sslc_reader *reader);

/* Reads a 4-byte big-endian integer. */
uint32_t sslc_reader_u32(struct sslc_reader *reader);

/*
 * Gives a view of the next size bytes and advances past them; returns
 * NULL (and sets error) when fewer are left.  size 0 gives a non-NULL
 * pointer to the position (or NULL with no error for an empty reader;
 * callers use the size, not the pointer, to tell).
 */
const uint8_t *sslc_reader_bytes(struct sslc_reader *reader, size_t size);

/*
 * Reads a vector: a length prefix of prefix bytes (1, 2 or 3), then that
 * many bytes, given as a new reader over them (3.4: <floor..ceiling>).
 *
 * The length must be at least min and at most max (and fit what is left);
 * otherwise error is set on the outer reader and *inner is an empty
 * reader with error set.  Returns 0 or EINVAL.
 */
int sslc_reader_vector(struct sslc_reader *reader, unsigned prefix, size_t min, size_t max, struct sslc_reader *inner);

/*
 * Requires the reader to be at its end without an error: returns 0, or
 * EINVAL when bytes are left or an error was set (a message with
 * trailing bytes is malformed, 4.1.2 and the like).
 */
int sslc_reader_finish(const struct sslc_reader *reader);

/* Starts a writer into room bytes at data. */
void sslc_writer_init(struct sslc_writer *writer, uint8_t *data, size_t room);

/* Writes a 1-byte integer (the low 8 bits of value). */
void sslc_writer_u8(struct sslc_writer *writer, uint32_t value);

/* Writes a 2-byte big-endian integer (the low 16 bits). */
void sslc_writer_u16(struct sslc_writer *writer, uint32_t value);

/* Writes a 3-byte big-endian integer (the low 24 bits). */
void sslc_writer_u24(struct sslc_writer *writer, uint32_t value);

/* Writes size bytes (data may be NULL only when size is 0). */
void sslc_writer_bytes(struct sslc_writer *writer, const uint8_t *data, size_t size);

/*
 * Begins a vector with a length prefix of prefix bytes (1, 2 or 3): the
 * prefix's room is written as zeros and filled by sslc_writer_end.
 */
void sslc_writer_begin(struct sslc_writer *writer, unsigned prefix);

/*
 * Ends the innermost vector: writes its length into its prefix.  A length
 * that does not fit the prefix (2^8, 2^16 or 2^24 bytes or more) sets
 * error.
 */
void sslc_writer_end(struct sslc_writer *writer);

/*
 * Returns 0 when no error was set and no vector is left open, else
 * EINVAL (a vector left open, a too-long vector) or ENOBUFS (room
 * exhausted).
 */
int sslc_writer_finish(const struct sslc_writer *writer);

#endif
