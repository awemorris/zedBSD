/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * AES-GCM, the authenticated encryption of TLS's AES-GCM cipher suites
 * (RFC 5288 for TLS 1.2, RFC 8446 5.2 for TLS 1.3).
 *
 * Constant time.  The key, the hash subkey H, the plaintext and the GHASH
 * state are secret.  The GHASH multiplication (6.3, Algorithm 1) is done
 * without a table indexed by H or by the data: the intended form is
 * Algorithm 1 itself, with each "if x_i = 1" and "if LSB1(V) = 1" turned
 * into a mask; any faster form the implementer chooses keeps that
 * property.  Tags are compared with cryc_ct_equal.
 *
 * Only 16-byte tags are produced and checked (TLS uses no shorter tag).
 */

#ifndef LIBCRYPTO_COMPAT_CRYC_GCM_H
#define LIBCRYPTO_COMPAT_CRYC_GCM_H

#include "cryc-aes.h"

/*
 * The section and algorithm numbers in the comments of this file are
 * those of NIST SP 800-38D (November 2007).
 */

/* The tag size in bytes (5.2.1.2, t = 128). */
#define CRYC_GCM_TAG		16U

/* The IV size TLS uses (8.2.1's recommended 96 bits). */
#define CRYC_GCM_IV		12U

/*
 * The largest plaintext of one call in bytes: 2^32 - 2 blocks (5.2.1.1,
 * len(P) <= 2^39 - 256 bits), kept within size_t on 32-bit machines by
 * also limiting it to SIZE_MAX - CRYC_GCM_TAG where that is smaller.
 */
#define CRYC_GCM_TEXT_MAX_BLOCKS	0xfffffffeU

/*
 * An AES-GCM key: the expanded block cipher key and H = CIPH_K(0^128)
 * (7.1 step 1).
 *
 * Secret; owns no memory; wiped by cryc_gcm_clear.  Not changed by seal
 * or open, so one key serves several threads.
 */
struct cryc_gcm {
	struct cryc_aes aes;
	uint8_t h[16];
};

/*
 * Multiplies the 16-byte block x by the 16-byte block y in GF(2^128)
 * (6.3, Algorithm 1) and writes the product to out.
 *
 * out may be x or y.  Bit order as in 6.3 (the leftmost bit of x[0] is
 * x_0).  Constant time in x and y.  Exposed for the unit tests of the
 * multiplication alone.
 */
void cryc_gcm_multiply(uint8_t *out, const uint8_t *x, const uint8_t *y);

/*
 * Absorbs size bytes into the GHASH state y with hash subkey h (6.4,
 * Algorithm 2): for each 16-byte block X_i, y = (y xor X_i) * h.  A last
 * partial block is padded with zeros on the right, as GCM's 0^v and 0^u
 * padding requires (7.1 step 5).
 *
 * data may be NULL only when size is 0.  Constant time.
 */
void cryc_gcm_ghash(uint8_t *y, const uint8_t *h, const uint8_t *data, size_t size);

/*
 * Expands key (16, 24 or 32 bytes) and computes H.
 *
 * Returns 0 or EINVAL for another key size (*gcm is then wiped).
 */
int cryc_gcm_init(struct cryc_gcm *gcm, const uint8_t *key, size_t key_size);

/*
 * Encrypts and authenticates (GCM-AE, 7.1, Algorithm 4).
 *
 * iv: iv_size bytes, 1 or more (12 is the fast case of 7.1 step 2; other
 * sizes use GHASH to form J0).  aad: aad_size bytes, may be NULL only
 * when aad_size is 0.  in: size bytes of plaintext, may be NULL only when
 * size is 0; size is at most CRYC_GCM_TEXT_MAX_BLOCKS * 16 bytes.  out:
 * room for size bytes of ciphertext; out may be in.  tag: 16 bytes
 * written.
 *
 * Returns 0, or EINVAL for an empty IV or a plaintext over the limit
 * (nothing is written).
 */
int cryc_gcm_seal(const struct cryc_gcm *gcm, const uint8_t *iv, size_t iv_size, const uint8_t *aad, size_t aad_size, const uint8_t *in, size_t size, uint8_t *out, uint8_t *tag);

/*
 * Checks and decrypts (GCM-AD, 7.2, Algorithm 5).
 *
 * Arguments as for cryc_gcm_seal, with in the ciphertext and tag the 16
 * received bytes.  The tag is computed and compared in constant time
 * before the plaintext is released: on a mismatch out is filled with zeros
 * (no plaintext of a forged message is ever visible to the caller) and
 * EBADMSG is returned.  out may be in.
 *
 * Returns 0, EBADMSG, or EINVAL (empty IV, ciphertext over the limit; out
 * is then not written).
 */
int cryc_gcm_open(const struct cryc_gcm *gcm, const uint8_t *iv, size_t iv_size, const uint8_t *aad, size_t aad_size, const uint8_t *in, size_t size, uint8_t *out, const uint8_t *tag);

/* Wipes the key and H. */
void cryc_gcm_clear(struct cryc_gcm *gcm);

#endif
