/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The primitives of the kernel random number generator: the ChaCha20 block
 * function (RFC 8439) and BLAKE2s-256 (RFC 7693).  They touch nothing but
 * their arguments, so a host test compiles them as they are.
 */

#ifndef KERN_KERN_RANDOM_CRYPTO_H
#define KERN_KERN_RANDOM_CRYPTO_H

#include <stddef.h>
#include <stdint.h>

#define CHACHA20_KEY_BYTES	32U
#define CHACHA20_BLOCK_BYTES	64U
#define BLAKE2S_BLOCK_BYTES	64U
#define BLAKE2S_DIGEST_BYTES	32U

/* Writes one 64-byte block for a key, a 32-bit counter and a 96-bit nonce. */
void
chacha20_block(
	const uint8_t key[CHACHA20_KEY_BYTES],
	uint32_t counter,
	const uint8_t nonce[12],
	uint8_t out[CHACHA20_BLOCK_BYTES]);

/* A BLAKE2s-256 hash in progress. */
struct blake2s_state {
	uint32_t h[8];
	uint32_t t[2];
	uint8_t buffer[BLAKE2S_BLOCK_BYTES];
	size_t used;
};

void
blake2s_init(
	struct blake2s_state *state);

void
blake2s_update(
	struct blake2s_state *state,
	const void *data,
	size_t length);

/* Writes the digest; the state must be initialized again before reuse. */
void
blake2s_final(
	struct blake2s_state *state,
	uint8_t digest[BLAKE2S_DIGEST_BYTES]);

#endif
