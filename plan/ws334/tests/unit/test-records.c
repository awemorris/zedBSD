/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The unit test of the storage records (storage/records.c): the FNV-1a
 * hash, the key texts, and the encoding and decoding of cookie, cache and
 * localStorage records, including records that are damaged.
 *
 * Every record that is decoded is first copied to a block of exactly its
 * size, so that a read past its end is caught by the address sanitizer.
 */

#include "check.h"

#include "storage/records.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The number of random records fed to the decoders. */
#define RANDOM_RECORDS		10000U

/*
 * What a localStorage decoding passed to its function: the items in order,
 * and the value to stop with after a number of items.
 */
struct local_seen {
	size_t count;
	size_t stop_after;
	int stop_value;
	uint16_t first_key[8];
	size_t first_key_length;
	uint16_t last_key[8];
	size_t last_key_length;
	uint16_t last_value[8];
	size_t last_value_length;
	size_t total_units;
};

/*
 * The state of the pseudo-random generator that makes the damaged records;
 * a fixed seed makes each run the same.
 */
static uint32_t random_state = 12345U;

static void test_hash(void);
static void test_key_text(void);
static void test_key_valid(void);
static void test_cookie_original_key(void);
static void test_cookie_round_trip(void);
static void test_cookie_exact_bytes(void);
static void test_cookie_errors(void);
static void test_cache(void);
static void test_local_v2(void);
static void test_local_v1(void);
static void test_local_errors(void);
static void test_random(void);
static void test_mutation(void);
static void test_sink(void);
static unsigned char *exact_copy(const void *bytes, size_t length);
static int decode_cookie_copy(const void *bytes, size_t length);
static int decode_cache_copy(const void *bytes, size_t length);
static int decode_local_copy(const void *bytes, size_t length);
static int local_each(void *context, const uint16_t *key, size_t key_length, const uint16_t *value, size_t value_length);
static int count_each(void *context, const uint16_t *key, size_t key_length, const uint16_t *value, size_t value_length);
static void put_u32(struct wb_buffer *out, uint32_t value);
static void put_u64(struct wb_buffer *out, uint64_t value);
static void put_units(struct wb_buffer *out, const uint16_t *units, size_t count);
static void put_text(struct wb_buffer *out, const char *text);
static void fill_cookie(struct wb_storage_cookie *cookie, unsigned flags);
static uint32_t random_next(void);
static int is_zero(const void *bytes, size_t length);
static int same_bytes(const void *left, size_t left_length, const void *right, size_t right_length);

/*
 * Runs every group of checks.
 */
int
main(
	int argc,
	char **argv)
{
	int differs;

	/* The harness passes the system's name. */
	CHECK(argc == 2);
	differs = 1;
	if (argc == 2)
		differs = strcmp(argv[1], "posix") != 0 && strcmp(argv[1], "win32") != 0;
	CHECK(differs == 0);

	/* Each group of checks. */
	test_hash();
	test_key_text();
	test_key_valid();
	test_cookie_original_key();
	test_cookie_round_trip();
	test_cookie_exact_bytes();
	test_cookie_errors();
	test_cache();
	test_local_v2();
	test_local_v1();
	test_local_errors();
	test_random();
	test_mutation();
	test_sink();

	/* Reports the result. */
	return check_finish("records");
}

/*
 * Checks the hash against the published FNV-1a vectors and a hand calculation.
 */
static void
test_hash(void)
{
	uint64_t basis_xor_a;
	uint64_t by_hand;

	/* The published values of the 64-bit FNV-1a hash. */
	CHECK(wb_storage_hash(NULL, 0) == UINT64_C(0xcbf29ce484222325));
	CHECK(wb_storage_hash("", 0) == UINT64_C(0xcbf29ce484222325));
	CHECK(wb_storage_hash("a", 1) == UINT64_C(0xaf63dc4c8601ec8c));
	CHECK(wb_storage_hash("b", 1) == UINT64_C(0xaf63df4c8601f1a5));
	CHECK(wb_storage_hash("foo", 3) == UINT64_C(0xdcb27518fed9d577));
	CHECK(wb_storage_hash("foobar", 6) == UINT64_C(0x85944171f73967e8));

	/*
	 * By hand for "a": the basis XOR 0x61, times 2^40 + 0x1b3 (the prime
	 * 0x100000001b3 split into its two parts), modulo 2^64.
	 */
	basis_xor_a = UINT64_C(0xcbf29ce484222325) ^ UINT64_C(0x61);
	by_hand = (basis_xor_a << 40) + basis_xor_a * UINT64_C(0x1b3);
	CHECK(wb_storage_hash("a", 1) == by_hand);

	/* The length counts, not a NUL: a NUL byte is mixed in like any other. */
	CHECK(wb_storage_hash("a\0", 2) != wb_storage_hash("a", 1));
}

/*
 * Checks the key texts: the hexadecimal form, the hash form and the errors.
 */
static void
test_key_text(void)
{
	unsigned char original[101];
	char expected[64];
	char *text;
	size_t index;
	size_t at;
	int error;

	/* The text has exactly the room the header promises, so an overrun is caught. */
	text = (char *)malloc(BROWSER_STORAGE_KEY_MAX + 1U);
	CHECK(text != NULL);
	if (text == NULL)
		return;

	/* One byte is two hexadecimal digits. */
	error = wb_storage_key_text("a", 1, 0, text);
	CHECK_EQ(error, 0);
	CHECK(strcmp(text, "61") == 0);
	CHECK(wb_storage_key_valid(text) == 1);

	/* The bytes are in lower case and the first byte comes first. */
	error = wb_storage_key_text("\x01\xab\xff", 3, 0, text);
	CHECK_EQ(error, 0);
	CHECK(strcmp(text, "01abff") == 0);

	/* A hundred bytes make two hundred digits, the longest hexadecimal key. */
	for (index = 0; index < sizeof(original); index++)
		original[index] = (unsigned char)(index + 1U);
	error = wb_storage_key_text(original, 100, 0, text);
	CHECK_EQ(error, 0);
	CHECK_EQ(strlen(text), 200);
	at = 0;
	for (index = 0; index < 100; index++) {
		snprintf(expected, 3, "%02x", (unsigned)original[index]);
		CHECK(text[at] == expected[0]);
		CHECK(text[at + 1U] == expected[1]);
		at += 2U;
	}
	CHECK(wb_storage_key_valid(text) == 1);

	/* A hundred and one bytes are too long: the hash and the number 0. */
	error = wb_storage_key_text(original, 101, 0, text);
	CHECK_EQ(error, 0);
	snprintf(expected, sizeof(expected), "%016llx-0", (unsigned long long)wb_storage_hash(original, 101));
	CHECK(strcmp(text, expected) == 0);
	CHECK(wb_storage_key_valid(text) == 1);

	/* A short original after a collision gets the hash form too: the hash of "a" is published. */
	error = wb_storage_key_text("a", 1, 3, text);
	CHECK_EQ(error, 0);
	CHECK(strcmp(text, "af63dc4c8601ec8c-3") == 0);
	CHECK(wb_storage_key_valid(text) == 1);

	/* The number is in decimal without leading zeros, and the largest one fits. */
	error = wb_storage_key_text("a", 1, 10, text);
	CHECK_EQ(error, 0);
	CHECK(strcmp(text, "af63dc4c8601ec8c-10") == 0);
	error = wb_storage_key_text("a", 1, 4294967295U, text);
	CHECK_EQ(error, 0);
	CHECK(strcmp(text, "af63dc4c8601ec8c-4294967295") == 0);
	CHECK(wb_storage_key_valid(text) == 1);

	/* The hash keeps its leading zeros: sixteen digits always. */
	error = wb_storage_key_text(original, 101, 7, text);
	CHECK_EQ(error, 0);
	CHECK_EQ(strlen(text), 18);
	CHECK(text[16] == '-');
	CHECK(text[17] == '7');

	/* Arguments that are missing, and an original with nothing in it. */
	CHECK_EQ(wb_storage_key_text(NULL, 1, 0, text), EINVAL);
	CHECK_EQ(wb_storage_key_text("a", 0, 0, text), EINVAL);
	CHECK_EQ(wb_storage_key_text("a", 1, 0, NULL), EINVAL);

	free(text);
}

/*
 * Checks which texts are valid keys.
 */
