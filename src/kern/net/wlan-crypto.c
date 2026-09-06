/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The WLAN cryptographic primitives.
 *
 * SHA-1, HMAC-SHA1, PBKDF2, and the WPA pseudo-random function derive
 * the WPA2 keys; AES-128 and the RFC 3394 key unwrap recover the group
 * key from the handshake.  Everything is written for correctness rather
 * than speed: no tables, constant-time comparison and erasure, and
 * overlap checks on every caller-supplied buffer.
 */

#include "wlan-crypto.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>

#define SHA1_LENGTH_MAX_BYTES (UINT64_MAX / 8U)
#define WPA_PRF_OUTPUT_MAX (256U * WLAN_SHA1_DIGEST_SIZE)

static int ranges_overlap(const void *left, size_t left_length, const void *right, size_t right_length);
static uint32_t rotate_left32(uint32_t value, unsigned amount);
static uint32_t read_be32(const uint8_t *bytes);
static void write_be32(uint8_t *bytes, uint32_t value);
static void sha1_transform(struct wlan_sha1_context *context, const uint8_t block[64]);
static uint8_t aes_gf_multiply(uint8_t left, uint8_t right);
static uint8_t aes_gf_inverse(uint8_t value);
static uint8_t rotate_left8(uint8_t value, unsigned amount);
static uint8_t aes_sbox(uint8_t value);
static uint8_t aes_inverse_sbox(uint8_t value);
static void aes128_expand_key(const uint8_t key[16], uint8_t expanded[176]);
static void aes_add_round_key(uint8_t state[16], const uint8_t *round_key);
static void aes_sub_bytes(uint8_t state[16]);
static void aes_inverse_sub_bytes(uint8_t state[16]);
static void aes_shift_rows(uint8_t state[16]);
static void aes_inverse_shift_rows(uint8_t state[16]);
static void aes_mix_columns(uint8_t state[16]);
static void aes_inverse_mix_columns(uint8_t state[16]);
static void aes128_encrypt_expanded(const uint8_t expanded[176], const uint8_t input[16], uint8_t output[16]);
static void aes128_decrypt_expanded(const uint8_t expanded[176], const uint8_t input[16], uint8_t output[16]);

/*
 * Overwrites secret data in a way the compiler cannot elide.
 */
void
wlan_crypto_erase(
	void *data,
	size_t length)
{
	volatile uint8_t *bytes;

	/* Ignores a missing buffer. */
	if (data == NULL)
		return;

	/* Clears through a volatile pointer so the stores are kept. */
	bytes = (volatile uint8_t *)data;
	while (length != 0U) {
		*bytes++ = 0U;
		length--;
	}
}

/*
 * Compares two buffers in constant time.
 */
int
wlan_crypto_equal(
	const void *left,
	const void *right,
	size_t length)
{
	volatile const uint8_t *left_bytes;
	volatile const uint8_t *right_bytes;
	volatile uint8_t difference;
	size_t index;

	/* Empty buffers are equal; a missing one never is. */
	if (length == 0U)
		return 1;
	if (left == NULL || right == NULL)
		return 0;

	/* Accumulates every differing bit without an early exit. */
	left_bytes = (volatile const uint8_t *)left;
	right_bytes = (volatile const uint8_t *)right;
	difference = 0U;
	for (index = 0U; index < length; index++)
		difference |= (uint8_t)(left_bytes[index] ^ right_bytes[index]);

	/* Reports equality when no bit differed. */
	if (difference != 0U)
		return 0;
	return 1;
}

/*
 * Starts a SHA-1 computation.
 */
int
wlan_sha1_init(
	struct wlan_sha1_context *context)
{
	/* Rejects a missing context. */
	if (context == NULL)
		return EINVAL;

	/* Loads the initial state with an empty block. */
	context->state[0] = 0x67452301U;
	context->state[1] = 0xefcdab89U;
	context->state[2] = 0x98badcfeU;
	context->state[3] = 0x10325476U;
	context->state[4] = 0xc3d2e1f0U;
	context->total_bytes = 0U;
	context->block_length = 0U;
	context->failed = 0;
	memset(context->block, 0, sizeof(context->block));

	/* Reports the started computation. */
	return 0;
}

/*
 * Feeds data into a SHA-1 computation.
 *
 * Data that overlaps the context, or a total length beyond what the
 * length field can represent, marks the context failed.
 */
int
wlan_sha1_update(
	struct wlan_sha1_context *context,
	const void *data,
	size_t length)
{
	const uint8_t *bytes;
	size_t amount;

	/* Rejects a missing context or data, or a failed context. */
	if (context == NULL || (data == NULL && length != 0U))
		return EINVAL;
	if (context->failed)
		return EINVAL;
	if (ranges_overlap(context, sizeof(*context), data, length)) {
		context->failed = 1;
		return EINVAL;
	}
	if (context->total_bytes > SHA1_LENGTH_MAX_BYTES ||
	    (uint64_t)length > SHA1_LENGTH_MAX_BYTES - context->total_bytes) {
		context->failed = 1;
		return EOVERFLOW;
	}

	/* Fills the block buffer, transforming each block as it completes. */
	context->total_bytes += (uint64_t)length;
	bytes = (const uint8_t *)data;
	while (length != 0U) {
		amount = WLAN_SHA1_BLOCK_SIZE - context->block_length;
		if (amount > length)
			amount = length;
		memcpy(context->block + context->block_length, bytes, amount);
		context->block_length += amount;
		bytes += amount;
		length -= amount;
		if (context->block_length == WLAN_SHA1_BLOCK_SIZE) {
			sha1_transform(context, context->block);
			context->block_length = 0U;
		}
	}

	/* Reports the absorbed data. */
	return 0;
}

