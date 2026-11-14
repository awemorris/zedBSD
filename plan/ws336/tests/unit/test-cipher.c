/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The test of ssh-cipher.c.  The known answer is the worked example of
 * draft-ietf-sshm-chacha20-poly1305-04 Appendix A (its key material
 * Figure 5, sequence number 7, plaintext packet Figure 4, encrypted packet
 * Figure 12 and tag Figure 17).  The other expectations are built here from
 * the construction of section 6 with ssh_chacha20_xor and ssh_poly1305
 * called directly, apart from the module: K_1 is the first 32 bytes of the
 * key material and K_2 the last 32, the nonce is the sequence number as a
 * 64-bit big-endian integer, the length is encrypted by K_2 at block
 * counter 0, the rest by K_1 from block counter 1, and the Poly1305 key is
 * the first 32 bytes of K_1's block 0.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "wire.h"
#include "ssh-cipher.h"

/* The size of the worked example's packet without the tag, in bytes (Figure 12: 76 bytes). */
#define TEST_EXAMPLE_SIZE	76U

/* The size of the largest packet this test builds, in bytes. */
#define TEST_PACKET_MAX		300U

/*
 * The worked example, as hexadecimal text.
 *
 * Values referenced: draft-ietf-sshm-chacha20-poly1305-04 Appendix A, Figure 5
 * (key material, 64 bytes), Figure 4 (the packet before encryption, 76
 * bytes), Figure 12 (the encrypted packet, 76 bytes) and Figure 17 (the tag,
 * 16 bytes).  The sequence number is 7 (Appendix A, before Figure 5).
 */
static const char example_key[] =
	"8b bf f6 85  5f c1 02 33  8c 37 3e 73  aa c0 c9 14"
	"f0 76 a9 05  b2 44 4a 32  ee ca ff ea  e2 2b ec c5"
	"e9 b7 a7 a5  82 5a 82 49  34 6e c1 c2  83 01 cf 39"
	"45 43 fc 75  69 88 7d 76  e1 68 f3 75  62 ac 07 40";
static const char example_plain[] =
	"00 00 00 48  06 5e 00 00  00 00 00 00  00 38 4c 6f"
	"72 65 6d 20  69 70 73 75  6d 20 64 6f  6c 6f 72 20"
	"73 69 74 20  61 6d 65 74  2c 20 63 6f  6e 73 65 63"
	"74 65 74 75  72 20 61 64  69 70 69 73  69 63 69 6e"
	"67 20 65 6c  69 74 4e 43  e8 04 dc 6c";
static const char example_cipher[] =
	"2c 3e cc e4  a5 bc 05 89  5b f0 7a 7b  a9 56 b6 c6"
	"88 29 ac 7c  83 b7 80 b7  00 0e cd e7  45 af c7 05"
	"bb c3 78 ce  03 a2 80 23  6b 87 b5 3b  ed 58 39 66"
	"23 02 b1 64  b6 28 6a 48  cd 1e 09 71  38 e3 cb 90"
	"9b 8b 2b 82  9d d1 8d 2a  35 ff 82 d9";
static const char example_tag[] =
	"95 34 9e 85  5b f0 2c 29  8e f7 75 f2  d1 a7 e8 b8";

static void test_known_answer(void);
static void test_construction(void);
static void test_tampering(void);
static void test_sizes(void);
static void test_init_clear(void);
static void build_expected(const uint8_t *key, uint32_t seq, const uint8_t *plain, size_t size, uint8_t *cipher, uint8_t *tag);
static void fill_key(uint8_t *key, uint8_t seed);

/* Runs every check of the cipher and prints the last line. */
int
main(void)
{
	/* Runs each group of checks. */
	test_known_answer();
	test_construction();
	test_tampering();
	test_sizes();
	test_init_clear();

	/* Prints the verdict. */
	return check_finish("cipher");
}

/* Fills a 64-byte key with bytes that differ from byte to byte and from seed to seed. */
static void
fill_key(
	uint8_t *key,
	uint8_t seed)
{
	size_t index;

	/* Makes each byte depend on its place. */
	for (index = 0; index < SSH_CHACHAPOLY_KEY; index++)
		key[index] = (uint8_t)(seed + index * 7U + (index >> 3));
}

/*
 * Builds the expected encrypted packet and tag of the construction of
 * section 6 from the primitives, without the module.
 */
