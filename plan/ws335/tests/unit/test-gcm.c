/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Checks AES-GCM (cryc-gcm.h).
 *
 * Expected values: the CAVP GCM vectors (gcmEncryptExtIV and gcmDecrypt,
 * 128, 192 and 256 bit keys, IV of 8, 96 and 1024 bits) and the Wycheproof
 * aes_gcm_test vectors read from the vector files, the hand-computed
 * products of GF(2^128) (the identity element 1000...0, x^1 times x^127 =
 * x^128 = x^7 + x^2 + x + 1), and a reference written from the text of
 * NIST SP 800-38D alone: Algorithm 1 bit by bit on arrays of bits, GHASH
 * of 6.4, the pre-counter block of 7.1 step 2, GCTR of 6.5 block by block
 * with the single-block cipher, and the tag of 7.1.  Only the 16-byte tag
 * is in range: CAVP records with a shorter tag are checked on the
 * ciphertext and the leading tag bytes only (encryption) or counted as out
 * of range (decryption).
 */

#include "check.h"
#include "vecfile.h"

#include "userland/base/libcrypto-compat/cryc-gcm.h"

/* The sizes of the buffers that hold decoded vectors and references. */
#define VECTOR_BYTES		4096U

/* A byte that fills a buffer to show later that nothing was written to it. */
#define SENTINEL_BYTE		0x5aU

/* The largest message of the generated tests. */
#define MESSAGE_BYTES		1100U

/* The records of the vector files that were used, and those skipped as out of range. */
static unsigned long used_records;
static unsigned long skipped_records;

/* The state of a pseudo-random generator (xorshift64, Marsaglia 2003). */
static uint64_t random_state = CRYC_U64(0x2545f491U, 0x4f6cdd1dU);

/* Returns the next pseudo-random byte. */
static uint8_t
random_byte(void)
{
	/* One xorshift step, then one byte of the state. */
	random_state ^= random_state << 13;
	random_state ^= random_state >> 7;
	random_state ^= random_state << 17;
	return (uint8_t)(random_state >> 24);
}

/* Fills size bytes with pseudo-random bytes. */
static void
random_fill(
	uint8_t *out,
	size_t size)
{
	size_t index;

	/* One byte at a time. */
	for (index = 0; index < size; index++)
		out[index] = random_byte();
}

/* Returns 1 when all size bytes are zero. */
static int
all_zero(
	const uint8_t *bytes,
	size_t size)
{
	size_t index;

	/* Any nonzero byte ends the search. */
	for (index = 0; index < size; index++) {
		if (bytes[index] != 0)
			return 0;
	}
	return 1;
}

/* Returns 1 when all size bytes equal value. */
static int
all_equal(
	const uint8_t *bytes,
	size_t size,
	uint8_t value)
{
	size_t index;

	/* Any other byte ends the search. */
	for (index = 0; index < size; index++) {
		if (bytes[index] != value)
			return 0;
	}
	return 1;
}

/* The reference product of 6.3, Algorithm 1, on arrays with one bit per element. */
static void
reference_multiply(
	uint8_t *out,
	const uint8_t *x,
	const uint8_t *y)
{
	uint8_t z[128];
	uint8_t v[128];
	unsigned step;
	unsigned bit;
	unsigned last;

	/* Z = 0 and V = Y, bit 0 being the leftmost bit of the first byte. */
	for (bit = 0; bit < 128U; bit++) {
		z[bit] = 0;
		v[bit] = (uint8_t)((y[bit >> 3] >> (7U - (bit & 7U))) & 1U);
	}

	/* The 128 steps of the algorithm. */
	for (step = 0; step < 128U; step++) {
		/* If x_i is 1, Z = Z xor V. */
		if (((x[step >> 3] >> (7U - (step & 7U))) & 1U) != 0) {
			for (bit = 0; bit < 128U; bit++)
				z[bit] = (uint8_t)(z[bit] ^ v[bit]);
		}

		/* V is shifted right by one bit, and R = 11100001 || 0^120 is added when the bit shifted out was 1. */
		last = v[127];
		for (bit = 127; bit > 0; bit--)
			v[bit] = v[bit - 1U];
		v[0] = 0;
		if (last != 0) {
			v[0] = (uint8_t)(v[0] ^ 1U);
			v[1] = (uint8_t)(v[1] ^ 1U);
			v[2] = (uint8_t)(v[2] ^ 1U);
			v[7] = (uint8_t)(v[7] ^ 1U);
		}
	}

	/* Packs Z into the output bytes. */
	memset(out, 0, 16);
	for (bit = 0; bit < 128U; bit++)
		out[bit >> 3] = (uint8_t)(out[bit >> 3] | (z[bit] << (7U - (bit & 7U))));
}

/* The reference GHASH of 6.4 over data padded with zeros to a whole number of blocks. */
static void
reference_ghash(
	uint8_t *y,
	const uint8_t *h,
	const uint8_t *data,
	size_t size)
{
	uint8_t padded[2U * VECTOR_BYTES];
	size_t blocks;
	size_t block;
	size_t index;

	/* The zero-padded copy. */
	blocks = (size + 15U) / 16U;
	memset(padded, 0, blocks * 16U);
	if (size > 0)
		memcpy(padded, data, size);

	/* y = (y xor X_i) * H for each block. */
	for (block = 0; block < blocks; block++) {
		for (index = 0; index < 16U; index++)
			y[index] = (uint8_t)(y[index] ^ padded[block * 16U + index]);
		reference_multiply(y, y, h);
	}
}

/* Adds 1 modulo 2^32 to the last four bytes of a block, byte by byte (6.2). */
static void
reference_inc32(
	uint8_t *block)
{
	unsigned index;

	/* The carry runs from byte 15 toward byte 12 and stops there. */
	for (index = 16; index > 12U; index--) {
		block[index - 1U] = (uint8_t)(block[index - 1U] + 1U);
		if (block[index - 1U] != 0U)
			return;
	}
}

