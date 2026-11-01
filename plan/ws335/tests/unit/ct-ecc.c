/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The constant-time checks of key generation and ECDH on P-256 and P-384.
 *
 * ECDH: the private key is secret, so memcheck reports any branch or
 * address that depends on it, including the range check of the key and the
 * check that the product is not at infinity.  The shared secret and the
 * error value are made public only after the call, as a caller does when it
 * uses or compares them.  A private key of zero and one equal to n are also
 * run (the secret verdict must not change the control flow).
 *
 * Key generation: whether a candidate is rejected is a branch on the
 * candidate by design (a rejected candidate is discarded), so the random
 * source here hands out candidates that are already public.  What the call
 * does with the accepted candidate after that (the arithmetic and the
 * encoding) has no branch to report, and the private key it writes is then
 * used as the secret of an ECDH call.
 */

#include "check.h"
#include "ct.h"

#include "userland/base/libcrypto-compat/cryc-ecc.h"

/* A random source that hands out pseudo-random public bytes. */
static int
public_random(
	void *context,
	uint8_t *out,
	size_t size)
{
	uint32_t *state;
	size_t index;

	/* Marsaglia's xorshift32, one byte of each word; the bytes are public. */
	state = (uint32_t *)context;
	for (index = 0; index < size; index++) {
		*state ^= *state << 13;
		*state ^= *state >> 17;
		*state ^= *state << 5;
		out[index] = (uint8_t)(*state >> 11);
	}
	return 0;
}

/* Runs the operations on one curve. */
static unsigned
run(
	enum cryc_curve_id id)
{
	struct cryc_ecp ec;
	uint32_t state;
	uint8_t private_key[CRYC_ECP_MAX_BYTES];
	uint8_t other_private[CRYC_ECP_MAX_BYTES];
	uint8_t public_key[CRYC_ECP_MAX_POINT];
	uint8_t other_public[CRYC_ECP_MAX_POINT];
	uint8_t shared[CRYC_ECP_MAX_BYTES];
	uint8_t secret[CRYC_ECP_MAX_BYTES];
	unsigned total;
	size_t index;
	int error;

	/* The curve is public. */
	total = 0;
	state = 0x1b873593U;
	error = cryc_ecp_init(&ec, id);
	CHECK(error == 0);

	/* Two key pairs from public candidates. */
	error = cryc_ecc_keygen(&ec, private_key, public_key, public_random, &state);
	CHECK(error == 0);
	error = cryc_ecc_keygen(&ec, other_private, other_public, public_random, &state);
	CHECK(error == 0);

	/* ECDH with a secret private key: the secret and the verdict are public afterwards. */
	CT_SECRET(private_key, ec.size);
	error = cryc_ecc_ecdh(&ec, private_key, other_public, 1U + 2U * ec.size, shared);
	CT_PUBLIC(&error, sizeof(error));
	CT_PUBLIC(shared, ec.size);
	CT_PUBLIC(private_key, ec.size);
	CHECK(error == 0);
	total += shared[0] & 1U;

	/* The other direction gives the same secret. */
	CT_SECRET(other_private, ec.size);
	error = cryc_ecc_ecdh(&ec, other_private, public_key, 1U + 2U * ec.size, secret);
	CT_PUBLIC(&error, sizeof(error));
	CT_PUBLIC(secret, ec.size);
	CT_PUBLIC(other_private, ec.size);
	CHECK(error == 0);
	CHECK_BYTES(secret, shared, ec.size);

	/* A secret private key of zero: refused, and the verdict does not change the control flow. */
	memset(secret, 0, sizeof(secret));
	CT_SECRET(secret, ec.size);
	error = cryc_ecc_ecdh(&ec, secret, other_public, 1U + 2U * ec.size, shared);
	CT_PUBLIC(&error, sizeof(error));
	CT_PUBLIC(shared, ec.size);
	CT_PUBLIC(secret, ec.size);
	CHECK(error == EINVAL);
	for (index = 0; index < ec.size; index++)
		total += shared[index];

	/* A secret private key of all ones (not below n): refused the same way. */
	memset(secret, 0xff, sizeof(secret));
	CT_SECRET(secret, ec.size);
	error = cryc_ecc_ecdh(&ec, secret, other_public, 1U + 2U * ec.size, shared);
	CT_PUBLIC(&error, sizeof(error));
	CT_PUBLIC(shared, ec.size);
	CT_PUBLIC(secret, ec.size);
	CHECK(error == EINVAL);
	for (index = 0; index < ec.size; index++)
		total += shared[index];

	/* The folded value is used so that no call can be dropped. */
	return total;
}

/* Runs the checks. */
int
main(void)
{
	unsigned total;

	/* Both curves. */
	total = run(CRYC_CURVE_P256);
	total += run(CRYC_CURVE_P384);

	/* The folded value is used so that no call can be dropped. */
	CHECK(total < 100000U);

	/* The result. */
	return check_finish("ct-ecc");
}
