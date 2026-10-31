/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The records of the storage: keys, cookies, cache entries and localStorage areas.
 */

#include "storage/records.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * The 64-bit FNV-1a hash (draft-eastlake-fnv, section 2): the offset basis
 * the hash starts from, and the prime it is multiplied by after each byte.
 */
#define RECORDS_FNV_BASIS	UINT64_C(0xcbf29ce484222325)
#define RECORDS_FNV_PRIME	UINT64_C(0x100000001b3)

/* The most that a length field of a record can say. */
#define RECORDS_LENGTH_MAX	UINT32_C(0xffffffff)

/* The longest decimal number of a collision: the digits of UINT_MAX on a 32-bit unsigned. */
#define RECORDS_DECIMAL_MAX	10U

/*
 * A cursor over the bytes of a record being decoded.
 *
 * It lives on the decoder's stack.  Every read checks the bytes that
 * remain first, so a damaged record can only make a read fail.
 */
struct records_reader {
	const unsigned char *bytes;
	size_t length;
	size_t at;
};

/* The digits that write a byte or a nibble in hexadecimal; read-only, program lifetime. */
static const char records_hex_digits[] = "0123456789abcdef";

/* The helpers that write a record's pieces and that read them back with every length checked. */
static void records_truncate(struct wb_buffer *out, size_t length);
static int records_too_long(size_t length);
static int records_append_u32(struct wb_buffer *out, uint32_t value);
static int records_append_i64(struct wb_buffer *out, int64_t value);
static int records_append_field(struct wb_buffer *out, const void *bytes, size_t length);
static int records_append_units(struct wb_buffer *out, const uint16_t *units, size_t length);
static int records_append_cookie_key(const struct wb_storage_cookie *cookie, struct wb_buffer *out);
static void records_reader_init(struct records_reader *reader, const void *bytes, size_t length);
static int records_read_magic(struct records_reader *reader, const char *magic);
static int records_read_u32(struct records_reader *reader, uint32_t *value);
static int records_read_i64(struct records_reader *reader, int64_t *value);
static int records_read_field(struct records_reader *reader, const unsigned char **bytes, size_t *length);
static int records_read_text(struct records_reader *reader, const char **text, size_t *length);
static int records_read_units(struct records_reader *reader, size_t count, struct wb_units *units);
static int records_expect_field(struct records_reader *reader, const void *text, size_t length);
static int records_cookie_key_matches(const unsigned char *original, size_t original_length, const struct wb_storage_cookie *cookie);

/*
 * Binds a sink to a buffer for one read.
 */
void
wb_storage_sink_init(
	struct browser_storage_sink *sink,
	struct wb_buffer *buffer)
{
	/* The record is what is appended after the buffer's present end. */
	sink->buffer = buffer;
	sink->start = buffer->length;
	sink->error = 0;
}

/*
 * Reports the first error of a sink's appends.
 */
int
wb_storage_sink_error(
	const struct browser_storage_sink *sink)
{
	/* A sink that is not there cannot say it is clean. */
	if (sink == NULL)
		return EINVAL;

	/* The first error of an append, or 0 while there was none. */
	return sink->error;
}

/*
 * Appends bytes of the record being read to a sink.
 *
 * The first failure is kept in the sink and every later append fails with
 * it, so a backend that ignores a result still reports the error at the end.
 */
int
browser_storage_sink_append(
	struct browser_storage_sink *sink,
	const void *bytes,
	size_t length)
{
	size_t present;
	int error;

	/* Refuses a sink that is not there, or bytes that are missing. */
	if (sink == NULL)
		return EINVAL;
	if (sink->buffer == NULL)
		return EINVAL;
	if (bytes == NULL && length != 0)
		return EINVAL;

	/* A sink that already failed fails again the same way. */
	if (sink->error != 0)
		return sink->error;

	/* Refuses bytes that would make the record larger than the largest one. */
	present = sink->buffer->length - sink->start;
	if (length > WB_STORAGE_RECORD_MAX - present) {
		sink->error = EFBIG;
		return EFBIG;
	}

	/* Adds the bytes after the ones that came before. */
	error = wb_buffer_append(sink->buffer, bytes, length);
	if (error != 0) {
		sink->error = error;
		return error;
	}

	/* Succeeded: the bytes are part of the record. */
	return 0;
}