/*
 * Finishes a SHA-1 computation and erases the context.
 */
int
wlan_sha1_final(
	struct wlan_sha1_context *context,
	uint8_t digest[WLAN_SHA1_DIGEST_SIZE])
{
	uint64_t bit_length;
	unsigned index;

	/* Rejects a missing context or digest, an overlap, or a failed context. */
	if (context == NULL || digest == NULL)
		return EINVAL;
	if (ranges_overlap(context, sizeof(*context), digest,
	    WLAN_SHA1_DIGEST_SIZE)) {
		context->failed = 1;
		return EINVAL;
	}
	if (context->failed) {
		wlan_crypto_erase(context, sizeof(*context));
		return EINVAL;
	}

	/* Pads the last block with a one bit, zeros, and the bit length. */
	bit_length = context->total_bytes * 8U;
	context->block[context->block_length++] = 0x80U;
	if (context->block_length > 56U) {
		memset(context->block + context->block_length, 0,
		    WLAN_SHA1_BLOCK_SIZE - context->block_length);
		sha1_transform(context, context->block);
		context->block_length = 0U;
	}
	memset(context->block + context->block_length, 0,
	    56U - context->block_length);
	for (index = 0U; index < 8U; index++)
		context->block[63U - index] = (uint8_t)(bit_length >>
		    (index * 8U));
	sha1_transform(context, context->block);

	/* Writes the digest and erases the context. */
	for (index = 0U; index < 5U; index++)
		write_be32(digest + index * 4U, context->state[index]);
	wlan_crypto_erase(context, sizeof(*context));

	/* Reports the finished digest. */
	return 0;
}

/*
 * Computes the SHA-1 digest of a buffer.
 */
int
wlan_sha1(
	const void *data,
	size_t length,
	uint8_t digest[WLAN_SHA1_DIGEST_SIZE])
{
	struct wlan_sha1_context context;
	int error;

#if SIZE_MAX > UINT64_MAX / 8U
	/* A length the bit counter cannot hold is refused. */
	if ((uint64_t)length > SHA1_LENGTH_MAX_BYTES)
		return EOVERFLOW;
#endif

	/* Rejects missing data or digest, or a digest inside the data. */
	if ((data == NULL && length != 0U) ||
	    digest == NULL ||
	    ranges_overlap(data, length, digest, WLAN_SHA1_DIGEST_SIZE))
		return EINVAL;

	/* Runs the three phases, erasing the context on failure. */
	error = wlan_sha1_init(&context);
	if (error == 0)
		error = wlan_sha1_update(&context, data, length);
	if (error == 0)
		error = wlan_sha1_final(&context, digest);
	else
		wlan_crypto_erase(&context, sizeof(context));

	/* Reports the digest result. */
	return error;
}

/*
 * Starts an HMAC-SHA1 computation with a key.
 *
 * A key longer than a block is hashed first.  The context is erased on
 * failure.
 */
int
wlan_hmac_sha1_init(
	struct wlan_hmac_sha1_context *context,
	const void *key,
	size_t key_length)
{
	uint8_t key_block[WLAN_SHA1_BLOCK_SIZE];
	uint8_t key_digest[WLAN_SHA1_DIGEST_SIZE];
	uint8_t inner_pad[WLAN_SHA1_BLOCK_SIZE];
	uint8_t outer_pad[WLAN_SHA1_BLOCK_SIZE];
	const uint8_t *key_bytes;
	size_t normalized_length;
	size_t index;
	int error;

#if SIZE_MAX > UINT64_MAX / 8U
	/* A key the bit counter cannot hold is refused. */
	if ((uint64_t)key_length > SHA1_LENGTH_MAX_BYTES)
		return EOVERFLOW;
#endif

	/* Rejects a missing context or key, or a key inside the context. */
	if (context == NULL ||
	    (key == NULL && key_length != 0U) ||
	    ranges_overlap(context, sizeof(*context), key, key_length))
		return EINVAL;

	/* Normalizes the key to one block, hashing a long one. */
	memset(key_block, 0, sizeof(key_block));
	memset(key_digest, 0, sizeof(key_digest));
	key_bytes = (const uint8_t *)key;
	normalized_length = key_length;
	if (key_length > WLAN_SHA1_BLOCK_SIZE) {
		error = wlan_sha1(key, key_length, key_digest);
		if (error != 0)
			goto fail;
		key_bytes = key_digest;
		normalized_length = sizeof(key_digest);
	}
	if (normalized_length != 0U)
		memcpy(key_block, key_bytes, normalized_length);

	/* Derives the inner and outer pads and starts both hashes. */
	for (index = 0U; index < WLAN_SHA1_BLOCK_SIZE; index++) {
		inner_pad[index] = (uint8_t)(key_block[index] ^ 0x36U);
		outer_pad[index] = (uint8_t)(key_block[index] ^ 0x5cU);
	}
	context->initialized = 0;
	context->failed = 0;
	error = wlan_sha1_init(&context->inner);
	if (error == 0)
		error = wlan_sha1_update(&context->inner, inner_pad,
		    sizeof(inner_pad));
	if (error == 0)
		error = wlan_sha1_init(&context->outer);
	if (error == 0)
		error = wlan_sha1_update(&context->outer, outer_pad,
		    sizeof(outer_pad));
	if (error != 0)
		goto fail;
	context->initialized = 1;

	/* Erases the key material. */
	wlan_crypto_erase(key_block, sizeof(key_block));
	wlan_crypto_erase(key_digest, sizeof(key_digest));
	wlan_crypto_erase(inner_pad, sizeof(inner_pad));
	wlan_crypto_erase(outer_pad, sizeof(outer_pad));

	/* Reports the started computation. */
	return 0;

fail:
	wlan_crypto_erase(key_block, sizeof(key_block));
	wlan_crypto_erase(key_digest, sizeof(key_digest));
	wlan_crypto_erase(inner_pad, sizeof(inner_pad));
	wlan_crypto_erase(outer_pad, sizeof(outer_pad));
	wlan_crypto_erase(context, sizeof(*context));

	/* Reports the failure. */
	return error;
}

