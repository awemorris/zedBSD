/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * libpasskey's CBOR (cbor.h; ws161-p004): CTAP2's canonical subset of
 * RFC 8949, written as given and read strictly.
 */

#include "cbor.h"

#include <errno.h>
#include <string.h>

/* The major types (RFC 8949 section 3.1), and the additional information's meanings. */
#define CBOR_MAJOR_UNSIGNED	0U
#define CBOR_MAJOR_NEGATIVE	1U
#define CBOR_MAJOR_BYTES	2U
#define CBOR_MAJOR_TEXT		3U
#define CBOR_MAJOR_ARRAY	4U
#define CBOR_MAJOR_MAP		5U
#define CBOR_MAJOR_TAG		6U
#define CBOR_MAJOR_SIMPLE	7U
#define CBOR_ONE_BYTE		24U
#define CBOR_TWO_BYTES		25U
#define CBOR_FOUR_BYTES		26U
#define CBOR_EIGHT_BYTES	27U
#define CBOR_FALSE		20U
#define CBOR_TRUE		21U
#define CBOR_NULL		22U

/*
 * A walk of pk_cbor_check(): the items seen so far, against
 * PK_CBOR_ITEMS_MAX, and the flags.
 */
struct cbor_walk {
	struct pk_cbor_reader reader;
	unsigned items;
	unsigned flags;
};

static void cbor_put_head(struct pk_cbor_writer *writer, unsigned major, uint64_t value);
static void cbor_put_raw(struct pk_cbor_writer *writer, const uint8_t *bytes, size_t length);
static int cbor_walk_item(struct cbor_walk *walk, unsigned depth);
static int cbor_key_order(const uint8_t *previous, size_t previous_length, const uint8_t *key, size_t key_length);

/* Starts a writer on a buffer. */
void
pk_cbor_writer_init(
	struct pk_cbor_writer *writer,
	uint8_t *buffer,
	size_t capacity)
{
	/* Empty, with no error. */
	writer->buffer = buffer;
	writer->capacity = capacity;
	writer->length = 0U;
	writer->error = 0;
}

/* Writes an unsigned integer. */
void
pk_cbor_put_unsigned(
	struct pk_cbor_writer *writer,
	uint64_t value)
{
	/* The head carries it. */
	cbor_put_head(writer, CBOR_MAJOR_UNSIGNED, value);
}

/* Writes an integer, negative or not. */
void
pk_cbor_put_integer(
	struct pk_cbor_writer *writer,
	int64_t value)
{
	/* A negative n is written as -1 - n. */
	if (value < 0) {
		cbor_put_head(writer, CBOR_MAJOR_NEGATIVE, (uint64_t)(-(value + 1)));
		return;
	}

	/* Zero and above. */
	cbor_put_head(writer, CBOR_MAJOR_UNSIGNED, (uint64_t)value);
}

/* Writes a byte string. */
void
pk_cbor_put_bytes(
	struct pk_cbor_writer *writer,
	const uint8_t *bytes,
	size_t length)
{
	/* Its length, then its bytes. */
	cbor_put_head(writer, CBOR_MAJOR_BYTES, length);
	cbor_put_raw(writer, bytes, length);
}

/* Writes a text string (UTF-8, given by the caller). */
void
pk_cbor_put_text(
	struct pk_cbor_writer *writer,
	const char *text,
	size_t length)
{
	/* Its length, then its bytes. */
	cbor_put_head(writer, CBOR_MAJOR_TEXT, length);
	cbor_put_raw(writer, (const uint8_t *)text, length);
}

/* Starts an array of count items (the caller writes them next). */
void
pk_cbor_put_array(
	struct pk_cbor_writer *writer,
	size_t count)
{
	/* The head with the count. */
	cbor_put_head(writer, CBOR_MAJOR_ARRAY, count);
}

/* Starts a map of count pairs (the caller writes each key and value next, keys in canonical order). */
void
pk_cbor_put_map(
	struct pk_cbor_writer *writer,
	size_t count)
{
	/* The head with the count. */
	cbor_put_head(writer, CBOR_MAJOR_MAP, count);
}