/*
 * Hashes bytes with the 64-bit FNV-1a function.
 */
uint64_t
wb_storage_hash(
	const void *bytes,
	size_t length)
{
	const unsigned char *data;
	uint64_t hash;
	size_t index;

	/* Starts from the offset basis, which is the hash of nothing. */
	data = (const unsigned char *)bytes;
	hash = RECORDS_FNV_BASIS;

	/* Mixes each byte in: XOR it into the hash, then multiply by the prime. */
	for (index = 0;
	     index < length;
	     index++) {
		hash ^= (uint64_t)data[index];
		hash *= RECORDS_FNV_PRIME;
	}

	/* Succeeded: the hash of all the bytes. */
	return hash;
}

/*
 * Appends the original key of a cookie: domain, path and name, each with its length in front.
 */
int
wb_storage_cookie_original_key(
	const struct wb_storage_cookie *cookie,
	struct wb_buffer *out)
{
	size_t start;
	int error;

	/* Refuses arguments that are not there. */
	if (cookie == NULL)
		return EINVAL;
	if (out == NULL)
		return EINVAL;

	/* Writes the three fields; a failure takes back what was written. */
	start = out->length;
	error = records_append_cookie_key(cookie, out);
	if (error != 0) {
		records_truncate(out, start);
		return error;
	}

	/* Succeeded: the original key is at the end of out. */
	return 0;
}

/*
 * Writes the key text that a record is stored under.
 */
int
wb_storage_key_text(
	const void *original,
	size_t length,
	unsigned collision,
	char *text)
{
	const unsigned char *data;
	char digits[RECORDS_DECIMAL_MAX];
	uint64_t hash;
	unsigned number;
	size_t index;
	size_t count;
	size_t at;
	int shift;

	/* Refuses arguments that are not there, and an original key with nothing in it. */
	if (original == NULL)
		return EINVAL;
	if (length == 0)
		return EINVAL;
	if (text == NULL)
		return EINVAL;

	/* A short original key on its first try is its own bytes in hexadecimal. */
	data = (const unsigned char *)original;
	if (collision == 0 && length <= BROWSER_STORAGE_KEY_MAX / 2U) {
		at = 0;
		for (index = 0;
		     index < length;
		     index++) {
			text[at] = records_hex_digits[data[index] >> 4];
			text[at + 1U] = records_hex_digits[data[index] & 0x0fU];
			at += 2U;
		}
		text[at] = '\0';

		/* Succeeded: the key is the original key in hexadecimal. */
		return 0;
	}

	/* Otherwise the key is the hash in sixteen digits, most significant first. */
	hash = wb_storage_hash(original, length);
	at = 0;
	for (shift = 60;
	     shift >= 0;
	     shift -= 4) {
		text[at] = records_hex_digits[(hash >> shift) & 0x0fU];
		at++;
	}
	text[at] = '-';
	at++;

	/* Then the collision number in decimal, built from its last digit backwards. */
	number = collision;
	count = 0;
	do {
		digits[count] = (char)('0' + (number % 10U));
		number /= 10U;
		count++;
	} while (number != 0);
	while (count > 0) {
		count--;
		text[at] = digits[count];
		at++;
	}
	text[at] = '\0';

	/* Succeeded: the key is the hash and the collision number. */
	return 0;
}

/*
 * Reports whether a text can be a key.
 */
int
wb_storage_key_valid(
	const char *text)
{
	size_t index;
	char character;

	/* A key that is not there is not valid. */
	if (text == NULL)
		return 0;

	/* Looks at each character, stopping at the first that is not allowed or when the key is too long. */
	for (index = 0;
	     text[index] != '\0';
	     index++) {
		if (index >= BROWSER_STORAGE_KEY_MAX)
			return 0;
		character = text[index];
		if (character >= '0' && character <= '9')
			continue;
		if (character >= 'a' && character <= 'f')
			continue;
		if (character == '-')
			continue;
		return 0;
	}

	/* An empty key is not valid. */
	if (index == 0)
		return 0;

	/* Succeeded: the key is valid. */
	return 1;
}

