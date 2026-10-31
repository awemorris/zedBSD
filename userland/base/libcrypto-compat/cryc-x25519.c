/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * X25519 (RFC 7748).
 */

#include "cryc-x25519.h"

/*
 * Computes X25519(scalar, u) (section 5) into out (32 bytes).
 */
int
cryc_x25519(
	uint8_t *out,
	const uint8_t *scalar,
	const uint8_t *u)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(out);
	UNUSED_PARAMETER(scalar);
	UNUSED_PARAMETER(u);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}

/*
 * Computes the public key X25519(scalar, 9) (section 6.1) into out.
 */
void
cryc_x25519_public(
	uint8_t *out,
	const uint8_t *scalar)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(out);
	UNUSED_PARAMETER(scalar);
}

/*
 * Makes a key pair: 32 random bytes from random as the private scalar, and
 * its public key.
 */
int
cryc_x25519_keygen(
	uint8_t *private_key,
	uint8_t *public_key,
	cryc_random_fn random,
	void *random_context)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(private_key);
	UNUSED_PARAMETER(public_key);
	UNUSED_PARAMETER(random);
	UNUSED_PARAMETER(random_context);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}
