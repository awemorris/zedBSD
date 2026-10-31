/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Arithmetic modulo 2^255 - 19.
 */

#include "cryc-f25519.h"

/*
 * Decodes 32 little-endian bytes (RFC 7748 5, decodeUCoordinate: the top
 * bit of byte 31 is ignored; a value from p to 2^255 - 1 is accepted and
 * reduced).
 */
void
cryc_f25519_decode(
	struct cryc_f25519 *out,
	const uint8_t *in)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(out);
	UNUSED_PARAMETER(in);
}

/*
 * Encodes an element (reduced or loose) as 32 little-endian bytes of its
 * canonical value in [0, p - 1] (RFC 7748 5, encodeUCoordinate; RFC 8032
 * 5.1.2).
 */
void
cryc_f25519_encode(
	uint8_t *out,
	const struct cryc_f25519 *in)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(out);
	UNUSED_PARAMETER(in);
}

/*
 * out = 0.
 */
void
cryc_f25519_zero(
	struct cryc_f25519 *out)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(out);
}

/*
 * out = 1.
 */
void
cryc_f25519_one(
	struct cryc_f25519 *out)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(out);
}

/*
 * out = a + b (no carry; loose result from reduced inputs).
 */
void
cryc_f25519_add(
	struct cryc_f25519 *out,
	const struct cryc_f25519 *a,
	const struct cryc_f25519 *b)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(out);
	UNUSED_PARAMETER(a);
	UNUSED_PARAMETER(b);
}

/*
 * out = a - b (no carry, with a multiple of p added to keep limbs
 * positive; loose result from reduced inputs).
 */
void
cryc_f25519_sub(
	struct cryc_f25519 *out,
	const struct cryc_f25519 *a,
	const struct cryc_f25519 *b)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(out);
	UNUSED_PARAMETER(a);
	UNUSED_PARAMETER(b);
}

/*
 * out = -a (as 0 - a; loose result).
 */
void
cryc_f25519_neg(
	struct cryc_f25519 *out,
	const struct cryc_f25519 *a)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(out);
	UNUSED_PARAMETER(a);
}

/*
 * Carries a loose element back to reduced form.
 */
void
cryc_f25519_carry(
	struct cryc_f25519 *out,
	const struct cryc_f25519 *a)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(out);
	UNUSED_PARAMETER(a);
}

/*
 * out = a * b; inputs reduced or loose; result reduced.
 */
void
cryc_f25519_mul(
	struct cryc_f25519 *out,
	const struct cryc_f25519 *a,
	const struct cryc_f25519 *b)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(out);
	UNUSED_PARAMETER(a);
	UNUSED_PARAMETER(b);
}

/*
 * out = a * a; as cryc_f25519_mul.
 */
void
cryc_f25519_square(
	struct cryc_f25519 *out,
	const struct cryc_f25519 *a)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(out);
	UNUSED_PARAMETER(a);
}

/*
 * out = a * small, for small below 2^17 (X25519's a24 = 121665, RFC 7748
 * 5); result reduced.
 */
void
cryc_f25519_mul_small(
	struct cryc_f25519 *out,
	const struct cryc_f25519 *a,
	uint32_t small)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(out);
	UNUSED_PARAMETER(a);
	UNUSED_PARAMETER(small);
}

/*
 * out = a^(p - 2) = a^-1 (0 for a = 0), a fixed chain of squarings and
 * multiplications (RFC 7748 5).
 */
void
cryc_f25519_invert(
	struct cryc_f25519 *out,
	const struct cryc_f25519 *a)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(out);
	UNUSED_PARAMETER(a);
}

/*
 * out = a^((p - 5) / 8) = a^(2^252 - 3), the exponentiation of RFC 8032
 * 5.1.3 step 2's square root candidate, a fixed chain.
 */
void
cryc_f25519_pow_p58(
	struct cryc_f25519 *out,
	const struct cryc_f25519 *a)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(out);
	UNUSED_PARAMETER(a);
}

/*
 * Swaps a and b when swap is 1 and leaves them when it is 0 (cswap of RFC
 * 7748 5), by mask.
 */
void
cryc_f25519_cswap(
	struct cryc_f25519 *a,
	struct cryc_f25519 *b,
	uint32_t swap)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(a);
	UNUSED_PARAMETER(b);
	UNUSED_PARAMETER(swap);
}

/*
 * Returns 0xffffffff when a (reduced or loose) is 0 modulo p, else 0.
 */
uint32_t
cryc_f25519_is_zero(
	const struct cryc_f25519 *a)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(a);

	/* Not implemented: the failure value. */
	return 0;
}

/*
 * Returns the low bit of a's canonical value (RFC 8032 5.1.2's x_0,
 * "negative").
 */
uint32_t
cryc_f25519_is_negative(
	const struct cryc_f25519 *a)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(a);

	/* Not implemented: the failure value. */
	return 0;
}