/*
 * Appends a cookie record.
 */
int
wb_storage_cookie_encode(
	const struct wb_storage_cookie *cookie,
	struct wb_buffer *out)
{
	struct wb_buffer key;
	size_t start;
	int error;

	/* Refuses arguments that are not there, and flags that this version does not know. */
	if (cookie == NULL)
		return EINVAL;
	if (out == NULL)
		return EINVAL;
	if ((cookie->flags & ~WB_STORAGE_COOKIE_FLAGS) != 0)
		return EINVAL;

	/* Builds the original key apart, because its length goes in front of it. */
	wb_buffer_init(&key);
	error = records_append_cookie_key(cookie, &key);
	if (error != 0) {
		wb_buffer_release(&key);
		return error;
	}

	/* Writes the magic line and the original key. */
	start = out->length;
	error = wb_buffer_append_string(out, WB_STORAGE_MAGIC_COOKIE);
	if (error == 0)
		error = records_append_field(out, key.data, key.length);
	wb_buffer_release(&key);

	/* Writes the name, value, domain and path in this order. */
	if (error == 0)
		error = records_append_field(out, cookie->name, cookie->name_length);
	if (error == 0)
		error = records_append_field(out, cookie->value, cookie->value_length);
	if (error == 0)
		error = records_append_field(out, cookie->domain, cookie->domain_length);
	if (error == 0)
		error = records_append_field(out, cookie->path, cookie->path_length);

	/* Writes the three times and the flags. */
	if (error == 0)
		error = records_append_i64(out, cookie->expires);
	if (error == 0)
		error = records_append_i64(out, cookie->created);
	if (error == 0)
		error = records_append_i64(out, cookie->used);
	if (error == 0)
		error = records_append_u32(out, (uint32_t)cookie->flags);

	/* A record that could not be written whole is taken back. */
	if (error != 0) {
		records_truncate(out, start);
		return error;
	}

	/* Succeeded: the record is at the end of out. */
	return 0;
}

/*
 * Decodes a cookie record.
 */
int
wb_storage_cookie_decode(
	const void *bytes,
	size_t length,
	struct wb_storage_cookie *cookie,
	const unsigned char **original,
	size_t *original_length)
{
	struct wb_storage_cookie found;
	struct records_reader reader;
	const unsigned char *key;
	size_t key_length;
	uint32_t flags;
	int matches;
	int error;

	/* Refuses a place for the result that is not there; the others are optional. */
	if (cookie == NULL)
		return EINVAL;

	/* A failure leaves nothing behind: the outputs start empty. */
	memset(cookie, 0, sizeof(*cookie));
	if (original != NULL)
		*original = NULL;
	if (original_length != NULL)
		*original_length = 0;

	/* Reads the magic line and the original key. */
	memset(&found, 0, sizeof(found));
	records_reader_init(&reader, bytes, length);
	error = records_read_magic(&reader, WB_STORAGE_MAGIC_COOKIE);
	if (error == 0)
		error = records_read_field(&reader, &key, &key_length);

	/* Reads the name, value, domain and path in this order. */
	if (error == 0) {
		error = records_read_text(
			&reader,
			&found.name,
			&found.name_length);
	}
	if (error == 0) {
		error = records_read_text(
			&reader,
			&found.value,
			&found.value_length);
	}
	if (error == 0) {
		error = records_read_text(
			&reader,
			&found.domain,
			&found.domain_length);
	}
	if (error == 0) {
		error = records_read_text(
			&reader,
			&found.path,
			&found.path_length);
	}

	/* Reads the three times and the flags. */
	if (error == 0)
		error = records_read_i64(&reader, &found.expires);
	if (error == 0)
		error = records_read_i64(&reader, &found.created);
	if (error == 0)
		error = records_read_i64(&reader, &found.used);
	if (error == 0)
		error = records_read_u32(&reader, &flags);
	if (error != 0)
		return error;

	/* Refuses bytes left after the last field, and flags this version does not know. */
	if (reader.at != reader.length)
		return EINVAL;
	if ((flags & ~(uint32_t)WB_STORAGE_COOKIE_FLAGS) != 0)
		return EINVAL;
	found.flags = (unsigned)flags;

	/* Refuses a record whose original key is not the one its own fields make. */
	matches = records_cookie_key_matches(key, key_length, &found);
	if (!matches)
		return EINVAL;

	/* Hands the fields and the original key to the caller. */
	*cookie = found;
	if (original != NULL)
		*original = key;
	if (original_length != NULL)
		*original_length = key_length;

	/* Succeeded: the cookie is decoded. */
	return 0;
}