static void
test_key_valid(void)
{
	char long_key[203];
	size_t index;

	/* Keys of the allowed characters. */
	CHECK(wb_storage_key_valid("0") == 1);
	CHECK(wb_storage_key_valid("0123456789abcdef-") == 1);
	CHECK(wb_storage_key_valid("-") == 1);

	/* Two hundred characters are the longest. */
	for (index = 0; index < 200; index++)
		long_key[index] = 'a';
	long_key[200] = '\0';
	CHECK(wb_storage_key_valid(long_key) == 1);
	long_key[200] = 'a';
	long_key[201] = '\0';
	CHECK(wb_storage_key_valid(long_key) == 0);

	/* No key, an empty key and characters outside the set. */
	CHECK(wb_storage_key_valid(NULL) == 0);
	CHECK(wb_storage_key_valid("") == 0);
	CHECK(wb_storage_key_valid("A") == 0);
	CHECK(wb_storage_key_valid("g") == 0);
	CHECK(wb_storage_key_valid("/") == 0);
	CHECK(wb_storage_key_valid("a.b") == 0);
	CHECK(wb_storage_key_valid("a b") == 0);
	CHECK(wb_storage_key_valid("0a\x80") == 0);

	/* The characters next to the allowed ranges. */
	CHECK(wb_storage_key_valid("/") == 0);
	CHECK(wb_storage_key_valid(":") == 0);
	CHECK(wb_storage_key_valid("`") == 0);
	CHECK(wb_storage_key_valid("G") == 0);
	CHECK(wb_storage_key_valid("@") == 0);
}

/*
 * Checks the cookie's original key: each field has its length in front.
 */
static void
test_cookie_original_key(void)
{
	struct wb_storage_cookie first;
	struct wb_storage_cookie second;
	struct wb_buffer left;
	struct wb_buffer right;
	struct wb_buffer expected;
	int error;

	/* A cookie with an empty name and one with the name "x" differ. */
	memset(&first, 0, sizeof(first));
	memset(&second, 0, sizeof(second));
	first.domain = "a.example";
	first.domain_length = 9;
	first.path = "/";
	first.path_length = 1;
	first.name = "";
	first.name_length = 0;
	second = first;
	second.name = "x";
	second.name_length = 1;
	wb_buffer_init(&left);
	wb_buffer_init(&right);
	error = wb_storage_cookie_original_key(&first, &left);
	CHECK_EQ(error, 0);
	error = wb_storage_cookie_original_key(&second, &right);
	CHECK_EQ(error, 0);
	CHECK(same_bytes(left.data, left.length, right.data, right.length) == 0);

	/* The bytes: each length is 32 bits little-endian, in the order domain, path, name. */
	wb_buffer_init(&expected);
	put_u32(&expected, 9);
	put_text(&expected, "a.example");
	put_u32(&expected, 1);
	put_text(&expected, "/");
	put_u32(&expected, 0);
	CHECK(same_bytes(left.data, left.length, expected.data, expected.length) == 1);

	/* Moving a byte between fields makes a different key, as a concatenation would not. */
	wb_buffer_clear(&left);
	wb_buffer_clear(&right);
	first.domain = "ab";
	first.domain_length = 2;
	first.path = "c";
	first.path_length = 1;
	second = first;
	second.domain = "a";
	second.domain_length = 1;
	second.path = "bc";
	second.path_length = 2;
	CHECK_EQ(wb_storage_cookie_original_key(&first, &left), 0);
	CHECK_EQ(wb_storage_cookie_original_key(&second, &right), 0);
	CHECK(same_bytes(left.data, left.length, right.data, right.length) == 0);

	/* The key is appended after the bytes already in the buffer. */
	wb_buffer_clear(&left);
	CHECK_EQ(wb_buffer_append_string(&left, "xyz"), 0);
	CHECK_EQ(wb_storage_cookie_original_key(&first, &left), 0);
	CHECK(memcmp(left.data, "xyz", 3) == 0);
	CHECK_EQ(left.length, 3 + 4 + 2 + 4 + 1 + 4);

	/* Missing arguments, and a string that is missing but has a length. */
	CHECK_EQ(wb_storage_cookie_original_key(NULL, &left), EINVAL);
	CHECK_EQ(wb_storage_cookie_original_key(&first, NULL), EINVAL);
	first.name = NULL;
	first.name_length = 1;
	CHECK_EQ(wb_storage_cookie_original_key(&first, &left), EINVAL);
	CHECK_EQ(left.length, 3 + 4 + 2 + 4 + 1 + 4);

	/* A missing string with length 0 is an empty one. */
	first.name_length = 0;
	CHECK_EQ(wb_storage_cookie_original_key(&first, &left), 0);

#if SIZE_MAX > 0xffffffffUL
	/* A length above 32 bits is refused without reading the bytes. */
	first.name = "n";
	first.name_length = (size_t)1 << 32;
	CHECK_EQ(wb_storage_cookie_original_key(&first, &left), EINVAL);
#endif

	wb_buffer_release(&left);
	wb_buffer_release(&right);
	wb_buffer_release(&expected);
}

/*
 * Checks that every combination of flags, odd times and a large value survive a round trip.
 */
static void
test_cookie_round_trip(void)
{
	static const int64_t times[] = {
		0, 1, -1, 1700000000123, INT64_MIN, INT64_MAX, -4294967296, 4294967296
	};
	struct wb_storage_cookie cookie;
	struct wb_storage_cookie decoded;
	struct wb_buffer record;
	struct wb_buffer key;
	unsigned char *copy;
	char *big;
	const unsigned char *original;
	size_t original_length;
	unsigned flags;
	size_t index;
	int error;

	/* All flag combinations: three single bits and the four SameSite values. */
	for (flags = 0; flags <= WB_STORAGE_COOKIE_FLAGS; flags++) {
		fill_cookie(&cookie, flags);
		wb_buffer_init(&record);
		error = wb_storage_cookie_encode(&cookie, &record);
		CHECK_EQ(error, 0);
		copy = exact_copy(record.data, record.length);
		original = NULL;
		original_length = 99;
		error = wb_storage_cookie_decode(copy, record.length, &decoded, &original, &original_length);
		CHECK_EQ(error, 0);
		CHECK(same_bytes(decoded.name, decoded.name_length, cookie.name, cookie.name_length) == 1);
		CHECK(same_bytes(decoded.value, decoded.value_length, cookie.value, cookie.value_length) == 1);
		CHECK(same_bytes(decoded.domain, decoded.domain_length, cookie.domain, cookie.domain_length) == 1);
		CHECK(same_bytes(decoded.path, decoded.path_length, cookie.path, cookie.path_length) == 1);
		CHECK(decoded.expires == cookie.expires);
		CHECK(decoded.created == cookie.created);
		CHECK(decoded.used == cookie.used);
		CHECK_EQ(decoded.flags, flags);

		/* The original key is the one the fields make, and it lies inside the record. */
		wb_buffer_init(&key);
		CHECK_EQ(wb_storage_cookie_original_key(&cookie, &key), 0);
		CHECK(same_bytes(original, original_length, key.data, key.length) == 1);
		CHECK(original >= copy);
		CHECK(original + original_length <= copy + record.length);
		wb_buffer_release(&key);
		free(copy);
		wb_buffer_release(&record);
	}

	/* The extreme times, in every position. */
	for (index = 0; index < sizeof(times) / sizeof(times[0]); index++) {
		fill_cookie(&cookie, WB_STORAGE_COOKIE_SECURE);
		cookie.expires = times[index];
		cookie.created = times[(index + 1U) % (sizeof(times) / sizeof(times[0]))];
		cookie.used = times[(index + 2U) % (sizeof(times) / sizeof(times[0]))];
		wb_buffer_init(&record);
		CHECK_EQ(wb_storage_cookie_encode(&cookie, &record), 0);
		copy = exact_copy(record.data, record.length);
		error = wb_storage_cookie_decode(copy, record.length, &decoded, &original, &original_length);
		CHECK_EQ(error, 0);
		CHECK(decoded.expires == cookie.expires);
		CHECK(decoded.created == cookie.created);
		CHECK(decoded.used == cookie.used);
		free(copy);
		wb_buffer_release(&record);
	}

	/* An empty value and an empty name (a name with no bytes is allowed). */
	fill_cookie(&cookie, 0);
	cookie.value = NULL;
	cookie.value_length = 0;
	cookie.name = "";
	cookie.name_length = 0;
	wb_buffer_init(&record);
	CHECK_EQ(wb_storage_cookie_encode(&cookie, &record), 0);
	copy = exact_copy(record.data, record.length);
	error = wb_storage_cookie_decode(copy, record.length, &decoded, &original, &original_length);
	CHECK_EQ(error, 0);
	CHECK_EQ(decoded.value_length, 0);
	CHECK_EQ(decoded.name_length, 0);
	free(copy);
	wb_buffer_release(&record);

	/* A value of one mebibyte. */
	big = (char *)malloc(1U << 20);
	CHECK(big != NULL);
	if (big != NULL) {
		for (index = 0; index < (1U << 20); index++)
			big[index] = (char)(index * 31U + 7U);
		fill_cookie(&cookie, WB_STORAGE_COOKIE_HTTP_ONLY);
		cookie.value = big;
		cookie.value_length = 1U << 20;
		wb_buffer_init(&record);
		CHECK_EQ(wb_storage_cookie_encode(&cookie, &record), 0);
		copy = exact_copy(record.data, record.length);
		error = wb_storage_cookie_decode(copy, record.length, &decoded, &original, &original_length);
		CHECK_EQ(error, 0);
		CHECK(same_bytes(decoded.value, decoded.value_length, big, 1U << 20) == 1);
		free(copy);
		wb_buffer_release(&record);
		free(big);
	}

	/* Bytes with NULs and high bits are kept as they are. */
	fill_cookie(&cookie, 0);
	cookie.value = "a\0b\xff\x80";
	cookie.value_length = 5;
	wb_buffer_init(&record);
	CHECK_EQ(wb_storage_cookie_encode(&cookie, &record), 0);
	copy = exact_copy(record.data, record.length);
	CHECK_EQ(wb_storage_cookie_decode(copy, record.length, &decoded, &original, &original_length), 0);
	CHECK(same_bytes(decoded.value, decoded.value_length, "a\0b\xff\x80", 5) == 1);
	free(copy);
	wb_buffer_release(&record);
}