/* The reference GCM-AE of 7.1, built on the single-block cipher alone. */
static void
reference_seal(
	const uint8_t *key,
	size_t key_size,
	const uint8_t *iv,
	size_t iv_size,
	const uint8_t *aad,
	size_t aad_size,
	const uint8_t *in,
	size_t size,
	uint8_t *out,
	uint8_t *tag)
{
	struct cryc_aes aes;
	uint8_t zero[16];
	uint8_t h[16];
	uint8_t j0[16];
	uint8_t counter[16];
	uint8_t keystream[16];
	uint8_t state[16];
	uint8_t scratch[2U * VECTOR_BYTES];
	size_t length;
	size_t done;
	size_t take;
	size_t index;

	/* The cipher, and H = CIPH(0^128). */
	CHECK(cryc_aes_init(&aes, key, key_size) == 0);
	memset(zero, 0, sizeof(zero));
	cryc_aes_encrypt_blocks(&aes, zero, h, 1);

	/* J0: IV || 0^31 || 1 for 96 bits, else GHASH of the padded IV and a block of 64 zero bits and the IV length in bits. */
	if (iv_size == 12U) {
		memcpy(j0, iv, 12U);
		j0[12] = 0;
		j0[13] = 0;
		j0[14] = 0;
		j0[15] = 1;
	} else {
		length = (iv_size + 15U) / 16U * 16U;
		memset(scratch, 0, length + 16U);
		memcpy(scratch, iv, iv_size);
		scratch[length + 15U] = (uint8_t)((iv_size * 8U) & 0xffU);
		scratch[length + 14U] = (uint8_t)(((iv_size * 8U) >> 8) & 0xffU);
		scratch[length + 13U] = (uint8_t)(((iv_size * 8U) >> 16) & 0xffU);
		memset(j0, 0, sizeof(j0));
		reference_ghash(j0, h, scratch, length + 16U);
	}

	/* GCTR starting at inc32(J0), one block at a time. */
	memcpy(counter, j0, sizeof(counter));
	reference_inc32(counter);
	for (done = 0; done < size; done += 16U) {
		cryc_aes_encrypt_blocks(&aes, counter, keystream, 1);
		take = size - done;
		if (take > 16U)
			take = 16U;
		for (index = 0; index < take; index++)
			out[done + index] = (uint8_t)(in[done + index] ^ keystream[index]);
		reference_inc32(counter);
	}

	/* The GHASH input: padded A, padded C, then len(A) and len(C) in bits as 64-bit integers. */
	memset(state, 0, sizeof(state));
	reference_ghash(state, h, aad, aad_size);
	reference_ghash(state, h, out, size);
	memset(scratch, 0, 16);
	scratch[15] = (uint8_t)((size * 8U) & 0xffU);
	scratch[14] = (uint8_t)(((size * 8U) >> 8) & 0xffU);
	scratch[13] = (uint8_t)(((size * 8U) >> 16) & 0xffU);
	scratch[7] = (uint8_t)((aad_size * 8U) & 0xffU);
	scratch[6] = (uint8_t)(((aad_size * 8U) >> 8) & 0xffU);
	scratch[5] = (uint8_t)(((aad_size * 8U) >> 16) & 0xffU);
	reference_ghash(state, h, scratch, 16);

	/* T = E(J0) xor S. */
	cryc_aes_encrypt_blocks(&aes, j0, keystream, 1);
	for (index = 0; index < 16U; index++)
		tag[index] = (uint8_t)(keystream[index] ^ state[index]);
	cryc_aes_clear(&aes);
}

