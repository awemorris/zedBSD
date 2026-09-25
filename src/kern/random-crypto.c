/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * ChaCha20 (RFC 8439) and BLAKE2s-256 (RFC 7693) for the kernel random
 * number generator.
 */

#include "kern/random-crypto.h"

static uint32_t rotate_left(uint32_t value, unsigned count);
static uint32_t rotate_right(uint32_t value, unsigned count);
static uint32_t load32(const uint8_t *bytes);
static void store32(uint8_t *bytes, uint32_t value);
static void blake2s_compress(struct blake2s_state *state, int last);

/* The first 32 bits of the fractional parts of the square roots of 2..19. */
static const uint32_t blake2s_iv[8] = {
	0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
	0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U
};

/* The message word order of each of the ten rounds. */
static const uint8_t blake2s_sigma[10][16] = {
	{ 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 },
	{ 14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3 },
	{ 11, 8, 12, 0, 5, 2, 15, 13, 10, 14, 3, 6, 7, 1, 9, 4 },
	{ 7, 9, 3, 1, 13, 12, 11, 14, 2, 6, 5, 10, 4, 0, 15, 8 },
	{ 9, 0, 5, 7, 2, 4, 10, 15, 14, 1, 11, 12, 6, 8, 3, 13 },
	{ 2, 12, 6, 10, 0, 11, 8, 3, 4, 13, 7, 5, 15, 14, 1, 9 },
	{ 12, 5, 1, 15, 14, 13, 4, 10, 0, 7, 6, 3, 9, 2, 8, 11 },
	{ 13, 11, 7, 14, 12, 1, 3, 9, 5, 0, 15, 4, 8, 6, 2, 10 },
	{ 6, 15, 14, 9, 11, 3, 0, 8, 12, 2, 13, 7, 1, 4, 10, 5 },
	{ 10, 2, 8, 4, 7, 6, 1, 5, 15, 11, 9, 14, 3, 12, 13, 0 }
};

/* The ChaCha quarter round on four words of the state. */
#define CHACHA_QUARTER(x, a, b, c, d) do { \
	(x)[a] += (x)[b]; (x)[d] = rotate_left((x)[d] ^ (x)[a], 16U); \
	(x)[c] += (x)[d]; (x)[b] = rotate_left((x)[b] ^ (x)[c], 12U); \
	(x)[a] += (x)[b]; (x)[d] = rotate_left((x)[d] ^ (x)[a], 8U); \
	(x)[c] += (x)[d]; (x)[b] = rotate_left((x)[b] ^ (x)[c], 7U); \
} while (0)

/* The BLAKE2s mixing function on four words of the work vector. */
#define BLAKE2S_G(v, a, b, c, d, x, y) do { \
	(v)[a] = (v)[a] + (v)[b] + (x); \
	(v)[d] = rotate_right((v)[d] ^ (v)[a], 16U); \
	(v)[c] = (v)[c] + (v)[d]; \
	(v)[b] = rotate_right((v)[b] ^ (v)[c], 12U); \
	(v)[a] = (v)[a] + (v)[b] + (y); \
	(v)[d] = rotate_right((v)[d] ^ (v)[a], 8U); \
	(v)[c] = (v)[c] + (v)[d]; \
	(v)[b] = rotate_right((v)[b] ^ (v)[c], 7U); \
} while (0)

/*
 * Writes one ChaCha20 block.
 */
void
chacha20_block(
	const uint8_t key[CHACHA20_KEY_BYTES],
	uint32_t counter,
	const uint8_t nonce[12],
	uint8_t out[CHACHA20_BLOCK_BYTES])
{
	uint32_t state[16];
	uint32_t work[16];
	unsigned index;

	/* "expand 32-byte k", the key, the counter and the nonce. */
	state[0] = 0x61707865U;
	state[1] = 0x3320646eU;
	state[2] = 0x79622d32U;
	state[3] = 0x6b206574U;
	for (index = 0; index < 8U; index++)
		state[4U + index] = load32(key + 4U * index);
	state[12] = counter;
	for (index = 0; index < 3U; index++)
		state[13U + index] = load32(nonce + 4U * index);

	/* Twenty rounds: ten of a column and a diagonal round each. */
	for (index = 0; index < 16U; index++)
		work[index] = state[index];
	for (index = 0; index < 10U; index++) {
		CHACHA_QUARTER(work, 0, 4, 8, 12);
		CHACHA_QUARTER(work, 1, 5, 9, 13);
		CHACHA_QUARTER(work, 2, 6, 10, 14);
		CHACHA_QUARTER(work, 3, 7, 11, 15);
		CHACHA_QUARTER(work, 0, 5, 10, 15);
		CHACHA_QUARTER(work, 1, 6, 11, 12);
		CHACHA_QUARTER(work, 2, 7, 8, 13);
		CHACHA_QUARTER(work, 3, 4, 9, 14);
	}

	/* The block is the worked state added to the input state. */
	for (index = 0; index < 16U; index++)
		store32(out + 4U * index, work[index] + state[index]);
	for (index = 0; index < 16U; index++) {
		work[index] = 0;
		state[index] = 0;
	}
}