/*
 * Feeds data into an HMAC-SHA1 computation.
 */
int
wlan_hmac_sha1_update(
	struct wlan_hmac_sha1_context *context,
	const void *data,
	size_t length)
{
	int error;

	/* Rejects a missing or unusable context, or missing data. */
	if (context == NULL || (data == NULL && length != 0U))
		return EINVAL;
	if (!context->initialized || context->failed)
		return EINVAL;
	if (ranges_overlap(context, sizeof(*context), data, length)) {
		context->failed = 1;
		return EINVAL;
	}

	/* Feeds the inner hash. */
	error = wlan_sha1_update(&context->inner, data, length);
	if (error != 0)
		context->failed = 1;

	/* Reports the update result. */
	return error;
}

/*
 * Finishes an HMAC-SHA1 computation and erases the context.
 */
int
wlan_hmac_sha1_final(
	struct wlan_hmac_sha1_context *context,
	uint8_t digest[WLAN_SHA1_DIGEST_SIZE])
{
	uint8_t inner_digest[WLAN_SHA1_DIGEST_SIZE];
	int error;

	/* Rejects a missing context or digest, an overlap, or an unusable context. */
	if (context == NULL || digest == NULL)
		return EINVAL;
	if (ranges_overlap(context, sizeof(*context), digest,
	    WLAN_SHA1_DIGEST_SIZE)) {
		context->failed = 1;
		return EINVAL;
	}
	if (!context->initialized || context->failed) {
		wlan_crypto_erase(context, sizeof(*context));
		return EINVAL;
	}

	/* Hashes the inner digest under the outer pad. */
	error = wlan_sha1_final(&context->inner, inner_digest);
	if (error == 0)
		error = wlan_sha1_update(&context->outer, inner_digest,
		    sizeof(inner_digest));
	if (error == 0)
		error = wlan_sha1_final(&context->outer, digest);
	wlan_crypto_erase(inner_digest, sizeof(inner_digest));
	wlan_crypto_erase(context, sizeof(*context));

	/* Reports the finished digest. */
	return error;
}

/*
 * Computes the HMAC-SHA1 of a buffer under a key.
 */
int
wlan_hmac_sha1(
	const void *key,
	size_t key_length,
	const void *data,
	size_t data_length,
	uint8_t digest[WLAN_SHA1_DIGEST_SIZE])
{
	struct wlan_hmac_sha1_context context;
	int error;

#if SIZE_MAX > UINT64_MAX / 8U
	/* Lengths the bit counter cannot hold are refused. */
	if ((uint64_t)key_length > SHA1_LENGTH_MAX_BYTES ||
	    (uint64_t)data_length >
	    SHA1_LENGTH_MAX_BYTES - WLAN_SHA1_BLOCK_SIZE)
		return EOVERFLOW;
#endif

	/* Rejects a missing operand or a digest inside the inputs. */
	if ((key == NULL && key_length != 0U) ||
	    (data == NULL && data_length != 0U) ||
	    digest == NULL ||
	    ranges_overlap(digest, WLAN_SHA1_DIGEST_SIZE, key, key_length) ||
	    ranges_overlap(digest, WLAN_SHA1_DIGEST_SIZE, data, data_length))
		return EINVAL;

	/* Runs the three phases, erasing the context on failure. */
	error = wlan_hmac_sha1_init(&context, key, key_length);
	if (error == 0)
		error = wlan_hmac_sha1_update(&context, data, data_length);
	if (error == 0)
		error = wlan_hmac_sha1_final(&context, digest);
	else
		wlan_crypto_erase(&context, sizeof(context));

	/* Reports the digest result. */
	return error;
}

/*
 * Derives a key from a password with PBKDF2-HMAC-SHA1.
 *
 * On failure whatever was written to the output is erased.
 */