/* Checks the products of GF(2^128) worked out by hand and the algebra of the field. */
static void
check_multiply(void)
{
	uint8_t x[16];
	uint8_t y[16];
	uint8_t z[16];
	uint8_t out[16];
	uint8_t expected[16];
	uint8_t left[16];
	uint8_t right[16];
	uint8_t other[16];
	uint8_t sum[16];
	unsigned round;
	unsigned index;

	/* 0 times anything is 0, and the element 1 (bit 0 set: 0x80 then zeros) times y is y. */
	random_fill(y, 16);
	memset(x, 0, sizeof(x));
	cryc_gcm_multiply(out, x, y);
	CHECK(all_zero(out, 16));
	cryc_gcm_multiply(out, y, x);
	CHECK(all_zero(out, 16));
	x[0] = 0x80;
	cryc_gcm_multiply(out, x, y);
	CHECK_BYTES(out, y, 16);
	cryc_gcm_multiply(out, y, x);
	CHECK_BYTES(out, y, 16);

	/* x^1 (0x40...) times x^1 is x^2 (0x20...): V = Y shifted right twice. */
	memset(x, 0, sizeof(x));
	x[0] = 0x40;
	memset(expected, 0, sizeof(expected));
	expected[0] = 0x20;
	cryc_gcm_multiply(out, x, x);
	CHECK_BYTES(out, expected, 16);

	/* x^1 times x^127 (0x00...01) is x^128 = x^7 + x^2 + x + 1: the bits 0, 1, 2 and 7, 0xe1 then zeros. */
	memset(y, 0, sizeof(y));
	y[15] = 0x01;
	memset(expected, 0, sizeof(expected));
	expected[0] = 0xe1;
	cryc_gcm_multiply(out, x, y);
	CHECK_BYTES(out, expected, 16);
	cryc_gcm_multiply(out, y, x);
	CHECK_BYTES(out, expected, 16);

	/* x^127 times x^127 is x^254 = x^126 * x^128: x^126 times (x^7 + x^2 + x + 1) = x^133 + x^128 + x^127 + x^126, reduced again. */
	cryc_gcm_multiply(out, y, y);

	/*
	 * By hand: x^254 = x^126 * x^128 = x^126 * (x^7 + x^2 + x + 1)
	 *        = x^133 + x^128 + x^127 + x^126.
	 * x^133 = x^5 * x^128 = x^12 + x^7 + x^6 + x^5, and
	 * x^128 = x^7 + x^2 + x + 1, so the sum (the two x^7 cancel) is
	 * x^126 + x^127 + x^12 + x^6 + x^5 + x^2 + x + 1.
	 * Bit i is the term x^i: byte i / 8, mask 0x80 >> (i % 8).
	 */
	memset(expected, 0, sizeof(expected));
	expected[0] = 0x80U | 0x40U | 0x20U | 0x04U | 0x02U;
	expected[1] = 0x08U;
	expected[15] = 0x03U;
	CHECK_BYTES(out, expected, 16);

	/* The result is the same when out is x or y. */
	random_fill(x, 16);
	random_fill(y, 16);
	cryc_gcm_multiply(expected, x, y);
	memcpy(z, x, 16);
	cryc_gcm_multiply(z, z, y);
	CHECK_BYTES(z, expected, 16);
	memcpy(z, y, 16);
	cryc_gcm_multiply(z, x, z);
	CHECK_BYTES(z, expected, 16);
	memcpy(z, x, 16);
	cryc_gcm_multiply(z, z, z);
	cryc_gcm_multiply(expected, x, x);
	CHECK_BYTES(z, expected, 16);

	/* The reference, and commutativity, distributivity and associativity, on pseudo-random operands. */
	for (round = 0; round < 200U; round++) {
		random_fill(x, 16);
		random_fill(y, 16);
		random_fill(z, 16);
		if (round % 10U == 0)
			memset(x, 0, 16);
		if (round % 10U == 1U)
			memset(y, 0xff, 16);
		cryc_gcm_multiply(out, x, y);
		reference_multiply(expected, x, y);
		CHECK_BYTES(out, expected, 16);

		/* x * y = y * x. */
		cryc_gcm_multiply(left, y, x);
		CHECK_BYTES(left, out, 16);

		/* x * (y + z) = x * y + x * z. */
		for (index = 0; index < 16U; index++)
			sum[index] = (uint8_t)(y[index] ^ z[index]);
		cryc_gcm_multiply(left, x, sum);
		cryc_gcm_multiply(other, x, z);
		for (index = 0; index < 16U; index++)
			right[index] = (uint8_t)(out[index] ^ other[index]);
		CHECK_BYTES(left, right, 16);

		/* (x * y) * z = x * (y * z). */
		cryc_gcm_multiply(left, out, z);
		cryc_gcm_multiply(other, y, z);
		cryc_gcm_multiply(right, x, other);
		CHECK_BYTES(left, right, 16);
	}
}

/* Checks GHASH: padding, splitting, the empty input and the reference. */
static void
check_ghash(void)
{
	uint8_t h[16];
	uint8_t y[16];
	uint8_t expected[16];
	uint8_t data[200];
	uint8_t padded[208];
	uint8_t before[16];
	size_t size;
	size_t split;

	/* The empty input leaves the state as it is, with a NULL pointer as well. */
	random_fill(h, 16);
	random_fill(y, 16);
	memcpy(before, y, 16);
	cryc_gcm_ghash(y, h, NULL, 0);
	CHECK_BYTES(y, before, 16);
	cryc_gcm_ghash(y, h, data, 0);
	CHECK_BYTES(y, before, 16);

	/* Every size from 0 to 80: the reference, and the equality of a short last block with its zero-padded form. */
	for (size = 0; size <= 80U; size++) {
		random_fill(data, sizeof(data));
		random_fill(y, 16);
		memcpy(expected, y, 16);
		reference_ghash(expected, h, data, size);
		cryc_gcm_ghash(y, h, data, size);
		CHECK_BYTES(y, expected, 16);

		/* Zero padding to a block boundary changes nothing. */
		memset(padded, 0, sizeof(padded));
		memcpy(padded, data, size);
		memcpy(before, y, 16);
		random_fill(y, 16);
		memcpy(expected, y, 16);
		cryc_gcm_ghash(y, h, data, size);
		cryc_gcm_ghash(expected, h, padded, (size + 15U) / 16U * 16U);
		CHECK_BYTES(y, expected, 16);
	}

	/* Splitting at a block boundary equals one call. */
	for (split = 0; split <= 96U; split += 16U) {
		random_fill(data, 160U);
		random_fill(y, 16);
		memcpy(expected, y, 16);
		cryc_gcm_ghash(expected, h, data, 160U);
		cryc_gcm_ghash(y, h, data, split);
		cryc_gcm_ghash(y, h, data + split, 160U - split);
		CHECK_BYTES(y, expected, 16);
	}
}

