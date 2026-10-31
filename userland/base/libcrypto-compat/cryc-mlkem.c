/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * K-PKE and ML-KEM-768 (FIPS 203 5 to 7).
 */

#include "cryc-mlkem.h"

/*
 * ML-KEM.KeyGen_internal (Algorithm 16) from the seeds d and z (32 bytes
 * each): writes ek (1184 bytes) and dk (2400 bytes).
 */
void
cryc_mlkem768_keygen_internal(
	uint8_t *ek,
	uint8_t *dk,
	const uint8_t *d,
	const uint8_t *z)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(ek);
	UNUSED_PARAMETER(dk);
	UNUSED_PARAMETER(d);
	UNUSED_PARAMETER(z);
}

/*
 * ML-KEM.KeyGen (Algorithm 19): d and z from random, then KeyGen_internal.
 */
int
cryc_mlkem768_keygen(
	uint8_t *ek,
	uint8_t *dk,
	cryc_random_fn random,
	void *random_context)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(ek);
	UNUSED_PARAMETER(dk);
	UNUSED_PARAMETER(random);
	UNUSED_PARAMETER(random_context);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}

/*
 * Checks an encapsulation key (7.2 "modulus check": every 12-bit value
 * below q).
 */
int
cryc_mlkem768_check_ek(
	const uint8_t *ek)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(ek);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}

/*
 * Checks a decapsulation key (7.3 "hash check": H(ek part) equals the
 * stored hash).
 */
int
cryc_mlkem768_check_dk(
	const uint8_t *dk)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(dk);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}

/*
 * ML-KEM.Encaps_internal (Algorithm 17) with the message m (32 bytes):
 * writes the shared key (32 bytes) and the ciphertext (1088 bytes).
 */
void
cryc_mlkem768_encaps_internal(
	uint8_t *shared,
	uint8_t *ciphertext,
	const uint8_t *ek,
	const uint8_t *m)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(shared);
	UNUSED_PARAMETER(ciphertext);
	UNUSED_PARAMETER(ek);
	UNUSED_PARAMETER(m);
}

/*
 * ML-KEM.Encaps (Algorithm 20): m from random, then Encaps_internal.
 */
int
cryc_mlkem768_encaps(
	uint8_t *shared,
	uint8_t *ciphertext,
	const uint8_t *ek,
	cryc_random_fn random,
	void *random_context)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(shared);
	UNUSED_PARAMETER(ciphertext);
	UNUSED_PARAMETER(ek);
	UNUSED_PARAMETER(random);
	UNUSED_PARAMETER(random_context);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}

/*
 * ML-KEM.Decaps (Algorithm 21 with Decaps_internal, Algorithm 18): writes
 * the shared key (32 bytes) for a 1088-byte ciphertext and a 2400-byte dk
 * that passed cryc_mlkem768_check_dk.
 */
void
cryc_mlkem768_decaps(
	uint8_t *shared,
	const uint8_t *ciphertext,
	const uint8_t *dk)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(shared);
	UNUSED_PARAMETER(ciphertext);
	UNUSED_PARAMETER(dk);
}
