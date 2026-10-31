/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Fixed-width integers and Montgomery arithmetic.
 */

#include "cryc-bn.h"

/*
 * Prepares the modulus given as size big-endian bytes (leading zero bytes
 * allowed).
 */
int
cryc_bn_mont_init(
	struct cryc_bn_mont *mont,
	const uint8_t *modulus,
	size_t size)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(mont);
	UNUSED_PARAMETER(modulus);
	UNUSED_PARAMETER(size);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}

/*
 * Reads size big-endian bytes into out (limbs limbs).
 */
int
cryc_bn_decode(
	uint32_t *out,
	size_t limbs,
	const uint8_t *in,
	size_t size)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(out);
	UNUSED_PARAMETER(limbs);
	UNUSED_PARAMETER(in);
	UNUSED_PARAMETER(size);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}

/*
 * Writes the low size * 8 bits of in (limbs limbs) big-endian into size
 * bytes at out (zero-padded on the left when size * 8 is more than the
 * width).
 */
void
cryc_bn_encode(
	const uint32_t *in,
	size_t limbs,
	uint8_t *out,
	size_t size)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(in);
	UNUSED_PARAMETER(limbs);
	UNUSED_PARAMETER(out);
	UNUSED_PARAMETER(size);
}

/*
 * Returns 0xffffffff when a < b and 0 otherwise (limbs limbs each), in
 * constant time.
 */
uint32_t
cryc_bn_lt(
	const uint32_t *a,
	const uint32_t *b,
	size_t limbs)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(a);
	UNUSED_PARAMETER(b);
	UNUSED_PARAMETER(limbs);

	/* Not implemented: the failure value. */
	return 0;
}

/*
 * Returns 0xffffffff when a is zero and 0 otherwise, in constant time.
 */
uint32_t
cryc_bn_is_zero(
	const uint32_t *a,
	size_t limbs)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(a);
	UNUSED_PARAMETER(limbs);

	/* Not implemented: the failure value. */
	return 0;
}

/*
 * Sets out to a where mask is 0xffffffff and to b where it is 0 (limbs
 * limbs; out may alias either), in constant time.
 */
void
cryc_bn_select(
	uint32_t *out,
	const uint32_t *a,
	const uint32_t *b,
	size_t limbs,
	uint32_t mask)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(out);
	UNUSED_PARAMETER(a);
	UNUSED_PARAMETER(b);
	UNUSED_PARAMETER(limbs);
	UNUSED_PARAMETER(mask);
}

/*
 * out = (a + b) mod m, for a and b less than m (mont->limbs limbs).
 */
void
cryc_bn_mod_add(
	uint32_t *out,
	const uint32_t *a,
	const uint32_t *b,
	const struct cryc_bn_mont *mont)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(out);
	UNUSED_PARAMETER(a);
	UNUSED_PARAMETER(b);
	UNUSED_PARAMETER(mont);
}

/*
 * out = (a - b) mod m, for a and b less than m; aliasing and timing as
 * cryc_bn_mod_add.
 */
void
cryc_bn_mod_sub(
	uint32_t *out,
	const uint32_t *a,
	const uint32_t *b,
	const struct cryc_bn_mont *mont)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(out);
	UNUSED_PARAMETER(a);
	UNUSED_PARAMETER(b);
	UNUSED_PARAMETER(mont);
}

/*
 * out = a mod m for a less than 2m (one conditional subtraction, by mask):
 * reduces a hash taken as an integer of the order's bit length (FIPS 186-5
 * 6.4.2 step 4's e) into the range.
 */
void
cryc_bn_reduce_once(
	uint32_t *out,
	const uint32_t *a,
	const struct cryc_bn_mont *mont)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(out);
	UNUSED_PARAMETER(a);
	UNUSED_PARAMETER(mont);
}

/*
 * out = a * b * R^-1 mod m (Montgomery multiplication), for a and b less
 * than m.
 */
void
cryc_bn_mont_mul(
	uint32_t *out,
	const uint32_t *a,
	const uint32_t *b,
	const struct cryc_bn_mont *mont)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(out);
	UNUSED_PARAMETER(a);
	UNUSED_PARAMETER(b);
	UNUSED_PARAMETER(mont);
}

/*
 * out = a * R mod m (into Montgomery form), for a less than m; constant
 * time.
 */
void
cryc_bn_to_mont(
	uint32_t *out,
	const uint32_t *a,
	const struct cryc_bn_mont *mont)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(out);
	UNUSED_PARAMETER(a);
	UNUSED_PARAMETER(mont);
}

/*
 * out = a * R^-1 mod m (out of Montgomery form), for a less than m;
 * constant time.
 */
void
cryc_bn_from_mont(
	uint32_t *out,
	const uint32_t *a,
	const struct cryc_bn_mont *mont)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(out);
	UNUSED_PARAMETER(a);
	UNUSED_PARAMETER(mont);
}

/*
 * out = R mod m (the Montgomery form of 1).
 */
void
cryc_bn_mont_one(
	uint32_t *out,
	const struct cryc_bn_mont *mont)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(out);
	UNUSED_PARAMETER(mont);
}

/*
 * out = base^exponent in Montgomery form, for base in Montgomery form and
 * less than m; exponent is size big-endian bytes (leading zeros allowed;
 * an exponent of 0 gives the Montgomery form of 1).
 */
void
cryc_bn_mont_exp(
	uint32_t *out,
	const uint32_t *base,
	const uint8_t *exponent,
	size_t size,
	const struct cryc_bn_mont *mont)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(out);
	UNUSED_PARAMETER(base);
	UNUSED_PARAMETER(exponent);
	UNUSED_PARAMETER(size);
	UNUSED_PARAMETER(mont);
}