/* Checks one record as a valid message: seal gives the ciphertext and the tag, open gives the plaintext, and forgeries fail. */
static void
check_valid_message(
	const uint8_t *key,
	size_t key_size,
	const uint8_t *iv,
	size_t iv_size,
	const uint8_t *aad,
	size_t aad_size,
	const uint8_t *plain,
	const uint8_t *cipher,
	size_t size,
	const uint8_t *tag,
	size_t tag_size)
{
	struct cryc_gcm gcm;
	uint8_t out[VECTOR_BYTES + 16U];
	uint8_t made_tag[16];
	uint8_t forged[16];
	uint8_t copy[VECTOR_BYTES];
	int error;

	/* The key. */
	error = cryc_gcm_init(&gcm, key, key_size);
	CHECK(error == 0);
	if (error != 0)
		return;

	/* Seal: the ciphertext, the leading tag bytes the record has, and nothing written past the ciphertext. */
	memset(out, SENTINEL_BYTE, sizeof(out));
	memset(made_tag, 0, sizeof(made_tag));
	error = cryc_gcm_seal(&gcm, iv, iv_size, aad, aad_size, plain, size, out, made_tag);
	CHECK(error == 0);
	CHECK_BYTES(out, cipher, size);
	CHECK(all_equal(out + size, 16, SENTINEL_BYTE));
	CHECK_BYTES(made_tag, tag, tag_size);

	/* Only a record with the whole 16-byte tag can be opened. */
	if (tag_size == 16U) {
		/* Open: the plaintext, and the same in place. */
		memset(out, SENTINEL_BYTE, sizeof(out));
		error = cryc_gcm_open(&gcm, iv, iv_size, aad, aad_size, cipher, size, out, tag);
		CHECK(error == 0);
		CHECK_BYTES(out, plain, size);
		CHECK(all_equal(out + size, 16, SENTINEL_BYTE));
		memcpy(copy, cipher, size);
		error = cryc_gcm_open(&gcm, iv, iv_size, aad, aad_size, copy, size, copy, tag);
		CHECK(error == 0);
		CHECK_BYTES(copy, plain, size);

		/* A tag with one bit changed fails with zeros in out. */
		memcpy(forged, tag, 16);
		forged[used_records % 16U] ^= (uint8_t)(1U << (used_records % 8U));
		memset(out, SENTINEL_BYTE, sizeof(out));
		error = cryc_gcm_open(&gcm, iv, iv_size, aad, aad_size, cipher, size, out, forged);
		CHECK(error == EBADMSG);
		CHECK(all_zero(out, size));
		CHECK(all_equal(out + size, 16, SENTINEL_BYTE));

		/* One byte of the ciphertext changed. */
		if (size > 0) {
			memcpy(copy, cipher, size);
			copy[used_records % size] ^= 0x01;
			memset(out, SENTINEL_BYTE, sizeof(out));
			error = cryc_gcm_open(&gcm, iv, iv_size, aad, aad_size, copy, size, out, tag);
			CHECK(error == EBADMSG);
			CHECK(all_zero(out, size));
		}

		/* One byte of the additional data changed. */
		if (aad_size > 0) {
			memcpy(copy, aad, aad_size);
			copy[used_records % aad_size] ^= 0x80;
			memset(out, SENTINEL_BYTE, sizeof(out));
			error = cryc_gcm_open(&gcm, iv, iv_size, copy, aad_size, cipher, size, out, tag);
			CHECK(error == EBADMSG);
			CHECK(all_zero(out, size));
		}

		/* One byte of the IV changed. */
		memcpy(copy, iv, iv_size);
		copy[used_records % iv_size] ^= 0x10;
		memset(out, SENTINEL_BYTE, sizeof(out));
		error = cryc_gcm_open(&gcm, copy, iv_size, aad, aad_size, cipher, size, out, tag);
		CHECK(error == EBADMSG);
		CHECK(all_zero(out, size));
	}

	/* The key is wiped. */
	cryc_gcm_clear(&gcm);
}

/* Checks the CAVP gcmEncryptExtIV file of one key size. */
static void
check_cavp_encrypt(
	const char *name)
{
	struct vec_file file;
	uint8_t key[32];
	uint8_t iv[VECTOR_BYTES];
	uint8_t aad[VECTOR_BYTES];
	uint8_t plain[VECTOR_BYTES];
	uint8_t cipher[VECTOR_BYTES];
	uint8_t tag[16];
	size_t key_size;
	size_t iv_size;
	size_t aad_size;
	size_t size;
	size_t cipher_size;
	size_t tag_size;
	size_t tag_bits;

	/* The file. */
	if (vec_open(&file, name) != 0) {
		CHECK(0);
		return;
	}

	/* Each record: key, IV, plaintext, additional data, ciphertext and a tag of Taglen bits. */
	while (vec_next(&file)) {
		key_size = vec_hex(&file, "Key", key, sizeof(key));
		iv_size = vec_hex(&file, "IV", iv, sizeof(iv));
		size = vec_hex(&file, "PT", plain, sizeof(plain));
		aad_size = vec_hex(&file, "AAD", aad, sizeof(aad));
		cipher_size = vec_hex(&file, "CT", cipher, sizeof(cipher));
		tag_size = vec_hex(&file, "Tag", tag, sizeof(tag));
		tag_bits = (size_t)atoi(vec_get(&file, "section.Taglen"));
		if (!check_one(key_size != (size_t)-1 && iv_size != (size_t)-1 && size != (size_t)-1 &&
		    aad_size != (size_t)-1 && cipher_size == size && tag_size == tag_bits / 8U && tag_size > 0,
		    __FILE__, __LINE__, "record is well formed"))
			continue;
		used_records++;
		check_valid_message(key, key_size, iv, iv_size, aad, aad_size, plain, cipher, size, tag, tag_size);

		/* A record whose tag is shorter than 16 bytes cannot be opened by the 16-byte-tag interface. */
		if (tag_size != 16U)
			skipped_records++;
	}
	vec_close(&file);
}

/* Checks the CAVP gcmDecrypt file of one key size: the 16-byte-tag records open or fail as the record says. */
static void
check_cavp_decrypt(
	const char *name)
{
	struct vec_file file;
	struct cryc_gcm gcm;
	uint8_t key[32];
	uint8_t iv[VECTOR_BYTES];
	uint8_t aad[VECTOR_BYTES];
	uint8_t plain[VECTOR_BYTES];
	uint8_t cipher[VECTOR_BYTES];
	uint8_t tag[16];
	uint8_t out[VECTOR_BYTES + 16U];
	size_t key_size;
	size_t iv_size;
	size_t aad_size;
	size_t size;
	size_t cipher_size;
	size_t tag_size;
	size_t tag_bits;
	int expect_fail;
	int error;

	/* The file. */
	if (vec_open(&file, name) != 0) {
		CHECK(0);
		return;
	}

	/* Each record: either FAIL, or the plaintext PT. */
	while (vec_next(&file)) {
		tag_bits = (size_t)atoi(vec_get(&file, "section.Taglen"));
		if (tag_bits != 128U) {
			skipped_records++;
			continue;
		}
		key_size = vec_hex(&file, "Key", key, sizeof(key));
		iv_size = vec_hex(&file, "IV", iv, sizeof(iv));
		aad_size = vec_hex(&file, "AAD", aad, sizeof(aad));
		cipher_size = vec_hex(&file, "CT", cipher, sizeof(cipher));
		tag_size = vec_hex(&file, "Tag", tag, sizeof(tag));
		expect_fail = vec_get(&file, "FAIL") != NULL;
		size = 0;
		if (!expect_fail)
			size = vec_hex(&file, "PT", plain, sizeof(plain));
		if (!check_one(key_size != (size_t)-1 && iv_size != (size_t)-1 && aad_size != (size_t)-1 &&
		    cipher_size != (size_t)-1 && tag_size == 16U && size != (size_t)-1 &&
		    (expect_fail || size == cipher_size), __FILE__, __LINE__, "record is well formed"))
			continue;
		used_records++;
		error = cryc_gcm_init(&gcm, key, key_size);
		CHECK(error == 0);
		memset(out, SENTINEL_BYTE, sizeof(out));
		error = cryc_gcm_open(&gcm, iv, iv_size, aad, aad_size, cipher, cipher_size, out, tag);
		if (expect_fail) {
			CHECK(error == EBADMSG);
			CHECK(all_zero(out, cipher_size));
		} else {
			CHECK(error == 0);
			CHECK_BYTES(out, plain, size);
		}
		CHECK(all_equal(out + cipher_size, 16, SENTINEL_BYTE));
		cryc_gcm_clear(&gcm);
	}
	vec_close(&file);
}

