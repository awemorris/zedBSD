/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Arithmetic in the field GF(p), p = 2^255 - 19: the field of X25519 (RFC
 * 7748, cryc-x25519.h) and of Ed25519 (RFC 8032, cryc-ed25519.h).
 *
 * An element is ten limbs in radix 2^25.5 (limb i holds 26 bits for even
 * i and 25 bits for odd i, value = sum of limb[i] * 2^ceil(25.5 * i)),
 * with 64-bit products, so the code is the same on 32-bit machines.
 * Limbs are int32_t: after an addition or a subtraction they may carry
 * a few bits more than their nominal width (the carries of a
 * multiplication bring them back); each function says how far its inputs
 * may have grown (a "loose" input is the sum or difference of two
 * reduced elements; a "reduced" input is the output of a multiplication,
 * squaring, cryc_f25519_decode or cryc_f25519_carry).
 *
 * Constant time: every function takes the same time and touches the same
 * memory whatever the values (elements are secret in both users).  No
 * operation shifts a negative value: the carries are written with
 * arithmetic that is defined on negative values (adding a bias before a
 * shift, or dividing by a power of two with the rounding written out).
 */

#ifndef LIBCRYPTO_COMPAT_CRYC_F25519_H
#define LIBCRYPTO_COMPAT_CRYC_F25519_H

#include "cryc.h"

/* One element of GF(2^255 - 19), as above.  Owns no memory; copyable. */
struct cryc_f25519 {
	int32_t limb[10];
};

/*
 * Decodes 32 little-endian bytes (RFC 7748 5, decodeUCoordinate: the top
 * bit of byte 31 is ignored; a value from p to 2^255 - 1 is accepted and
 * reduced).  The result is reduced.
 */
void cryc_f25519_decode(struct cryc_f25519 *out, const uint8_t *in);

/*
 * Encodes an element (reduced or loose) as 32 little-endian bytes of its
 * canonical value in [0, p - 1] (RFC 7748 5, encodeUCoordinate; RFC 8032
 * 5.1.2).
 */
void cryc_f25519_encode(uint8_t *out, const struct cryc_f25519 *in);

/* out = 0. */
void cryc_f25519_zero(struct cryc_f25519 *out);

/* out = 1. */
void cryc_f25519_one(struct cryc_f25519 *out);

/* out = a + b (no carry; loose result from reduced inputs).  out may alias a or b. */
void cryc_f25519_add(struct cryc_f25519 *out, const struct cryc_f25519 *a, const struct cryc_f25519 *b);

/* out = a - b (no carry, with a multiple of p added to keep limbs positive; loose result from reduced inputs). */
void cryc_f25519_sub(struct cryc_f25519 *out, const struct cryc_f25519 *a, const struct cryc_f25519 *b);

/* out = -a (as 0 - a; loose result). */
void cryc_f25519_neg(struct cryc_f25519 *out, const struct cryc_f25519 *a);

/* Carries a loose element back to reduced form. */
void cryc_f25519_carry(struct cryc_f25519 *out, const struct cryc_f25519 *a);

/* out = a * b; inputs reduced or loose; result reduced.  out may alias a or b. */
void cryc_f25519_mul(struct cryc_f25519 *out, const struct cryc_f25519 *a, const struct cryc_f25519 *b);

/* out = a * a; as cryc_f25519_mul. */
void cryc_f25519_square(struct cryc_f25519 *out, const struct cryc_f25519 *a);

/* out = a * small, for small below 2^17 (X25519's a24 = 121665, RFC 7748 5); result reduced. */
void cryc_f25519_mul_small(struct cryc_f25519 *out, const struct cryc_f25519 *a, uint32_t small);

/* out = a^(p - 2) = a^-1 (0 for a = 0), a fixed chain of squarings and multiplications (RFC 7748 5). */
void cryc_f25519_invert(struct cryc_f25519 *out, const struct cryc_f25519 *a);

/*
 * out = a^((p - 5) / 8) = a^(2^252 - 3), the exponentiation of RFC 8032
 * 5.1.3 step 2's square root candidate, a fixed chain.  Used by Ed25519.
 */
void cryc_f25519_pow_p58(struct cryc_f25519 *out, const struct cryc_f25519 *a);

/* Swaps a and b when swap is 1 and leaves them when it is 0 (cswap of RFC 7748 5), by mask. */
void cryc_f25519_cswap(struct cryc_f25519 *a, struct cryc_f25519 *b, uint32_t swap);

/* Returns 0xffffffff when a (reduced or loose) is 0 modulo p, else 0. */
uint32_t cryc_f25519_is_zero(const struct cryc_f25519 *a);

/* Returns the low bit of a's canonical value (RFC 8032 5.1.2's x_0, "negative"). */
uint32_t cryc_f25519_is_negative(const struct cryc_f25519 *a);

#endif
