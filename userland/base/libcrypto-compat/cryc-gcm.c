/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * AES-GCM (NIST SP 800-38D).
 */

#include "cryc-gcm.h"

/*
 * Multiplies the 16-byte block x by the 16-byte block y in GF(2^128) (6.3,
 * Algorithm 1) and writes the product to out.
 */
void
cryc_gcm_multiply(
	uint8_t *out,
	const uint8_t *x,
	const uint8_t *y)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(out);
	UNUSED_PARAMETER(x);
	UNUSED_PARAMETER(y);
}

/*
 * Absorbs size bytes into the GHASH state y with hash subkey h (6.4,
 * Algorithm 2): for each 16-byte block X_i, y = (y xor X_i) * h.
 */
void
cryc_gcm_ghash(
	uint8_t *y,
	const uint8_t *h,
	const uint8_t *data,
	size_t size)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(y);
	UNUSED_PARAMETER(h);
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(size);
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
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(gcm);
	UNUSED_PARAMETER(key);
	UNUSED_PARAMETER(key_size);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
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
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(gcm);
	UNUSED_PARAMETER(iv);
	UNUSED_PARAMETER(iv_size);
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
 * Checks and decrypts (GCM-AD, 7.2, Algorithm 5).
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
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(gcm);
	UNUSED_PARAMETER(iv);
	UNUSED_PARAMETER(iv_size);
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
 * Wipes the key and H.
 */
void
cryc_gcm_clear(
	struct cryc_gcm *gcm)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(gcm);
}