static void
build_expected(
	const uint8_t *key,
	uint32_t seq,
	const uint8_t *plain,
	size_t size,
	uint8_t *cipher,
	uint8_t *tag)
{
	uint8_t nonce[SSH_CHACHA20_NONCE];
	uint8_t zeros[SSH_POLY1305_KEY];
	uint8_t poly_key[SSH_POLY1305_KEY];
	const uint8_t *k1;
	const uint8_t *k2;
	int error;

	/* Splits the 64-byte key into the main key and the header key. */
	k1 = key;
	k2 = key + 32;

	/* The nonce is the sequence number as a 64-bit big-endian integer. */
	memset(nonce, 0, sizeof(nonce));
	nonce[4] = (uint8_t)(seq >> 24);
	nonce[5] = (uint8_t)(seq >> 16);
	nonce[6] = (uint8_t)(seq >> 8);
	nonce[7] = (uint8_t)seq;

	/* The length is encrypted by K_2 at block counter 0. */
	error = ssh_chacha20_xor(k2, 0, nonce, plain, cipher, 4);
	CHECK_EQ(error, 0);

	/* The rest is encrypted by K_1 from block counter 1. */
	error = ssh_chacha20_xor(k1, 1, nonce, plain + 4, cipher + 4, size - 4);
	CHECK_EQ(error, 0);

	/* The Poly1305 key is the first 32 bytes of K_1's block 0. */
	memset(zeros, 0, sizeof(zeros));
	error = ssh_chacha20_xor(k1, 0, nonce, zeros, poly_key, sizeof(poly_key));
	CHECK_EQ(error, 0);

	/* The tag covers all the encrypted bytes. */
	error = ssh_poly1305(poly_key, cipher, size, tag);
	CHECK_EQ(error, 0);
}

/* Checks the worked example of Appendix A: seal gives its bytes, length and open take them back. */
static void
test_known_answer(void)
{
	struct ssh_chachapoly cipher;
	uint8_t key[SSH_CHACHAPOLY_KEY];
	uint8_t plain[TEST_EXAMPLE_SIZE];
	uint8_t expected_cipher[TEST_EXAMPLE_SIZE];
	uint8_t expected_tag[SSH_CHACHAPOLY_TAG];
	uint8_t packet[TEST_EXAMPLE_SIZE];
	uint8_t tag[SSH_CHACHAPOLY_TAG];
	uint32_t length;
	size_t count;
	int error;

	/* Reads the values of the example. */
	count = wire_hex(example_key, key, sizeof(key));
	CHECK_EQ(count, SSH_CHACHAPOLY_KEY);
	count = wire_hex(example_plain, plain, sizeof(plain));
	CHECK_EQ(count, TEST_EXAMPLE_SIZE);
	count = wire_hex(example_cipher, expected_cipher, sizeof(expected_cipher));
	CHECK_EQ(count, TEST_EXAMPLE_SIZE);
	count = wire_hex(example_tag, expected_tag, sizeof(expected_tag));
	CHECK_EQ(count, SSH_CHACHAPOLY_TAG);

	/* Starts a cipher with the example key. */
	ssh_chachapoly_init(&cipher, key);

	/* Encrypts the packet with sequence number 7 and compares byte by byte. */
	memcpy(packet, plain, sizeof(packet));
	memset(tag, 0, sizeof(tag));
	error = ssh_chachapoly_seal(&cipher, 7, packet, sizeof(packet), tag);
	CHECK_EQ(error, 0);
	CHECK_BYTES(packet, expected_cipher, sizeof(packet));
	CHECK_BYTES(tag, expected_tag, sizeof(tag));

	/* Decrypts the length of the encrypted packet. */
	length = 0;
	error = ssh_chachapoly_length(&cipher, 7, expected_cipher, &length);
	CHECK_EQ(error, 0);
	CHECK_EQ(length, 0x48);

	/* Opens the example's wire bytes and gets the plaintext back. */
	memcpy(packet, expected_cipher, sizeof(packet));
	error = ssh_chachapoly_open(&cipher, 7, packet, sizeof(packet), expected_tag);
	CHECK_EQ(error, 0);
	CHECK_BYTES(packet, plain, sizeof(packet));

	/* The same bytes under another sequence number do not open (the nonce differs). */
	memcpy(packet, expected_cipher, sizeof(packet));
	error = ssh_chachapoly_open(&cipher, 8, packet, sizeof(packet), expected_tag);
	CHECK_EQ(error, EBADMSG);
	CHECK_BYTES(packet, expected_cipher, sizeof(packet));

	/* Wipes the cipher. */
	ssh_chachapoly_clear(&cipher);
}