/*
 * Appends a cache record.
 */
int
wb_storage_cache_encode(
	const struct wb_storage_cache_entry *entry,
	struct wb_buffer *out)
{
	size_t start;
	int error;

	/* Refuses arguments that are not there, and values the record cannot hold. */
	if (entry == NULL)
		return EINVAL;
	if (out == NULL)
		return EINVAL;
	if (entry->url == NULL || entry->url_length == 0)
		return EINVAL;
	if (entry->max_age < -1)
		return EINVAL;
	if (entry->no_cache != 0 && entry->no_cache != 1)
		return EINVAL;

	/* Writes the magic line and the URL, the original key. */
	start = out->length;
	error = wb_buffer_append_string(out, WB_STORAGE_MAGIC_CACHE);
	if (error == 0)
		error = records_append_field(out, entry->url, entry->url_length);

	/* Writes the content type and the entity tag. */
	if (error == 0)
		error = records_append_field(out, entry->content_type, entry->content_type_length);
	if (error == 0)
		error = records_append_field(out, entry->etag, entry->etag_length);

	/* Writes the time, the freshness and the no-cache flag. */
	if (error == 0)
		error = records_append_i64(out, entry->stored);
	if (error == 0)
		error = records_append_i64(out, entry->max_age);
	if (error == 0)
		error = records_append_u32(out, (uint32_t)entry->no_cache);

	/* Writes the body. */
	if (error == 0)
		error = records_append_field(out, entry->body, entry->body_length);

	/* A record that could not be written whole is taken back. */
	if (error != 0) {
		records_truncate(out, start);
		return error;
	}

	/* Succeeded: the record is at the end of out. */
	return 0;
}

/*
 * Decodes a cache record.
 */
int
wb_storage_cache_decode(
	const void *bytes,
	size_t length,
	struct wb_storage_cache_entry *entry)
{
	struct wb_storage_cache_entry found;
	struct records_reader reader;
	uint32_t no_cache;
	int error;

	/* Refuses a place for the result that is not there. */
	if (entry == NULL)
		return EINVAL;

	/* A failure leaves nothing behind: the output starts empty. */
	memset(entry, 0, sizeof(*entry));
	memset(&found, 0, sizeof(found));

	/* Reads the magic line and the URL, the original key. */
	records_reader_init(&reader, bytes, length);
	error = records_read_magic(&reader, WB_STORAGE_MAGIC_CACHE);
	if (error == 0) {
		error = records_read_text(
			&reader,
			&found.url,
			&found.url_length);
	}

	/* Reads the content type and the entity tag. */
	if (error == 0) {
		error = records_read_text(
			&reader,
			&found.content_type,
			&found.content_type_length);
	}
	if (error == 0) {
		error = records_read_text(
			&reader,
			&found.etag,
			&found.etag_length);
	}

	/* Reads the time, the freshness and the no-cache flag. */
	if (error == 0)
		error = records_read_i64(&reader, &found.stored);
	if (error == 0)
		error = records_read_i64(&reader, &found.max_age);
	if (error == 0)
		error = records_read_u32(&reader, &no_cache);

	/* Reads the body. */
	if (error == 0)
		error = records_read_field(&reader, &found.body, &found.body_length);
	if (error != 0)
		return error;

