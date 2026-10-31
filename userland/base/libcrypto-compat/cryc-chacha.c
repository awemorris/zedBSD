/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * ChaCha20, Poly1305 and AEAD_CHACHA20_POLY1305 (RFC 8439).
 */

#include "cryc-chacha.h"

/*
 * Computes one ChaCha20 block (2.3, the block function) for key, counter
 * and nonce into out (64 bytes).
 */
void
cryc_chacha20_block(
	const uint8_t *key,
	uint32_t counter,
	const uint8_t *nonce,
	uint8_t *out)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(key);
	UNUSED_PARAMETER(counter);
	UNUSED_PARAMETER(nonce);
	UNUSED_PARAMETER(out);
}

/*
 * XORs size bytes of in with the ChaCha20 keystream starting at block
 * counter (2.4) into out.
 */
int
cryc_chacha20_xor(
	const uint8_t *key,
	uint32_t counter,
	const uint8_t *nonce,
	const uint8_t *in,
	uint8_t *out,
	size_t size)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(key);
	UNUSED_PARAMETER(counter);
	UNUSED_PARAMETER(nonce);
	UNUSED_PARAMETER(in);
	UNUSED_PARAMETER(out);
	UNUSED_PARAMETER(size);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}

/*
 * XORs size bytes with the keystream of ChaCha20 in its original form with
 * a 64-bit block counter and a 64-bit nonce: state words 12 and 13 are the
 * counter's low and high halves and words 14 and 15 the nonce read little-
 * endian (D.
 */
int
cryc_chacha20_xor64(
	const uint8_t *key,
	uint64_t counter,
	const uint8_t *nonce8,
	const uint8_t *in,
	uint8_t *out,
	size_t size)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(key);
	UNUSED_PARAMETER(counter);
	UNUSED_PARAMETER(nonce8);
	UNUSED_PARAMETER(in);
	UNUSED_PARAMETER(out);
	UNUSED_PARAMETER(size);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}

/*
 * Starts Poly1305 with the 32-byte one-time key (r then s, 2.5).
 */
void
cryc_poly1305_init(
	struct cryc_poly1305 *mac,
	const uint8_t *key)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(mac);
	UNUSED_PARAMETER(key);
}

/*
 * Absorbs size bytes (each 16-byte block with its 0x01 byte appended,
 * 2.5); data may be NULL only when size is 0.
 */
void
cryc_poly1305_update(
	struct cryc_poly1305 *mac,
	const uint8_t *data,
	size_t size)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(mac);
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(size);
}

/*
 * Absorbs the last partial block, reduces modulo 2^130 - 5, adds s, writes
 * the 16-byte tag, and wipes *mac.
 */
void
cryc_poly1305_final(
	struct cryc_poly1305 *mac,
	uint8_t *tag)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(mac);
	UNUSED_PARAMETER(tag);
}

/*
 * Encrypts and authenticates with AEAD_CHACHA20_POLY1305 (2.8).
 */
int
cryc_chacha20_poly1305_seal(
	const uint8_t *key,
	const uint8_t *nonce,
	const uint8_t *aad,
	size_t aad_size,
	const uint8_t *in,
	size_t size,
	uint8_t *out,
	uint8_t *tag)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(key);
	UNUSED_PARAMETER(nonce);
	UNUSED_PARAMETER(aad);
	UNUSED_PARAMETER(aad_size);
	UNUSED_PARAMETER(in);
	UNUSED_PARAMETER(size);
	UNUSED_PARAMETER(out);
	UNUSED_PARAMETER(tag);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}

/*
 * Checks and decrypts with AEAD_CHACHA20_POLY1305 (2.8).
 */
int
cryc_chacha20_poly1305_open(
	const uint8_t *key,
	const uint8_t *nonce,
	const uint8_t *aad,
	size_t aad_size,
	const uint8_t *in,
	size_t size,
	uint8_t *out,
	const uint8_t *tag)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(key);
	UNUSED_PARAMETER(nonce);
	UNUSED_PARAMETER(aad);
	UNUSED_PARAMETER(aad_size);
	UNUSED_PARAMETER(in);
	UNUSED_PARAMETER(size);
	UNUSED_PARAMETER(out);
	UNUSED_PARAMETER(tag);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}