int
wlan_pbkdf2_hmac_sha1(
	const void *password,
	size_t password_length,
	const void *salt,
	size_t salt_length,
	uint32_t iterations,
	uint8_t *output,
	size_t output_length)
{
	struct wlan_hmac_sha1_context hmac;
	uint8_t accumulated[WLAN_SHA1_DIGEST_SIZE];
	uint8_t current[WLAN_SHA1_DIGEST_SIZE];
	uint8_t next[WLAN_SHA1_DIGEST_SIZE];
	uint8_t counter_bytes[4];
	size_t block_count;
	size_t block;
	size_t amount;
	size_t offset;
	uint32_t iteration;
	uint32_t counter;
	unsigned index;
	int error;

	/* Rejects a missing operand or no iterations. */
	if ((password == NULL && password_length != 0U) ||
	    (salt == NULL && salt_length != 0U) ||
	    iterations == 0U ||
	    (output == NULL && output_length != 0U))
		return EINVAL;
#if SIZE_MAX > UINT64_MAX / 8U
	if ((uint64_t)password_length > SHA1_LENGTH_MAX_BYTES ||
	    (uint64_t)salt_length > SHA1_LENGTH_MAX_BYTES -
	    WLAN_SHA1_BLOCK_SIZE - 4U)
		return EOVERFLOW;
#endif

	/* Sizes the output in digest blocks; the block counter is 32 bits. */
	block_count = output_length / WLAN_SHA1_DIGEST_SIZE;
	if (output_length % WLAN_SHA1_DIGEST_SIZE != 0U)
		block_count++;
	if (block_count > UINT32_MAX)
		return EOVERFLOW;
	if (ranges_overlap(output, output_length, password, password_length) ||
	    ranges_overlap(output, output_length, salt, salt_length))
		return EINVAL;

	/* Derives each output block by iterated HMAC over the salt and counter. */
	offset = 0U;
	error = 0;
	for (block = 0U; block < block_count; block++) {
		counter = (uint32_t)block + 1U;
		counter_bytes[0] = (uint8_t)(counter >> 24);
		counter_bytes[1] = (uint8_t)(counter >> 16);
		counter_bytes[2] = (uint8_t)(counter >> 8);
		counter_bytes[3] = (uint8_t)counter;
		error = wlan_hmac_sha1_init(&hmac, password, password_length);
		if (error == 0)
			error = wlan_hmac_sha1_update(&hmac, salt, salt_length);
		if (error == 0)
			error = wlan_hmac_sha1_update(&hmac, counter_bytes,
			    sizeof(counter_bytes));
		if (error == 0)
			error = wlan_hmac_sha1_final(&hmac, current);
		if (error != 0)
			break;
		memcpy(accumulated, current, sizeof(accumulated));
		for (iteration = 1U; iteration < iterations; iteration++) {
			error = wlan_hmac_sha1(password, password_length, current,
			    sizeof(current), next);
			if (error != 0)
				break;
			memcpy(current, next, sizeof(current));
			for (index = 0U; index < WLAN_SHA1_DIGEST_SIZE; index++)
				accumulated[index] ^= current[index];
		}
		if (error != 0)
			break;
		amount = output_length - offset;
		if (amount > WLAN_SHA1_DIGEST_SIZE)
			amount = WLAN_SHA1_DIGEST_SIZE;
		memcpy(output + offset, accumulated, amount);
		offset += amount;
	}

	/* Erases a partial result and every intermediate. */
	if (error != 0 && output != NULL)
		wlan_crypto_erase(output, offset);
	wlan_crypto_erase(&hmac, sizeof(hmac));
	wlan_crypto_erase(accumulated, sizeof(accumulated));
	wlan_crypto_erase(current, sizeof(current));
	wlan_crypto_erase(next, sizeof(next));
	wlan_crypto_erase(counter_bytes, sizeof(counter_bytes));

	/* Reports the derivation result. */
	return error;
}

/*
 * Expands a key with the WPA pseudo-random function PRF-n.
 *
 * Each block is HMAC-SHA1 over the label, a zero byte, the data, and a
 * block counter.  On failure whatever was written is erased.
 */
