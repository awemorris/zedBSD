/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * AES in bitsliced, constant-time form, and its counter mode (FIPS 197, SP
 * 800-38D GCTR).
 */

#include "cryc-aes.h"

/*
 * Expands key (KeyExpansion, 5.2) into *aes.
 */
int
cryc_aes_init(
	struct cryc_aes *aes,
	const uint8_t *key,
	size_t key_size)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(aes);
	UNUSED_PARAMETER(key);
	UNUSED_PARAMETER(key_size);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}

/*
 * Encrypts count blocks of 16 bytes (Cipher, 5.1, Algorithm 1) from in to
 * out, each block independently (the caller builds a mode on top).
 */
void
cryc_aes_encrypt_blocks(
	const struct cryc_aes *aes,
	const uint8_t *in,
	uint8_t *out,
	size_t count)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(aes);
	UNUSED_PARAMETER(in);
	UNUSED_PARAMETER(out);
	UNUSED_PARAMETER(count);
}

/*
 * XORs size bytes of in with the keystream E(CB), E(inc32(CB)),
 * E(inc32(inc32(CB))), ...
 */
void
cryc_aes_ctr32_xor(
	const struct cryc_aes *aes,
	uint8_t *counter,
	const uint8_t *in,
	uint8_t *out,
	size_t size)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(aes);
	UNUSED_PARAMETER(counter);
	UNUSED_PARAMETER(in);
	UNUSED_PARAMETER(out);
	UNUSED_PARAMETER(size);
}

/*
 * Wipes the expanded key.
 */
void
cryc_aes_clear(
	struct cryc_aes *aes)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(aes);
}