/*
 * Checks the exact bytes of a cookie record against the layout written out by hand.
 */
static void
test_cookie_exact_bytes(void)
{
	struct wb_storage_cookie cookie;
	struct wb_buffer record;
	struct wb_buffer expected;

	/* A small cookie. */
	memset(&cookie, 0, sizeof(cookie));
	cookie.name = "n";
	cookie.name_length = 1;
	cookie.value = "vv";
	cookie.value_length = 2;
	cookie.domain = "d.example";
	cookie.domain_length = 9;
	cookie.path = "/p";
	cookie.path_length = 2;
	cookie.expires = 0x0102030405060708LL;
	cookie.created = -2;
	cookie.used = 3;
	cookie.flags = WB_STORAGE_COOKIE_SECURE | WB_STORAGE_COOKIE_SAME_SITE_LAX;

	/* The record by the header's layout: magic, key, name, value, domain, path, times, flags. */
	wb_buffer_init(&expected);
	put_text(&expected, "KEILAND COOKIE 1\n");
	put_u32(&expected, 4U + 9U + 4U + 2U + 4U + 1U);
	put_u32(&expected, 9);
	put_text(&expected, "d.example");
	put_u32(&expected, 2);
	put_text(&expected, "/p");
	put_u32(&expected, 1);
	put_text(&expected, "n");
	put_u32(&expected, 1);
	put_text(&expected, "n");
	put_u32(&expected, 2);
	put_text(&expected, "vv");
	put_u32(&expected, 9);
	put_text(&expected, "d.example");
	put_u32(&expected, 2);
	put_text(&expected, "/p");
	put_u64(&expected, UINT64_C(0x0102030405060708));
	put_u64(&expected, UINT64_C(0xfffffffffffffffe));
	put_u64(&expected, 3);
	put_u32(&expected, 0x09U);
	wb_buffer_init(&record);
	CHECK_EQ(wb_storage_cookie_encode(&cookie, &record), 0);
	CHECK(same_bytes(record.data, record.length, expected.data, expected.length) == 1);
	wb_buffer_release(&record);
	wb_buffer_release(&expected);
}

/*
 * Checks the cookie encoder's and decoder's refusals.
 */
static void
test_cookie_errors(void)
{
	struct wb_storage_cookie cookie;
	struct wb_storage_cookie decoded;
	struct wb_buffer record;
	struct wb_buffer broken;
	size_t offsets[5];
	size_t index;
	size_t length_at;
	unsigned char *copy;
	const unsigned char *original;
	size_t original_length;
	int error;

	/* The encoder refuses missing arguments and flags it does not know, leaving out unchanged. */
	fill_cookie(&cookie, 0);
	wb_buffer_init(&record);
	CHECK_EQ(wb_buffer_append_string(&record, "keep"), 0);
	CHECK_EQ(wb_storage_cookie_encode(NULL, &record), EINVAL);
	CHECK_EQ(wb_storage_cookie_encode(&cookie, NULL), EINVAL);
	cookie.flags = 0x20U;
	CHECK_EQ(wb_storage_cookie_encode(&cookie, &record), EINVAL);
	cookie.flags = 0x1fU | 0x100U;
	CHECK_EQ(wb_storage_cookie_encode(&cookie, &record), EINVAL);
	fill_cookie(&cookie, 0);
	cookie.path = NULL;
	cookie.path_length = 1;
	CHECK_EQ(wb_storage_cookie_encode(&cookie, &record), EINVAL);
	CHECK_EQ(record.length, 4);
	CHECK(memcmp(record.data, "keep", 4) == 0);
#if SIZE_MAX > 0xffffffffUL
	fill_cookie(&cookie, 0);
	cookie.value_length = (size_t)1 << 32;
	CHECK_EQ(wb_storage_cookie_encode(&cookie, &record), EINVAL);
	CHECK_EQ(record.length, 4);
#endif

	/* A good record to damage; the offsets of the five length fields, found from the layout. */
	wb_buffer_clear(&record);
	fill_cookie(&cookie, WB_STORAGE_COOKIE_HOST_ONLY);
	CHECK_EQ(wb_storage_cookie_encode(&cookie, &record), 0);
	offsets[0] = 17U;
	offsets[1] = offsets[0] + 4U + (4U + cookie.domain_length + 4U + cookie.path_length + 4U + cookie.name_length);
	offsets[2] = offsets[1] + 4U + cookie.name_length;
	offsets[3] = offsets[2] + 4U + cookie.value_length;
	offsets[4] = offsets[3] + 4U + cookie.domain_length;
	copy = exact_copy(record.data, record.length);
	CHECK_EQ(decode_cookie_copy(copy, record.length), 0);
	free(copy);

	/* A wrong magic: each byte of the line, changed. */
	for (index = 0; index < 17U; index++) {
		wb_buffer_init(&broken);
		CHECK_EQ(wb_buffer_append(&broken, record.data, record.length), 0);
		broken.data[index] ^= 0x20U;
		CHECK_EQ(decode_cookie_copy(broken.data, broken.length), EINVAL);
		wb_buffer_release(&broken);
	}
	CHECK_EQ(decode_cookie_copy(record.data, 0), EINVAL);
	CHECK_EQ(decode_cookie_copy(NULL, 0), EINVAL);
	CHECK_EQ(decode_cookie_copy("KEILAND CACHE 1\n", 16), EINVAL);

	/* Each length field made one larger. */
	for (index = 0; index < 5U; index++) {
		wb_buffer_init(&broken);
		CHECK_EQ(wb_buffer_append(&broken, record.data, record.length), 0);
		length_at = offsets[index];
		broken.data[length_at]++;
		CHECK_EQ(decode_cookie_copy(broken.data, broken.length), EINVAL);
		wb_buffer_release(&broken);
	}

	/* Each length field made one smaller (only for non-zero ones), and a huge length. */
	for (index = 0; index < 5U; index++) {
		wb_buffer_init(&broken);
		CHECK_EQ(wb_buffer_append(&broken, record.data, record.length), 0);
		length_at = offsets[index];
		if (broken.data[length_at] != 0)
			broken.data[length_at]--;
		CHECK_EQ(decode_cookie_copy(broken.data, broken.length), EINVAL);
		broken.data[length_at] = 0xffU;
		broken.data[length_at + 1U] = 0xffU;
		broken.data[length_at + 2U] = 0xffU;
		broken.data[length_at + 3U] = 0xffU;
		CHECK_EQ(decode_cookie_copy(broken.data, broken.length), EINVAL);
		wb_buffer_release(&broken);
	}

	/* The last byte taken away, one added, and every shorter prefix. */
	CHECK_EQ(decode_cookie_copy(record.data, record.length - 1U), EINVAL);
	wb_buffer_init(&broken);
	CHECK_EQ(wb_buffer_append(&broken, record.data, record.length), 0);
	CHECK_EQ(wb_buffer_append_byte(&broken, 0), 0);
	CHECK_EQ(decode_cookie_copy(broken.data, broken.length), EINVAL);
	wb_buffer_release(&broken);
	for (index = 0; index < record.length; index++)
		CHECK_EQ(decode_cookie_copy(record.data, index), EINVAL);

	/* A flag bit this version does not know: the lowest and the highest unknown bits. */
	wb_buffer_init(&broken);
	CHECK_EQ(wb_buffer_append(&broken, record.data, record.length), 0);
	broken.data[broken.length - 4U] |= 0x20U;
	CHECK_EQ(decode_cookie_copy(broken.data, broken.length), EINVAL);
	broken.data[broken.length - 4U] &= (unsigned char)~0x20U;
	broken.data[broken.length - 1U] = 0x80U;
	CHECK_EQ(decode_cookie_copy(broken.data, broken.length), EINVAL);
	wb_buffer_release(&broken);

	/* A record whose original key is not the one its fields make: a byte of the key changed. */
	wb_buffer_init(&broken);
	CHECK_EQ(wb_buffer_append(&broken, record.data, record.length), 0);
	broken.data[offsets[0] + 4U + 4U] ^= 0x01U;
	CHECK_EQ(decode_cookie_copy(broken.data, broken.length), EINVAL);
	broken.data[offsets[0] + 4U + 4U] ^= 0x01U;
	CHECK_EQ(decode_cookie_copy(broken.data, broken.length), 0);

	/* The same record with a byte of the name changed is a different cookie than its key says. */
	broken.data[offsets[1] + 4U] ^= 0x01U;
	CHECK_EQ(decode_cookie_copy(broken.data, broken.length), EINVAL);
	wb_buffer_release(&broken);

	/* A failure zeroes the cookie and the original key, however full they were. */
	memset(&decoded, 0xa5, sizeof(decoded));
	original = (const unsigned char *)&decoded;
	original_length = 77;
	error = wb_storage_cookie_decode(record.data, record.length - 1U, &decoded, &original, &original_length);
	CHECK_EQ(error, EINVAL);
	CHECK(is_zero(&decoded, sizeof(decoded)) == 1);
	CHECK(original == NULL);
	CHECK_EQ(original_length, 0);

	/* A missing place for the cookie is refused; the original key outputs may be left out. */
	CHECK_EQ(wb_storage_cookie_decode(record.data, record.length, NULL, &original, &original_length), EINVAL);
	CHECK_EQ(wb_storage_cookie_decode(record.data, record.length, &decoded, NULL, NULL), 0);

	wb_buffer_release(&record);
}