int
wlan_crypto_prf_sha1(
	const void *key,
	size_t key_length,
	const void *label,
	size_t label_length,
	const void *data,
	size_t data_length,
	uint8_t *output,
	size_t output_length)
{
	struct wlan_hmac_sha1_context hmac;
	uint8_t digest[WLAN_SHA1_DIGEST_SIZE];
	uint8_t separator;
	uint8_t counter;
	size_t block_count;
	size_t block;
	size_t offset;
	size_t amount;
	int error;

	/* Rejects an output beyond the one-byte counter or a missing operand. */
	if (output_length > WPA_PRF_OUTPUT_MAX)
		return EOVERFLOW;
	if ((key == NULL && key_length != 0U) ||
	    (label == NULL && label_length != 0U) ||
	    (data == NULL && data_length != 0U) ||
	    (output == NULL && output_length != 0U))
		return EINVAL;
	if (label_length > SIZE_MAX - data_length)
		return EOVERFLOW;
#if SIZE_MAX > UINT64_MAX / 8U
	if ((uint64_t)key_length > SHA1_LENGTH_MAX_BYTES ||
	    (uint64_t)label_length > SHA1_LENGTH_MAX_BYTES -
	    WLAN_SHA1_BLOCK_SIZE - 2U ||
	    (uint64_t)data_length > SHA1_LENGTH_MAX_BYTES -
	    WLAN_SHA1_BLOCK_SIZE - 2U - (uint64_t)label_length)
		return EOVERFLOW;
#endif
	if (ranges_overlap(output, output_length, key, key_length) ||
	    ranges_overlap(output, output_length, label, label_length) ||
	    ranges_overlap(output, output_length, data, data_length))
		return EINVAL;

	/* Derives each output block with its counter. */
	block_count = output_length / WLAN_SHA1_DIGEST_SIZE;
	if (output_length % WLAN_SHA1_DIGEST_SIZE != 0U)
		block_count++;
	separator = 0U;
	offset = 0U;
	error = 0;
	for (block = 0U; block < block_count; block++) {
		counter = (uint8_t)block;
		error = wlan_hmac_sha1_init(&hmac, key, key_length);
		if (error == 0)
			error = wlan_hmac_sha1_update(&hmac, label, label_length);
		if (error == 0)
			error = wlan_hmac_sha1_update(&hmac, &separator, 1U);
		if (error == 0)
			error = wlan_hmac_sha1_update(&hmac, data, data_length);
		if (error == 0)
			error = wlan_hmac_sha1_update(&hmac, &counter, 1U);
		if (error == 0)
			error = wlan_hmac_sha1_final(&hmac, digest);
		if (error != 0)
			break;
		amount = output_length - offset;
		if (amount > WLAN_SHA1_DIGEST_SIZE)
			amount = WLAN_SHA1_DIGEST_SIZE;
		memcpy(output + offset, digest, amount);
		offset += amount;
	}

	/* Erases a partial result and the intermediates. */
	if (error != 0 && output != NULL)
		wlan_crypto_erase(output, offset);
	wlan_crypto_erase(&hmac, sizeof(hmac));
	wlan_crypto_erase(digest, sizeof(digest));

	/* Reports the expansion result. */
	return error;
}

/*
 * Encrypts one block with AES-128.
 */
int
wlan_aes128_encrypt_block(
	const uint8_t key[WLAN_AES128_KEY_SIZE],
	const uint8_t input[WLAN_AES128_BLOCK_SIZE],
	uint8_t output[WLAN_AES128_BLOCK_SIZE])
{
	uint8_t expanded[176];

	/* Rejects a missing operand or an output inside the inputs. */
	if (key == NULL ||
	    input == NULL ||
	    output == NULL ||
	    ranges_overlap(output, WLAN_AES128_BLOCK_SIZE, key,
	    WLAN_AES128_KEY_SIZE) ||
	    ranges_overlap(output, WLAN_AES128_BLOCK_SIZE, input,
	    WLAN_AES128_BLOCK_SIZE))
		return EINVAL;

	/* Expands the key, encrypts, and erases the schedule. */
	aes128_expand_key(key, expanded);
	aes128_encrypt_expanded(expanded, input, output);
	wlan_crypto_erase(expanded, sizeof(expanded));

	/* Reports the encrypted block. */
	return 0;
}

/*
 * Unwraps a key wrapped with AES-128 under RFC 3394.
 *
 * The integrity check value must match the standard initial value; a
 * mismatch erases the output and reports EACCES.
 */
int
wlan_rfc3394_unwrap(
	const uint8_t kek[WLAN_AES128_KEY_SIZE],
	const uint8_t *wrapped,
	size_t wrapped_length,
	uint8_t *output,
	size_t output_capacity,
	size_t *output_length)
{
	static const uint8_t initial_value[8] = {
		0xa6U, 0xa6U, 0xa6U, 0xa6U,
		0xa6U, 0xa6U, 0xa6U, 0xa6U
	};
	uint8_t expanded[176];
	uint8_t accumulator[8];
	uint8_t block[16];
	uint8_t decrypted[16];
	size_t needed;
	size_t n;
	size_t index;
	uint64_t t;
	int round;

	/* Rejects a missing operand or a malformed wrapped length. */
	if (kek == NULL ||
	    wrapped == NULL ||
	    output == NULL ||
	    output_length == NULL)
		return EINVAL;
	if (wrapped_length > WLAN_RFC3394_WRAPPED_MAX)
		return EOVERFLOW;
	if (wrapped_length < 24U ||
	    wrapped_length % 8U != 0U)
		return EINVAL;
	needed = wrapped_length - 8U;
	if (output_capacity < needed)
		return ENOSPC;
	if (ranges_overlap(output, needed, wrapped, wrapped_length) ||
	    ranges_overlap(output, needed, kek, WLAN_AES128_KEY_SIZE) ||
	    ranges_overlap(output_length, sizeof(*output_length), output, needed) ||
	    ranges_overlap(output_length, sizeof(*output_length), wrapped,
	    wrapped_length) ||
	    ranges_overlap(output_length, sizeof(*output_length), kek,
	    WLAN_AES128_KEY_SIZE))
		return EINVAL;

	/* Runs the six unwrapping rounds over the 64-bit registers. */
	*output_length = 0U;
	memcpy(accumulator, wrapped, sizeof(accumulator));
	memcpy(output, wrapped + 8U, needed);
	n = needed / 8U;
	aes128_expand_key(kek, expanded);
	for (round = 5; round >= 0; round--) {
		for (index = n; index > 0U; index--) {
			t = (uint64_t)n * (uint64_t)round + (uint64_t)index;
			memcpy(block, accumulator, 8U);
			block[7] ^= (uint8_t)t;
			block[6] ^= (uint8_t)(t >> 8);
			block[5] ^= (uint8_t)(t >> 16);
			block[4] ^= (uint8_t)(t >> 24);
			block[3] ^= (uint8_t)(t >> 32);
			block[2] ^= (uint8_t)(t >> 40);
			block[1] ^= (uint8_t)(t >> 48);
			block[0] ^= (uint8_t)(t >> 56);
			memcpy(block + 8U, output + (index - 1U) * 8U, 8U);
			aes128_decrypt_expanded(expanded, block, decrypted);
			memcpy(accumulator, decrypted, 8U);
			memcpy(output + (index - 1U) * 8U, decrypted + 8U, 8U);
		}
	}
	wlan_crypto_erase(expanded, sizeof(expanded));
	wlan_crypto_erase(block, sizeof(block));
	wlan_crypto_erase(decrypted, sizeof(decrypted));

	/* The integrity check value must be the standard one. */
	if (!wlan_crypto_equal(accumulator, initial_value,
	    sizeof(initial_value))) {
		wlan_crypto_erase(accumulator, sizeof(accumulator));
		wlan_crypto_erase(output, needed);
		return EACCES;
	}
	wlan_crypto_erase(accumulator, sizeof(accumulator));
	*output_length = needed;

	/* Reports the unwrapped key. */
	return 0;
}