	/* Refuses bytes left after the body, and values that no encoder writes. */
	if (reader.at != reader.length)
		return EINVAL;
	if (found.url_length == 0)
		return EINVAL;
	if (found.max_age < -1)
		return EINVAL;
	if (no_cache > 1U)
		return EINVAL;
	found.no_cache = (int)no_cache;

	/* Hands the entry to the caller. */
	*entry = found;

	/* Succeeded: the entry is decoded. */
	return 0;
}

/*
 * Appends a localStorage record of version 2.
 */
int
wb_storage_local_encode(
	const char *origin,
	size_t origin_length,
	const struct wb_storage_local_item *items,
	size_t count,
	struct wb_buffer *out)
{
	size_t start;
	size_t index;
	int key_too_long;
	int value_too_long;
	int error;

	/* Refuses arguments that are not there, and an origin with nothing in it. */
	if (origin == NULL)
		return EINVAL;
	if (origin_length == 0)
		return EINVAL;
	if (out == NULL)
		return EINVAL;
	if (items == NULL && count != 0)
		return EINVAL;

	/* Writes the magic line and the origin, the original key. */
	start = out->length;
	error = wb_buffer_append_string(out, WB_STORAGE_MAGIC_LOCAL_2);
	if (error == 0)
		error = records_append_field(out, origin, origin_length);

	/* Writes each item: the two lengths in units, then the key's units and the value's. */
	for (index = 0;
	     index < count && error == 0;
	     index++) {
		key_too_long = records_too_long(items[index].key_length);
		value_too_long = records_too_long(items[index].value_length);
		if (key_too_long || value_too_long)
			error = EINVAL;
		if (error == 0)
			error = records_append_u32(out, (uint32_t)items[index].key_length);
		if (error == 0)
			error = records_append_u32(out, (uint32_t)items[index].value_length);
		if (error == 0)
			error = records_append_units(out, items[index].key, items[index].key_length);
		if (error == 0)
			error = records_append_units(out, items[index].value, items[index].value_length);
	}

	/* A record that could not be written whole is taken back. */
	if (error != 0) {
		records_truncate(out, start);
		return error;
	}

	/* Succeeded: the record is at the end of out. */
	return 0;
}

/*
 * Decodes a localStorage record of version 1 or 2, calling a function for each item.
 */
int
wb_storage_local_decode(
	const void *bytes,
	size_t length,
	const unsigned char **origin,
	size_t *origin_length,
	wb_storage_local_each each,
	void *context)
{
	struct records_reader reader;
	struct wb_units key;
	struct wb_units value;
	const unsigned char *found_origin;
	size_t found_origin_length;
	uint32_t key_count;
	uint32_t value_count;
	uint64_t needed;
	int version;
	int error;

	/* Refuses a function that is not there; the origin outputs are optional. */
	if (each == NULL)
		return EINVAL;
	if (origin != NULL)
		*origin = NULL;
	if (origin_length != NULL)
		*origin_length = 0;

	/* Finds the version by the magic line; version 1 has no origin. */
	records_reader_init(&reader, bytes, length);
	version = 2;
	error = records_read_magic(&reader, WB_STORAGE_MAGIC_LOCAL_2);
	if (error != 0) {
		records_reader_init(&reader, bytes, length);
		version = 1;
		error = records_read_magic(&reader, WB_STORAGE_MAGIC_LOCAL_1);
	}
	if (error != 0)
		return error;

	/* Reads the origin of version 2, which is never empty. */
	found_origin = NULL;
	found_origin_length = 0;
	if (version == 2) {
		error = records_read_field(&reader, &found_origin, &found_origin_length);
		if (error != 0)
			return error;
		if (found_origin_length == 0)
			return EINVAL;
		if (origin != NULL)
			*origin = found_origin;
		if (origin_length != NULL)
			*origin_length = found_origin_length;
	}

	/* The units of an item are built in buffers that live until the end. */
	wb_units_init(&key);
	wb_units_init(&value);
	error = wb_units_reserve(&key, 1);
	if (error == 0)
		error = wb_units_reserve(&value, 1);

	/* Reads each item until the bytes end, calling the function with it. */
	while (error == 0 && reader.at < reader.length) {
		error = records_read_u32(&reader, &key_count);
		if (error == 0)
			error = records_read_u32(&reader, &value_count);
		if (error != 0)
			break;

		/* The units must be in the bytes that remain, two bytes each; checked in a wide type. */
		needed = ((uint64_t)key_count + (uint64_t)value_count) * 2U;
		if (needed > (uint64_t)(reader.length - reader.at)) {
			error = EINVAL;
			break;
		}

		/* Reads the key's units, then the value's. */
		error = records_read_units(&reader, (size_t)key_count, &key);
		if (error == 0)
			error = records_read_units(&reader, (size_t)value_count, &value);
		if (error != 0)
			break;

		/* Gives the item to the caller, who may stop the reading. */
		error = each(context, key.data, key.length, value.data, value.length);
	}

	/* The buffers are done with. */
	wb_units_release(&key);
	wb_units_release(&value);

	/* An error, or the value that stopped the reading. */
	if (error != 0)
		return error;

	/* Succeeded: every item was passed on. */
	return 0;
}