/*
 * Checks the cache record: round trips, the refusals and every damaged length.
 */
static void
test_cache(void)
{
	static const int64_t ages[] = { -1, 0, 1, 31536000, INT64_MAX };
	struct wb_storage_cache_entry entry;
	struct wb_storage_cache_entry decoded;
	struct wb_buffer record;
	struct wb_buffer broken;
	struct wb_buffer expected;
	unsigned char *copy;
	size_t index;
	size_t offsets[4];
	int no_cache;
	int error;

	/* Round trips over the freshness values and the no-cache flag. */
	for (index = 0; index < sizeof(ages) / sizeof(ages[0]); index++) {
		for (no_cache = 0; no_cache <= 1; no_cache++) {
			memset(&entry, 0, sizeof(entry));
			entry.url = "https://a.example/x?y=1";
			entry.url_length = 23;
			entry.content_type = "text/html";
			entry.content_type_length = 9;
			entry.etag = "\"abc\"";
			entry.etag_length = 5;
			entry.stored = -123456789012LL;
			entry.max_age = ages[index];
			entry.no_cache = no_cache;
			entry.body = (const unsigned char *)"<p>\0\xff</p>";
			entry.body_length = 9;
			wb_buffer_init(&record);
			CHECK_EQ(wb_storage_cache_encode(&entry, &record), 0);
			copy = exact_copy(record.data, record.length);
			error = wb_storage_cache_decode(copy, record.length, &decoded);
			CHECK_EQ(error, 0);
			CHECK(same_bytes(decoded.url, decoded.url_length, entry.url, entry.url_length) == 1);
			CHECK(same_bytes(decoded.content_type, decoded.content_type_length, entry.content_type, entry.content_type_length) == 1);
			CHECK(same_bytes(decoded.etag, decoded.etag_length, entry.etag, entry.etag_length) == 1);
			CHECK(same_bytes(decoded.body, decoded.body_length, entry.body, entry.body_length) == 1);
			CHECK(decoded.stored == entry.stored);
			CHECK(decoded.max_age == entry.max_age);
			CHECK_EQ(decoded.no_cache, no_cache);
			free(copy);
			wb_buffer_release(&record);
		}
	}

	/* An entry with an empty content type, etag and body. */
	memset(&entry, 0, sizeof(entry));
	entry.url = "u";
	entry.url_length = 1;
	entry.max_age = -1;
	wb_buffer_init(&record);
	CHECK_EQ(wb_storage_cache_encode(&entry, &record), 0);
	copy = exact_copy(record.data, record.length);
	CHECK_EQ(wb_storage_cache_decode(copy, record.length, &decoded), 0);
	CHECK_EQ(decoded.content_type_length, 0);
	CHECK_EQ(decoded.etag_length, 0);
	CHECK_EQ(decoded.body_length, 0);
	CHECK_EQ(decoded.url_length, 1);
	free(copy);

	/* The exact bytes by the header's layout. */
	wb_buffer_init(&expected);
	put_text(&expected, "KEILAND CACHE 1\n");
	put_u32(&expected, 1);
	put_text(&expected, "u");
	put_u32(&expected, 0);
	put_u32(&expected, 0);
	put_u64(&expected, 0);
	put_u64(&expected, UINT64_C(0xffffffffffffffff));
	put_u32(&expected, 0);
	put_u32(&expected, 0);
	CHECK(same_bytes(record.data, record.length, expected.data, expected.length) == 1);
	wb_buffer_release(&expected);
	wb_buffer_release(&record);

	/* The encoder's refusals leave the buffer as it was. */
	wb_buffer_init(&record);
	CHECK_EQ(wb_buffer_append_string(&record, "keep"), 0);
	CHECK_EQ(wb_storage_cache_encode(NULL, &record), EINVAL);
	CHECK_EQ(wb_storage_cache_encode(&entry, NULL), EINVAL);
	entry.url_length = 0;
	CHECK_EQ(wb_storage_cache_encode(&entry, &record), EINVAL);
	entry.url = NULL;
	entry.url_length = 1;
	CHECK_EQ(wb_storage_cache_encode(&entry, &record), EINVAL);
	entry.url = "u";
	entry.max_age = -2;
	CHECK_EQ(wb_storage_cache_encode(&entry, &record), EINVAL);
	entry.max_age = INT64_MIN;
	CHECK_EQ(wb_storage_cache_encode(&entry, &record), EINVAL);
	entry.max_age = 0;
	entry.no_cache = 2;
	CHECK_EQ(wb_storage_cache_encode(&entry, &record), EINVAL);
	entry.no_cache = -1;
	CHECK_EQ(wb_storage_cache_encode(&entry, &record), EINVAL);
	entry.no_cache = 0;
	entry.body = NULL;
	entry.body_length = 1;
	CHECK_EQ(wb_storage_cache_encode(&entry, &record), EINVAL);
	entry.body_length = 0;
#if SIZE_MAX > 0xffffffffUL
	entry.etag = "e";
	entry.etag_length = (size_t)1 << 32;
	CHECK_EQ(wb_storage_cache_encode(&entry, &record), EINVAL);
	entry.etag = NULL;
	entry.etag_length = 0;
#endif
	CHECK_EQ(record.length, 4);
	CHECK(memcmp(record.data, "keep", 4) == 0);
	wb_buffer_release(&record);

	/* A good record to damage; the offsets of its four length fields. */
	memset(&entry, 0, sizeof(entry));
	entry.url = "http://h/p";
	entry.url_length = 10;
	entry.content_type = "text/css";
	entry.content_type_length = 8;
	entry.etag = "W/1";
	entry.etag_length = 3;
	entry.max_age = 60;
	entry.body = (const unsigned char *)"body{}";
	entry.body_length = 6;
	wb_buffer_init(&record);
	CHECK_EQ(wb_storage_cache_encode(&entry, &record), 0);
	offsets[0] = 16U;
	offsets[1] = offsets[0] + 4U + entry.url_length;
	offsets[2] = offsets[1] + 4U + entry.content_type_length;
	offsets[3] = offsets[2] + 4U + entry.etag_length + 8U + 8U + 4U;
	CHECK_EQ(decode_cache_copy(record.data, record.length), 0);

	/* Each length one larger, and a huge length. */
	for (index = 0; index < 4U; index++) {
		wb_buffer_init(&broken);
		CHECK_EQ(wb_buffer_append(&broken, record.data, record.length), 0);
		broken.data[offsets[index]]++;
		CHECK_EQ(decode_cache_copy(broken.data, broken.length), EINVAL);
		broken.data[offsets[index]]--;
		broken.data[offsets[index] + 3U] = 0x80U;
		CHECK_EQ(decode_cache_copy(broken.data, broken.length), EINVAL);
		wb_buffer_release(&broken);
	}

	/* Every prefix, and one byte too many. */
	for (index = 0; index < record.length; index++)
		CHECK_EQ(decode_cache_copy(record.data, index), EINVAL);
	wb_buffer_init(&broken);
	CHECK_EQ(wb_buffer_append(&broken, record.data, record.length), 0);
	CHECK_EQ(wb_buffer_append_byte(&broken, 0), 0);
	CHECK_EQ(decode_cache_copy(broken.data, broken.length), EINVAL);
	wb_buffer_release(&broken);

	/* Wrong magic. */
	wb_buffer_init(&broken);
	CHECK_EQ(wb_buffer_append(&broken, record.data, record.length), 0);
	broken.data[0] = 'X';
	CHECK_EQ(decode_cache_copy(broken.data, broken.length), EINVAL);
	wb_buffer_release(&broken);
	CHECK_EQ(decode_cache_copy(NULL, 0), EINVAL);

	/* An empty URL, max_age -2 and no_cache 2, written by hand. */
	wb_buffer_init(&broken);
	put_text(&broken, "KEILAND CACHE 1\n");
	put_u32(&broken, 0);
	put_u32(&broken, 0);
	put_u32(&broken, 0);
	put_u64(&broken, 0);
	put_u64(&broken, 5);
	put_u32(&broken, 0);
	put_u32(&broken, 0);
	CHECK_EQ(decode_cache_copy(broken.data, broken.length), EINVAL);
	wb_buffer_release(&broken);

	wb_buffer_init(&broken);
	put_text(&broken, "KEILAND CACHE 1\n");
	put_u32(&broken, 1);
	put_text(&broken, "u");
	put_u32(&broken, 0);
	put_u32(&broken, 0);
	put_u64(&broken, 0);
	put_u64(&broken, UINT64_C(0xfffffffffffffffe));
	put_u32(&broken, 0);
	put_u32(&broken, 0);
	CHECK_EQ(decode_cache_copy(broken.data, broken.length), EINVAL);
	wb_buffer_release(&broken);

	wb_buffer_init(&broken);
	put_text(&broken, "KEILAND CACHE 1\n");
	put_u32(&broken, 1);
	put_text(&broken, "u");
	put_u32(&broken, 0);
	put_u32(&broken, 0);
	put_u64(&broken, 0);
	put_u64(&broken, 0);
	put_u32(&broken, 2);
	put_u32(&broken, 0);
	CHECK_EQ(decode_cache_copy(broken.data, broken.length), EINVAL);
	wb_buffer_release(&broken);

	/* The same record with no_cache 1 and max_age 0 is good. */
	wb_buffer_init(&broken);
	put_text(&broken, "KEILAND CACHE 1\n");
	put_u32(&broken, 1);
	put_text(&broken, "u");
	put_u32(&broken, 0);
	put_u32(&broken, 0);
	put_u64(&broken, 0);
	put_u64(&broken, 0);
	put_u32(&broken, 1);
	put_u32(&broken, 0);
	CHECK_EQ(decode_cache_copy(broken.data, broken.length), 0);
	wb_buffer_release(&broken);

	/* A failure zeroes the entry; a missing entry is refused. */
	memset(&decoded, 0xa5, sizeof(decoded));
	CHECK_EQ(wb_storage_cache_decode(record.data, record.length - 1U, &decoded), EINVAL);
	CHECK(is_zero(&decoded, sizeof(decoded)) == 1);
	CHECK_EQ(wb_storage_cache_decode(record.data, record.length, NULL), EINVAL);

	wb_buffer_release(&record);
}

