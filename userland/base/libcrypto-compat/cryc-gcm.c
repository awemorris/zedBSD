/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * AES-GCM (NIST SP 800-38D).
 *
 * The section and algorithm numbers in the comments of this file are
 * those of NIST SP 800-38D (November 2007).
 *
 * The GHASH multiplication is Algorithm 1 written with 64-bit halves:
 * every "if" of the algorithm is a mask, so no branch and no address
 * depends on the hash subkey or on the data.  The counter mode is written
 * here on the block cipher of cryc-aes.h.  A tag is always 16 bytes (t = 128).
 */

#include <string.h>

#include "cryc-gcm.h"

/* The largest length in bytes whose length in bits still fits 64 bits: 2^61 - 1 (5.2.1.1, len(A) and len(IV) below 2^64 bits). */
#define GCM_LENGTH_MAX_BYTES	CRYC_U64(0x1fffffffU, 0xffffffffU)

/* The bytes per block the plaintext limit is counted in. */
#define GCM_BLOCK		16U

/* The most plaintext bytes of one call, 2^32 - 2 blocks (5.2.1.1). */
#define GCM_TEXT_MAX_BYTES	((uint64_t)CRYC_GCM_TEXT_MAX_BLOCKS * (uint64_t)GCM_BLOCK)

/* The blocks one pass of the counter mode encrypts: the counter blocks of a pass are always made four at a time. */
#define GCM_COUNTER_BLOCKS	4U

/* The bytes decrypted into the scratch buffer at a time by cryc_gcm_open (a multiple of 16: only the last chunk is partial). */
#define GCM_OPEN_CHUNK		256U

/*
 * The high 64-bit half of the reduction constant R of Algorithm 1: R is
 * 11100001 followed by 120 zero bits.
 *
 * Values referenced: NIST SP 800-38D (November 2007) 6.3, the constant R.
 */
#define GCM_R_HIGH		CRYC_U64(0xe1000000U, 0x00000000U)

/* Checks the sizes of one call; returns 0 or EINVAL. */
static int gcm_check_sizes(size_t iv_size, size_t aad_size, size_t size);

/* Makes the pre-counter block J0 from the IV (7.1, step 2). */
static void gcm_make_j0(const struct cryc_gcm *gcm, const uint8_t *iv, size_t iv_size, uint8_t *j0);

/* Makes the tag over the additional data and the ciphertext (7.1, steps 5 to 8). */
static void gcm_make_tag(const struct cryc_gcm *gcm, const uint8_t *j0, const uint8_t *aad, size_t aad_size, const uint8_t *cipher, size_t size, uint8_t *tag);

/* Adds 1 modulo 2^32 to the last four bytes of a counter block (6.2, inc32). */
static void gcm_inc32(uint8_t *block);

/* Exclusive-ors size bytes with the keystream of the counter blocks from counter on, and advances counter (6.5, GCTR). */
static void gcm_counter_xor(const struct cryc_gcm *gcm, uint8_t *counter, const uint8_t *in, uint8_t *out, size_t size);

/*
 * Multiplies the 16-byte block x by the 16-byte block y in GF(2^128) (6.3,
 * Algorithm 1) and writes the product to out.
 *
 * Both inputs are read before out is written, so out may be x or y.  The
 * product is the sum of the 128 shifted copies of y that the bits of x
 * select; the selection and the reduction by R are masks.
 */
void
cryc_gcm_multiply(
	uint8_t *out,
	const uint8_t *x,
	const uint8_t *y)
{
	uint64_t z_high;
	uint64_t z_low;
	uint64_t v_high;
	uint64_t v_low;
	uint64_t select_mask;
	uint64_t reduce_mask;
	unsigned bit_index;
	unsigned bit;

	/* Z = 0^128 and V = Y, the two registers of the algorithm. */
	z_high = 0;
	z_low = 0;
	v_high = cryc_load_be64(y);
	v_low = cryc_load_be64(y + 8);

	/* The 128 steps, one for each bit x_i of x taken from the leftmost bit of x[0]. */
	for (bit_index = 0; bit_index < 128U; bit_index++) {
		/* All ones when x_i is 1: Z is then replaced by Z xor V. */
		bit = (unsigned)((x[bit_index >> 3] >> (7U - (bit_index & 7U))) & 1U);
		select_mask = (uint64_t)0 - (uint64_t)bit;
		z_high ^= v_high & select_mask;
		z_low ^= v_low & select_mask;

		/* All ones when LSB1(V) is 1: V is then shifted right one bit and R is added. */
		reduce_mask = (uint64_t)0 - (v_low & 1U);
		v_low = (v_low >> 1) | (v_high << 63);
		v_high = v_high >> 1;
		v_high ^= GCM_R_HIGH & reduce_mask;
	}

	/* Writes the product Z. */
	cryc_store_be64(out, z_high);
	cryc_store_be64(out + 8, z_low);
}

