/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * ChaCha20, Poly1305 and their AEAD construction, the cipher of TLS's
 * ChaCha20-Poly1305 suites (RFC 7905 for TLS 1.2, RFC 8446 5.2 for TLS
 * 1.3).
 *
 * Constant time.  Keys, the Poly1305 accumulator and the plaintext are
 * secret.  ChaCha20 is additions, XORs and rotations of 32-bit words.
 * Poly1305 is computed in radix 2^26 limbs with 64-bit products (no
 * 128-bit type, so it works on 32-bit machines), and its final reduction
 * and the comparison of tags use masks, never branches.
 */

#ifndef LIBCRYPTO_COMPAT_CRYC_CHACHA_H
#define LIBCRYPTO_COMPAT_CRYC_CHACHA_H

#include "cryc.h"

/*
 * The section numbers in the comments of this file are those of RFC 8439
 * (June 2018).
 */

/* The sizes in bytes (2.3, 2.5, 2.8). */
#define CRYC_CHACHA20_KEY	32U
#define CRYC_CHACHA20_NONCE	12U
#define CRYC_CHACHA20_BLOCK	64U
#define CRYC_POLY1305_KEY	32U
#define CRYC_POLY1305_TAG	16U

/*
 * A Poly1305 computation in progress (2.5).
 *
 * r is the clamped r (2.5, "clamp(r)") in five 26-bit limbs, s the other
 * half of the one-time key as four little-endian words, accumulator the
 * accumulator a in five 26-bit limbs (each limb may exceed 2^26 between
 * reductions, as the implementation's carries allow).  buffer holds used
 * bytes (0 to 15) of a block not yet absorbed.  Secret; wiped by
 * cryc_poly1305_final.
 */
struct cryc_poly1305 {
	uint32_t r[5];
	uint32_t s[4];
	uint32_t accumulator[5];
	uint8_t buffer[16];
	size_t used;
};

/*
 * Computes one ChaCha20 block (2.3, the block function) for key, counter
 * and nonce into out (64 bytes).
 */
void cryc_chacha20_block(const uint8_t *key, uint32_t counter, const uint8_t *nonce, uint8_t *out);

/*
 * XORs size bytes of in with the ChaCha20 keystream starting at block
 * counter (2.4) into out.
 *
 * in and out may be the same buffer.  The keystream must not reach
 * counter 2^32 (the 32-bit block counter wrapping): size is at most
 * (2^32 - counter) * 64; a larger size returns EOVERFLOW and writes
 * nothing.  Returns 0 or EOVERFLOW.
 */
int cryc_chacha20_xor(const uint8_t *key, uint32_t counter, const uint8_t *nonce, const uint8_t *in, uint8_t *out, size_t size);

/*
 * XORs size bytes with the keystream of ChaCha20 in its original form
 * with a 64-bit block counter and a 64-bit nonce: state words 12 and 13
 * are the counter's low and high halves and words 14 and 15 the nonce
 * read little-endian (D. J. Bernstein's ChaCha; RFC 8439 2.3 describes
 * the same block function with a 32-bit counter and a 96-bit nonce in
 * those words).  The block function is the same as cryc_chacha20_block's.
 *
 * TLS does not use this form; SSH's chacha20-poly1305@openssh.com cipher
 * and EVP_chacha20's IV layout do.  in and out may be the same
 * buffer; the counter is not allowed to wrap past 2^64 - 1 (EOVERFLOW,
 * nothing written).  Returns 0 or EOVERFLOW.
 */
int cryc_chacha20_xor64(const uint8_t *key, uint64_t counter, const uint8_t *nonce8, const uint8_t *in, uint8_t *out, size_t size);

/* Starts Poly1305 with the 32-byte one-time key (r then s, 2.5). */
void cryc_poly1305_init(struct cryc_poly1305 *mac, const uint8_t *key);

/* Absorbs size bytes (each 16-byte block with its 0x01 byte appended, 2.5); data may be NULL only when size is 0. */
void cryc_poly1305_update(struct cryc_poly1305 *mac, const uint8_t *data, size_t size);

/* Absorbs the last partial block, reduces modulo 2^130 - 5, adds s, writes the 16-byte tag, and wipes *mac. */
void cryc_poly1305_final(struct cryc_poly1305 *mac, uint8_t *tag);

/*
 * Encrypts and authenticates with AEAD_CHACHA20_POLY1305 (2.8).
 *
 * key: 32 bytes; nonce: 12 bytes; aad may be NULL only when aad_size is 0;
 * in may be NULL only when size is 0; out has room for size bytes and may
 * be in; tag receives 16 bytes.  The one-time Poly1305 key is block 0 of
 * the keystream (2.6); the plaintext uses blocks from 1 (2.8).  size is at
 * most (2^32 - 1) * 64 bytes (EOVERFLOW above, nothing written).
 *
 * Returns 0 or EOVERFLOW.
 */
int cryc_chacha20_poly1305_seal(const uint8_t *key, const uint8_t *nonce, const uint8_t *aad, size_t aad_size, const uint8_t *in, size_t size, uint8_t *out, uint8_t *tag);

/*
 * Checks and decrypts with AEAD_CHACHA20_POLY1305 (2.8).
 *
 * Arguments as for the seal, with in the ciphertext and tag the 16
 * received bytes.  The tag is computed over the ciphertext and compared in
 * constant time before any plaintext is written: on a mismatch out is
 * filled with zeros and EBADMSG is returned.
 *
 * Returns 0, EBADMSG or EOVERFLOW.
 */
int cryc_chacha20_poly1305_open(const uint8_t *key, const uint8_t *nonce, const uint8_t *aad, size_t aad_size, const uint8_t *in, size_t size, uint8_t *out, const uint8_t *tag);

#endif