/*
 * Checks localStorage records of version 2.
 */
static void
test_local_v2(void)
{
	static const uint16_t surrogates[] = { 0xd800, 0x0041, 0xdc00, 0xffff, 0x0000 };
	struct wb_storage_local_item *items;
	struct wb_storage_local_item one;
	struct local_seen seen;
	struct wb_buffer record;
	struct wb_buffer expected;
	uint16_t *units;
	unsigned char *copy;
	const unsigned char *origin;
	size_t origin_length;
	size_t index;
	int error;

	/* No items: just the magic and the origin. */
	wb_buffer_init(&record);
	error = wb_storage_local_encode("https://a.example", 17, NULL, 0, &record);
	CHECK_EQ(error, 0);
	wb_buffer_init(&expected);
	put_text(&expected, "KEILAND LOCAL STORAGE 2\n");
	put_u32(&expected, 17);
	put_text(&expected, "https://a.example");
	CHECK(same_bytes(record.data, record.length, expected.data, expected.length) == 1);
	memset(&seen, 0, sizeof(seen));
	copy = exact_copy(record.data, record.length);
	origin = NULL;
	origin_length = 0;
	error = wb_storage_local_decode(copy, record.length, &origin, &origin_length, local_each, &seen);
	CHECK_EQ(error, 0);
	CHECK_EQ(seen.count, 0);
	CHECK(same_bytes(origin, origin_length, "https://a.example", 17) == 1);
	free(copy);
	wb_buffer_release(&expected);
	wb_buffer_release(&record);

	/* One item with an empty key and an empty value. */
	memset(&one, 0, sizeof(one));
	wb_buffer_init(&record);
	error = wb_storage_local_encode("o", 1, &one, 1, &record);
	CHECK_EQ(error, 0);
	memset(&seen, 0, sizeof(seen));
	copy = exact_copy(record.data, record.length);
	error = wb_storage_local_decode(copy, record.length, &origin, &origin_length, local_each, &seen);
	CHECK_EQ(error, 0);
	CHECK_EQ(seen.count, 1);
	CHECK_EQ(seen.first_key_length, 0);
	CHECK_EQ(seen.last_value_length, 0);
	free(copy);
	wb_buffer_release(&record);

	/* One item whose key and value hold surrogate units that are not pairs, and a NUL unit. */
	one.key = surrogates;
	one.key_length = 5;
	one.value = surrogates + 1;
	one.value_length = 4;
	wb_buffer_init(&record);
	error = wb_storage_local_encode("o", 1, &one, 1, &record);
	CHECK_EQ(error, 0);
	wb_buffer_init(&expected);
	put_text(&expected, "KEILAND LOCAL STORAGE 2\n");
	put_u32(&expected, 1);
	put_text(&expected, "o");
	put_u32(&expected, 5);
	put_u32(&expected, 4);
	put_units(&expected, surrogates, 5);
	put_units(&expected, surrogates + 1, 4);
	CHECK(same_bytes(record.data, record.length, expected.data, expected.length) == 1);
	wb_buffer_release(&expected);
	memset(&seen, 0, sizeof(seen));
	copy = exact_copy(record.data, record.length);
	error = wb_storage_local_decode(copy, record.length, &origin, &origin_length, local_each, &seen);
	CHECK_EQ(error, 0);
	CHECK_EQ(seen.first_key_length, 5);
	CHECK(memcmp(seen.first_key, surrogates, sizeof(surrogates)) == 0);
	CHECK_EQ(seen.last_value_length, 4);
	CHECK(memcmp(seen.last_value, surrogates + 1, 4 * sizeof(uint16_t)) == 0);
	free(copy);
	wb_buffer_release(&record);

	/* A thousand items, in the order they were given. */
	items = (struct wb_storage_local_item *)calloc(1000, sizeof(*items));
	units = (uint16_t *)calloc(1000, sizeof(*units));
	CHECK(items != NULL && units != NULL);
	if (items != NULL && units != NULL) {
		for (index = 0; index < 1000; index++) {
			units[index] = (uint16_t)(index * 65U + 1U);
			items[index].key = units + index;
			items[index].key_length = 1;
			items[index].value = units + index;
			items[index].value_length = (index % 3U == 0) ? 1U : 0U;
		}
		wb_buffer_init(&record);
		error = wb_storage_local_encode("origin", 6, items, 1000, &record);
		CHECK_EQ(error, 0);
		memset(&seen, 0, sizeof(seen));
		seen.stop_after = 0;
		copy = exact_copy(record.data, record.length);
		error = wb_storage_local_decode(copy, record.length, &origin, &origin_length, count_each, &seen);
		CHECK_EQ(error, 0);
		CHECK_EQ(seen.count, 1000);
		CHECK_EQ(seen.total_units, 1000 + 334);
		free(copy);

		/* The last item reached the function last, with its own key. */
		memset(&seen, 0, sizeof(seen));
		copy = exact_copy(record.data, record.length);
		error = wb_storage_local_decode(copy, record.length, &origin, &origin_length, local_each, &seen);
		CHECK_EQ(error, 0);
		CHECK_EQ(seen.count, 1000);
		CHECK_EQ(seen.first_key[0], 1);
		CHECK_EQ(seen.last_key[0], units[999]);
		free(copy);

		/* A function that returns 9 after the fifth item stops the decoding with 9. */
		memset(&seen, 0, sizeof(seen));
		seen.stop_after = 5;
		seen.stop_value = 9;
		copy = exact_copy(record.data, record.length);
		error = wb_storage_local_decode(copy, record.length, &origin, &origin_length, local_each, &seen);
		CHECK_EQ(error, 9);
		CHECK_EQ(seen.count, 5);
		free(copy);
		wb_buffer_release(&record);
	}
	free(items);
	free(units);

	/* The encoder's refusals leave the buffer as it was. */
	wb_buffer_init(&record);
	CHECK_EQ(wb_buffer_append_string(&record, "keep"), 0);
	CHECK_EQ(wb_storage_local_encode(NULL, 1, NULL, 0, &record), EINVAL);
	CHECK_EQ(wb_storage_local_encode("o", 0, NULL, 0, &record), EINVAL);
	CHECK_EQ(wb_storage_local_encode("o", 1, NULL, 0, NULL), EINVAL);
	CHECK_EQ(wb_storage_local_encode("o", 1, NULL, 1, &record), EINVAL);
	memset(&one, 0, sizeof(one));
	one.key_length = 1;
	CHECK_EQ(wb_storage_local_encode("o", 1, &one, 1, &record), EINVAL);
	memset(&one, 0, sizeof(one));
	one.value_length = 1;
	CHECK_EQ(wb_storage_local_encode("o", 1, &one, 1, &record), EINVAL);
#if SIZE_MAX > 0xffffffffUL
	memset(&one, 0, sizeof(one));
	one.key = surrogates;
	one.key_length = (size_t)1 << 32;
	CHECK_EQ(wb_storage_local_encode("o", 1, &one, 1, &record), EINVAL);
#endif
	CHECK_EQ(record.length, 4);
	CHECK(memcmp(record.data, "keep", 4) == 0);
	wb_buffer_release(&record);
}