/*
 * Takes back what was appended after a length, leaving the buffer as it was.
 */
static void
records_truncate(
	struct wb_buffer *out,
	size_t length)
{
	/* Cuts the bytes back and keeps the NUL after them. */
	out->length = length;
	if (out->data != NULL)
		out->data[length] = '\0';
}

/*
 * Reports whether a length cannot be written in a 32-bit length field.
 */
static int
records_too_long(
	size_t length)
{
	/* A length above the field's largest value. */
	if ((uint64_t)length > (uint64_t)RECORDS_LENGTH_MAX)
		return 1;

	/* The length fits. */
	return 0;
}

/*
 * Appends a 32-bit value, least significant byte first.
 */
static int
records_append_u32(
	struct wb_buffer *out,
	uint32_t value)
{
	unsigned char bytes[4];
	int error;

	/* Splits the value into bytes without a shift of a signed type. */
	bytes[0] = (unsigned char)(value & 0xffU);
	bytes[1] = (unsigned char)((value >> 8) & 0xffU);
	bytes[2] = (unsigned char)((value >> 16) & 0xffU);
	bytes[3] = (unsigned char)((value >> 24) & 0xffU);

	/* Adds them to the buffer. */
	error = wb_buffer_append(out, bytes, sizeof(bytes));
	if (error != 0)
		return error;

	/* Succeeded: the value is at the end. */
	return 0;
}

/*
 * Appends a signed 64-bit value as its two's complement, least significant byte first.
 */
static int
records_append_i64(
	struct wb_buffer *out,
	int64_t value)
{
	unsigned char bytes[8];
	uint64_t bits;
	size_t index;
	int error;

	/* The conversion of a signed value to an unsigned one is defined modulo 2^64. */
	bits = (uint64_t)value;

	/* Splits the bits into bytes. */
	for (index = 0;
	     index < sizeof(bytes);
	     index++)
		bytes[index] = (unsigned char)((bits >> (8U * index)) & 0xffU);

	/* Adds them to the buffer. */
	error = wb_buffer_append(out, bytes, sizeof(bytes));
	if (error != 0)
		return error;

	/* Succeeded: the value is at the end. */
	return 0;
}

/*
 * Appends bytes with their 32-bit length in front.
 */
static int
records_append_field(
	struct wb_buffer *out,
	const void *bytes,
	size_t length)
{
	int too_long;
	int error;

	/* Refuses bytes that are missing, and a length the field cannot say. */
	if (bytes == NULL && length != 0)
		return EINVAL;
	too_long = records_too_long(length);
	if (too_long)
		return EINVAL;

	/* Writes the length and then the bytes. */
	error = records_append_u32(out, (uint32_t)length);
	if (error == 0 && length != 0)
		error = wb_buffer_append(out, bytes, length);
	if (error != 0)
		return error;

	/* Succeeded: the field is at the end. */
	return 0;
}