/* Writes true or false. */
void
pk_cbor_put_bool(
	struct pk_cbor_writer *writer,
	int value)
{
	/* The simple value. */
	if (value) {
		cbor_put_head(writer, CBOR_MAJOR_SIMPLE, CBOR_TRUE);
		return;
	}

	/* False. */
	cbor_put_head(writer, CBOR_MAJOR_SIMPLE, CBOR_FALSE);
}

/* Writes null. */
void
pk_cbor_put_null(
	struct pk_cbor_writer *writer)
{
	/* The simple value. */
	cbor_put_head(writer, CBOR_MAJOR_SIMPLE, CBOR_NULL);
}

/* Writes an item already encoded (a COSE key made elsewhere), as it is. */
void
pk_cbor_put_encoded(
	struct pk_cbor_writer *writer,
	const uint8_t *item,
	size_t length)
{
	/* The bytes. */
	cbor_put_raw(writer, item, length);
}

/* Starts a reader at the beginning of a buffer. */
void
pk_cbor_reader_init(
	struct pk_cbor_reader *reader,
	const uint8_t *data,
	size_t size)
{
	/* At the first byte. */
	reader->data = data;
	reader->size = size;
	reader->offset = 0U;
}

/*
 * Reads the next item's head (and a string's bytes).  Returns 0, EINVAL
 * for an item this subset does not take or one not of the shortest form,
 * or EMSGSIZE for one that runs past the buffer.
 */
int
pk_cbor_read(
	struct pk_cbor_reader *reader,
	struct pk_cbor_item *item)
{
	uint64_t value;
	size_t width;
	size_t left;
	size_t index;
	unsigned major;
	unsigned additional;
	uint8_t first;

	/* The initial byte. */
	if (reader->offset >= reader->size)
		return EMSGSIZE;
	first = reader->data[reader->offset];
	reader->offset++;
	major = first >> 5U;
	additional = first & 0x1fU;

	/* The argument: in the byte, or in the 1, 2, 4 or 8 bytes after it (none indefinite, none reserved). */
	width = 0U;
	value = additional;
	if (additional == CBOR_ONE_BYTE)
		width = 1U;
	else if (additional == CBOR_TWO_BYTES)
		width = 2U;
	else if (additional == CBOR_FOUR_BYTES)
		width = 4U;
	else if (additional == CBOR_EIGHT_BYTES)
		width = 8U;
	else if (additional > CBOR_EIGHT_BYTES)
		return EINVAL;

	/* The argument's bytes, most significant first. */
	left = reader->size - reader->offset;
	if (width > left)
		return EMSGSIZE;
	if (width != 0U) {
		value = 0U;
		for (index = 0U; index < width; index++)
			value = (value << 8U) | reader->data[reader->offset + index];
		reader->offset += width;
	}

	/* The shortest form only: a value that fits a narrower head is refused. */
	if (width == 1U && value < CBOR_ONE_BYTE)
		return EINVAL;
	if (width == 2U && value <= 0xffU)
		return EINVAL;
	if (width == 4U && value <= 0xffffU)
		return EINVAL;
	if (width == 8U && value <= 0xffffffffU)
		return EINVAL;