/* Checks the Wycheproof aes_gcm_test records. */
static void
check_wycheproof(void)
{
	struct vec_file file;
	struct cryc_gcm gcm;
	uint8_t key[32];
	uint8_t iv[VECTOR_BYTES];
	uint8_t aad[VECTOR_BYTES];
	uint8_t msg[VECTOR_BYTES];
	uint8_t cipher[VECTOR_BYTES];
	uint8_t tag[64];
	uint8_t out[VECTOR_BYTES + 16U];
	uint8_t made_tag[16];
	size_t key_size;
	size_t iv_size;
	size_t aad_size;
	size_t size;
	size_t cipher_size;
	size_t tag_size;
	const char *result;
	int valid;
	int error;

	/* The file. */
	if (vec_open(&file, "wycheproof-aes_gcm_test") != 0) {
		CHECK(0);
		return;
	}

	/* Each record. */
	while (vec_next(&file)) {
		key_size = vec_hex(&file, "key", key, sizeof(key));
		iv_size = vec_hex(&file, "iv", iv, sizeof(iv));
		aad_size = vec_hex(&file, "aad", aad, sizeof(aad));
		size = vec_hex(&file, "msg", msg, sizeof(msg));
		cipher_size = vec_hex(&file, "ct", cipher, sizeof(cipher));
		tag_size = vec_hex(&file, "tag", tag, sizeof(tag));
		result = vec_get(&file, "result");
		if (!check_one(key_size != (size_t)-1 && iv_size != (size_t)-1 && aad_size != (size_t)-1 &&
		    size != (size_t)-1 && cipher_size != (size_t)-1 && tag_size != (size_t)-1 && result != NULL,
		    __FILE__, __LINE__, "record is well formed"))
			continue;

		/* A tag that is not 16 bytes is out of range. */
		if (tag_size != 16U) {
			skipped_records++;
			continue;
		}
		used_records++;
		valid = strcmp(result, "valid") == 0;
		error = cryc_gcm_init(&gcm, key, key_size);
		CHECK(error == 0);
		memset(out, SENTINEL_BYTE, sizeof(out));

		/* An empty IV is an invalid vector that this interface refuses outright, in both directions, writing nothing. */
		if (iv_size == 0) {
			CHECK(!valid);
			error = cryc_gcm_open(&gcm, iv, iv_size, aad, aad_size, cipher, cipher_size, out, tag);
			CHECK(error == EINVAL);
			CHECK(all_equal(out, sizeof(out), SENTINEL_BYTE));
			memset(made_tag, SENTINEL_BYTE, sizeof(made_tag));
			error = cryc_gcm_seal(&gcm, iv, iv_size, aad, aad_size, msg, size, out, made_tag);
			CHECK(error == EINVAL);
			CHECK(all_equal(out, sizeof(out), SENTINEL_BYTE));
			CHECK(all_equal(made_tag, sizeof(made_tag), SENTINEL_BYTE));
		} else if (valid) {
			/* A valid vector seals to the same ciphertext and tag, and opens to the message. */
			check_valid_message(key, key_size, iv, iv_size, aad, aad_size, msg, cipher, size, tag, 16U);
		} else {
			/* An invalid vector fails to open and leaves zeros. */
			error = cryc_gcm_open(&gcm, iv, iv_size, aad, aad_size, cipher, cipher_size, out, tag);
			CHECK(error == EBADMSG);
			CHECK(all_zero(out, cipher_size));
			CHECK(all_equal(out + cipher_size, 16, SENTINEL_BYTE));
		}
		cryc_gcm_clear(&gcm);
	}
	vec_close(&file);
}