/*
 * Checks that version 1 records, written by hand, are read.
 */
static void
test_local_v1(void)
{
	static const uint16_t key[] = { 0x0061, 0x0062 };
	static const uint16_t value[] = { 0x3042 };
	struct local_seen seen;
	struct wb_buffer record;
	const unsigned char *origin;
	size_t origin_length;
	int error;

	/* The magic line and one item: the key's length, the value's, the key's units, the value's. */
	wb_buffer_init(&record);
	put_text(&record, "KEILAND LOCAL STORAGE 1\n");
	put_u32(&record, 2);
	put_u32(&record, 1);
	put_units(&record, key, 2);
	put_units(&record, value, 1);
	put_u32(&record, 0);
	put_u32(&record, 0);
	memset(&seen, 0, sizeof(seen));
	origin = (const unsigned char *)&seen;
	origin_length = 5;
	error = wb_storage_local_decode(record.data, record.length, &origin, &origin_length, local_each, &seen);
	CHECK_EQ(error, 0);
	CHECK(origin == NULL);
	CHECK_EQ(origin_length, 0);
	CHECK_EQ(seen.count, 2);
	CHECK_EQ(seen.first_key_length, 2);
	CHECK_EQ(seen.first_key[0], 0x61);
	CHECK_EQ(seen.first_key[1], 0x62);
	CHECK_EQ(seen.last_key_length, 0);
	CHECK_EQ(seen.last_value_length, 0);
	wb_buffer_release(&record);

	/* Only the magic line: no items. */
	wb_buffer_init(&record);
	put_text(&record, "KEILAND LOCAL STORAGE 1\n");
	memset(&seen, 0, sizeof(seen));
	error = wb_storage_local_decode(record.data, record.length, NULL, NULL, local_each, &seen);
	CHECK_EQ(error, 0);
	CHECK_EQ(seen.count, 0);

	/* An item cut in the middle of a key unit is refused. */
	put_u32(&record, 2);
	put_u32(&record, 1);
	CHECK_EQ(wb_buffer_append_byte(&record, 0x61), 0);
	CHECK_EQ(decode_local_copy(record.data, record.length), EINVAL);
	wb_buffer_release(&record);
}

/*
 * Checks the localStorage decoder's refusals, and the prefixes of a record.
 */
static void
test_local_errors(void)
{
	static const uint16_t units[] = { 0x0041, 0x0042, 0x0043 };
	struct wb_storage_local_item items[3];
	struct wb_buffer record;
	struct wb_buffer broken;
	size_t index;
	size_t good_prefixes;
	size_t item_ends[4];
	size_t offset;
	int error;

	/* A record of three items, with the ends of the items known. */
	items[0].key = units;
	items[0].key_length = 1;
	items[0].value = units + 1;
	items[0].value_length = 2;
	items[1].key = units;
	items[1].key_length = 3;
	items[1].value = units;
	items[1].value_length = 0;
	items[2].key = units;
	items[2].key_length = 0;
	items[2].value = units + 2;
	items[2].value_length = 1;
	wb_buffer_init(&record);
	error = wb_storage_local_encode("ab", 2, items, 3, &record);
	CHECK_EQ(error, 0);
	offset = 24U + 4U + 2U;
	item_ends[0] = offset;
	offset += 8U + 2U * (1U + 2U);
	item_ends[1] = offset;
	offset += 8U + 2U * (3U + 0U);
	item_ends[2] = offset;
	offset += 8U + 2U * (0U + 1U);
	item_ends[3] = offset;
	CHECK_EQ(item_ends[3], record.length);

	/* A prefix is good exactly where an item ends: that many items fewer. */
	good_prefixes = 0;
	for (index = 0; index <= record.length; index++) {
		error = decode_local_copy(record.data, index);
		if (error == 0)
			good_prefixes++;
		else
			CHECK_EQ(error, EINVAL);
	}
	CHECK_EQ(good_prefixes, 4);
	for (index = 0; index < 4U; index++)
		CHECK_EQ(decode_local_copy(record.data, item_ends[index]), 0);

	/* Version 1: the same, with the items alone. */
	wb_buffer_init(&broken);
	put_text(&broken, "KEILAND LOCAL STORAGE 1\n");
	CHECK_EQ(wb_buffer_append(&broken, record.data + item_ends[0], record.length - item_ends[0]), 0);
	good_prefixes = 0;
	for (index = 0; index <= broken.length; index++) {
		error = decode_local_copy(broken.data, index);
		if (error == 0)
			good_prefixes++;
	}
	CHECK_EQ(good_prefixes, 4);
	wb_buffer_release(&broken);

	/* A huge length of a key, and of a value, in the first item. */
	wb_buffer_init(&broken);
	CHECK_EQ(wb_buffer_append(&broken, record.data, record.length), 0);
	broken.data[item_ends[0] + 3U] = 0xffU;
	CHECK_EQ(decode_local_copy(broken.data, broken.length), EINVAL);
	broken.data[item_ends[0] + 3U] = 0;
	broken.data[item_ends[0] + 4U + 3U] = 0xffU;
	CHECK_EQ(decode_local_copy(broken.data, broken.length), EINVAL);
	broken.data[item_ends[0] + 4U + 3U] = 0;

	/* Both lengths at the largest value: their sum would overflow a 32-bit count. */
	memset(broken.data + item_ends[0], 0xff, 8);
	CHECK_EQ(decode_local_copy(broken.data, broken.length), EINVAL);
	wb_buffer_release(&broken);

	/* An origin that runs past the end, and an empty origin, which no encoder writes. */
	wb_buffer_init(&broken);
	put_text(&broken, "KEILAND LOCAL STORAGE 2\n");
	put_u32(&broken, 5);
	put_text(&broken, "abc");
	CHECK_EQ(decode_local_copy(broken.data, broken.length), EINVAL);
	wb_buffer_release(&broken);
	wb_buffer_init(&broken);
	put_text(&broken, "KEILAND LOCAL STORAGE 2\n");
	put_u32(&broken, 0);
	CHECK_EQ(decode_local_copy(broken.data, broken.length), EINVAL);
	wb_buffer_release(&broken);

	/* The magic: other versions, other kinds, none, and a missing function. */
	wb_buffer_init(&broken);
	put_text(&broken, "KEILAND LOCAL STORAGE 3\n");
	CHECK_EQ(decode_local_copy(broken.data, broken.length), EINVAL);
	wb_buffer_release(&broken);
	CHECK_EQ(decode_local_copy(NULL, 0), EINVAL);
	CHECK_EQ(decode_local_copy("KEILAND COOKIE 1\n", 17), EINVAL);
	CHECK_EQ(wb_storage_local_decode(record.data, record.length, NULL, NULL, NULL, NULL), EINVAL);

	wb_buffer_release(&record);
}

/*
 * Feeds records made of random bytes, some starting with a real magic line, to the decoders.
 */