	/* Each major type. */
	item->value = value;
	item->bytes = NULL;
	left = reader->size - reader->offset;
	switch (major) {
	case CBOR_MAJOR_UNSIGNED:
		item->kind = PK_CBOR_UNSIGNED;
		return 0;
	case CBOR_MAJOR_NEGATIVE:
		item->kind = PK_CBOR_NEGATIVE;
		return 0;
	case CBOR_MAJOR_BYTES:
	case CBOR_MAJOR_TEXT:
		/* The string's bytes are all there. */
		if (value > left)
			return EMSGSIZE;
		item->kind = PK_CBOR_BYTES;
		if (major == CBOR_MAJOR_TEXT)
			item->kind = PK_CBOR_TEXT;
		item->bytes = reader->data + reader->offset;
		reader->offset += (size_t)value;
		return 0;
	case CBOR_MAJOR_ARRAY:
		/* Every item takes a byte at least. */
		if (value > left)
			return EMSGSIZE;
		item->kind = PK_CBOR_ARRAY;
		return 0;
	case CBOR_MAJOR_MAP:
		/* Every pair takes two bytes at least. */
		if (value > left / 2U)
			return EMSGSIZE;
		item->kind = PK_CBOR_MAP;
		return 0;
	case CBOR_MAJOR_SIMPLE:
		/* false, true and null only, in their one-byte form. */
		if (width != 0U)
			return EINVAL;
		if (value == CBOR_FALSE)
			item->kind = PK_CBOR_FALSE;
		else if (value == CBOR_TRUE)
			item->kind = PK_CBOR_TRUE;
		else if (value == CBOR_NULL)
			item->kind = PK_CBOR_NULL;
		else
			return EINVAL;
		return 0;
	default:
		break;
	}

	/* A tag. */
	return EINVAL;
}

/*
 * Skips one whole item (an array's items and a map's pairs with it).
 * Returns 0 or the error of reading it.
 */
int
pk_cbor_skip(
	struct pk_cbor_reader *reader)
{
	struct cbor_walk walk;
	int error;

	/* A walk from where the reader is. */
	walk.reader = *reader;
	walk.items = 0U;
	walk.flags = 0U;
	error = cbor_walk_item(&walk, 0U);
	if (error != 0)
		return error;

	/* Succeeded: the reader is after the item. */
	*reader = walk.reader;
	return 0;
}

/*
 * Checks that a buffer starts with one well-formed item of this subset:
 * at most PK_CBOR_DEPTH_MAX deep and PK_CBOR_ITEMS_MAX items, every head
 * of the shortest form, and (PK_CBOR_STRICT_MAPS) every map's keys in
 * canonical order and none repeated.  Gives the item's length.  Returns 0,
 * EINVAL, or EMSGSIZE.
 */
int
pk_cbor_check(
	const uint8_t *data,
	size_t size,
	unsigned flags,
	size_t *length)
{
	struct cbor_walk walk;
	int error;

	/* The walk of the whole item. */
	pk_cbor_reader_init(&walk.reader, data, size);
	walk.items = 0U;
	walk.flags = flags;
	error = cbor_walk_item(&walk, 0U);
	if (error != 0)
		return error;

	/* Succeeded: the item and its length. */
	*length = walk.reader.offset;
	return 0;
}

/*
 * Gives an integer item's value as a signed number.  Returns 0, EINVAL for
 * an item that is no integer, or ERANGE for one beyond 64 signed bits.
 */
int
pk_cbor_integer(
	const struct pk_cbor_item *item,
	int64_t *value)
{
	/* Zero and above. */
	if (item->kind == PK_CBOR_UNSIGNED) {
		if (item->value > (uint64_t)INT64_MAX)
			return ERANGE;
		*value = (int64_t)item->value;
		return 0;
	}

	/* Below zero: -1 - n. */
	if (item->kind == PK_CBOR_NEGATIVE) {
		if (item->value > (uint64_t)INT64_MAX)
			return ERANGE;
		*value = -1 - (int64_t)item->value;
		return 0;
	}

	/* Anything else is no integer. */
	return EINVAL;
}