/* Tests whether two byte ranges overlap, treating address wraparound as overlap. */
static int
ranges_overlap(
	const void *left,
	size_t left_length,
	const void *right,
	size_t right_length)
{
	uintptr_t left_start;
	uintptr_t left_end;
	uintptr_t right_start;
	uintptr_t right_end;

	/* An empty or missing range overlaps nothing. */
	if (left_length == 0U || right_length == 0U)
		return 0;
	if (left == NULL || right == NULL)
		return 0;

	/* A range that wraps the address space is treated as overlapping. */
	left_start = (uintptr_t)left;
	right_start = (uintptr_t)right;
	if (left_start > UINTPTR_MAX - (left_length - 1U) ||
	    right_start > UINTPTR_MAX - (right_length - 1U))
		return 1;

	/* Each range must start before the other ends. */
	left_end = left_start + left_length - 1U;
	right_end = right_start + right_length - 1U;
	if (left_start > right_end)
		return 0;
	if (right_start > left_end)
		return 0;

	/* Reports an overlap. */
	return 1;
}

/* Rotates a 32-bit word left. */
static uint32_t
rotate_left32(
	uint32_t value,
	unsigned amount)
{
	return (value << amount) | (value >> (32U - amount));
}

/* Reads a big-endian 32-bit word. */
static uint32_t
read_be32(
	const uint8_t *bytes)
{
	return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) |
	    ((uint32_t)bytes[2] << 8) | (uint32_t)bytes[3];
}

/* Writes a big-endian 32-bit word. */
static void
write_be32(
	uint8_t *bytes,
	uint32_t value)
{
	bytes[0] = (uint8_t)(value >> 24);
	bytes[1] = (uint8_t)(value >> 16);
	bytes[2] = (uint8_t)(value >> 8);
	bytes[3] = (uint8_t)value;
}

/* Absorbs one 64-byte block into a SHA-1 state. */
static void
sha1_transform(
	struct wlan_sha1_context *context,
	const uint8_t block[64])
{
	uint32_t words[80];
	uint32_t a;
	uint32_t b;
	uint32_t c;
	uint32_t d;
	uint32_t e;
	uint32_t f;
	uint32_t constant;
	uint32_t temporary;
	unsigned index;

	/* Expands the block into the eighty message words. */
	for (index = 0U; index < 16U; index++)
		words[index] = read_be32(block + index * 4U);
	for (; index < 80U; index++)
		words[index] = rotate_left32(words[index - 3U] ^
		    words[index - 8U] ^ words[index - 14U] ^
		    words[index - 16U], 1U);

	/* Runs the eighty rounds in their four groups. */
	a = context->state[0];
	b = context->state[1];
	c = context->state[2];
	d = context->state[3];
	e = context->state[4];
	for (index = 0U; index < 80U; index++) {
		if (index < 20U) {
			f = (b & c) | ((~b) & d);
			constant = 0x5a827999U;
		} else if (index < 40U) {
			f = b ^ c ^ d;
			constant = 0x6ed9eba1U;
		} else if (index < 60U) {
			f = (b & c) | (b & d) | (c & d);
			constant = 0x8f1bbcdcU;
		} else {
			f = b ^ c ^ d;
			constant = 0xca62c1d6U;
		}
		temporary = rotate_left32(a, 5U) + f + e + constant +
		    words[index];
		e = d;
		d = c;
		c = rotate_left32(b, 30U);
		b = a;
		a = temporary;
	}

	/* Adds the round result into the state and erases the schedule. */
	context->state[0] += a;
	context->state[1] += b;
	context->state[2] += c;
	context->state[3] += d;
	context->state[4] += e;
	wlan_crypto_erase(words, sizeof(words));
}

