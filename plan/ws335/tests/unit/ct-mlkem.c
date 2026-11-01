/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The constant-time checks of K-PKE and ML-KEM-768: the message m, the
 * secret part of dk (the K-PKE decryption key and z) and the seed z are
 * marked secret, so memcheck reports any branch or address that depends
 * on them.  The matrix seed rho is public by design (it is part of ek)
 * and SampleNTT branches on its rejection test, so the seed d, which
 * determines rho, is left public in the key generation check: the secret
 * vector and the noise derived from sigma are not covered there, but the
 * same arithmetic runs in the encryption of the decapsulation with
 * secret input, which is.  The embedded ek of dk and its hash are public.
 * Outputs derived from secrets are made public with CT_PUBLIC only to
 * read them.
 */

#include "check.h"
#include "ct.h"

#include "userland/base/libcrypto-compat/cryc-mlkem.h"

/* Fills size bytes with a fixed pattern. */
static void
fill(
	uint8_t *out,
	size_t size,
	unsigned seed)
{
	size_t i;

	/* A spread of byte values. */
	for (i = 0; i < size; i++)
		out[i] = (uint8_t)(seed + i * 29U + (i >> 3) * 7U);
}

/* Runs the checks. */
int
main(void)
{
	uint8_t d[32];
	uint8_t z[32];
	uint8_t m[32];
	uint8_t ek[CRYC_MLKEM768_EK];
	uint8_t dk[CRYC_MLKEM768_DK];
	uint8_t c[CRYC_MLKEM768_CT];
	uint8_t bad[CRYC_MLKEM768_CT];
	uint8_t shared[32];
	uint8_t other[32];
	unsigned total;
	int error;

	total = 0U;

	/* Key generation with a secret z (d is public, see the file comment). */
	fill(d, sizeof(d), 3U);
	fill(z, sizeof(z), 5U);
	CT_SECRET(z, sizeof(z));
	cryc_mlkem768_keygen_internal(ek, dk, d, z);
	CT_PUBLIC(dk + 2368, 32U);
	CT_PUBLIC(z, sizeof(z));
	total += ek[0] + dk[0];

	/* Encapsulation of a secret message under a public key. */
	fill(m, sizeof(m), 9U);
	CT_SECRET(m, sizeof(m));
	cryc_mlkem768_encaps_internal(shared, c, ek, m);
	CT_PUBLIC(shared, sizeof(shared));
	CT_PUBLIC(c, sizeof(c));
	CT_PUBLIC(m, sizeof(m));
	total += shared[0] + c[0];

	/* Decapsulation of a valid ciphertext with a secret decryption key and z. */
	CT_SECRET(dk, 1152U);
	CT_SECRET(dk + 2368, 32U);
	cryc_mlkem768_decaps(other, c, dk);
	CT_PUBLIC(other, sizeof(other));
	CT_PUBLIC(dk, 1152U);
	CT_PUBLIC(dk + 2368, 32U);
	CHECK(memcmp(other, shared, 32U) == 0);
	total += other[0];

	/* Decapsulation of a ciphertext that does not re-encrypt: the same instructions run, the answer is the rejection key. */
	memcpy(bad, c, sizeof(bad));
	bad[100] ^= 0x04U;
	CT_SECRET(dk, 1152U);
	CT_SECRET(dk + 2368, 32U);
	cryc_mlkem768_decaps(other, bad, dk);
	CT_PUBLIC(other, sizeof(other));
	CT_PUBLIC(dk, 1152U);
	CT_PUBLIC(dk + 2368, 32U);
	CHECK(memcmp(other, shared, 32U) != 0);
	total += other[0];

	/* The hash check with the whole of dk secret: the outcome is a value without a branch, made public here to read it. */
	CT_SECRET(dk, sizeof(dk));
	error = cryc_mlkem768_check_dk(dk);
	CT_PUBLIC(&error, sizeof(error));
	CT_PUBLIC(dk, sizeof(dk));
	CHECK(error == 0);

	/* The folded value is used so that no call can be dropped. */
	CHECK(total < 100000000U);

	/* The result. */
	return check_finish("ct-mlkem");
}
