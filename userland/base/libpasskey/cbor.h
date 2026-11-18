/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * libpasskey's CBOR (ws161-p004): the part of RFC 8949 that CTAP2 uses,
 * written in CTAP2's canonical form and read strictly.
 *
 * Items: unsigned and negative integers, byte strings, text strings,
 * arrays, maps, and the simple values false, true and null.  Lengths are
 * always definite.  Tags, floating point numbers, other simple values and
 * indefinite lengths are refused when read.
 *
 * The writer puts out items in the order it is given them; a caller
 * writing a map gives its keys in CTAP2's canonical order (shorter
 * encodings first, then bytewise).  The reader takes one item at a time
 * from a buffer; pk_cbor_check() first walks a whole item and refuses one
 * that is not well formed, too deep, too long, not of the shortest form,
 * or (with PK_CBOR_STRICT_MAPS) a map whose keys are out of canonical
 * order or repeated.
 */

#ifndef LIBPASSKEY_CBOR_H
#define LIBPASSKEY_CBOR_H

#include <stddef.h>
#include <stdint.h>

/* The kinds of item. */
#define PK_CBOR_UNSIGNED	0
#define PK_CBOR_NEGATIVE	1
#define PK_CBOR_BYTES		2
#define PK_CBOR_TEXT		3
#define PK_CBOR_ARRAY		4
#define PK_CBOR_MAP		5
#define PK_CBOR_FALSE		6
#define PK_CBOR_TRUE		7
#define PK_CBOR_NULL		8

/* The deepest nesting and the most items pk_cbor_check() takes. */
#define PK_CBOR_DEPTH_MAX	8U
#define PK_CBOR_ITEMS_MAX	4096U

/* pk_cbor_check(): the maps' keys must be in canonical order, with none repeated. */
#define PK_CBOR_STRICT_MAPS	0x0001U

/*
 * Where a writer puts its bytes: capacity bytes at buffer, length of them
 * used, and the first error (ENOSPC once an item did not fit; then every
 * later item is dropped).
 */
struct pk_cbor_writer {
	uint8_t *buffer;
	size_t capacity;
	size_t length;
	int error;
};

/*
 * One item read: its kind; for an integer, value is the unsigned number
 * (a negative integer is -1 - value); for a string, value is its length
 * and bytes its first byte; for an array or a map, value is its count of
 * items or pairs, which follow it.
 */
struct pk_cbor_item {
	int kind;
	uint64_t value;
	const uint8_t *bytes;
};

/* Where a reader is in a buffer of size bytes. */
struct pk_cbor_reader {
	const uint8_t *data;
	size_t size;
	size_t offset;
};

void pk_cbor_writer_init(struct pk_cbor_writer *writer, uint8_t *buffer, size_t capacity);
void pk_cbor_put_unsigned(struct pk_cbor_writer *writer, uint64_t value);
void pk_cbor_put_integer(struct pk_cbor_writer *writer, int64_t value);
void pk_cbor_put_bytes(struct pk_cbor_writer *writer, const uint8_t *bytes, size_t length);
void pk_cbor_put_text(struct pk_cbor_writer *writer, const char *text, size_t length);
void pk_cbor_put_array(struct pk_cbor_writer *writer, size_t count);
void pk_cbor_put_map(struct pk_cbor_writer *writer, size_t count);
void pk_cbor_put_bool(struct pk_cbor_writer *writer, int value);
void pk_cbor_put_null(struct pk_cbor_writer *writer);
void pk_cbor_put_encoded(struct pk_cbor_writer *writer, const uint8_t *item, size_t length);

void pk_cbor_reader_init(struct pk_cbor_reader *reader, const uint8_t *data, size_t size);
int pk_cbor_read(struct pk_cbor_reader *reader, struct pk_cbor_item *item);
int pk_cbor_skip(struct pk_cbor_reader *reader);
int pk_cbor_check(const uint8_t *data, size_t size, unsigned flags, size_t *length);
int pk_cbor_integer(const struct pk_cbor_item *item, int64_t *value);

#endif
