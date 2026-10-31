/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Key generation, ECDH and ECDSA verification on P-256 and P-384.
 */

#include "cryc-ecc.h"

/*
 * Makes a key pair by rejection sampling (FIPS 186-5 A.2.2): c is bits(n)
 * random bits; c > n - 2 is rejected; d = c + 1; Q = d * G.
 */
int
cryc_ecc_keygen(
	const struct cryc_ecp *ec,
	uint8_t *private_key,
	uint8_t *public_key,
	cryc_random_fn random,
	void *random_context)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(ec);
	UNUSED_PARAMETER(private_key);
	UNUSED_PARAMETER(public_key);
	UNUSED_PARAMETER(random);
	UNUSED_PARAMETER(random_context);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}

/*
 * Computes the ECDH shared secret (NIST SP 800-56A Rev.
 */
int
cryc_ecc_ecdh(
	const struct cryc_ecp *ec,
	const uint8_t *private_key,
	const uint8_t *peer,
	size_t peer_size,
	uint8_t *shared)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(ec);
	UNUSED_PARAMETER(private_key);
	UNUSED_PARAMETER(peer);
	UNUSED_PARAMETER(peer_size);
	UNUSED_PARAMETER(shared);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}

/*
 * Verifies an ECDSA signature (FIPS 186-5 6.4.2) over a message whose hash
 * is given.
 */
int
cryc_ecdsa_verify(
	const struct cryc_ecp *ec,
	const uint8_t *public_key,
	size_t public_key_size,
	const uint8_t *hash,
	size_t hash_size,
	const uint8_t *r,
	const uint8_t *s)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(ec);
	UNUSED_PARAMETER(public_key);
	UNUSED_PARAMETER(public_key_size);
	UNUSED_PARAMETER(hash);
	UNUSED_PARAMETER(hash_size);
	UNUSED_PARAMETER(r);
	UNUSED_PARAMETER(s);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}
