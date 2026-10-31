/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Ed25519 (RFC 8032).
 */

#include "cryc-ed25519.h"

/*
 * Computes the public key of a 32-byte private key (seed) (5.1.5).
 */
int
cryc_ed25519_public(
	uint8_t *public_key,
	const uint8_t *seed)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(public_key);
	UNUSED_PARAMETER(seed);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}

/*
 * Signs message with the private key seed (the public key is derived
 * inside).
 */
int
cryc_ed25519_sign(
	uint8_t *signature,
	const uint8_t *message,
	size_t message_size,
	const uint8_t *seed)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(signature);
	UNUSED_PARAMETER(message);
	UNUSED_PARAMETER(message_size);
	UNUSED_PARAMETER(seed);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}

/*
 * Verifies a signature (5.1.7) with the cofactorless equation [S]B = R +
 * [k]A' of step 3, rejecting S not less than L (step 1) and a public key
 * or R that does not decode (5.1.3).
 */
int
cryc_ed25519_verify(
	const uint8_t *signature,
	const uint8_t *message,
	size_t message_size,
	const uint8_t *public_key)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(signature);
	UNUSED_PARAMETER(message);
	UNUSED_PARAMETER(message_size);
	UNUSED_PARAMETER(public_key);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}