/* Compares the implementation with the reference on one combination of sizes, with buffers of exactly the sizes in use. */
static void
check_reference_one(
	size_t key_size,
	size_t iv_size,
	size_t aad_size,
	size_t size)
{
	struct cryc_gcm gcm;
	struct cryc_gcm before;
	uint8_t key[32];
	uint8_t tag[16];
	uint8_t expected_tag[16];
	uint8_t *iv;
	uint8_t *aad;
	uint8_t *plain;
	uint8_t *cipher;
	uint8_t *expected;
	uint8_t *back;
	int error;

	/* The inputs, in buffers of exactly the sizes (a write or read past them is an ASan error). */
	iv = malloc(iv_size);
	aad = malloc(aad_size > 0 ? aad_size : 1U);
	plain = malloc(size > 0 ? size : 1U);
	cipher = malloc(size > 0 ? size : 1U);
	expected = malloc(size > 0 ? size : 1U);
	back = malloc(size > 0 ? size : 1U);
	if (!check_one(iv != NULL && aad != NULL && plain != NULL && cipher != NULL && expected != NULL && back != NULL,
	    __FILE__, __LINE__, "allocated")) {
		free(iv);
		free(aad);
		free(plain);
		free(cipher);
		free(expected);
		free(back);
		return;
	}
	random_fill(key, key_size);
	random_fill(iv, iv_size);
	random_fill(aad, aad_size);
	random_fill(plain, size);

	/* The reference. */
	reference_seal(key, key_size, iv, iv_size, aad, aad_size, plain, size, expected, expected_tag);

	/* Seal, and the key structure unchanged by it. */
	CHECK(cryc_gcm_init(&gcm, key, key_size) == 0);
	memcpy(&before, &gcm, sizeof(gcm));
	error = cryc_gcm_seal(&gcm, iv, iv_size, aad, aad_size, plain, size, cipher, tag);
	CHECK(error == 0);
	CHECK_BYTES(cipher, expected, size);
	CHECK_BYTES(tag, expected_tag, 16);
	CHECK(memcmp(&gcm, &before, sizeof(gcm)) == 0);

	/* Open, and the key structure unchanged by it. */
	error = cryc_gcm_open(&gcm, iv, iv_size, aad, aad_size, cipher, size, back, tag);
	CHECK(error == 0);
	CHECK_BYTES(back, plain, size);
	CHECK(memcmp(&gcm, &before, sizeof(gcm)) == 0);

	/* In place both ways. */
	memcpy(back, plain, size);
	error = cryc_gcm_seal(&gcm, iv, iv_size, aad, aad_size, back, size, back, tag);
	CHECK(error == 0);
	CHECK_BYTES(back, expected, size);
	CHECK_BYTES(tag, expected_tag, 16);
	error = cryc_gcm_open(&gcm, iv, iv_size, aad, aad_size, back, size, back, tag);
	CHECK(error == 0);
	CHECK_BYTES(back, plain, size);

	/* The same plaintext with a changed IV gives another ciphertext and tag (the IV matters). */
	iv[iv_size - 1U] ^= 0x01;
	error = cryc_gcm_seal(&gcm, iv, iv_size, aad, aad_size, plain, size, cipher, tag);
	CHECK(error == 0);
	CHECK(memcmp(tag, expected_tag, 16) != 0);

	/* The key is wiped. */
	cryc_gcm_clear(&gcm);
	free(iv);
	free(aad);
	free(plain);
	free(cipher);
	free(expected);
	free(back);
}

/* Compares the implementation with the reference over the key sizes and a spread of IV, additional data and message sizes. */
static void
check_reference(void)
{
	static const size_t key_sizes[] = {16U, 24U, 32U};
	static const size_t iv_sizes[] = {1U, 8U, 11U, 12U, 13U, 16U, 17U, 60U};
	static const size_t aad_sizes[] = {0U, 1U, 16U, 33U};
	static const size_t sizes[] = {0U, 1U, 15U, 16U, 17U, 63U, 64U, 65U, 255U, 256U, 257U, 1000U};
	size_t key_index;
	size_t iv_index;
	size_t aad_index;
	size_t size_index;

	/* Every combination. */
	for (key_index = 0; key_index < sizeof(key_sizes) / sizeof(key_sizes[0]); key_index++) {
		for (iv_index = 0; iv_index < sizeof(iv_sizes) / sizeof(iv_sizes[0]); iv_index++) {
			for (aad_index = 0; aad_index < sizeof(aad_sizes) / sizeof(aad_sizes[0]); aad_index++) {
				for (size_index = 0; size_index < sizeof(sizes) / sizeof(sizes[0]); size_index++) {
					check_reference_one(key_sizes[key_index], iv_sizes[iv_index], aad_sizes[aad_index],
					    sizes[size_index]);
				}
			}
		}
	}
}

