/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * RSA signature verification (RFC 8017).
 */

#include "cryc-rsa.h"

/*
 * Verifies an RSASSA-PKCS1-v1_5 signature (8.2.2) with hash hash_id over a
 * message whose hash is given.
 */
int
cryc_rsa_pkcs1_verify(
	const struct cryc_rsa_key *key,
	enum cryc_hash_id hash_id,
	const uint8_t *hash,
	size_t hash_size,
	const uint8_t *signature,
	size_t signature_size)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(key);
	UNUSED_PARAMETER(hash_id);
	UNUSED_PARAMETER(hash);
	UNUSED_PARAMETER(hash_size);
	UNUSED_PARAMETER(signature);
	UNUSED_PARAMETER(signature_size);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}

/*
 * Verifies an RSASSA-PSS signature (8.1.2, EMSA-PSS-VERIFY of 9.1.2) with
 * hash hash_id, MGF1 (B.2.1) with the same hash, and a salt of salt_size
 * bytes.
 */
int
cryc_rsa_pss_verify(
	const struct cryc_rsa_key *key,
	enum cryc_hash_id hash_id,
	const uint8_t *hash,
	size_t hash_size,
	size_t salt_size,
	const uint8_t *signature,
	size_t signature_size)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(key);
	UNUSED_PARAMETER(hash_id);
	UNUSED_PARAMETER(hash);
	UNUSED_PARAMETER(hash_size);
	UNUSED_PARAMETER(salt_size);
	UNUSED_PARAMETER(signature);
	UNUSED_PARAMETER(signature_size);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}
