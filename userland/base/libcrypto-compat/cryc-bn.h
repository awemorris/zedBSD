/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Fixed-width unsigned integers and Montgomery arithmetic modulo an odd
 * number, for the prime fields and group orders of P-256 and P-384
 * (cryc-ecp.h) and for RSA public keys (cryc-rsa.h).
 *
 * A number is an array of uint32_t limbs, least significant first, of a
 * width (limbs) the caller names; every limb of the array is meaningful
 * (no length field: a value shorter than the width has zero high limbs).
 * Products are formed in uint64_t, so the code is the same on 32-bit and
 * 64-bit machines.
 *
 * Montgomery form: with R = 2^(32 * limbs) for the modulus m's width,
 * the Montgomery form of a is a * R mod m.  cryc_bn_mont_mul(a, b) gives
 * a * b * R^-1 mod m (REDC, in its word-by-word form whose per-word
 * factor is -m^-1 mod 2^32), so the product of two Montgomery forms is
 * the Montgomery form of the product.
 *
 * Constant time.  The moduli are public.  Values (scalars, coordinates,
 * an RSA message representative) may be secret: every function below
 * except cryc_bn_mont_init takes the same time and touches the same
 * addresses whatever the values, depending only on the widths.
 * cryc_bn_mont_exp's time depends on the exponent, which must be public
 * (an RSA public exponent, p - 2 for an inversion).  The final
 * subtraction of REDC is done with a mask, always.
 */

#ifndef LIBCRYPTO_COMPAT_CRYC_BN_H
#define LIBCRYPTO_COMPAT_CRYC_BN_H

#include "cryc.h"

/*
 * The largest modulus in bits: an RSA key of 8192 bits (the largest any
 * root in the ca-certificates package and any common server uses is 4096;
 * 8192 leaves room and bounds the work of a hostile certificate).
 */
#define CRYC_BN_MAX_BITS	8192U
#define CRYC_BN_MAX_LIMBS	(CRYC_BN_MAX_BITS / 32U)

/*
 * A modulus prepared for Montgomery arithmetic.
 *
 * modulus is m (odd, at least 3) in limbs limbs, the high limb not zero;
 * bits is its bit length.  r2 is R^2 mod m.  m0inv is -m^-1 mod 2^32 (m
 * odd makes it exist).  Limbs of modulus and r2 at or above limbs are
 * zero.  Public; the caller allocates it (2 KiB, a local or a member);
 * made by cryc_bn_mont_init and never changed after, so it may be shared
 * read-only between threads.
 */
struct cryc_bn_mont {
	uint32_t modulus[CRYC_BN_MAX_LIMBS];
	uint32_t r2[CRYC_BN_MAX_LIMBS];
	uint32_t m0inv;
	size_t limbs;
	size_t bits;
};

/*
 * Prepares the modulus given as size big-endian bytes (leading zero bytes
 * allowed).
 *
 * Returns 0; EINVAL when the modulus is even or less than 3; ENOTSUP when
 * it has more than CRYC_BN_MAX_BITS bits.  On an error *mont is zeroed.
 * Variable time (the modulus is public).
 */
int cryc_bn_mont_init(struct cryc_bn_mont *mont, const uint8_t *modulus, size_t size);

/*
 * Reads size big-endian bytes into out (limbs limbs).
 *
 * Returns 0, or EINVAL when the value does not fit in limbs limbs (a
 * non-zero byte above 32 * limbs bits); out is then zeroed.  The time
 * depends only on size and limbs, except for which of the two results it
 * returns.
 */
int cryc_bn_decode(uint32_t *out, size_t limbs, const uint8_t *in, size_t size);

/*
 * Writes the low size * 8 bits of in (limbs limbs) big-endian into
 * size bytes at out (zero-padded on the left when size * 8 is more than
 * the width).  The caller ensures the value fits (its use of the result
 * decides).  Constant time.
 */
void cryc_bn_encode(const uint32_t *in, size_t limbs, uint8_t *out, size_t size);

/* Returns 0xffffffff when a < b and 0 otherwise (limbs limbs each), in constant time. */
uint32_t cryc_bn_lt(const uint32_t *a, const uint32_t *b, size_t limbs);

/* Returns 0xffffffff when a is zero and 0 otherwise, in constant time. */
uint32_t cryc_bn_is_zero(const uint32_t *a, size_t limbs);

/* Sets out to a where mask is 0xffffffff and to b where it is 0 (limbs limbs; out may alias either), in constant time. */
void cryc_bn_select(uint32_t *out, const uint32_t *a, const uint32_t *b, size_t limbs, uint32_t mask);

/*
 * out = (a + b) mod m, for a and b less than m (mont->limbs limbs).
 *
 * out may alias a or b.  Constant time.
 */
void cryc_bn_mod_add(uint32_t *out, const uint32_t *a, const uint32_t *b, const struct cryc_bn_mont *mont);

/* out = (a - b) mod m, for a and b less than m; aliasing and timing as cryc_bn_mod_add. */
void cryc_bn_mod_sub(uint32_t *out, const uint32_t *a, const uint32_t *b, const struct cryc_bn_mont *mont);

/*
 * out = a mod m for a less than 2m (one conditional subtraction, by
 * mask): reduces a hash taken as an integer of the order's bit length
 * (FIPS 186-5 6.4.2 step 3's E) into the range.
 *
 * out may alias a.  Constant time.
 */
void cryc_bn_reduce_once(uint32_t *out, const uint32_t *a, const struct cryc_bn_mont *mont);

/*
 * out = a * b * R^-1 mod m (Montgomery multiplication), for a and b less
 * than m.
 *
 * Procedure referenced: P. L. Montgomery, "Modular multiplication
 * without trial division", Mathematics of Computation 44 (1985) (REDC).
 *
 * out may alias a or b (the product is formed in a local buffer).
 * Constant time.
 */
void cryc_bn_mont_mul(uint32_t *out, const uint32_t *a, const uint32_t *b, const struct cryc_bn_mont *mont);

/* out = a * R mod m (into Montgomery form), for a less than m; constant time. */
void cryc_bn_to_mont(uint32_t *out, const uint32_t *a, const struct cryc_bn_mont *mont);

/* out = a * R^-1 mod m (out of Montgomery form), for a less than m; constant time. */
void cryc_bn_from_mont(uint32_t *out, const uint32_t *a, const struct cryc_bn_mont *mont);

/* out = R mod m (the Montgomery form of 1). */
void cryc_bn_mont_one(uint32_t *out, const struct cryc_bn_mont *mont);

/*
 * out = base^exponent in Montgomery form, for base in Montgomery form and
 * less than m; exponent is size big-endian bytes (leading zeros allowed;
 * an exponent of 0 gives the Montgomery form of 1).
 *
 * The time depends on the exponent's bits, which must be public; it does
 * not depend on base.  out may alias base.
 */
void cryc_bn_mont_exp(uint32_t *out, const uint32_t *base, const uint8_t *exponent, size_t size, const struct cryc_bn_mont *mont);

#endif
