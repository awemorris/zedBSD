/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The constant-time checks of X25519 and the field arithmetic under it:
 * the scalar (the private key) and every intermediate value are secret,
 * so memcheck reports any branch or address that depends on them.  The
 * u-coordinate of the peer is public.  The result and the return value
 * (the all-zero test) are made public only after the call, as a caller
 * does when it sends the public key or checks the shared secret.
 *
 * The field operations are also run directly on secret elements, since
 * Ed25519 uses them on its secret scalar's points.
 */

#include "check.h"
#include "ct.h"

#include "userland/base/libcrypto-compat/cryc-x25519.h"

/* Fills size bytes at out with a fixed pattern. */
static void fill(uint8_t *out, size_t size, unsigned seed);

/* Folds a public output into one value, to keep the call from being dropped. */
static unsigned fold(const uint8_t *bytes, size_t size);

/* A random source whose output is secret. */
static int secret_random(void *context, uint8_t *out, size_t size);

/* Fills size bytes at out with a fixed pattern. */
static void
fill(
	uint8_t *out,
	size_t size,
	unsigned seed)
{
	size_t index;

	/* One byte at a time. */
	for (index = 0; index < size; index++)
		out[index] = (uint8_t)(seed + index * 29U + (index >> 2));
}

/* Folds a public output into one value, to keep the call from being dropped. */
static unsigned
fold(
	const uint8_t *bytes,
	size_t size)
{
	unsigned value;
	size_t index;

	/* Exclusive-or of the bytes, rotated. */
	value = 0;
	for (index = 0; index < size; index++)
		value = ((value << 1) | (value >> 7)) ^ bytes[index];

	/* Reports the folded value. */
	return value & 0xffU;
}

/* A random source whose output is secret. */
static int
secret_random(
	void *context,
	uint8_t *out,
	size_t size)
{
	/* A fixed pattern, marked secret as a real source's bytes are. */
	UNUSED_PARAMETER(context);
	fill(out, size, 91U);
	CT_SECRET(out, size);
	return 0;
}

/* Runs the checks. */
int
main(void)
{
	uint8_t scalar[32];
	uint8_t u[32];
	uint8_t out[32];
	uint8_t private_key[32];
	uint8_t public_key[32];
	struct cryc_f25519 a;
	struct cryc_f25519 b;
	struct cryc_f25519 r;
	uint8_t bytes[32];
	uint32_t mask;
	unsigned total;
	int error;

	/* The sum of the folded outputs starts at zero. */
	total = 0;

	/* X25519 of a secret scalar and a public u. */
	fill(scalar, sizeof(scalar), 5U);
	fill(u, sizeof(u), 17U);
	CT_SECRET(scalar, sizeof(scalar));
	error = cryc_x25519(out, scalar, u);
	CT_PUBLIC(&error, sizeof(error));
	CHECK(error == 0);
	CT_PUBLIC(out, sizeof(out));
	total += fold(out, sizeof(out));

	/* A point of small order: the result is zero and the error is reported without a branch on it. */
	memset(u, 0, sizeof(u));
	CT_SECRET(scalar, sizeof(scalar));
	error = cryc_x25519(out, scalar, u);
	CT_PUBLIC(&error, sizeof(error));
	CHECK(error == EINVAL);
	CT_PUBLIC(out, sizeof(out));
	total += fold(out, sizeof(out));
	CT_PUBLIC(scalar, sizeof(scalar));

	/* The public key of a secret scalar. */
	fill(scalar, sizeof(scalar), 9U);
	CT_SECRET(scalar, sizeof(scalar));
	cryc_x25519_public(out, scalar);
	CT_PUBLIC(out, sizeof(out));
	total += fold(out, sizeof(out));
	CT_PUBLIC(scalar, sizeof(scalar));

	/* Key generation from a secret random source. */
	error = cryc_x25519_keygen(private_key, public_key, secret_random, NULL);
	CHECK(error == 0);
	CT_PUBLIC(public_key, sizeof(public_key));
	total += fold(public_key, sizeof(public_key));
	CT_PUBLIC(private_key, sizeof(private_key));

	/* The field operations on secret elements, including the encoding and the predicates. */
	fill(bytes, sizeof(bytes), 33U);
	CT_SECRET(bytes, sizeof(bytes));
	cryc_f25519_decode(&a, bytes);
	fill(bytes, sizeof(bytes), 71U);
	CT_SECRET(bytes, sizeof(bytes));
	cryc_f25519_decode(&b, bytes);
	cryc_f25519_add(&r, &a, &b);
	cryc_f25519_sub(&r, &r, &b);
	cryc_f25519_neg(&r, &r);
	cryc_f25519_carry(&r, &r);
	cryc_f25519_mul(&r, &r, &b);
	cryc_f25519_square(&r, &r);
	cryc_f25519_mul_small(&r, &r, 121665U);
	cryc_f25519_cswap(&a, &b, (uint32_t)(r.limb[0] & 1));
	cryc_f25519_invert(&r, &a);
	cryc_f25519_pow_p58(&r, &r);
	mask = cryc_f25519_is_zero(&r);
	CT_PUBLIC(&mask, sizeof(mask));
	total += mask & 1U;
	mask = cryc_f25519_is_negative(&r);
	CT_PUBLIC(&mask, sizeof(mask));
	total += mask;
	cryc_f25519_encode(bytes, &r);
	CT_PUBLIC(bytes, sizeof(bytes));
	total += fold(bytes, sizeof(bytes));

	/* The folded value is used so that no call can be dropped. */
	CHECK(total < 1000000U);

	/* The result. */
	return check_finish("ct-x25519");
}