/* Checks every single-byte change of a message fails to open and leaves zeros, in place and not. */
static void
check_tamper(void)
{
	static const size_t iv_sizes[] = {1U, 12U, 16U};
	struct cryc_gcm gcm;
	uint8_t key[16];
	uint8_t iv[16];
	uint8_t aad[20];
	uint8_t plain[40];
	uint8_t cipher[40];
	uint8_t tag[16];
	uint8_t bad[40];
	uint8_t bad_tag[16];
	uint8_t bad_aad[20];
	uint8_t bad_iv[16];
	uint8_t out[40];
	size_t iv_index;
	size_t iv_size;
	size_t index;
	unsigned round;
	int error;

	/* One key and one message per IV size. */
	for (iv_index = 0; iv_index < sizeof(iv_sizes) / sizeof(iv_sizes[0]); iv_index++) {
		iv_size = iv_sizes[iv_index];
		random_fill(key, sizeof(key));
		random_fill(iv, sizeof(iv));
		random_fill(aad, sizeof(aad));
		random_fill(plain, sizeof(plain));
		CHECK(cryc_gcm_init(&gcm, key, sizeof(key)) == 0);
		CHECK(cryc_gcm_seal(&gcm, iv, iv_size, aad, sizeof(aad), plain, sizeof(plain), cipher, tag) == 0);

		/* Each bit of the ciphertext. */
		for (index = 0; index < sizeof(cipher) * 8U; index++) {
			memcpy(bad, cipher, sizeof(bad));
			bad[index / 8U] ^= (uint8_t)(1U << (index % 8U));
			memset(out, SENTINEL_BYTE, sizeof(out));
			error = cryc_gcm_open(&gcm, iv, iv_size, aad, sizeof(aad), bad, sizeof(bad), out, tag);
			CHECK(error == EBADMSG);
			CHECK(all_zero(out, sizeof(out)));
			error = cryc_gcm_open(&gcm, iv, iv_size, aad, sizeof(aad), bad, sizeof(bad), bad, tag);
			CHECK(error == EBADMSG);
			CHECK(all_zero(bad, sizeof(bad)));
		}

		/* Each bit of the tag. */
		for (index = 0; index < sizeof(tag) * 8U; index++) {
			memcpy(bad_tag, tag, sizeof(bad_tag));
			bad_tag[index / 8U] ^= (uint8_t)(1U << (index % 8U));
			memset(out, SENTINEL_BYTE, sizeof(out));
			error = cryc_gcm_open(&gcm, iv, iv_size, aad, sizeof(aad), cipher, sizeof(cipher), out, bad_tag);
			CHECK(error == EBADMSG);
			CHECK(all_zero(out, sizeof(out)));
		}

		/* Each bit of the additional data. */
		for (index = 0; index < sizeof(aad) * 8U; index++) {
			memcpy(bad_aad, aad, sizeof(bad_aad));
			bad_aad[index / 8U] ^= (uint8_t)(1U << (index % 8U));
			memset(out, SENTINEL_BYTE, sizeof(out));
			error = cryc_gcm_open(&gcm, iv, iv_size, bad_aad, sizeof(bad_aad), cipher, sizeof(cipher), out, tag);
			CHECK(error == EBADMSG);
			CHECK(all_zero(out, sizeof(out)));
		}

		/* Each bit of the IV in use. */
		for (index = 0; index < iv_size * 8U; index++) {
			memcpy(bad_iv, iv, sizeof(bad_iv));
			bad_iv[index / 8U] ^= (uint8_t)(1U << (index % 8U));
			memset(out, SENTINEL_BYTE, sizeof(out));
			error = cryc_gcm_open(&gcm, bad_iv, iv_size, aad, sizeof(aad), cipher, sizeof(cipher), out, tag);
			CHECK(error == EBADMSG);
			CHECK(all_zero(out, sizeof(out)));
		}

		/* A shorter additional data, a shorter ciphertext, and an IV one byte longer or shorter. */
		memset(out, SENTINEL_BYTE, sizeof(out));
		error = cryc_gcm_open(&gcm, iv, iv_size, aad, sizeof(aad) - 1U, cipher, sizeof(cipher), out, tag);
		CHECK(error == EBADMSG);
		CHECK(all_zero(out, sizeof(out)));
		memset(out, SENTINEL_BYTE, sizeof(out));
		error = cryc_gcm_open(&gcm, iv, iv_size, aad, sizeof(aad), cipher, sizeof(cipher) - 1U, out, tag);
		CHECK(error == EBADMSG);
		CHECK(all_zero(out, sizeof(cipher) - 1U));
		CHECK(out[sizeof(cipher) - 1U] == SENTINEL_BYTE);
		if (iv_size > 1U) {
			memset(out, SENTINEL_BYTE, sizeof(out));
			error = cryc_gcm_open(&gcm, iv, iv_size - 1U, aad, sizeof(aad), cipher, sizeof(cipher), out, tag);
			CHECK(error == EBADMSG);
			CHECK(all_zero(out, sizeof(out)));
		}

		/* The additional data and the ciphertext are not interchangeable (the lengths are in the tag). */
		memset(out, SENTINEL_BYTE, sizeof(out));
		error = cryc_gcm_open(&gcm, iv, iv_size, cipher, 20U, cipher + 20, 20U, out, tag);
		CHECK(error == EBADMSG);
		CHECK(all_zero(out, 20U));

		/* Random tags and ciphertexts all fail. */
		for (round = 0; round < 50U; round++) {
			random_fill(bad, sizeof(bad));
			random_fill(bad_tag, sizeof(bad_tag));
			memset(out, SENTINEL_BYTE, sizeof(out));
			error = cryc_gcm_open(&gcm, iv, iv_size, aad, sizeof(aad), bad, sizeof(bad), out, bad_tag);
			CHECK(error == EBADMSG);
			CHECK(all_zero(out, sizeof(out)));
		}
		cryc_gcm_clear(&gcm);
	}
}

/* Checks the sizes the interface refuses and that a refusal writes nothing. */
static void
check_refusals(void)
{
	struct cryc_gcm gcm;
	uint8_t key[16];
	uint8_t iv[16];
	uint8_t out[64];
	uint8_t tag[16];
	uint8_t input[64];
	size_t over;
	size_t huge;
	int error;

	/* A key. */
	random_fill(key, sizeof(key));
	random_fill(iv, sizeof(iv));
	random_fill(input, sizeof(input));
	CHECK(cryc_gcm_init(&gcm, key, sizeof(key)) == 0);

	/* An empty IV is EINVAL for seal and for open, and nothing is written. */
	memset(out, SENTINEL_BYTE, sizeof(out));
	memset(tag, SENTINEL_BYTE, sizeof(tag));
	error = cryc_gcm_seal(&gcm, iv, 0, NULL, 0, input, 32U, out, tag);
	CHECK(error == EINVAL);
	CHECK(all_equal(out, sizeof(out), SENTINEL_BYTE));
	CHECK(all_equal(tag, sizeof(tag), SENTINEL_BYTE));
	error = cryc_gcm_open(&gcm, iv, 0, NULL, 0, input, 32U, out, tag);
	CHECK(error == EINVAL);
	CHECK(all_equal(out, sizeof(out), SENTINEL_BYTE));

	/* The empty message and no additional data (NULL pointers) are allowed. */
	error = cryc_gcm_seal(&gcm, iv, 12U, NULL, 0, NULL, 0, NULL, tag);
	CHECK(error == 0);
	error = cryc_gcm_open(&gcm, iv, 12U, NULL, 0, NULL, 0, NULL, tag);
	CHECK(error == 0);
	tag[0] ^= 0x01;
	error = cryc_gcm_open(&gcm, iv, 12U, NULL, 0, NULL, 0, NULL, tag);
	CHECK(error == EBADMSG);

	/* Sizes that do not fit are refused before any byte is read or written (only on a machine whose size_t holds them). */
	if (sizeof(size_t) > 4U) {
		/* One block over the plaintext limit of 2^32 - 2 blocks. */
		over = (size_t)((uint64_t)CRYC_GCM_TEXT_MAX_BLOCKS * 16U + 1U);
		memset(out, SENTINEL_BYTE, sizeof(out));
		memset(tag, SENTINEL_BYTE, sizeof(tag));
		error = cryc_gcm_seal(&gcm, iv, 12U, NULL, 0, input, over, out, tag);
		CHECK(error == EINVAL);
		CHECK(all_equal(out, sizeof(out), SENTINEL_BYTE));
		CHECK(all_equal(tag, sizeof(tag), SENTINEL_BYTE));
		error = cryc_gcm_open(&gcm, iv, 12U, NULL, 0, input, over, out, tag);
		CHECK(error == EINVAL);
		CHECK(all_equal(out, sizeof(out), SENTINEL_BYTE));

		/* An IV, or additional data, whose length in bits does not fit 64 bits (2^61 bytes). */
		huge = (size_t)CRYC_U64(0x20000000U, 0x00000000U);
		error = cryc_gcm_seal(&gcm, iv, huge, NULL, 0, input, 16U, out, tag);
		CHECK(error == EINVAL);
		error = cryc_gcm_seal(&gcm, iv, 12U, input, huge, input, 16U, out, tag);
		CHECK(error == EINVAL);
		error = cryc_gcm_open(&gcm, iv, huge, NULL, 0, input, 16U, out, tag);
		CHECK(error == EINVAL);
		error = cryc_gcm_open(&gcm, iv, 12U, input, huge, input, 16U, out, tag);
		CHECK(error == EINVAL);
		CHECK(all_equal(out, sizeof(out), SENTINEL_BYTE));
	}

	/* The largest size_t is refused on any machine. */
	error = cryc_gcm_seal(&gcm, iv, 12U, NULL, 0, input, (size_t)-1, out, tag);
	CHECK(error == EINVAL);
	error = cryc_gcm_open(&gcm, iv, 12U, NULL, 0, input, (size_t)-1, out, tag);
	CHECK(error == EINVAL);
	CHECK(all_equal(out, sizeof(out), SENTINEL_BYTE));
	cryc_gcm_clear(&gcm);
}