static void
test_random(void)
{
	static const char *const magics[] = {
		"KEILAND COOKIE 1\n", "KEILAND CACHE 1\n",
		"KEILAND LOCAL STORAGE 1\n", "KEILAND LOCAL STORAGE 2\n"
	};
	struct wb_buffer record;
	size_t length;
	size_t index;
	size_t count;
	size_t magic;
	unsigned char byte;
	int error;

	/* Each record is a random length of random bytes, behind a magic line half of the time. */
	for (count = 0; count < RANDOM_RECORDS; count++) {
		wb_buffer_init(&record);
		magic = random_next() % 8U;
		if (magic < 4U)
			put_text(&record, magics[magic]);
		length = random_next() % 4097U;
		error = 0;
		for (index = 0; index < length; index++) {
			byte = (unsigned char)(random_next() >> 16);

			/* A third of the bytes are zero or 0xff, which make lengths of interesting sizes. */
			if (random_next() % 3U == 0)
				byte = (unsigned char)((random_next() & 1U) ? 0xffU : 0x00U);
			error = wb_buffer_append_byte(&record, byte);
			if (error != 0)
				break;
		}
		CHECK_EQ(error, 0);

		/* All three decoders must end, with 0 or an error, and never read outside the record. */
		(void)decode_cookie_copy(record.data, record.length);
		(void)decode_cache_copy(record.data, record.length);
		(void)decode_local_copy(record.data, record.length);
		wb_buffer_release(&record);
	}
	CHECK(count == RANDOM_RECORDS);
}

/*
 * Changes each byte of good records one at a time and decodes the result.
 */
static void
test_mutation(void)
{
	static const uint16_t units[] = { 0x0041, 0xd800, 0x0043 };
	struct wb_storage_cookie cookie;
	struct wb_storage_cache_entry entry;
	struct wb_storage_local_item items[2];
	struct wb_buffer cookie_record;
	struct wb_buffer cache_record;
	struct wb_buffer local_record;
	size_t index;
	unsigned char saved;
	unsigned variant;
	unsigned char changed;
	size_t accepted;

	/* Three small good records. */
	fill_cookie(&cookie, WB_STORAGE_COOKIE_SECURE);
	wb_buffer_init(&cookie_record);
	CHECK_EQ(wb_storage_cookie_encode(&cookie, &cookie_record), 0);
	memset(&entry, 0, sizeof(entry));
	entry.url = "http://h/x";
	entry.url_length = 10;
	entry.content_type = "t";
	entry.content_type_length = 1;
	entry.etag = "e";
	entry.etag_length = 1;
	entry.max_age = 5;
	entry.body = (const unsigned char *)"bbbb";
	entry.body_length = 4;
	wb_buffer_init(&cache_record);
	CHECK_EQ(wb_storage_cache_encode(&entry, &cache_record), 0);
	items[0].key = units;
	items[0].key_length = 3;
	items[0].value = units;
	items[0].value_length = 1;
	items[1].key = units + 1;
	items[1].key_length = 1;
	items[1].value = units;
	items[1].value_length = 0;
	wb_buffer_init(&local_record);
	CHECK_EQ(wb_storage_local_encode("origin", 6, items, 2, &local_record), 0);

	/* Each byte becomes its complement, zero, 0xff and its successor, in turn. */
	accepted = 0;
	for (index = 0; index < cookie_record.length; index++) {
		saved = cookie_record.data[index];
		for (variant = 0; variant < 4U; variant++) {
			changed = saved;
			if (variant == 0)
				changed = (unsigned char)~saved;
			else if (variant == 1)
				changed = 0;
			else if (variant == 2)
				changed = 0xffU;
			else
				changed = (unsigned char)(saved + 1U);
			cookie_record.data[index] = changed;
			if (decode_cookie_copy(cookie_record.data, cookie_record.length) == 0)
				accepted++;
		}
		cookie_record.data[index] = saved;
	}
	for (index = 0; index < cache_record.length; index++) {
		saved = cache_record.data[index];
		for (variant = 0; variant < 4U; variant++) {
			changed = (unsigned char)(saved ^ (0x01U << variant));
			cache_record.data[index] = changed;
			if (decode_cache_copy(cache_record.data, cache_record.length) == 0)
				accepted++;
			changed = (unsigned char)(saved ^ (0x10U << variant));
			cache_record.data[index] = changed;
			if (decode_cache_copy(cache_record.data, cache_record.length) == 0)
				accepted++;
		}
		cache_record.data[index] = saved;
	}
	for (index = 0; index < local_record.length; index++) {
		saved = local_record.data[index];
		for (variant = 0; variant < 8U; variant++) {
			changed = (unsigned char)(saved ^ (0x01U << variant));
			local_record.data[index] = changed;
			if (decode_local_copy(local_record.data, local_record.length) == 0)
				accepted++;
		}
		local_record.data[index] = saved;
	}

	/* The records are whole again, and the changes that were accepted are only value bytes. */
	CHECK_EQ(decode_cookie_copy(cookie_record.data, cookie_record.length), 0);
	CHECK_EQ(decode_cache_copy(cache_record.data, cache_record.length), 0);
	CHECK_EQ(decode_local_copy(local_record.data, local_record.length), 0);
	CHECK(accepted > 0);
	CHECK(accepted < 8U * (cookie_record.length + cache_record.length + local_record.length));

	wb_buffer_release(&cookie_record);
	wb_buffer_release(&cache_record);
	wb_buffer_release(&local_record);
}

/*
 * Checks the sink: binding, appending, the size limit and the sticky error.
 */
static void
test_sink(void)
{
	struct browser_storage_sink sink;
	struct browser_storage_sink other;
	struct wb_buffer buffer;
	struct wb_buffer other_buffer;
	unsigned char *chunk;
	size_t index;
	int error;

	/* A sink binds to a buffer that already holds bytes; the record is what follows them. */
	wb_buffer_init(&buffer);
	CHECK_EQ(wb_buffer_append_string(&buffer, "head"), 0);
	wb_storage_sink_init(&sink, &buffer);
	CHECK_EQ(sink.start, 4);
	CHECK_EQ(wb_storage_sink_error(&sink), 0);
	CHECK_EQ(browser_storage_sink_append(&sink, "ab", 2), 0);
	CHECK_EQ(browser_storage_sink_append(&sink, "cde", 3), 0);
	CHECK_EQ(browser_storage_sink_append(&sink, NULL, 0), 0);
	CHECK_EQ(buffer.length, 9);
	CHECK(memcmp(buffer.data, "headabcde", 9) == 0);
	CHECK_EQ(wb_storage_sink_error(&sink), 0);

	/* Arguments that are missing; they do not poison the sink. */
	CHECK_EQ(browser_storage_sink_append(NULL, "a", 1), EINVAL);
	CHECK_EQ(browser_storage_sink_append(&sink, NULL, 1), EINVAL);
	CHECK_EQ(wb_storage_sink_error(&sink), 0);
	CHECK_EQ(browser_storage_sink_append(&sink, "f", 1), 0);
	CHECK_EQ(wb_storage_sink_error(NULL), EINVAL);

	/* The record may be exactly the largest size; the next byte is too much. */
	wb_buffer_init(&other_buffer);
	wb_storage_sink_init(&other, &other_buffer);
	chunk = (unsigned char *)calloc(1, 1U << 20);
	CHECK(chunk != NULL);
	if (chunk != NULL) {
		for (index = 0; index < WB_STORAGE_RECORD_MAX / (1U << 20); index++) {
			error = browser_storage_sink_append(&other, chunk, 1U << 20);
			CHECK_EQ(error, 0);
		}
		CHECK_EQ(other_buffer.length, WB_STORAGE_RECORD_MAX);
		CHECK_EQ(wb_storage_sink_error(&other), 0);
		CHECK_EQ(browser_storage_sink_append(&other, chunk, 1), EFBIG);
		CHECK_EQ(wb_storage_sink_error(&other), EFBIG);

		/* Every later append fails the same way, even an empty one, and nothing is added. */
		CHECK_EQ(browser_storage_sink_append(&other, chunk, 0), EFBIG);
		CHECK_EQ(browser_storage_sink_append(&other, "x", 1), EFBIG);
		CHECK_EQ(other_buffer.length, WB_STORAGE_RECORD_MAX);
		free(chunk);
	}
	wb_buffer_release(&other_buffer);

	/* The limit counts the record, not the bytes before it. */
	wb_buffer_init(&other_buffer);
	CHECK_EQ(wb_buffer_append_string(&other_buffer, "0123456789"), 0);
	wb_storage_sink_init(&other, &other_buffer);
	chunk = (unsigned char *)calloc(1, WB_STORAGE_RECORD_MAX);
	CHECK(chunk != NULL);
	if (chunk != NULL) {
		CHECK_EQ(browser_storage_sink_append(&other, chunk, WB_STORAGE_RECORD_MAX), 0);
		CHECK_EQ(browser_storage_sink_append(&other, chunk, 1), EFBIG);
		free(chunk);
	}

	/* An append whose length alone is larger than the limit, and one that would wrap the sum. */
	wb_buffer_release(&other_buffer);
	wb_buffer_init(&other_buffer);
	wb_storage_sink_init(&other, &other_buffer);
	CHECK_EQ(browser_storage_sink_append(&other, "x", SIZE_MAX), EFBIG);
	CHECK_EQ(wb_storage_sink_error(&other), EFBIG);
	CHECK_EQ(other_buffer.length, 0);

	wb_buffer_release(&other_buffer);
	wb_buffer_release(&buffer);
}

