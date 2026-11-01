/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * X25519 (RFC 7748).
 *
 * The section numbers in the comments of this file are those of RFC 7748
 * (January 2016).
 */

#include <string.h>

#include "cryc-x25519.h"

/*
 * The constant a24 = (486662 - 2) / 4 of the ladder of section 5.
 *
 * Values referenced: RFC 7748 (January 2016) 5, a24 = 121665.
 */
#define X25519_A24	121665U

/* The u-coordinate of the base point: 9, little-endian (section 4.1). */
#define X25519_BASE_U	9U

/*
 * Runs the ladder of section 5 for a scalar and a u-coordinate and
 * writes the 32-byte result; returns 0, or EINVAL when it is all zeros.
 *
 * scalar and u are 32 bytes each.  out may not overlap scalar.
 */
static int
x25519_ladder(
	uint8_t *out,
	const uint8_t *scalar,
	const uint8_t *u)
{
	uint8_t k[32];
	struct cryc_f25519 x1;
	struct cryc_f25519 x2;
	struct cryc_f25519 z2;
	struct cryc_f25519 x3;
	struct cryc_f25519 z3;
	struct cryc_f25519 a;
	struct cryc_f25519 aa;
	struct cryc_f25519 b;
	struct cryc_f25519 bb;
	struct cryc_f25519 e;
	struct cryc_f25519 c;
	struct cryc_f25519 d;
	struct cryc_f25519 da;
	struct cryc_f25519 cb;
	struct cryc_f25519 t;
	uint32_t swap;
	uint32_t bit;
	uint32_t folded;
	uint32_t mask;
	unsigned int index;
	int step;

	/*
	 * Decodes the scalar (decodeScalar25519, section 5): the three low
	 * bits are cleared, the top bit is cleared and the second bit is set.
	 */
	memcpy(k, scalar, sizeof(k));
	k[0] &= 248U;
	k[31] &= 127U;
	k[31] |= 64U;

	/* Decodes the u-coordinate and sets up the ladder (section 5). */
	cryc_f25519_decode(&x1, u);
	cryc_f25519_one(&x2);
	cryc_f25519_zero(&z2);
	x3 = x1;
	cryc_f25519_one(&z3);
	swap = 0U;

	/* The 255 steps, from the top bit down; the swaps are by mask. */
	for (step = 254; step >= 0; step--) {
		bit = ((uint32_t)k[step >> 3] >> (unsigned int)(step & 7)) & 1U;
		swap ^= bit;
		cryc_f25519_cswap(&x2, &x3, swap);
		cryc_f25519_cswap(&z2, &z3, swap);
		swap = bit;

		/* A = x_2 + z_2, AA = A^2, B = x_2 - z_2, BB = B^2, E = AA - BB. */
		cryc_f25519_add(&a, &x2, &z2);
		cryc_f25519_square(&aa, &a);
		cryc_f25519_sub(&b, &x2, &z2);
		cryc_f25519_square(&bb, &b);
		cryc_f25519_sub(&e, &aa, &bb);

		/* C = x_3 + z_3, D = x_3 - z_3, DA = D * A, CB = C * B. */
		cryc_f25519_add(&c, &x3, &z3);
		cryc_f25519_sub(&d, &x3, &z3);
		cryc_f25519_mul(&da, &d, &a);
		cryc_f25519_mul(&cb, &c, &b);

		/* x_3 = (DA + CB)^2, z_3 = x_1 * (DA - CB)^2. */
		cryc_f25519_add(&t, &da, &cb);
		cryc_f25519_square(&x3, &t);
		cryc_f25519_sub(&t, &da, &cb);
		cryc_f25519_square(&t, &t);
		cryc_f25519_mul(&z3, &x1, &t);

		/* x_2 = AA * BB, z_2 = E * (AA + a24 * E). */
		cryc_f25519_mul(&x2, &aa, &bb);
		cryc_f25519_mul_small(&t, &e, X25519_A24);
		cryc_f25519_add(&t, &aa, &t);
		cryc_f25519_mul(&z2, &e, &t);
	}

	/* The last swap, then x_2 * z_2^(p - 2) (section 5). */
	cryc_f25519_cswap(&x2, &x3, swap);
	cryc_f25519_cswap(&z2, &z3, swap);
	cryc_f25519_invert(&t, &z2);
	cryc_f25519_mul(&t, &x2, &t);
	cryc_f25519_encode(out, &t);

	/* The result is all zeros exactly for a point of small order; the test reads every byte. */
	folded = 0U;
	for (index = 0; index < 32U; index++)
		folded |= (uint32_t)out[index];
	mask = cryc_ct_mask_zero32(folded);

	/* The scalar and every intermediate value are secret. */
	cryc_wipe(k, sizeof(k));
	cryc_wipe(&x1, sizeof(x1));
	cryc_wipe(&x2, sizeof(x2));
	cryc_wipe(&z2, sizeof(z2));
	cryc_wipe(&x3, sizeof(x3));
	cryc_wipe(&z3, sizeof(z3));
	cryc_wipe(&a, sizeof(a));
	cryc_wipe(&aa, sizeof(aa));
	cryc_wipe(&b, sizeof(b));
	cryc_wipe(&bb, sizeof(bb));
	cryc_wipe(&e, sizeof(e));
	cryc_wipe(&c, sizeof(c));
	cryc_wipe(&d, sizeof(d));
	cryc_wipe(&da, sizeof(da));
	cryc_wipe(&cb, sizeof(cb));
	cryc_wipe(&t, sizeof(t));

	/* EINVAL for the all-zero result, without a branch on it. */
	return (int)(mask & (uint32_t)EINVAL);
}

/*
 * Computes X25519(scalar, u) (section 5) into out (32 bytes).
 */
int
cryc_x25519(
	uint8_t *out,
	const uint8_t *scalar,
	const uint8_t *u)
{
	/* The ladder does everything. */
	return x25519_ladder(out, scalar, u);
}

/*
 * Computes the public key X25519(scalar, 9) (section 6.1) into out.
 */
void
cryc_x25519_public(
	uint8_t *out,
	const uint8_t *scalar)
{
	uint8_t base[32];
	unsigned int index;

	/* The base point's u-coordinate is 9 (section 4.1). */
	for (index = 0; index < sizeof(base); index++)
		base[index] = 0;
	base[0] = (uint8_t)X25519_BASE_U;

	/* The result of the base point is never zero for a clamped scalar. */
	(void)x25519_ladder(out, scalar, base);
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
	int error;

	/* The private scalar is 32 random bytes. */
	error = random(random_context, private_key, CRYC_X25519_SIZE);
	if (error != 0) {
		/* Nothing is left in either output. */
		cryc_wipe(private_key, CRYC_X25519_SIZE);
		cryc_wipe(public_key, CRYC_X25519_SIZE);
		return EIO;
	}

	/* The public key is X25519(scalar, 9). */
	cryc_x25519_public(public_key, private_key);
	return 0;
}