/*
 * Appends UTF-16 units, each as two bytes, least significant first.
 */
static int
records_append_units(
	struct wb_buffer *out,
	const uint16_t *units,
	size_t length)
{
	unsigned char bytes[2];
	size_t index;
	int error;

	/* Refuses units that are missing. */
	if (units == NULL && length != 0)
		return EINVAL;

	/* Writes each unit as a pair of bytes. */
	error = 0;
	for (index = 0;
	     index < length && error == 0;
	     index++) {
		bytes[0] = (unsigned char)(units[index] & 0xffU);
		bytes[1] = (unsigned char)((units[index] >> 8) & 0xffU);
		error = wb_buffer_append(out, bytes, sizeof(bytes));
	}
	if (error != 0)
		return error;

	/* Succeeded: the units are at the end. */
	return 0;
}

/*
 * Appends a cookie's original key without taking anything back on a failure.
 */
static int
records_append_cookie_key(
	const struct wb_storage_cookie *cookie,
	struct wb_buffer *out)
{
	int error;

	/* The domain, then the path, then the name, each with its length in front. */
	error = records_append_field(out, cookie->domain, cookie->domain_length);
	if (error == 0)
		error = records_append_field(out, cookie->path, cookie->path_length);
	if (error == 0)
		error = records_append_field(out, cookie->name, cookie->name_length);
	if (error != 0)
		return error;

	/* Succeeded: the three fields are at the end. */
	return 0;
}

/*
 * Starts a cursor at the first byte of a record.
 */
static void
records_reader_init(
	struct records_reader *reader,
	const void *bytes,
	size_t length)
{
	/* Bytes that are missing cannot be read, so they count as none. */
	reader->bytes = (const unsigned char *)bytes;
	reader->length = length;
	reader->at = 0;
	if (bytes == NULL)
		reader->length = 0;
}

/*
 * Reads a record's magic line, moving past it when it is the expected one.
 */
static int
records_read_magic(
	struct records_reader *reader,
	const char *magic)
{
	size_t magic_length;
	int differs;

	/* Refuses a record too short to hold the line. */
	magic_length = strlen(magic);
	if (reader->length - reader->at < magic_length)
		return EINVAL;

	/* Refuses a record that starts with another line. */
	differs = memcmp(reader->bytes + reader->at, magic, magic_length);
	if (differs != 0)
		return EINVAL;

	/* Moves past the line. */
	reader->at += magic_length;

	/* Succeeded: the record has the expected magic. */
	return 0;
}

/*
 * Reads a 32-bit little-endian value.
 */
static int
records_read_u32(
	struct records_reader *reader,
	uint32_t *value)
{
	const unsigned char *bytes;

	/* Refuses a record with fewer than four bytes left. */
	if (reader->length - reader->at < 4U)
		return EINVAL;

	/* Puts the four bytes together, the first being the least significant. */
	bytes = reader->bytes + reader->at;
	*value = (uint32_t)bytes[0];
	*value |= (uint32_t)bytes[1] << 8;
	*value |= (uint32_t)bytes[2] << 16;
	*value |= (uint32_t)bytes[3] << 24;
	reader->at += 4U;

	/* Succeeded: the value is read. */
	return 0;
}

/*
 * Reads a signed 64-bit little-endian value.
 */
static int
records_read_i64(
	struct records_reader *reader,
	int64_t *value)
{
	const unsigned char *bytes;
	uint64_t bits;
	size_t index;

	/* Refuses a record with fewer than eight bytes left. */
	if (reader->length - reader->at < 8U)
		return EINVAL;

	/* Puts the eight bytes together, the first being the least significant. */
	bytes = reader->bytes + reader->at;
	bits = 0;
	for (index = 0;
	     index < 8U;
	     index++)
		bits |= (uint64_t)bytes[index] << (8U * index);
	reader->at += 8U;

	/*
	 * Turns the two's complement bits into the value without relying on
	 * how an out-of-range unsigned value converts: a set top bit means
	 * the value is below zero.
	 */
	if (bits <= (uint64_t)INT64_MAX)
		*value = (int64_t)bits;
	else
		*value = -(int64_t)(UINT64_MAX - bits) - 1;

	/* Succeeded: the value is read. */
	return 0;
}