/*
 * Copies bytes to a block of exactly their size, so a read past the end is caught.
 */
static unsigned char *
exact_copy(
	const void *bytes,
	size_t length)
{
	unsigned char *copy;

	/* A block of the size, at least one byte so that an empty record has an address. */
	copy = (unsigned char *)malloc(length == 0 ? 1U : length);
	if (copy == NULL) {
		CHECK(copy != NULL);
		abort();
	}
	if (length != 0)
		memcpy(copy, bytes, length);

	/* Succeeded: the copy. */
	return copy;
}

/*
 * Decodes a cookie record from an exact copy and reports the error.
 */
static int
decode_cookie_copy(
	const void *bytes,
	size_t length)
{
	struct wb_storage_cookie cookie;
	unsigned char *copy;
	const unsigned char *original;
	size_t original_length;
	int error;

	/* NULL stays NULL, so the decoder's handling of no bytes is tried. */
	copy = NULL;
	if (bytes != NULL)
		copy = exact_copy(bytes, length);
	error = wb_storage_cookie_decode(copy, length, &cookie, &original, &original_length);

	/* A failure leaves everything zero. */
	if (error != 0) {
		CHECK(is_zero(&cookie, sizeof(cookie)) == 1);
		CHECK(original == NULL);
		CHECK_EQ(original_length, 0);
	}
	free(copy);

	/* Succeeded: the error, or 0. */
	return error;
}

/*
 * Decodes a cache record from an exact copy and reports the error.
 */
static int
decode_cache_copy(
	const void *bytes,
	size_t length)
{
	struct wb_storage_cache_entry entry;
	unsigned char *copy;
	int error;

	/* NULL stays NULL. */
	copy = NULL;
	if (bytes != NULL)
		copy = exact_copy(bytes, length);
	error = wb_storage_cache_decode(copy, length, &entry);

	/* A failure leaves the entry zero. */
	if (error != 0)
		CHECK(is_zero(&entry, sizeof(entry)) == 1);
	free(copy);

	/* Succeeded: the error, or 0. */
	return error;
}

/*
 * Decodes a localStorage record from an exact copy, counting the items, and reports the error.
 */
static int
decode_local_copy(
	const void *bytes,
	size_t length)
{
	struct local_seen seen;
	unsigned char *copy;
	int error;

	/* NULL stays NULL. */
	memset(&seen, 0, sizeof(seen));
	copy = NULL;
	if (bytes != NULL)
		copy = exact_copy(bytes, length);
	error = wb_storage_local_decode(copy, length, NULL, NULL, count_each, &seen);
	free(copy);

	/* Succeeded: the error, or 0. */
	return error;
}

/*
 * Records an item, and stops the decoding when the stop count is reached.
 */
static int
local_each(
	void *context,
	const uint16_t *key,
	size_t key_length,
	const uint16_t *value,
	size_t value_length)
{
	struct local_seen *seen;
	size_t index;

	/* The units of an empty key or value are still a valid address. */
	seen = (struct local_seen *)context;
	CHECK(key != NULL);
	CHECK(value != NULL);

	/* Keeps the first item's key, and the last item's key and value (up to eight units). */
	if (seen->count == 0) {
		seen->first_key_length = key_length;
		for (index = 0; index < key_length && index < 8U; index++)
			seen->first_key[index] = key[index];
	}
	seen->last_key_length = key_length;
	for (index = 0; index < key_length && index < 8U; index++)
		seen->last_key[index] = key[index];
	seen->last_value_length = value_length;
	for (index = 0; index < value_length && index < 8U; index++)
		seen->last_value[index] = value[index];
	seen->count++;
	seen->total_units += value_length;

	/* Stops with the chosen value after the chosen number of items. */
	if (seen->stop_after != 0 && seen->count == seen->stop_after)
		return seen->stop_value;

	/* Goes on. */
	return 0;
}

/*
 * Counts the items and their units, touching every unit so that a bad pointer would be caught.
 */
static int
count_each(
	void *context,
	const uint16_t *key,
	size_t key_length,
	const uint16_t *value,
	size_t value_length)
{
	struct local_seen *seen;
	uint16_t sum;
	size_t index;

	/* Reads every unit. */
	seen = (struct local_seen *)context;
	sum = 0;
	for (index = 0; index < key_length; index++)
		sum = (uint16_t)(sum + key[index]);
	for (index = 0; index < value_length; index++)
		sum = (uint16_t)(sum + value[index]);
	seen->count++;
	seen->total_units += key_length + value_length;

	/* A sum that no test record makes would show in the count. */
	if (sum == 0xdeadU)
		seen->total_units++;

	/* Goes on. */
	return 0;
}

/* Appends a 32-bit value little-endian. */
static void
put_u32(
	struct wb_buffer *out,
	uint32_t value)
{
	int index;

	/* Each byte in turn, the least significant first. */
	for (index = 0; index < 4; index++)
		CHECK_EQ(wb_buffer_append_byte(out, (unsigned char)((value >> (8 * index)) & 0xffU)), 0);
}

/* Appends a 64-bit value little-endian. */
static void
put_u64(
	struct wb_buffer *out,
	uint64_t value)
{
	int index;

	/* Each byte in turn, the least significant first. */
	for (index = 0; index < 8; index++)
		CHECK_EQ(wb_buffer_append_byte(out, (unsigned char)((value >> (8 * index)) & 0xffU)), 0);
}

/* Appends UTF-16 units, each little-endian. */
static void
put_units(
	struct wb_buffer *out,
	const uint16_t *units,
	size_t count)
{
	size_t index;

	/* Each unit as two bytes. */
	for (index = 0; index < count; index++) {
		CHECK_EQ(wb_buffer_append_byte(out, (unsigned char)(units[index] & 0xffU)), 0);
		CHECK_EQ(wb_buffer_append_byte(out, (unsigned char)(units[index] >> 8)), 0);
	}
}

/* Appends the characters of a string, without its NUL. */
static void
put_text(
	struct wb_buffer *out,
	const char *text)
{
	/* The characters as bytes. */
	CHECK_EQ(wb_buffer_append(out, text, strlen(text)), 0);
}

/* Fills a cookie with ordinary values and the given flags. */
static void
fill_cookie(
	struct wb_storage_cookie *cookie,
	unsigned flags)
{
	/* Fields of the usual sizes. */
	memset(cookie, 0, sizeof(*cookie));
	cookie->name = "session";
	cookie->name_length = 7;
	cookie->value = "abc=123";
	cookie->value_length = 7;
	cookie->domain = "www.example.com";
	cookie->domain_length = 15;
	cookie->path = "/shop";
	cookie->path_length = 5;
	cookie->expires = 1900000000000LL;
	cookie->created = 1700000000000LL;
	cookie->used = 1800000000000LL;
	cookie->flags = flags;
}

/* Gives the next number of a linear congruential generator (Numerical Recipes' constants). */
static uint32_t
random_next(void)
{
	/* The state is a 32-bit unsigned number, so the wrap-around is defined. */
	random_state = random_state * 1664525U + 1013904223U;
	return random_state;
}

/* Reports whether every byte is zero. */
static int
is_zero(
	const void *bytes,
	size_t length)
{
	const unsigned char *data;
	size_t index;

	/* Looks for a byte that is not zero. */
	data = (const unsigned char *)bytes;
	for (index = 0; index < length; index++) {
		if (data[index] != 0)
			return 0;
	}

	/* All zero. */
	return 1;
}

/* Reports whether two byte runs are the same (1) or differ (0). */
static int
same_bytes(
	const void *left,
	size_t left_length,
	const void *right,
	size_t right_length)
{
	/* Different lengths differ. */
	if (left_length != right_length)
		return 0;

	/* Empty runs are the same, wherever they point. */
	if (left_length == 0)
		return 1;

	/* The bytes. */
	if (memcmp(left, right, left_length) != 0)
		return 0;

	/* The same. */
	return 1;
}