/* Checks the key split, the nonce and the counters against a construction made from the primitives. */
static void
test_construction(void)
{
	static const uint32_t seqs[3] = { 0U, 1U, 0xffffffffU };
	struct ssh_chachapoly cipher;
	uint8_t key[SSH_CHACHAPOLY_KEY];
	uint8_t plain[TEST_PACKET_MAX];
	uint8_t expected[TEST_PACKET_MAX];
	uint8_t expected_tag[SSH_CHACHAPOLY_TAG];
	uint8_t packet[TEST_PACKET_MAX];
	uint8_t tag[SSH_CHACHAPOLY_TAG];
	static const size_t sizes[4] = { 5U, 12U, 64U, 300U };
	uint32_t length;
	size_t index;
	size_t fill;
	size_t size_index;
	size_t size;
	int error;

	/* Starts a cipher with a key made for this test. */
	fill_key(key, 3);
	ssh_chachapoly_init(&cipher, key);

	/* Makes a plaintext whose first four bytes are a plausible length. */
	for (fill = 0; fill < sizeof(plain); fill++)
		plain[fill] = (uint8_t)(fill * 31U + 5U);
	plain[0] = 0;
	plain[1] = 0;
	plain[2] = 0x01;
	plain[3] = 0x28;

	/* Tries each sequence number at each packet size. */
	for (index = 0; index < 3; index++) {
		for (size_index = 0; size_index < 4; size_index++) {
			/* Builds the expected bytes from the primitives for this size. */
			size = sizes[size_index];
			build_expected(key, seqs[index], plain, size, expected, expected_tag);

			/* Seal gives the constructed bytes and tag. */
			memcpy(packet, plain, size);
			error = ssh_chachapoly_seal(&cipher, seqs[index], packet, size, tag);
			CHECK_EQ(error, 0);
			CHECK_BYTES(packet, expected, size);
			CHECK_BYTES(tag, expected_tag, sizeof(tag));

			/* The length of the encrypted packet is the plaintext length. */
			length = 0;
			error = ssh_chachapoly_length(&cipher, seqs[index], packet, &length);
			CHECK_EQ(error, 0);
			CHECK_EQ(length, 0x128);

			/* Open takes the packet back to the plaintext. */
			error = ssh_chachapoly_open(&cipher, seqs[index], packet, size, tag);
			CHECK_EQ(error, 0);
			CHECK_BYTES(packet, plain, size);
		}
	}

	/* A different sequence number gives a different encryption of the same plaintext. */
	memcpy(packet, plain, 64);
	error = ssh_chachapoly_seal(&cipher, 0, packet, 64, tag);
	CHECK_EQ(error, 0);
	memcpy(expected, plain, 64);
	error = ssh_chachapoly_seal(&cipher, 1, expected, 64, expected_tag);
	CHECK_EQ(error, 0);
	CHECK(memcmp(packet, expected, 64) != 0);
	CHECK(memcmp(tag, expected_tag, sizeof(tag)) != 0);

	/* Wipes the cipher. */
	ssh_chachapoly_clear(&cipher);
}

/* Checks that a change of any one bit of the tag or of the encrypted bytes is refused and leaves the packet as it was. */
static void
test_tampering(void)
{
	struct ssh_chachapoly cipher;
	uint8_t key[SSH_CHACHAPOLY_KEY];
	uint8_t plain[40];
	uint8_t sealed[40];
	uint8_t packet[40];
	uint8_t changed[40];
	uint8_t tag[SSH_CHACHAPOLY_TAG];
	uint8_t bad_tag[SSH_CHACHAPOLY_TAG];
	size_t bit;
	size_t index;
	int error;

	/* Starts a cipher with a key made for this test. */
	fill_key(key, 91);
	ssh_chachapoly_init(&cipher, key);

	/* Makes a plaintext whose first four bytes are a length that fits the packet. */
	for (index = 0; index < sizeof(plain); index++)
		plain[index] = (uint8_t)(index + 1U);
	plain[0] = 0;
	plain[1] = 0;
	plain[2] = 0;
	plain[3] = 36;
	memcpy(sealed, plain, sizeof(sealed));
	error = ssh_chachapoly_seal(&cipher, 12, sealed, sizeof(sealed), tag);
	CHECK_EQ(error, 0);

	/* Flips each bit of the tag. */
	for (bit = 0; bit < SSH_CHACHAPOLY_TAG * 8U; bit++) {
		memcpy(packet, sealed, sizeof(packet));
		memcpy(bad_tag, tag, sizeof(bad_tag));
		bad_tag[bit / 8U] ^= (uint8_t)(1U << (bit % 8U));
		error = ssh_chachapoly_open(&cipher, 12, packet, sizeof(packet), bad_tag);
		CHECK_EQ(error, EBADMSG);
		CHECK_BYTES(packet, sealed, sizeof(packet));
	}

	/* Flips each bit of the encrypted length and body. */
	for (bit = 0; bit < sizeof(sealed) * 8U; bit++) {
		memcpy(packet, sealed, sizeof(packet));
		packet[bit / 8U] ^= (uint8_t)(1U << (bit % 8U));
		memcpy(changed, packet, sizeof(changed));
		error = ssh_chachapoly_open(&cipher, 12, packet, sizeof(packet), tag);
		CHECK_EQ(error, EBADMSG);
		CHECK_BYTES(packet, changed, sizeof(packet));
	}

	/* A shorter packet under the same tag is refused. */
	memcpy(packet, sealed, sizeof(packet));
	error = ssh_chachapoly_open(&cipher, 12, packet, sizeof(packet) - 1U, tag);
	CHECK_EQ(error, EBADMSG);
	CHECK_BYTES(packet, sealed, sizeof(packet));

	/* The untouched packet still opens. */
	error = ssh_chachapoly_open(&cipher, 12, packet, sizeof(packet), tag);
	CHECK_EQ(error, 0);
	CHECK_BYTES(packet, plain, sizeof(packet));

	/* Wipes the cipher. */
	ssh_chachapoly_clear(&cipher);
}