/*
 * Reads a field made of a 32-bit length and that many bytes.
 */
static int
records_read_field(
	struct records_reader *reader,
	const unsigned char **bytes,
	size_t *length)
{
	uint32_t field_length;
	int error;

	/* Reads the length. */
	error = records_read_u32(reader, &field_length);
	if (error != 0)
		return error;

	/* Refuses a length that runs past the end of the record. */
	if ((uint64_t)field_length > (uint64_t)(reader->length - reader->at))
		return EINVAL;

	/* Hands over where the bytes are and moves past them. */
	*bytes = reader->bytes + reader->at;
	*length = (size_t)field_length;
	reader->at += (size_t)field_length;

	/* Succeeded: the field is read. */
	return 0;
}

/*
 * Reads a field of text, which the caller sees as characters.
 */
static int
records_read_text(
	struct records_reader *reader,
	const char **text,
	size_t *length)
{
	const unsigned char *bytes;
	int error;

	/* Reads the field as bytes. */
	error = records_read_field(reader, &bytes, length);
	if (error != 0)
		return error;

	/* The same bytes, seen as characters. */
	*text = (const char *)bytes;

	/* Succeeded: the field is read. */
	return 0;
}

/*
 * Reads count UTF-16 units, each two bytes little-endian, replacing what a buffer held.
 */
static int
records_read_units(
	struct records_reader *reader,
	size_t count,
	struct wb_units *units)
{
	const unsigned char *bytes;
	uint16_t unit;
	size_t index;
	int error;

	/* Makes room for the units; the caller has checked the bytes are there. */
	wb_units_clear(units);
	error = wb_units_reserve(units, count);
	if (error != 0)
		return error;

	/* Reads each unit and adds it to the buffer. */
	for (index = 0;
	     index < count && error == 0;
	     index++) {
		bytes = reader->bytes + reader->at;
		unit = (uint16_t)((unsigned)bytes[0] | ((unsigned)bytes[1] << 8));
		reader->at += 2U;
		error = wb_units_append(units, &unit, 1);
	}
	if (error != 0)
		return error;

	/* Succeeded: the units are in the buffer. */
	return 0;
}

/*
 * Reads a field and checks that it is exactly the given bytes.
 */
static int
records_expect_field(
	struct records_reader *reader,
	const void *text,
	size_t length)
{
	const unsigned char *found;
	size_t found_length;
	int differs;
	int error;

	/* Reads the next field. */
	error = records_read_field(reader, &found, &found_length);
	if (error != 0)
		return error;

	/* Refuses a field of another length. */
	if (found_length != length)
		return EINVAL;

	/* Refuses a field with other bytes. */
	differs = 0;
	if (length != 0)
		differs = memcmp(found, text, length);
	if (differs != 0)
		return EINVAL;

	/* Succeeded: the field is the expected one. */
	return 0;
}

/*
 * Reports whether an original key is the one a decoded cookie's own fields make.
 *
 * It compares field by field instead of building the key, so a damaged
 * record can only fail, never run out of memory.
 */
static int
records_cookie_key_matches(
	const unsigned char *original,
	size_t original_length,
	const struct wb_storage_cookie *cookie)
{
	struct records_reader reader;
	int error;

	/* Reads the original key as the domain, the path and the name. */
	records_reader_init(&reader, original, original_length);
	error = records_expect_field(&reader, cookie->domain, cookie->domain_length);
	if (error == 0)
		error = records_expect_field(&reader, cookie->path, cookie->path_length);
	if (error == 0)
		error = records_expect_field(&reader, cookie->name, cookie->name_length);
	if (error != 0)
		return 0;

	/* Refuses bytes left over after the three fields. */
	if (reader.at != reader.length)
		return 0;

	/* Succeeded: the keys are the same. */
	return 1;
}