/* Writes a head: the major type and the shortest form of its argument. */
static void
cbor_put_head(
	struct pk_cbor_writer *writer,
	unsigned major,
	uint64_t value)
{
	uint8_t head[9];
	size_t width;
	size_t index;

	/* The argument's width: in the initial byte, or the fewest of 1, 2, 4 or 8 bytes after it. */
	if (value < CBOR_ONE_BYTE) {
		width = 0U;
		head[0] = (uint8_t)((major << 5U) | (unsigned)value);
	} else if (value <= 0xffU) {
		width = 1U;
		head[0] = (uint8_t)((major << 5U) | CBOR_ONE_BYTE);
	} else if (value <= 0xffffU) {
		width = 2U;
		head[0] = (uint8_t)((major << 5U) | CBOR_TWO_BYTES);
	} else if (value <= 0xffffffffU) {
		width = 4U;
		head[0] = (uint8_t)((major << 5U) | CBOR_FOUR_BYTES);
	} else {
		width = 8U;
		head[0] = (uint8_t)((major << 5U) | CBOR_EIGHT_BYTES);
	}

	/* The argument's bytes, most significant first. */
	for (index = 0U; index < width; index++)
		head[1U + index] = (uint8_t)(value >> (8U * (width - 1U - index)));
	cbor_put_raw(writer, head, 1U + width);
}

/* Appends bytes, or records ENOSPC when they do not fit. */
static void
cbor_put_raw(
	struct pk_cbor_writer *writer,
	const uint8_t *bytes,
	size_t length)
{
	/* Nothing more after an error. */
	if (writer->error != 0)
		return;

	/* The bytes fit, or the writer fails from here on. */
	if (length > writer->capacity - writer->length) {
		writer->error = ENOSPC;
		return;
	}

	/* The bytes after those before them. */
	if (length != 0U)
		memcpy(writer->buffer + writer->length, bytes, length);
	writer->length += length;
}

/* Walks one item and what it holds, depth levels down. */
static int
cbor_walk_item(
	struct cbor_walk *walk,
	unsigned depth)
{
	struct pk_cbor_item item;
	const uint8_t *previous;
	size_t previous_length;
	size_t key_start;
	uint64_t index;
	int error;

	/* Not too deep, not too many. */
	if (depth >= PK_CBOR_DEPTH_MAX)
		return EINVAL;
	walk->items++;
	if (walk->items > PK_CBOR_ITEMS_MAX)
		return EINVAL;

	/* The item's head. */
	error = pk_cbor_read(&walk->reader, &item);
	if (error != 0)
		return error;

	/* An array's items. */
	if (item.kind == PK_CBOR_ARRAY) {
		for (index = 0U; index < item.value; index++) {
			error = cbor_walk_item(walk, depth + 1U);
			if (error != 0)
				return error;
		}

		/* Succeeded: the whole array. */
		return 0;
	}

	/* Anything but a map ends here. */
	if (item.kind != PK_CBOR_MAP)
		return 0;

	/* A map's pairs, the keys in canonical order when asked. */
	previous = NULL;
	previous_length = 0U;
	for (index = 0U; index < item.value; index++) {
		/* The key. */
		key_start = walk->reader.offset;
		error = cbor_walk_item(walk, depth + 1U);
		if (error != 0)
			return error;

		/* After the key before it, and not the same. */
		if ((walk->flags & PK_CBOR_STRICT_MAPS) != 0U) {
			if (previous != NULL) {
				error = cbor_key_order(previous, previous_length, walk->reader.data + key_start,
				    walk->reader.offset - key_start);
				if (error != 0)
					return error;
			}

			/* The next key is compared with this one. */
			previous = walk->reader.data + key_start;
			previous_length = walk->reader.offset - key_start;
		}

		/* The value. */
		error = cbor_walk_item(walk, depth + 1U);
		if (error != 0)
			return error;
	}

	/* Succeeded: the whole map. */
	return 0;
}

/*
 * Tells whether a key comes after the one before it in CTAP2's canonical
 * order: a shorter encoding first, then bytewise.  Returns 0, or EINVAL
 * for a key out of order or the same.
 */
static int
cbor_key_order(
	const uint8_t *previous,
	size_t previous_length,
	const uint8_t *key,
	size_t key_length)
{
	int compared;

	/* A shorter key comes first. */
	if (previous_length < key_length)
		return 0;
	if (previous_length > key_length)
		return EINVAL;

	/* Of the same length, the smaller bytes first; the same key twice is refused. */
	compared = memcmp(previous, key, key_length);
	if (compared >= 0)
		return EINVAL;

	/* In order. */
	return 0;
}