/*
 * Starts an unkeyed BLAKE2s-256 hash.
 */
void
blake2s_init(
	struct blake2s_state *state)
{
	unsigned index;

	for (index = 0; index < 8U; index++)
		state->h[index] = blake2s_iv[index];

	/* Parameter block: digest length 32, no key, fanout 1, depth 1. */
	state->h[0] ^= 0x01010000U ^ BLAKE2S_DIGEST_BYTES;
	state->t[0] = 0;
	state->t[1] = 0;
	state->used = 0;
}

/*
 * Adds bytes to a hash in progress.
 */
void
blake2s_update(
	struct blake2s_state *state,
	const void *data,
	size_t length)
{
	const uint8_t *bytes;
	size_t room;

	bytes = data;
	while (length != 0) {
		/* A full buffer is compressed only once more input follows. */
		if (state->used == BLAKE2S_BLOCK_BYTES) {
			state->t[0] += BLAKE2S_BLOCK_BYTES;
			if (state->t[0] < BLAKE2S_BLOCK_BYTES)
				state->t[1]++;
			blake2s_compress(state, 0);
			state->used = 0;
		}
		room = BLAKE2S_BLOCK_BYTES - state->used;
		if (room > length)
			room = length;
		for (size_t index = 0; index < room; index++)
			state->buffer[state->used + index] = bytes[index];
		state->used += room;
		bytes += room;
		length -= room;
	}
}

/*
 * Finishes a hash.
 */
void
blake2s_final(
	struct blake2s_state *state,
	uint8_t digest[BLAKE2S_DIGEST_BYTES])
{
	unsigned index;

	/* The last block counts only its bytes and is padded with zeros. */
	state->t[0] += (uint32_t)state->used;
	if (state->t[0] < (uint32_t)state->used)
		state->t[1]++;
	for (index = (unsigned)state->used; index < BLAKE2S_BLOCK_BYTES; index++)
		state->buffer[index] = 0;
	blake2s_compress(state, 1);
	for (index = 0; index < 8U; index++)
		store32(digest + 4U * index, state->h[index]);
	for (index = 0; index < BLAKE2S_BLOCK_BYTES; index++)
		state->buffer[index] = 0;
}

/* Compresses the buffered block into the chaining value. */
static void
blake2s_compress(
	struct blake2s_state *state,
	int last)
{
	uint32_t message[16];
	uint32_t v[16];
	const uint8_t *s;
	unsigned round;
	unsigned index;

	for (index = 0; index < 16U; index++)
		message[index] = load32(state->buffer + 4U * index);
	for (index = 0; index < 8U; index++) {
		v[index] = state->h[index];
		v[8U + index] = blake2s_iv[index];
	}
	v[12] ^= state->t[0];
	v[13] ^= state->t[1];
	if (last)
		v[14] = ~v[14];

	for (round = 0; round < 10U; round++) {
		s = blake2s_sigma[round];
		BLAKE2S_G(v, 0, 4, 8, 12, message[s[0]], message[s[1]]);
		BLAKE2S_G(v, 1, 5, 9, 13, message[s[2]], message[s[3]]);
		BLAKE2S_G(v, 2, 6, 10, 14, message[s[4]], message[s[5]]);
		BLAKE2S_G(v, 3, 7, 11, 15, message[s[6]], message[s[7]]);
		BLAKE2S_G(v, 0, 5, 10, 15, message[s[8]], message[s[9]]);
		BLAKE2S_G(v, 1, 6, 11, 12, message[s[10]], message[s[11]]);
		BLAKE2S_G(v, 2, 7, 8, 13, message[s[12]], message[s[13]]);
		BLAKE2S_G(v, 3, 4, 9, 14, message[s[14]], message[s[15]]);
	}
	for (index = 0; index < 8U; index++)
		state->h[index] ^= v[index] ^ v[8U + index];
	for (index = 0; index < 16U; index++) {
		message[index] = 0;
		v[index] = 0;
	}
}

/* Rotates a word left. */
static uint32_t
rotate_left(
	uint32_t value,
	unsigned count)
{
	return (value << count) | (value >> (32U - count));
}

/* Rotates a word right. */
static uint32_t
rotate_right(
	uint32_t value,
	unsigned count)
{
	return (value >> count) | (value << (32U - count));
}

/* Reads a little-endian word. */
static uint32_t
load32(
	const uint8_t *bytes)
{
	return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
	    ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

/* Writes a little-endian word. */
static void
store32(
	uint8_t *bytes,
	uint32_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
	bytes[2] = (uint8_t)(value >> 16);
	bytes[3] = (uint8_t)(value >> 24);
}
