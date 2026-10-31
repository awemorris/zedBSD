/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The test of ssh-common.c: byte order, name-lists (RFC 4251 5 and 6) and
 * base64 (RFC 4648).  Every expected value comes from the RFC texts (the
 * test vectors of RFC 4648 section 10, the alphabet of Table 1) or from a
 * hand computation written in the comment beside it.
 */

#include <stdint.h>
#include <string.h>

#include "check.h"
#include "ssh-common.h"

/* A room for the longest name-list the tests build, and a little more. */
static char big_list[SSH_NAMELIST_MAX + 16];

static void test_byte_order(void);
static void test_namelist_check(void);
static void test_namelist_contains(void);
static void test_base64_sizes(void);
static void test_base64_vectors(void);
static void test_base64_alphabet(void);
static void test_base64_encode_errors(void);
static void test_base64_decode_errors(void);
static void test_base64_roundtrip(void);

/*
 * Runs the checks of the module.
 */
int
main(void)
{
	/* Runs each group of checks. */
	test_byte_order();
	test_namelist_check();
	test_namelist_contains();
	test_base64_sizes();
	test_base64_vectors();
	test_base64_alphabet();
	test_base64_encode_errors();
	test_base64_decode_errors();
	test_base64_roundtrip();

	/* Reports the result of the whole test. */
	return check_finish("common");
}

/*
 * Checks the big-endian loads and stores with zero, the largest value and
 * values whose bytes all differ.
 */