/* Multiplies two elements of the AES field in constant time. */
static uint8_t
aes_gf_multiply(
	uint8_t left,
	uint8_t right)
{
	uint8_t result;
	uint8_t low_mask;
	uint8_t high_mask;
	unsigned index;

	/* Shift-and-add with masks instead of branches. */
	result = 0U;
	for (index = 0U; index < 8U; index++) {
		low_mask = (uint8_t)(0U - (uint8_t)(right & 1U));
		result ^= left & low_mask;
		high_mask = (uint8_t)(0U - (uint8_t)(left >> 7));
		left = (uint8_t)(left << 1) ^ (uint8_t)(0x1bU & high_mask);
		right >>= 1;
	}

	/* Reports the product. */
	return result;
}

/* Inverts an element of the AES field by exponentiation. */
static uint8_t
aes_gf_inverse(
	uint8_t value)
{
	uint8_t result;
	uint8_t base;
	unsigned exponent;

	/* Raises to the 254th power by square and multiply. */
	result = 1U;
	base = value;
	exponent = 254U;
	while (exponent != 0U) {
		if ((exponent & 1U) != 0U)
			result = aes_gf_multiply(result, base);
		base = aes_gf_multiply(base, base);
		exponent >>= 1;
	}

	/* Reports the inverse, which is zero for zero. */
	return result;
}

/* Rotates a byte left. */
static uint8_t
rotate_left8(
	uint8_t value,
	unsigned amount)
{
	return (uint8_t)((uint8_t)(value << amount) |
	    (uint8_t)(value >> (8U - amount)));
}

/* Computes the AES S-box of a byte: inversion followed by the affine map. */
static uint8_t
aes_sbox(
	uint8_t value)
{
	uint8_t inverse;

	inverse = aes_gf_inverse(value);
	return (uint8_t)(inverse ^ rotate_left8(inverse, 1U) ^
	    rotate_left8(inverse, 2U) ^ rotate_left8(inverse, 3U) ^
	    rotate_left8(inverse, 4U) ^ 0x63U);
}

/* Computes the inverse AES S-box of a byte. */
static uint8_t
aes_inverse_sbox(
	uint8_t value)
{
	uint8_t affine_inverse;
	uint8_t result;

	/* Undoes the affine map, then inverts. */
	affine_inverse = (uint8_t)(rotate_left8(value, 1U) ^
	    rotate_left8(value, 3U) ^ rotate_left8(value, 6U) ^ 0x05U);
	result = aes_gf_inverse(affine_inverse);

	/* Reports the inverse S-box value. */
	return result;
}

/* Expands an AES-128 key into the eleven round keys. */
static void
aes128_expand_key(
	const uint8_t key[16],
	uint8_t expanded[176])
{
	static const uint8_t round_constants[10] = {
		0x01U, 0x02U, 0x04U, 0x08U, 0x10U,
		0x20U, 0x40U, 0x80U, 0x1bU, 0x36U
	};
	uint8_t temporary[4];
	size_t generated;
	unsigned rcon_index;
	unsigned index;
	uint8_t first;

	/* Derives each word from the previous one and the word 16 bytes back. */
	memcpy(expanded, key, 16U);
	generated = 16U;
	rcon_index = 0U;
	while (generated < 176U) {
		for (index = 0U; index < 4U; index++)
			temporary[index] = expanded[generated - 4U + index];

		/* Every fourth word is rotated, substituted, and mixed with a constant. */
		if (generated % 16U == 0U) {
			first = temporary[0];
			temporary[0] = aes_sbox(temporary[1]);
			temporary[1] = aes_sbox(temporary[2]);
			temporary[2] = aes_sbox(temporary[3]);
			temporary[3] = aes_sbox(first);
			temporary[0] ^= round_constants[rcon_index++];
		}
		for (index = 0U; index < 4U; index++) {
			expanded[generated] = (uint8_t)(expanded[generated - 16U] ^
			    temporary[index]);
			generated++;
		}
	}
	wlan_crypto_erase(temporary, sizeof(temporary));
}

/* Adds a round key into the state. */
static void
aes_add_round_key(
	uint8_t state[16],
	const uint8_t *round_key)
{
	unsigned index;

	for (index = 0U; index < 16U; index++)
		state[index] ^= round_key[index];
}

/* Substitutes every state byte through the S-box. */
static void
aes_sub_bytes(
	uint8_t state[16])
{
	unsigned index;

	for (index = 0U; index < 16U; index++)
		state[index] = aes_sbox(state[index]);
}

/* Substitutes every state byte through the inverse S-box. */
static void
aes_inverse_sub_bytes(
	uint8_t state[16])
{
	unsigned index;

	for (index = 0U; index < 16U; index++)
		state[index] = aes_inverse_sbox(state[index]);
}

/* Rotates the state rows left by their row number. */
static void
aes_shift_rows(
	uint8_t state[16])
{
	uint8_t temporary[16];

	temporary[0] = state[0];
	temporary[1] = state[5];
	temporary[2] = state[10];
	temporary[3] = state[15];
	temporary[4] = state[4];
	temporary[5] = state[9];
	temporary[6] = state[14];
	temporary[7] = state[3];
	temporary[8] = state[8];
	temporary[9] = state[13];
	temporary[10] = state[2];
	temporary[11] = state[7];
	temporary[12] = state[12];
	temporary[13] = state[1];
	temporary[14] = state[6];
	temporary[15] = state[11];
	memcpy(state, temporary, sizeof(temporary));
	wlan_crypto_erase(temporary, sizeof(temporary));
}