/*
 * Absorbs size bytes into the GHASH state y with hash subkey h (6.4,
 * Algorithm 2): for each 16-byte block X_i, y = (y xor X_i) * h.  A last
 * partial block is padded with zeros on the right.
 */
void
cryc_gcm_ghash(
	uint8_t *y,
	const uint8_t *h,
	const uint8_t *data,
	size_t size)
{
	uint8_t block[GCM_BLOCK];
	size_t offset;
	size_t take;
	size_t index;

	/* Each block of the data, the last one maybe partial. */
	for (offset = 0; offset < size; offset += GCM_BLOCK) {
		take = size - offset;
		if (take > GCM_BLOCK)
			take = GCM_BLOCK;

		/* The block, padded with zeros on the right when it is short. */
		memset(block, 0, sizeof(block));
		memcpy(block, data + offset, take);

		/* y = (y xor X_i) * h. */
		for (index = 0; index < GCM_BLOCK; index++)
			y[index] = (uint8_t)(y[index] ^ block[index]);
		cryc_gcm_multiply(y, y, h);
	}
}

/*
 * Expands key (16, 24 or 32 bytes) and computes H.
 */
int
cryc_gcm_init(
	struct cryc_gcm *gcm,
	const uint8_t *key,
	size_t key_size)
{
	uint8_t zero[GCM_BLOCK];
	int error;

	/* Starts from an all-zero structure, which is also what every failure leaves. */
	cryc_wipe(gcm, sizeof(*gcm));

	/* Expands the key (a wrong size or a missing key is EINVAL, with the cipher already wiped). */
	error = cryc_aes_init(&gcm->aes, key, key_size);
	if (error != 0) {
		cryc_wipe(gcm, sizeof(*gcm));
		return error;
	}

	/* H is the block cipher applied to the all-zero block (7.1, step 1). */
	memset(zero, 0, sizeof(zero));
	cryc_aes_encrypt_blocks(&gcm->aes, zero, gcm->h, 1);

	/* Succeeded: the key and H are ready. */
	return 0;
}

/*
 * Encrypts and authenticates (GCM-AE, 7.1, Algorithm 4).
 */
int
cryc_gcm_seal(
	const struct cryc_gcm *gcm,
	const uint8_t *iv,
	size_t iv_size,
	const uint8_t *aad,
	size_t aad_size,
	const uint8_t *in,
	size_t size,
	uint8_t *out,
	uint8_t *tag)
{
	uint8_t j0[GCM_BLOCK];
	uint8_t counter[GCM_BLOCK];
	int error;

	/* Refuses an empty IV and a message over the limits, before anything is written. */
	error = gcm_check_sizes(iv_size, aad_size, size);
	if (error != 0)
		return error;

	/* The pre-counter block J0. */
	gcm_make_j0(gcm, iv, iv_size, j0);

	/* The ciphertext: the counter mode starting at inc32(J0) (7.1, steps 3 and 4). */
	memcpy(counter, j0, sizeof(counter));
	gcm_inc32(counter);
	gcm_counter_xor(gcm, counter, in, out, size);

	/* The tag over the additional data and the ciphertext just written. */
	gcm_make_tag(gcm, j0, aad, aad_size, out, size, tag);

	/* Clears the counter blocks from the stack. */
	cryc_wipe(j0, sizeof(j0));
	cryc_wipe(counter, sizeof(counter));

	/* Succeeded: out and tag are written. */
	return 0;
}

/*
 * Checks and decrypts (GCM-AD, 7.2, Algorithm 5).
 *
 * The tag is made from the ciphertext first.  The plaintext is then made a
 * chunk at a time in a scratch buffer and copied to out through a mask that
 * is all ones for a good tag and zero for a bad one, so a forged message
 * leaves nothing but zeros in out and nothing branches on the tag.
 */
