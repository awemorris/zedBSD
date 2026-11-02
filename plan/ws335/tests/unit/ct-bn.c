/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The constant-time checks of the fixed-width integers and the Montgomery
 * arithmetic: the values (the operands, the base of a power, the bytes
 * decoded or encoded) are secret, so memcheck reports any branch or
 * address that depends on them.  The modulus and the exponent are public.
 * Each result and each mask is made public only after the call, as a
 * caller does when it uses the result.
 */

#include "check.h"
#include "ct.h"

#include "userland/base/libcrypto-compat/cryc-bn.h"

/* The width of the checks: 256 bits, the width of P-256, and 2048 bits for an RSA-sized modulus. */
#define SMALL_LIMBS	8U
#define LARGE_LIMBS	64U

/* Fills n limbs with a fixed pattern below a modulus with the top limb set. */
static void fill(uint32_t *out, size_t n, unsigned seed);

/* Runs the operations on one modulus of the given width. */
static unsigned run(size_t limbs);

/* Fills n limbs with a fixed pattern below a modulus with the top limb set. */
static void
fill(
	uint32_t *out,
	size_t n,
	unsigned seed)
{
	size_t index;

	/* One limb at a time, the top limb kept small so the value is below the modulus. */
	for (index = 0; index < n; index++)
		out[index] = (uint32_t)(seed * 2654435761U + index * 40503U + (index >> 1));
	out[n - 1U] = 0x7fffffffU;
}

/* Runs the operations on one modulus of the given width. */
static unsigned
run(
	size_t limbs)
{
	struct cryc_bn_mont mont;
	uint8_t modulus[LARGE_LIMBS * 4U];
	uint8_t exponent[8];
	uint8_t bytes[LARGE_LIMBS * 4U + 4U];
	uint32_t a[LARGE_LIMBS];
	uint32_t b[LARGE_LIMBS];
	uint32_t am[LARGE_LIMBS];
	uint32_t bm[LARGE_LIMBS];
	uint32_t out[LARGE_LIMBS];
	uint32_t mask;
	unsigned total;
	size_t index;
	int error;

	/* The modulus: public, full width, odd. */
	total = 0;
	for (index = 0; index < limbs * 4U; index++)
		modulus[index] = (uint8_t)(0xc3U + index * 37U);
	modulus[0] |= 0x80U;
	modulus[limbs * 4U - 1U] |= 1U;
	error = cryc_bn_mont_init(&mont, modulus, limbs * 4U);
	CHECK(error == 0);

	/* The exponent: public (a public exponent, or p - 2). */
	for (index = 0; index < sizeof(exponent); index++)
		exponent[index] = (uint8_t)(0x35U + index * 91U);
	exponent[sizeof(exponent) - 1U] |= 1U;

	/* The operands: secret. */
	fill(a, limbs, 3U);
	fill(b, limbs, 11U);
	CT_SECRET(a, limbs * sizeof(a[0]));
	CT_SECRET(b, limbs * sizeof(b[0]));

	/* The modular sum and difference, and one conditional reduction. */
	cryc_bn_mod_add(out, a, b, &mont);
	cryc_bn_mod_sub(out, out, b, &mont);
	cryc_bn_mod_sub(out, a, out, &mont);
	cryc_bn_reduce_once(out, a, &mont);
	cryc_bn_mod_add(out, out, a, &mont);
	CT_PUBLIC(out, limbs * sizeof(out[0]));
	total += out[0] & 1U;

	/* The Montgomery product, in and out of Montgomery form, and a square. */
	cryc_bn_to_mont(am, a, &mont);
	cryc_bn_to_mont(bm, b, &mont);
	cryc_bn_mont_mul(out, am, bm, &mont);
	cryc_bn_mont_mul(out, out, out, &mont);
	cryc_bn_from_mont(out, out, &mont);
	CT_PUBLIC(out, limbs * sizeof(out[0]));
	total += out[0] & 1U;

	/* The comparisons and the selection: the masks are public afterwards. */
	mask = cryc_bn_lt(a, b, limbs);
	CT_PUBLIC(&mask, sizeof(mask));
	total += mask & 1U;
	mask = cryc_bn_is_zero(a, limbs);
	CT_PUBLIC(&mask, sizeof(mask));
	total += mask & 1U;
	mask = cryc_bn_lt(a, b, limbs);
	cryc_bn_select(out, a, b, limbs, mask);
	CT_PUBLIC(out, limbs * sizeof(out[0]));
	total += out[0] & 1U;

	/* A power of a secret base with a public exponent. */
	cryc_bn_mont_exp(out, am, exponent, sizeof(exponent), &mont);
	CT_PUBLIC(out, limbs * sizeof(out[0]));
	total += out[0] & 1U;

	/* Decoding secret bytes: the result and the verdict are public afterwards. */
	for (index = 0; index < limbs * 4U; index++)
		bytes[index] = (uint8_t)(index * 53U + 7U);
	bytes[0] = 0;
	CT_SECRET(bytes, limbs * 4U);
	error = cryc_bn_decode(out, limbs, bytes, limbs * 4U);
	CT_PUBLIC(&error, sizeof(error));
	CHECK(error == 0);
	CT_PUBLIC(out, limbs * sizeof(out[0]));
	total += out[0] & 1U;

	/* Decoding more bytes than the width, with a secret byte above it: the verdict is public afterwards. */
	for (index = 0; index < limbs * 4U + 4U; index++)
		bytes[index] = (uint8_t)(index * 29U + 5U);
	bytes[0] = 0;
	bytes[1] = 0;
	bytes[2] = 0;
	bytes[3] = 0x80;
	CT_SECRET(bytes, limbs * 4U + 4U);
	error = cryc_bn_decode(out, limbs, bytes, limbs * 4U + 4U);
	CT_PUBLIC(&error, sizeof(error));
	CHECK(error == EINVAL);
	CT_PUBLIC(out, limbs * sizeof(out[0]));
	total += out[0] & 1U;

	/* Encoding a secret number, in the width, narrower and wider. */
	CT_SECRET(a, limbs * sizeof(a[0]));
	cryc_bn_encode(a, limbs, bytes, limbs * 4U);
	CT_PUBLIC(bytes, limbs * 4U);
	total += bytes[0] & 1U;
	CT_SECRET(a, limbs * sizeof(a[0]));
	cryc_bn_encode(a, limbs, bytes, limbs * 4U + 3U);
	CT_PUBLIC(bytes, limbs * 4U + 3U);
	total += bytes[0] & 1U;
	CT_SECRET(a, limbs * sizeof(a[0]));
	cryc_bn_encode(a, limbs, bytes, limbs * 4U - 3U);
	CT_PUBLIC(bytes, limbs * 4U - 3U);
	total += bytes[0] & 1U;

	/* Everything public again, so the final use is not reported. */
	CT_PUBLIC(a, limbs * sizeof(a[0]));
	CT_PUBLIC(b, limbs * sizeof(b[0]));

	/* Reports the folded outputs of this width, so no call is dropped. */
	return total;
}

/* Runs the checks. */
int
main(void)
{
	unsigned total;

	/* The 256-bit and the 2048-bit moduli. */
	total = run(SMALL_LIMBS);
	total += run(LARGE_LIMBS);

	/* The folded value is used so that no call can be dropped. */
	CHECK(total < 1000000U);

	/* The result. */
	return check_finish("ct-bn");
}
