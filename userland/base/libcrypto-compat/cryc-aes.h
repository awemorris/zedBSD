/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The AES block cipher (AES-128, AES-192, AES-256) in the forward
 * direction, and the counter mode GCM uses.  The inverse cipher is not
 * needed (GCM only encrypts blocks) and is not provided.
 *
 * Constant time.  The key, the round keys and every state byte are
 * secret.  The implementation has no table indexed by a secret: SubBytes
 * and InvSubBytes are computed, not looked up.  The intended form is
 * bitsliced: several blocks are held as eight 64-bit words (bit i of every
 * state byte of every block in word i), and SubBytes is evaluated as a
 * boolean circuit on the eight words.  The circuit is the S-box of 5.1.1
 * itself: the multiplicative inverse in GF(2^8) modulo m(x) of equation
 * (4.3) (4.4, with 0 mapped to 0), followed by the affine transformation
 * of equation (5.3).  The inverse may be computed as b^254 (equation
 * (4.11)) with the multiplication of 4.2 written as boolean operations on
 * the slices, or by any other circuit the implementer derives from 4.2
 * and 4.4; the unit tests check every one of the 256 inputs against the
 * S-box of Table 4.  ShiftRows and
 * MixColumns are permutations and XORs of the slices.  The key expansion
 * (5.2, Algorithm 2) uses the same SubBytes circuit for SubWord.
 *
 * Inputs and outputs are byte strings in the order of 3.4 (input byte n
 * is s[n mod 4, n / 4]).
 */

#ifndef LIBCRYPTO_COMPAT_CRYC_AES_H
#define LIBCRYPTO_COMPAT_CRYC_AES_H

#include "cryc.h"

/*
 * The section numbers in the comments of this file are those of FIPS 197
 * (May 2023 update, "FIPS 197-upd1").
 */

/* The block size in bytes (5, Figure 1). */
#define CRYC_AES_BLOCK		16U

/* The key sizes in bytes (Table 3). */
#define CRYC_AES128_KEY		16U
#define CRYC_AES192_KEY		24U
#define CRYC_AES256_KEY		32U

/*
 * The words of an expanded key: 15 round keys (Nr + 1 for AES-256,
 * Table 3) of eight 64-bit slices each.
 */
#define CRYC_AES_SCHEDULE_WORDS	120U

/*
 * An expanded AES key.
 *
 * schedule holds the round keys of 5.2 in the form the implementation's
 * rounds consume (the layout is the .c file's, documented there; with
 * four blocks bitsliced in uint64_t words, eight words per round key).
 * rounds is Nr (10, 12 or 14).  The structure is secret, owns no memory,
 * and is wiped by cryc_aes_clear.  It is not changed by the encryption
 * functions, so one key may be used from several threads at once.
 */
struct cryc_aes {
	uint64_t schedule[CRYC_AES_SCHEDULE_WORDS];
	unsigned rounds;
};

/*
 * Expands key (KeyExpansion, 5.2) into *aes.
 *
 * key_size is 16, 24 or 32; any other size returns EINVAL and leaves *aes
 * wiped.  key is not kept.  Constant time in the key.  Returns 0 or EINVAL.
 */
int cryc_aes_init(struct cryc_aes *aes, const uint8_t *key, size_t key_size);

/*
 * Encrypts count blocks of 16 bytes (Cipher, 5.1, Algorithm 1) from in to
 * out, each block independently (the caller builds a mode on top).
 *
 * in and out may be the same buffer, or must not overlap otherwise.
 * count may be 0.  Constant time.
 */
void cryc_aes_encrypt_blocks(const struct cryc_aes *aes, const uint8_t *in, uint8_t *out, size_t count);

/*
 * XORs size bytes of in with the keystream E(CB), E(inc32(CB)),
 * E(inc32(inc32(CB))), ... into out, where CB is counter[0..15] and
 * inc32 adds 1 modulo 2^32 to the last four bytes taken as a big-endian
 * integer (NIST SP 800-38D 6.2 and 6.5, GCTR).  On return counter holds
 * the next counter block not used (after a partial last block, the one
 * after it: the unused keystream is discarded).
 *
 * in and out may be the same buffer.  size may be 0 (counter unchanged).
 * The caller keeps the 32-bit counter from wrapping (GCM's limit on the
 * plaintext length does).  Constant time.
 */
void cryc_aes_ctr32_xor(const struct cryc_aes *aes, uint8_t *counter, const uint8_t *in, uint8_t *out, size_t size);

/* Wipes the expanded key. */
void cryc_aes_clear(struct cryc_aes *aes);

#endif