/* Rotates the state rows right by their row number. */
static void
aes_inverse_shift_rows(
	uint8_t state[16])
{
	uint8_t temporary[16];

	temporary[0] = state[0];
	temporary[1] = state[13];
	temporary[2] = state[10];
	temporary[3] = state[7];
	temporary[4] = state[4];
	temporary[5] = state[1];
	temporary[6] = state[14];
	temporary[7] = state[11];
	temporary[8] = state[8];
	temporary[9] = state[5];
	temporary[10] = state[2];
	temporary[11] = state[15];
	temporary[12] = state[12];
	temporary[13] = state[9];
	temporary[14] = state[6];
	temporary[15] = state[3];
	memcpy(state, temporary, sizeof(temporary));
	wlan_crypto_erase(temporary, sizeof(temporary));
}

/* Mixes each state column with the AES polynomial. */
static void
aes_mix_columns(
	uint8_t state[16])
{
	uint8_t a;
	uint8_t b;
	uint8_t c;
	uint8_t d;
	unsigned column;

	for (column = 0U; column < 4U; column++) {
		a = state[column * 4U];
		b = state[column * 4U + 1U];
		c = state[column * 4U + 2U];
		d = state[column * 4U + 3U];
		state[column * 4U] = (uint8_t)(aes_gf_multiply(a, 2U) ^
		    aes_gf_multiply(b, 3U) ^ c ^ d);
		state[column * 4U + 1U] = (uint8_t)(a ^
		    aes_gf_multiply(b, 2U) ^ aes_gf_multiply(c, 3U) ^ d);
		state[column * 4U + 2U] = (uint8_t)(a ^ b ^
		    aes_gf_multiply(c, 2U) ^ aes_gf_multiply(d, 3U));
		state[column * 4U + 3U] = (uint8_t)(aes_gf_multiply(a, 3U) ^
		    b ^ c ^ aes_gf_multiply(d, 2U));
	}
}

/* Unmixes each state column with the inverse AES polynomial. */
static void
aes_inverse_mix_columns(
	uint8_t state[16])
{
	uint8_t a;
	uint8_t b;
	uint8_t c;
	uint8_t d;
	unsigned column;

	for (column = 0U; column < 4U; column++) {
		a = state[column * 4U];
		b = state[column * 4U + 1U];
		c = state[column * 4U + 2U];
		d = state[column * 4U + 3U];
		state[column * 4U] = (uint8_t)(aes_gf_multiply(a, 14U) ^
		    aes_gf_multiply(b, 11U) ^ aes_gf_multiply(c, 13U) ^
		    aes_gf_multiply(d, 9U));
		state[column * 4U + 1U] = (uint8_t)(aes_gf_multiply(a, 9U) ^
		    aes_gf_multiply(b, 14U) ^ aes_gf_multiply(c, 11U) ^
		    aes_gf_multiply(d, 13U));
		state[column * 4U + 2U] = (uint8_t)(aes_gf_multiply(a, 13U) ^
		    aes_gf_multiply(b, 9U) ^ aes_gf_multiply(c, 14U) ^
		    aes_gf_multiply(d, 11U));
		state[column * 4U + 3U] = (uint8_t)(aes_gf_multiply(a, 11U) ^
		    aes_gf_multiply(b, 13U) ^ aes_gf_multiply(c, 9U) ^
		    aes_gf_multiply(d, 14U));
	}
}

/* Encrypts one block with an expanded AES-128 key. */
static void
aes128_encrypt_expanded(
	const uint8_t expanded[176],
	const uint8_t input[16],
	uint8_t output[16])
{
	uint8_t state[16];
	unsigned round;

	/* Runs the initial key addition, nine full rounds, and the final round. */
	memcpy(state, input, sizeof(state));
	aes_add_round_key(state, expanded);
	for (round = 1U; round < 10U; round++) {
		aes_sub_bytes(state);
		aes_shift_rows(state);
		aes_mix_columns(state);
		aes_add_round_key(state, expanded + round * 16U);
	}
	aes_sub_bytes(state);
	aes_shift_rows(state);
	aes_add_round_key(state, expanded + 160U);
	memcpy(output, state, sizeof(state));
	wlan_crypto_erase(state, sizeof(state));
}

/* Decrypts one block with an expanded AES-128 key. */
static void
aes128_decrypt_expanded(
	const uint8_t expanded[176],
	const uint8_t input[16],
	uint8_t output[16])
{
	uint8_t state[16];
	unsigned round;

	/* Runs the rounds of encryption in reverse with the inverse steps. */
	memcpy(state, input, sizeof(state));
	aes_add_round_key(state, expanded + 160U);
	for (round = 9U; round > 0U; round--) {
		aes_inverse_shift_rows(state);
		aes_inverse_sub_bytes(state);
		aes_add_round_key(state, expanded + round * 16U);
		aes_inverse_mix_columns(state);
	}
	aes_inverse_shift_rows(state);
	aes_inverse_sub_bytes(state);
	aes_add_round_key(state, expanded);
	memcpy(output, state, sizeof(state));
	wlan_crypto_erase(state, sizeof(state));
}