static void
test_byte_order(void)
{
	static const uint8_t zero[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
	static const uint8_t ones[8] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
	static const uint8_t counting[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
	static const uint8_t high_bit[8] = { 0x80, 0, 0, 0, 0, 0, 0, 1 };
	static const uint8_t top_byte_only[4] = { 0xff, 0x00, 0x00, 0x00 };
	uint8_t bytes[10];

	/* The 32-bit loads: 0, 0xffffffff, 0x01020304, and a value with the top bit set (0x80000000 followed by 0). */
	CHECK_EQ(ssh_load_be32(zero), 0);
	CHECK(ssh_load_be32(ones) == 0xffffffffUL);
	CHECK(ssh_load_be32(counting) == 0x01020304UL);
	CHECK(ssh_load_be32(high_bit) == 0x80000000UL);
	CHECK(ssh_load_be32(top_byte_only) == 0xff000000UL);

	/* The 64-bit loads: the halves are 0x01020304 and 0x05060708 (hand computed from the bytes 1 to 8). */
	CHECK(ssh_load_be64(zero) == 0);
	CHECK(ssh_load_be64(ones) == (((uint64_t)0xffffffffUL << 32) | 0xffffffffUL));
	CHECK(ssh_load_be64(counting) == (((uint64_t)0x01020304UL << 32) | 0x05060708UL));
	CHECK(ssh_load_be64(high_bit) == (((uint64_t)0x80000000UL << 32) | 0x00000001UL));

	/* The 32-bit store writes the most significant byte first and touches nothing after the fourth byte. */
	memset(bytes, 0x5a, sizeof(bytes));
	ssh_store_be32(bytes, 0x01020304UL);
	CHECK_BYTES(bytes, counting, 4);
	CHECK_EQ(bytes[4], 0x5a);
	ssh_store_be32(bytes, 0);
	CHECK_BYTES(bytes, zero, 4);
	ssh_store_be32(bytes, 0xffffffffUL);
	CHECK_BYTES(bytes, ones, 4);
	ssh_store_be32(bytes, 0x80000000UL);
	CHECK_EQ(bytes[0], 0x80);
	CHECK_EQ(bytes[1], 0);
	CHECK_EQ(bytes[2], 0);
	CHECK_EQ(bytes[3], 0);

	/* The 64-bit store writes the high half first and touches nothing after the eighth byte. */
	memset(bytes, 0x5a, sizeof(bytes));
	ssh_store_be64(bytes, ((uint64_t)0x01020304UL << 32) | 0x05060708UL);
	CHECK_BYTES(bytes, counting, 8);
	CHECK_EQ(bytes[8], 0x5a);
	ssh_store_be64(bytes, 0);
	CHECK_BYTES(bytes, zero, 8);
	ssh_store_be64(bytes, ((uint64_t)0xffffffffUL << 32) | 0xffffffffUL);
	CHECK_BYTES(bytes, ones, 8);
	ssh_store_be64(bytes, ((uint64_t)0x80000000UL << 32) | 0x00000001UL);
	CHECK_BYTES(bytes, high_bit, 8);
}

/*
 * Checks ssh_namelist_check against the rules of RFC 4251 sections 5 and 6
 * and the limits of ssh-common.h.
 */
static void
test_namelist_check(void)
{
	char name[SSH_NAME_MAX + 8];
	char list[SSH_NAME_MAX * 2 + 8];
	size_t index;
	unsigned byte;

	/* An empty list is valid, with or without a pointer. */
	CHECK_EQ(ssh_namelist_check(NULL, 0), 0);
	CHECK_EQ(ssh_namelist_check("", 0), 0);

	/* Bytes that are claimed but not there are malformed. */
	CHECK_EQ(ssh_namelist_check(NULL, 1), EINVAL);

	/* One name, and several names (RFC 4251 5's example is "zlib,none"). */
	CHECK_EQ(ssh_namelist_check("zlib", 4), 0);
	CHECK_EQ(ssh_namelist_check("zlib,none", 9), 0);
	CHECK_EQ(ssh_namelist_check("a,b,c", 5), 0);
	CHECK_EQ(ssh_namelist_check("x", 1), 0);

	/* A name may carry any printable US-ASCII character but the comma: 0x21 and 0x7e are the ends of the range. */
	CHECK_EQ(ssh_namelist_check("!", 1), 0);
	CHECK_EQ(ssh_namelist_check("~", 1), 0);
	CHECK_EQ(ssh_namelist_check("aes128-gcm@openssh.com", 22), 0);
	CHECK_EQ(ssh_namelist_check("ssh-ed25519,ecdsa-sha2-nistp256", 31), 0);

	/* An empty name is malformed wherever it stands. */
	CHECK_EQ(ssh_namelist_check("a,,b", 4), EINVAL);
	CHECK_EQ(ssh_namelist_check(",a", 2), EINVAL);
	CHECK_EQ(ssh_namelist_check("a,", 2), EINVAL);
	CHECK_EQ(ssh_namelist_check(",", 1), EINVAL);
	CHECK_EQ(ssh_namelist_check(",,", 2), EINVAL);

	/* Every byte below 0x21 or above 0x7e is refused: a space, a control character, DEL, and the bytes of 0x80 and over. */
	CHECK_EQ(ssh_namelist_check("a b", 3), EINVAL);
	CHECK_EQ(ssh_namelist_check(" ", 1), EINVAL);
	CHECK_EQ(ssh_namelist_check("a\x7f", 2), EINVAL);
	CHECK_EQ(ssh_namelist_check("a\x80", 2), EINVAL);
	CHECK_EQ(ssh_namelist_check("\xff", 1), EINVAL);
	CHECK_EQ(ssh_namelist_check("a\0b", 3), EINVAL);
	CHECK_EQ(ssh_namelist_check("a\nb", 3), EINVAL);
	for (byte = 0; byte < 256; byte++) {
		name[0] = 'a';
		name[1] = (char)byte;
		name[2] = 'b';

		/*
		 * A comma between two names is the separator and is valid; the
		 * other printable characters are valid inside a name; the rest
		 * are refused.
		 */
		if (byte >= 0x21 && byte <= 0x7e)
			CHECK_EQ(ssh_namelist_check(name, 3), 0);
		else
			CHECK_EQ(ssh_namelist_check(name, 3), EINVAL);
	}

	/* A name of 64 characters is valid and one of 65 is not (RFC 4251 6), alone and inside a list. */
	memset(name, 'n', sizeof(name));
	CHECK_EQ(ssh_namelist_check(name, SSH_NAME_MAX), 0);
	CHECK_EQ(ssh_namelist_check(name, SSH_NAME_MAX + 1), EINVAL);
	memset(list, 'n', sizeof(list));
	list[SSH_NAME_MAX] = ',';
	CHECK_EQ(ssh_namelist_check(list, SSH_NAME_MAX + 1 + 1), 0);
	CHECK_EQ(ssh_namelist_check(list, SSH_NAME_MAX + 1 + SSH_NAME_MAX), 0);
	CHECK_EQ(ssh_namelist_check(list, SSH_NAME_MAX + 1 + SSH_NAME_MAX + 1), EINVAL);
	memset(list, 'n', sizeof(list));
	list[65] = ',';
	list[66] = 'x';
	CHECK_EQ(ssh_namelist_check(list, 67), EINVAL);

	/* A list of exactly SSH_NAMELIST_MAX bytes made of short names is valid and one byte more is too long. */
	for (index = 0; index < sizeof(big_list); index++) {
		/* Every ninth byte is a comma, between eight-letter names. */
		if (index % 9 == 8)
			big_list[index] = ',';
		else
			big_list[index] = 'a';
	}
	CHECK_EQ(ssh_namelist_check(big_list, SSH_NAMELIST_MAX), 0);
	CHECK_EQ(ssh_namelist_check(big_list, SSH_NAMELIST_MAX + 1), EMSGSIZE);
	CHECK_EQ(ssh_namelist_check(big_list, SSH_NAMELIST_MAX - 1), 0);

	/* A list that is too long is refused for its size even when its content is also malformed. */
	CHECK_EQ(ssh_namelist_check(NULL, SSH_NAMELIST_MAX + 1), EMSGSIZE);
}

/*
 * Checks ssh_namelist_contains: whole-name matches only, case-sensitive.
 */
static void
test_namelist_contains(void)
{
	static const char list[] = "aes128-ctr,aes256-ctr";

	/* Names that are in the list. */
	CHECK_EQ(ssh_namelist_contains(list, 21, "aes128-ctr"), 1);
	CHECK_EQ(ssh_namelist_contains(list, 21, "aes256-ctr"), 1);

	/* A prefix, a suffix, a piece and the whole list with its comma are not names of the list. */
	CHECK_EQ(ssh_namelist_contains(list, 21, "aes"), 0);
	CHECK_EQ(ssh_namelist_contains(list, 21, "aes128"), 0);
	CHECK_EQ(ssh_namelist_contains(list, 21, "ctr"), 0);
	CHECK_EQ(ssh_namelist_contains(list, 21, "aes128-ctr,aes256-ctr"), 0);
	CHECK_EQ(ssh_namelist_contains(list, 21, "aes128-ctr,"), 0);
	CHECK_EQ(ssh_namelist_contains(list, 21, ",aes256-ctr"), 0);
	CHECK_EQ(ssh_namelist_contains(list, 21, "aes128-ctr-"), 0);

	/* Names are case-sensitive (RFC 4251 6). */
	CHECK_EQ(ssh_namelist_contains(list, 21, "AES128-CTR"), 0);
	CHECK_EQ(ssh_namelist_contains(list, 21, "Aes128-ctr"), 0);

	/* An empty name and a missing name are in no list. */
	CHECK_EQ(ssh_namelist_contains(list, 21, ""), 0);
	CHECK_EQ(ssh_namelist_contains(list, 21, NULL), 0);

	/* An empty list and a missing list hold nothing. */
	CHECK_EQ(ssh_namelist_contains("", 0, "a"), 0);
	CHECK_EQ(ssh_namelist_contains(NULL, 0, "a"), 0);
	CHECK_EQ(ssh_namelist_contains(NULL, 0, ""), 0);

	/* A list of one name. */
	CHECK_EQ(ssh_namelist_contains("abc", 3, "abc"), 1);
	CHECK_EQ(ssh_namelist_contains("abc", 3, "abcd"), 0);
	CHECK_EQ(ssh_namelist_contains("abc", 3, "ab"), 0);
	CHECK_EQ(ssh_namelist_contains("abc", 3, "bc"), 0);

	/* The size decides where the list ends, not a NUL or what follows. */
	CHECK_EQ(ssh_namelist_contains("abc,def", 3, "def"), 0);
	CHECK_EQ(ssh_namelist_contains("abc,def", 3, "abc"), 1);
	CHECK_EQ(ssh_namelist_contains("abcdef", 3, "abc"), 1);
	CHECK_EQ(ssh_namelist_contains("abc,def", 5, "d"), 1);
	CHECK_EQ(ssh_namelist_contains("abc,def", 5, "def"), 0);
	CHECK_EQ(ssh_namelist_contains("abc,def", 7, "def"), 1);

	/* The first, a middle and the last name of a longer list. */
	CHECK_EQ(ssh_namelist_contains("a,bb,ccc,dddd", 13, "a"), 1);
	CHECK_EQ(ssh_namelist_contains("a,bb,ccc,dddd", 13, "ccc"), 1);
	CHECK_EQ(ssh_namelist_contains("a,bb,ccc,dddd", 13, "dddd"), 1);
	CHECK_EQ(ssh_namelist_contains("a,bb,ccc,dddd", 13, "ddd"), 0);
	CHECK_EQ(ssh_namelist_contains("a,bb,ccc,dddd", 13, "b"), 0);
}

/*
 * Checks the encoded size of base64 (RFC 4648 4: four characters for each
 * started group of three bytes).
 */
static void
test_base64_sizes(void)
{
	size_t largest;

	/* Hand computed: ceil(size / 3) * 4. */
	CHECK_EQ(ssh_base64_encoded_size(0), 0);
	CHECK_EQ(ssh_base64_encoded_size(1), 4);
	CHECK_EQ(ssh_base64_encoded_size(2), 4);
	CHECK_EQ(ssh_base64_encoded_size(3), 4);
	CHECK_EQ(ssh_base64_encoded_size(4), 8);
	CHECK_EQ(ssh_base64_encoded_size(5), 8);
	CHECK_EQ(ssh_base64_encoded_size(6), 8);
	CHECK_EQ(ssh_base64_encoded_size(7), 12);
	CHECK_EQ(ssh_base64_encoded_size(32), 44);
	CHECK_EQ(ssh_base64_encoded_size(48), 64);

	/* The limit in the header: (SIZE_MAX / 4) * 3 - 3 is the largest size answered. */
	largest = (SIZE_MAX / 4) * 3 - 3;
	CHECK(ssh_base64_encoded_size(largest) == ((SIZE_MAX / 4) - 1) * 4);
	CHECK_EQ(ssh_base64_encoded_size(largest + 1), 0);
	CHECK_EQ(ssh_base64_encoded_size(SIZE_MAX), 0);
}

/*
 * Checks the encoding and the decoding of the test vectors of RFC 4648
 * section 10, and the form without padding.
 */
static void
test_base64_vectors(void)
{
	static const struct {
		const char *plain;
		const char *padded;
		const char *unpadded;
	} vectors[] = {
		{ "", "", "" },
		{ "f", "Zg==", "Zg" },
		{ "fo", "Zm8=", "Zm8" },
		{ "foo", "Zm9v", "Zm9v" },
		{ "foob", "Zm9vYg==", "Zm9vYg" },
		{ "fooba", "Zm9vYmE=", "Zm9vYmE" },
		{ "foobar", "Zm9vYmFy", "Zm9vYmFy" }
	};
	char text[64];
	uint8_t bytes[64];
	size_t decoded;
	size_t length;
	size_t index;

	/* Runs every vector in both directions. */
	for (index = 0; index < sizeof(vectors) / sizeof(vectors[0]); index++) {
		length = strlen(vectors[index].plain);

		/* The padded encoding has the characters of the vector followed by a NUL. */
		memset(text, 0x55, sizeof(text));
		CHECK_EQ(ssh_base64_encode((const uint8_t *)vectors[index].plain, length, text, sizeof(text), 1), 0);
		CHECK_EQ(strlen(text), strlen(vectors[index].padded));
		CHECK(strcmp(text, vectors[index].padded) == 0);
		CHECK_EQ(strlen(text), ssh_base64_encoded_size(length));

		/* The unpadded encoding leaves out the '='. */
		memset(text, 0x55, sizeof(text));
		CHECK_EQ(ssh_base64_encode((const uint8_t *)vectors[index].plain, length, text, sizeof(text), 0), 0);
		CHECK(strcmp(text, vectors[index].unpadded) == 0);

		/* The padded text decodes to the plain bytes and receives the length. */
		memset(bytes, 0x55, sizeof(bytes));
		decoded = 99;
		CHECK_EQ(ssh_base64_decode(vectors[index].padded, strlen(vectors[index].padded), bytes, sizeof(bytes), &decoded), 0);
		CHECK_EQ(decoded, length);
		CHECK_BYTES(bytes, vectors[index].plain, length);
		CHECK_EQ(bytes[length], 0x55);
	}

	/* The room is exactly the characters and the NUL. */
	memset(text, 0x55, sizeof(text));
	CHECK_EQ(ssh_base64_encode((const uint8_t *)"foobar", 6, text, 9, 1), 0);
	CHECK(strcmp(text, "Zm9vYmFy") == 0);
	CHECK_EQ(text[9], 0x55);

	/* The end of an empty encoding is a NUL in a room of one. */
	text[0] = 'x';
	CHECK_EQ(ssh_base64_encode(NULL, 0, text, 1, 1), 0);
	CHECK_EQ(text[0], 0);

	/* A decode of nothing succeeds and stores nothing, with no output buffer. */
	decoded = 99;
	CHECK_EQ(ssh_base64_decode("", 0, NULL, 0, &decoded), 0);
	CHECK_EQ(decoded, 0);
	decoded = 99;
	CHECK_EQ(ssh_base64_decode(NULL, 0, NULL, 0, &decoded), 0);
	CHECK_EQ(decoded, 0);
}

/*
 * Checks every character of Table 1 of RFC 4648: the bytes made by packing
 * the values 0 to 63 six bits at a time encode to the alphabet in order.
 */
static void
test_base64_alphabet(void)
{
	static const char alphabet[] =
	    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	uint8_t bytes[48];
	uint8_t back[48];
	char text[80];
	size_t decoded;
	size_t bit;
	unsigned value;
	unsigned six;
	size_t index;

	/* Packs the 64 six-bit values 0, 1, ... 63 into 48 bytes, most significant bit first (a hand written packer). */
	memset(bytes, 0, sizeof(bytes));
	for (index = 0; index < 64; index++) {
		value = (unsigned)index;
		for (six = 0; six < 6; six++) {
			bit = index * 6 + six;
			if ((value >> (5 - six)) & 1U)
				bytes[bit / 8] |= (uint8_t)(0x80U >> (bit % 8));
		}
	}

	/* Spot checks of the packing against the first three bytes: 000000 000001 000010 000011 is 00 10 83. */
	CHECK_EQ(bytes[0], 0x00);
	CHECK_EQ(bytes[1], 0x10);
	CHECK_EQ(bytes[2], 0x83);

	/* The 48 bytes encode to the alphabet, with no padding because 48 is a multiple of 3. */
	memset(text, 0x55, sizeof(text));
	CHECK_EQ(ssh_base64_encode(bytes, 48, text, sizeof(text), 1), 0);
	CHECK_EQ(strlen(text), 64);
	CHECK(strcmp(text, alphabet) == 0);

	/* The alphabet decodes to the 48 bytes. */
	memset(back, 0x55, sizeof(back));
	decoded = 0;
	CHECK_EQ(ssh_base64_decode(alphabet, 64, back, sizeof(back), &decoded), 0);
	CHECK_EQ(decoded, 48);
	CHECK_BYTES(back, bytes, 48);

	/* The two characters that differ from the URL-safe alphabet: 62 is '+' and 63 is '/' (bytes fb ff bf). */
	CHECK_EQ(ssh_base64_encode((const uint8_t *)"\xfb\xff\xbf", 3, text, sizeof(text), 1), 0);
	CHECK(strcmp(text, "+/+/") == 0);
	CHECK_EQ(ssh_base64_decode("+/+/", 4, back, sizeof(back), &decoded), 0);
	CHECK_EQ(decoded, 3);
	CHECK_BYTES(back, "\xfb\xff\xbf", 3);
	CHECK_EQ(ssh_base64_decode("-_-_", 4, back, sizeof(back), &decoded), EINVAL);
}

/*
 * Checks that an encoding into a room that is too small answers ENOBUFS and
 * writes nothing.
 */
static void
test_base64_encode_errors(void)
{
	char text[32];
	size_t index;

	/* One byte short of the characters and the NUL, for several lengths. */
	for (index = 1; index <= 7; index++) {
		memset(text, 0x55, sizeof(text));
		CHECK_EQ(ssh_base64_encode((const uint8_t *)"abcdefg", index, text, ssh_base64_encoded_size(index), 1), ENOBUFS);
		CHECK_EQ(text[0], 0x55);
		CHECK_EQ(text[ssh_base64_encoded_size(index) - 1], 0x55);
	}

	/* A room of 0 and a missing output. */
	CHECK_EQ(ssh_base64_encode((const uint8_t *)"a", 1, text, 0, 1), ENOBUFS);
	CHECK_EQ(ssh_base64_encode((const uint8_t *)"a", 1, NULL, 8, 1), ENOBUFS);
	CHECK_EQ(ssh_base64_encode(NULL, 0, text, 0, 1), ENOBUFS);

	/* The room is measured against the padded length also when no padding is written. */
	memset(text, 0x55, sizeof(text));
	CHECK_EQ(ssh_base64_encode((const uint8_t *)"f", 1, text, 4, 0), ENOBUFS);
	CHECK_EQ(text[0], 0x55);
	CHECK_EQ(ssh_base64_encode((const uint8_t *)"f", 1, text, 5, 0), 0);
	CHECK(strcmp(text, "Zg") == 0);

	/* Bytes that are missing are refused. */
	CHECK_EQ(ssh_base64_encode(NULL, 3, text, sizeof(text), 1), EINVAL);

	/* A size no buffer can hold is refused without reading the input. */
	CHECK_EQ(ssh_base64_encode((const uint8_t *)"a", SIZE_MAX, text, sizeof(text), 1), ENOBUFS);
	CHECK_EQ(ssh_base64_encode((const uint8_t *)"a", (SIZE_MAX / 4) * 3, text, sizeof(text), 1), ENOBUFS);
}

/*
 * Checks the inputs ssh_base64_decode refuses, and the room check.
 */
static void
test_base64_decode_errors(void)
{
	static const struct {
		const char *text;
		size_t size;
	} bad[] = {
		/* A length that is not a multiple of four. */
		{ "Z", 1 },
		{ "Zg", 2 },
		{ "Zg=", 3 },
		{ "Zm9vY", 5 },
		{ "Zm9vYg", 6 },
		{ "Zm9vYmE", 7 },
		/* Padding in the wrong place or too much of it. */
		{ "Z===", 4 },
		{ "====", 4 },
		{ "Zg=a", 4 },
		{ "Z=g=", 4 },
		{ "=Zg=", 4 },
		{ "Zm9v====", 8 },
		{ "Zg==Zg==", 8 },
		{ "Zm8=Zm8=", 8 },
		{ "Zm9vZg=a", 8 },
		/* Unused bits that are not zero: 'h' is 33 (100001, low four bits 0001), '9' is 61 (111101, low two bits 01). */
		{ "Zh==", 4 },
		{ "Zm9=", 4 },
		{ "Zm9vYh==", 8 },
		{ "Zm9vYm9=", 8 },
		/* The same for every bit of the unused part: 'C' is 2 (000010), 'D' is 3, 'E' is 4 (000100), 'I' is 8, 'S' is 18 (010010). */
		{ "AAC=", 4 },
		{ "AAD=", 4 },
		{ "AAB=", 4 },
		{ "AI==", 4 },
		{ "AE==", 4 },
		{ "AC==", 4 },
		{ "AB==", 4 },
		{ "AS==", 4 },
		/* Characters outside the alphabet. */
		{ "Zm9*", 4 },
		{ "Zm9 ", 4 },
		{ " Zm9v", 5 },
		{ "Zm9v\n", 5 },
		{ "Zm9\nv", 5 },
		{ "Zm9v\r\n", 6 },
		{ "-_-_", 4 },
		{ "Zm9\xc3", 4 },
		{ "\x80m9v", 4 },
		{ "Zm\0v", 4 },
		{ "Zm9\x7f", 4 }
	};
	static const struct {
		const char *text;
		size_t size;
	} good[] = {
		{ "/g==", 4 },
		{ "Zm8=", 4 },
		{ "Zm9vYg==", 8 },
		{ "AAAA", 4 },
		{ "AA==", 4 },
		{ "AAE=", 4 },
		{ "AAQ=", 4 },
		{ "AQ==", 4 },
		{ "AgA=", 4 }
	};
	uint8_t bytes[32];
	size_t decoded;
	size_t index;

	/* Every malformed input answers EINVAL, with 0 for the length, whatever the room, and writes nothing. */
	for (index = 0; index < sizeof(bad) / sizeof(bad[0]); index++) {
		memset(bytes, 0x55, sizeof(bytes));
		decoded = 99;
		CHECK_EQ(ssh_base64_decode(bad[index].text, bad[index].size, bytes, sizeof(bytes), &decoded), EINVAL);
		CHECK_EQ(decoded, 0);
		CHECK_EQ(bytes[0], 0x55);
		decoded = 99;
		CHECK_EQ(ssh_base64_decode(bad[index].text, bad[index].size, bytes, 0, &decoded), EINVAL);
		CHECK_EQ(decoded, 0);
	}

	/* The forms whose unused bits are zero are accepted: "/g==" is 0xfe (111111 100000), "AA==" is 0x00. */
	for (index = 0; index < sizeof(good) / sizeof(good[0]); index++) {
		decoded = 99;
		CHECK_EQ(ssh_base64_decode(good[index].text, good[index].size, bytes, sizeof(bytes), &decoded), 0);
		CHECK(decoded >= 1);
	}
	CHECK_EQ(ssh_base64_decode("/g==", 4, bytes, sizeof(bytes), &decoded), 0);
	CHECK_EQ(bytes[0], 0xfe);

	/* A room one byte short answers ENOBUFS and writes nothing: "foobar" is 6 bytes. */
	memset(bytes, 0x55, sizeof(bytes));
	decoded = 99;
	CHECK_EQ(ssh_base64_decode("Zm9vYmFy", 8, bytes, 5, &decoded), ENOBUFS);
	CHECK_EQ(decoded, 0);
	CHECK_EQ(bytes[0], 0x55);
	CHECK_EQ(ssh_base64_decode("Zm9vYmFy", 8, bytes, 6, &decoded), 0);
	CHECK_EQ(decoded, 6);

	/* Padding is counted: "Zg==" is one byte, "Zm8=" is two. */
	memset(bytes, 0x55, sizeof(bytes));
	CHECK_EQ(ssh_base64_decode("Zg==", 4, bytes, 0, &decoded), ENOBUFS);
	CHECK_EQ(ssh_base64_decode("Zg==", 4, bytes, 1, &decoded), 0);
	CHECK_EQ(decoded, 1);
	CHECK_EQ(bytes[0], 'f');
	CHECK_EQ(bytes[1], 0x55);
	CHECK_EQ(ssh_base64_decode("Zm8=", 4, bytes, 1, &decoded), ENOBUFS);
	CHECK_EQ(ssh_base64_decode("Zm8=", 4, bytes, 2, &decoded), 0);
	CHECK_EQ(decoded, 2);
	CHECK_EQ(bytes[2], 0x55);

	/* No output buffer for bytes that exist, and characters that are missing. */
	CHECK_EQ(ssh_base64_decode("Zg==", 4, NULL, 4, &decoded), ENOBUFS);
	CHECK_EQ(ssh_base64_decode(NULL, 4, bytes, 4, &decoded), EINVAL);
	CHECK_EQ(decoded, 0);

	/* Malformed input outranks a room that is too small. */
	CHECK_EQ(ssh_base64_decode("Zh==", 4, bytes, 0, &decoded), EINVAL);
}

/*
 * Checks that encoding then decoding gives the bytes back, for every length
 * from 0 to 200, and that the unpadded form decodes only when padding is
 * not needed.
 */
static void
test_base64_roundtrip(void)
{
	uint8_t bytes[256];
	uint8_t back[256];
	char text[400];
	size_t decoded;
	size_t length;
	size_t index;
	uint32_t state;
	int error;

	/* Fills the bytes with a simple deterministic pattern covering all values. */
	state = 12345;
	for (index = 0; index < sizeof(bytes); index++) {
		state = state * 1103515245UL + 12345UL;
		bytes[index] = (uint8_t)((state >> 16) & 0xffU);
	}

	/* Round trips every length. */
	for (length = 0; length <= 200; length++) {
		memset(text, 0x55, sizeof(text));
		memset(back, 0x55, sizeof(back));
		CHECK_EQ(ssh_base64_encode(bytes, length, text, sizeof(text), 1), 0);
		CHECK_EQ(strlen(text), ssh_base64_encoded_size(length));
		decoded = 999;
		CHECK_EQ(ssh_base64_decode(text, strlen(text), back, sizeof(back), &decoded), 0);
		CHECK_EQ(decoded, length);
		CHECK_BYTES(back, bytes, length);
		CHECK_EQ(back[length], 0x55);

		/* The unpadded text has a length that is a multiple of four only when no padding was needed. */
		CHECK_EQ(ssh_base64_encode(bytes, length, text, sizeof(text), 0), 0);
		error = ssh_base64_decode(text, strlen(text), back, sizeof(back), &decoded);
		if (length % 3 == 0)
			CHECK_EQ(error, 0);
		else
			CHECK_EQ(error, EINVAL);
	}
}