int
cryc_gcm_open(
	const struct cryc_gcm *gcm,
	const uint8_t *iv,
	size_t iv_size,
	const uint8_t *aad,
	size_t aad_size,
	const uint8_t *in,
	size_t size,
	uint8_t *out,
	const uint8_t *tag)
{
	uint8_t j0[GCM_BLOCK];
	uint8_t counter[GCM_BLOCK];
	uint8_t expected[GCM_BLOCK];
	uint8_t scratch[GCM_OPEN_CHUNK];
	uint32_t good_mask;
	uint32_t error_value;
	size_t offset;
	size_t take;
	size_t index;
	int equal;
	int error;

	/* Refuses an empty IV and a message over the limits, before anything is written. */
	error = gcm_check_sizes(iv_size, aad_size, size);
	if (error != 0)
		return error;

	/* The pre-counter block J0 and the tag the ciphertext should carry. */
	gcm_make_j0(gcm, iv, iv_size, j0);
	gcm_make_tag(gcm, j0, aad, aad_size, in, size, expected);

	/* All ones when the received tag is the expected one, zero otherwise. */
	equal = cryc_ct_equal(expected, tag, CRYC_GCM_TAG);
	good_mask = (uint32_t)0 - (uint32_t)equal;

	/* The plaintext, a chunk at a time, kept only under the mask. */
	memcpy(counter, j0, sizeof(counter));
	gcm_inc32(counter);
	for (offset = 0; offset < size; offset += GCM_OPEN_CHUNK) {
		take = size - offset;
		if (take > GCM_OPEN_CHUNK)
			take = GCM_OPEN_CHUNK;
		gcm_counter_xor(gcm, counter, in + offset, scratch, take);
		for (index = 0; index < take; index++)
			out[offset + index] = (uint8_t)(scratch[index] & (uint8_t)good_mask);
	}

	/* EBADMSG when the mask is zero and 0 when it is all ones, without a branch. */
	error_value = (uint32_t)EBADMSG & ~good_mask;
	error = (int)error_value;

	/* Clears the plaintext and the counter blocks from the stack. */
	cryc_wipe(scratch, sizeof(scratch));
	cryc_wipe(counter, sizeof(counter));
	cryc_wipe(expected, sizeof(expected));
	cryc_wipe(j0, sizeof(j0));

	/* Reports 0 for a good tag and EBADMSG for a bad one. */
	return error;
}

/*
 * Wipes the key and H.
 */
void
cryc_gcm_clear(
	struct cryc_gcm *gcm)
{
	/* The expanded key and H. */
	cryc_wipe(gcm, sizeof(*gcm));
}

/*
 * Checks the sizes of one call.
 *
 * The IV needs one byte or more, the IV and the additional data must have
 * a length in bits that fits 64 bits (7.1 uses it in the last GHASH block),
 * and the message is at most 2^32 - 2 blocks and leaves room for a tag in
 * size_t.
 */
static int
gcm_check_sizes(
	size_t iv_size,
	size_t aad_size,
	size_t size)
{
	/* An empty IV is refused (5.2.1.1: 1 <= len(IV)). */
	if (iv_size == 0)
		return EINVAL;

	/* An IV whose length in bits does not fit 64 bits. */
	if ((uint64_t)iv_size > GCM_LENGTH_MAX_BYTES)
		return EINVAL;

	/* Additional data whose length in bits does not fit 64 bits. */
	if ((uint64_t)aad_size > GCM_LENGTH_MAX_BYTES)
		return EINVAL;

	/* A message over 2^32 - 2 blocks, where the counter would reach J0 again. */
	if ((uint64_t)size > GCM_TEXT_MAX_BYTES)
		return EINVAL;

	/* A message so long that its size plus the tag does not fit size_t. */
	if (size > (size_t)-1 - CRYC_GCM_TAG)
		return EINVAL;

	/* Succeeded: the sizes are in range. */
	return 0;
}

/*
 * Makes the pre-counter block J0 from the IV (7.1, step 2).
 *
 * A 96-bit IV followed by 0^31 and 1 is J0.  Any other IV is absorbed by
 * GHASH, padded with zeros to a block boundary, followed by a block of 64
 * zero bits and the 64-bit length of the IV in bits.
 */
static void
gcm_make_j0(
	const struct cryc_gcm *gcm,
	const uint8_t *iv,
	size_t iv_size,
	uint8_t *j0)
{
	uint8_t length_block[GCM_BLOCK];

	/* The 96-bit IV: the IV, then the 32-bit value 1. */
	if (iv_size == CRYC_GCM_IV) {
		memcpy(j0, iv, CRYC_GCM_IV);
		cryc_store_be32(j0 + CRYC_GCM_IV, 1U);
		return;
	}

	/* Any other length: J0 = GHASH(IV || 0^(s+64) || [len(IV)]64). */
	memset(j0, 0, GCM_BLOCK);
	cryc_gcm_ghash(j0, gcm->h, iv, iv_size);
	memset(length_block, 0, sizeof(length_block));
	cryc_store_be64(length_block + 8, (uint64_t)iv_size * 8U);
	cryc_gcm_ghash(j0, gcm->h, length_block, sizeof(length_block));
}