/* Checks the sizes the functions refuse. */
static void
test_sizes(void)
{
	struct ssh_chachapoly cipher;
	uint8_t key[SSH_CHACHAPOLY_KEY];
	uint8_t packet[8];
	uint8_t tag[SSH_CHACHAPOLY_TAG];
	uint8_t copy[8];
	int error;

	/*
	 * Starts a cipher and a packet filled with a known byte, to try the sizes the
	 * functions refuse.
	 */
	fill_key(key, 5);
	ssh_chachapoly_init(&cipher, key);
	memset(packet, 0x5a, sizeof(packet));
	memcpy(copy, packet, sizeof(copy));
	memset(tag, 0, sizeof(tag));

	/* A packet without a byte after its length is refused by both. */
	error = ssh_chachapoly_seal(&cipher, 0, packet, 4, tag);
	CHECK_EQ(error, EINVAL);
	CHECK_BYTES(packet, copy, sizeof(packet));
	error = ssh_chachapoly_open(&cipher, 0, packet, 4, tag);
	CHECK_EQ(error, EINVAL);
	CHECK_BYTES(packet, copy, sizeof(packet));
	error = ssh_chachapoly_seal(&cipher, 0, packet, 0, tag);
	CHECK_EQ(error, EINVAL);

	/* The smallest packet works. */
	error = ssh_chachapoly_seal(&cipher, 0, packet, 5, tag);
	CHECK_EQ(error, 0);
	error = ssh_chachapoly_open(&cipher, 0, packet, 5, tag);
	CHECK_EQ(error, 0);
	CHECK_BYTES(packet, copy, 5);

	/* A size whose packet_length passes the limit is refused. */
	error = ssh_chachapoly_open(&cipher, 0, packet, (size_t)SSH_PACKET_LIMIT + 5U, tag);
	CHECK_EQ(error, EINVAL);

	/* Wipes the cipher. */
	ssh_chachapoly_clear(&cipher);
}

/* Checks that init splits the key as the header says and clear wipes it. */
static void
test_init_clear(void)
{
	struct ssh_chachapoly cipher;
	uint8_t key[SSH_CHACHAPOLY_KEY];
	uint8_t zeros[SSH_CHACHA20_KEY];

	/* Splits a known key and checks the two halves that init stores. */
	fill_key(key, 200);
	ssh_chachapoly_init(&cipher, key);
	CHECK_BYTES(cipher.main_key, key, SSH_CHACHA20_KEY);
	CHECK_BYTES(cipher.header_key, key + SSH_CHACHA20_KEY, SSH_CHACHA20_KEY);

	/* The cipher copied the key: wiping the caller's key does not change it. */
	memset(key, 0, sizeof(key));
	CHECK(cipher.main_key[1] != 0 || cipher.main_key[2] != 0);

	/* Clear leaves zeros. */
	memset(zeros, 0, sizeof(zeros));
	ssh_chachapoly_clear(&cipher);
	CHECK_BYTES(cipher.main_key, zeros, sizeof(zeros));
	CHECK_BYTES(cipher.header_key, zeros, sizeof(zeros));
}