/* Checks init: the key sizes, the missing key, the hash subkey, and clear. */
static void
check_init(void)
{
	struct cryc_gcm gcm;
	struct cryc_aes aes;
	uint8_t key[64];
	uint8_t zero[16];
	uint8_t expected[16];
	size_t key_size;
	size_t index;
	int error;

	/* Each key size from 0 to 64 and the largest size_t: only 16, 24 and 32 are accepted, the others leave all zeros. */
	random_fill(key, sizeof(key));
	for (key_size = 0; key_size <= 64U; key_size++) {
		memset(&gcm, 0xa5, sizeof(gcm));
		error = cryc_gcm_init(&gcm, key, key_size);
		if (key_size == 16U || key_size == 24U || key_size == 32U) {
			CHECK(error == 0);

			/* H is the block cipher of the zero block under the key. */
			memset(zero, 0, sizeof(zero));
			CHECK(cryc_aes_init(&aes, key, key_size) == 0);
			cryc_aes_encrypt_blocks(&aes, zero, expected, 1);
			CHECK_BYTES(gcm.h, expected, 16);
			CHECK(memcmp(&gcm.aes, &aes, sizeof(aes)) == 0);
			cryc_aes_clear(&aes);
		} else {
			CHECK(error == EINVAL);
			CHECK(all_zero((const uint8_t *)&gcm, sizeof(gcm)));
		}
		cryc_gcm_clear(&gcm);
		CHECK(all_zero((const uint8_t *)&gcm, sizeof(gcm)));
	}
	memset(&gcm, 0xa5, sizeof(gcm));
	error = cryc_gcm_init(&gcm, key, (size_t)-1);
	CHECK(error == EINVAL);
	CHECK(all_zero((const uint8_t *)&gcm, sizeof(gcm)));

	/* A missing key is EINVAL and leaves all zeros. */
	for (index = 0; index < 3U; index++) {
		memset(&gcm, 0xa5, sizeof(gcm));
		error = cryc_gcm_init(&gcm, NULL, 16U + 8U * index);
		CHECK(error == EINVAL);
		CHECK(all_zero((const uint8_t *)&gcm, sizeof(gcm)));
	}
}

/* Runs the checks. */
int
main(void)
{
	static const char *const encrypt_names[] = {
		"cavp-gcmtestvectors-gcmEncryptExtIV128", "cavp-gcmtestvectors-gcmEncryptExtIV192",
		"cavp-gcmtestvectors-gcmEncryptExtIV256"
	};
	static const char *const decrypt_names[] = {
		"cavp-gcmtestvectors-gcmDecrypt128", "cavp-gcmtestvectors-gcmDecrypt192",
		"cavp-gcmtestvectors-gcmDecrypt256"
	};
	size_t index;

	/* The multiplication, GHASH, the key. */
	check_multiply();
	check_ghash();
	check_init();

	/* The CAVP encryption and decryption vectors. */
	for (index = 0; index < 3U; index++) {
		check_cavp_encrypt(encrypt_names[index]);
		printf("cavp encrypt %lu: records used so far %lu, out of range (tag under 16 bytes) so far %lu\n",
		    (unsigned long)index, used_records, skipped_records);
	}
	for (index = 0; index < 3U; index++) {
		check_cavp_decrypt(decrypt_names[index]);
		printf("cavp decrypt %lu: records used so far %lu, out of range (tag under 16 bytes) so far %lu\n",
		    (unsigned long)index, used_records, skipped_records);
	}

	/* The Wycheproof vectors. */
	used_records = 0;
	skipped_records = 0;
	check_wycheproof();
	printf("wycheproof: records used %lu, out of range (tag not 16 bytes) %lu\n", used_records, skipped_records);

	/* The reference, the forgeries, the refusals. */
	check_reference();
	check_tamper();
	check_refusals();

	/* The result. */
	return check_finish("test-gcm");
}