/*
 * Makes the tag over the additional data and the ciphertext (7.1, steps 5
 * to 8): GHASH of A and C, each padded with zeros to a block boundary,
 * and the two lengths in bits as 64-bit integers, exclusive-ored with the
 * block cipher of J0 (GCTR of one block).
 */
static void
gcm_make_tag(
	const struct cryc_gcm *gcm,
	const uint8_t *j0,
	const uint8_t *aad,
	size_t aad_size,
	const uint8_t *cipher,
	size_t size,
	uint8_t *tag)
{
	uint8_t state[GCM_BLOCK];
	uint8_t length_block[GCM_BLOCK];
	uint8_t mask[GCM_BLOCK];
	size_t index;

	/* The additional data, then the ciphertext, each padded to a block boundary. */
	memset(state, 0, sizeof(state));
	cryc_gcm_ghash(state, gcm->h, aad, aad_size);
	cryc_gcm_ghash(state, gcm->h, cipher, size);

	/* The block of the two bit lengths, len(A) then len(C). */
	cryc_store_be64(length_block, (uint64_t)aad_size * 8U);
	cryc_store_be64(length_block + 8, (uint64_t)size * 8U);
	cryc_gcm_ghash(state, gcm->h, length_block, sizeof(length_block));

	/* The block cipher of J0, the mask that hides the GHASH value. */
	cryc_aes_encrypt_blocks(&gcm->aes, j0, mask, 1);

	/* T = GCTR(J0, S) = E(J0) xor S, the full 16 bytes. */
	for (index = 0; index < GCM_BLOCK; index++)
		tag[index] = (uint8_t)(state[index] ^ mask[index]);

	/* The GHASH value and the mask are secret until they are the tag. */
	cryc_wipe(state, sizeof(state));
	cryc_wipe(mask, sizeof(mask));
}

/*
 * Adds 1 modulo 2^32 to the last four bytes of a counter block (6.2,
 * inc32).
 */
static void
gcm_inc32(
	uint8_t *block)
{
	uint32_t value;

	/* The last four bytes as a big-endian integer, one more, wrapped. */
	value = cryc_load_be32(block + 12);
	value = value + 1U;
	cryc_store_be32(block + 12, value);
}

/*
 * Exclusive-ors size bytes with the keystream of the counter blocks
 * counter, inc32(counter), ... into out (6.5, GCTR), and leaves in counter
 * the first block not used.
 *
 * For an IV that is not 96 bits long the counter is the output of GHASH
 * under the secret H, so the counter blocks are made four at a time with a
 * fixed number of steps: no loop test then involves the counter value.
 * The number of blocks encrypted in a pass depends only on size.
 */
static void
gcm_counter_xor(
	const struct cryc_gcm *gcm,
	uint8_t *counter,
	const uint8_t *in,
	uint8_t *out,
	size_t size)
{
	uint8_t blocks[GCM_COUNTER_BLOCKS * GCM_BLOCK];
	uint8_t keystream[GCM_COUNTER_BLOCKS * GCM_BLOCK];
	uint32_t count32;
	size_t chunk;
	size_t block_count;
	size_t block;
	size_t index;

	/* The last four bytes of the counter block are the 32-bit counter. */
	count32 = cryc_load_be32(counter + 12);

	/* Each pass handles up to four blocks. */
	while (size > 0) {
		chunk = size;
		if (chunk > sizeof(blocks))
			chunk = sizeof(blocks);
		block_count = (chunk + GCM_BLOCK - 1U) / GCM_BLOCK;

		/* The four counter blocks of the pass, whether or not all of them are used. */
		for (block = 0; block < GCM_COUNTER_BLOCKS; block++) {
			memcpy(blocks + block * GCM_BLOCK, counter, 12);
			cryc_store_be32(blocks + block * GCM_BLOCK + 12, count32 + (uint32_t)block);
		}

		/* The counter is now past the blocks this pass uses (inc32 wraps modulo 2^32). */
		count32 = count32 + (uint32_t)block_count;

		/* The keystream of the blocks in use. */
		cryc_aes_encrypt_blocks(&gcm->aes, blocks, keystream, block_count);

		/* XORs the bytes in use; the rest of the last block's keystream is dropped. */
		for (index = 0; index < chunk; index++)
			out[index] = (uint8_t)(in[index] ^ keystream[index]);

		in += chunk;
		out += chunk;
		size -= chunk;
	}

	/* Hands the next counter block back. */
	cryc_store_be32(counter + 12, count32);

	/* The keystream is secret. */
	cryc_wipe(keystream, sizeof(keystream));
	cryc_wipe(blocks, sizeof(blocks));
}
