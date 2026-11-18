/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Arithmetic modulo 2^255 - 19.
 *
 * Representation.  An element is ten limbs; limb i carries
 * limb_bits[i] bits (26 for even i, 25 for odd i) and has the weight
 * 2^ceil(25.5 * i), so the weights are 1, 2^26, 2^51, 2^77, 2^102,
 * 2^128, 2^153, 2^179, 2^204, 2^230 and the next weight would be 2^255.
 * Because 2^255 = 19 (mod p), a product that lands at or beyond limb 10
 * is brought back by a factor of 19.  The weight of limb i times the
 * weight of limb j is the weight of limb i + j, doubled when i and j are
 * both odd (25 + 25 = 50 bits where the next position holds 51).
 *
 * Every limb is kept non-negative, so all the arithmetic below is on
 * unsigned values and no shift ever sees a negative number.  A reduced
 * element has every limb at most 2^limb_bits[i].  A loose element is the
 * sum of two reduced elements, or the difference of two reduced elements
 * with 2p added (which keeps each limb non-negative); every limb of a
 * loose element is below 3 * 2^limb_bits[i].
 *
 * Products.  A limb of a loose element is below 3 * 2^26 (even i) or
 * 3 * 2^25 (odd i).  The product of two limbs is below 9 * 2^52, with
 * the factor 19 for a wrapped position below 9 * 2^52 * 19.  A position
 * collects ten such terms at most, which is below 7.7e18, under
 * 2^63 = 9.22e18, so a 64-bit accumulator never wraps.
 */

#include "cryc-f25519.h"

/* Hides a mask from the optimizer. */
static uint32_t f25519_opaque(uint32_t value);

/* The count of limbs of an element. */
#define F25519_LIMBS	10

/*
 * The number of bits each limb carries.
 *
 * The widths alternate 26, 25 so the ten limbs hold 255 bits.
 */
static const uint32_t limb_bits[F25519_LIMBS] = {
	26U, 25U, 26U, 25U, 26U, 25U, 26U, 25U, 26U, 25U
};

/*
 * The mask of limb_bits[i] ones for each position, which keeps what a
 * carry leaves behind.
 */
static const uint32_t limb_mask[F25519_LIMBS] = {
	0x3ffffffU, 0x1ffffffU, 0x3ffffffU, 0x1ffffffU, 0x3ffffffU,
	0x1ffffffU, 0x3ffffffU, 0x1ffffffU, 0x3ffffffU, 0x1ffffffU
};

/*
 * Twice the prime p, in limbs.
 *
 * p = 2^255 - 19 has every limb full (2^bits - 1) except limb 0, which is
 * 2^26 - 19 (the 19 taken from the lowest bits), so twice p has limb 0
 * equal to 2^27 - 38 and every other limb equal to 2^(bits + 1) - 2.
 * Subtraction adds this before it subtracts: a limb of it (at least
 * 2^bits) is never below the matching limb of a reduced element, so the
 * difference stays non-negative.
 *
 * Values referenced: RFC 7748 4.1 (p = 2^255 - 19), 5.
 */
static const uint32_t twice_p[F25519_LIMBS] = {
	0x7ffffdaU, 0x3fffffeU, 0x7fffffeU, 0x3fffffeU, 0x7fffffeU,
	0x3fffffeU, 0x7fffffeU, 0x3fffffeU, 0x7fffffeU, 0x3fffffeU
};

/* Forward declarations. */
static void f25519_carry_wide(struct cryc_f25519 *out, uint64_t *wide);
static void f25519_carry_strict(uint64_t *wide);
static void f25519_canonical(uint32_t *limbs, const struct cryc_f25519 *in);
static void f25519_square_times(struct cryc_f25519 *out, const struct cryc_f25519 *in, unsigned int count);
static void f25519_chain_250(struct cryc_f25519 *x250, struct cryc_f25519 *x1_out, const struct cryc_f25519 *a);

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
	uint64_t accumulator;
	uint32_t held;
	uint32_t position;
	uint32_t byte_value;
	unsigned int index;

	/*
	 * Gathers the bits of the 255-bit number limb by limb, taking one
	 * more byte whenever the accumulator holds fewer bits than the next
	 * limb needs.  Byte 31 loses its top bit, which RFC 7748 5 says to
	 * ignore.
	 */
	accumulator = 0;
	held = 0;
	position = 0;
	for (index = 0; index < F25519_LIMBS; index++) {
		while (held < limb_bits[index]) {
			byte_value = in[position];
			if (position == 31U)
				byte_value &= 0x7fU;
			accumulator |= (uint64_t)byte_value << held;
			held += 8U;
			position++;
		}

		/* Cuts the limb off the accumulator. */
		out->limb[index] = (int32_t)(uint32_t)(accumulator & limb_mask[index]);
		accumulator >>= limb_bits[index];
		held -= limb_bits[index];
	}
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
	uint32_t limbs[F25519_LIMBS];
	uint64_t accumulator;
	uint32_t held;
	uint32_t position;
	unsigned int index;

	/* Finds the canonical value, one strict limb at a time. */
	f25519_canonical(limbs, in);

	/*
	 * Packs the limbs into bytes: each limb joins the accumulator above
	 * the bits already held, and every complete byte leaves from the
	 * bottom.  The ten limbs hold 255 bits, so 31 bytes leave in the loop
	 * and the last 7 bits make byte 31.
	 */
	accumulator = 0;
	held = 0;
	position = 0;
	for (index = 0; index < F25519_LIMBS; index++) {
		accumulator |= (uint64_t)limbs[index] << held;
		held += limb_bits[index];
		while (held >= 8U) {
			out[position] = (uint8_t)(accumulator & 0xffU);
			position++;
			accumulator >>= 8;
			held -= 8U;
		}
	}

	/* Writes the last partial byte. */
	out[position] = (uint8_t)(accumulator & 0xffU);

	/* The limbs are secret. */
	cryc_wipe(limbs, sizeof(limbs));
}

/*
 * out = 0.
 */
void
cryc_f25519_zero(
	struct cryc_f25519 *out)
{
	unsigned int index;

	/* Clears every limb. */
	for (index = 0; index < F25519_LIMBS; index++)
		out->limb[index] = 0;
}

/*
 * out = 1.
 */
void
cryc_f25519_one(
	struct cryc_f25519 *out)
{
	unsigned int index;

	/* Clears every limb, then sets the lowest. */
	for (index = 0; index < F25519_LIMBS; index++)
		out->limb[index] = 0;
	out->limb[0] = 1;
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
	uint32_t sum;
	unsigned int index;

	/*
	 * Adds limb by limb.  Reduced limbs are at most 2^26, so a sum is at
	 * most 2^27 and fits.
	 */
	for (index = 0; index < F25519_LIMBS; index++) {
		sum = (uint32_t)a->limb[index] + (uint32_t)b->limb[index];
		out->limb[index] = (int32_t)sum;
	}
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
	uint32_t difference;
	unsigned int index;

	/*
	 * Adds twice p before subtracting b.  A reduced limb of b is at most
	 * 2^bits and the matching limb of twice p is at least 2^bits, so the
	 * unsigned subtraction never wraps; the result limb is at most
	 * 2^bits + 2^(bits + 1) - 2, below 3 * 2^bits.
	 */
	for (index = 0; index < F25519_LIMBS; index++) {
		difference = (uint32_t)a->limb[index] + twice_p[index];
		difference -= (uint32_t)b->limb[index];
		out->limb[index] = (int32_t)difference;
	}
}

/*
 * out = -a (as 0 - a; loose result).
 */
void
cryc_f25519_neg(
	struct cryc_f25519 *out,
	const struct cryc_f25519 *a)
{
	struct cryc_f25519 zero;

	/* Subtracts from zero. */
	cryc_f25519_zero(&zero);
	cryc_f25519_sub(out, &zero, a);
}

/*
 * Carries a loose element back to reduced form.
 */
void
cryc_f25519_carry(
	struct cryc_f25519 *out,
	const struct cryc_f25519 *a)
{
	uint64_t wide[F25519_LIMBS];
	unsigned int index;

	/* Widens the limbs so the carries have room. */
	for (index = 0; index < F25519_LIMBS; index++)
		wide[index] = (uint64_t)(uint32_t)a->limb[index];

	/* Carries and stores the reduced result. */
	f25519_carry_wide(out, wide);

	/* The limbs are secret. */
	cryc_wipe(wide, sizeof(wide));
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
	uint64_t wide[F25519_LIMBS];
	uint64_t product;
	uint32_t factor;
	unsigned int left;
	unsigned int right;
	unsigned int position;

	/* Starts every position of the wide product at zero. */
	for (position = 0; position < F25519_LIMBS; position++)
		wide[position] = 0;

	/*
	 * Adds the product of every pair of limbs at its position.  The
	 * factor is 2 when both limbs are odd (their weights fall one bit
	 * short of the weight of the sum position) and is multiplied by 19
	 * when the position runs past limb 9 (2^255 = 19).  The loop runs
	 * over public indices only.
	 */
	for (left = 0; left < F25519_LIMBS; left++) {
		for (right = 0; right < F25519_LIMBS; right++) {
			product = (uint64_t)(uint32_t)a->limb[left] * (uint64_t)(uint32_t)b->limb[right];
			factor = 1U;
			if (((left & right) & 1U) != 0U)
				factor = 2U;
			position = left + right;
			if (position >= F25519_LIMBS) {
				position -= F25519_LIMBS;
				factor *= 19U;
			}

			/* Adds the weighted product into its limb. */
			wide[position] += product * factor;
		}
	}

	/* Carries the wide product back to ten reduced limbs. */
	f25519_carry_wide(out, wide);

	/* The product is secret. */
	cryc_wipe(wide, sizeof(wide));
}

/*
 * out = a * a; as cryc_f25519_mul.
 */
void
cryc_f25519_square(
	struct cryc_f25519 *out,
	const struct cryc_f25519 *a)
{
	/* A square is a product of the element with itself. */
	cryc_f25519_mul(out, a, a);
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
	uint64_t wide[F25519_LIMBS];
	unsigned int index;

	/*
	 * Multiplies each limb.  A loose limb is below 3 * 2^26 and small is
	 * below 2^17, so a product is below 3 * 2^43.
	 */
	for (index = 0; index < F25519_LIMBS; index++)
		wide[index] = (uint64_t)(uint32_t)a->limb[index] * (uint64_t)small;

	/* Carries back to reduced limbs. */
	f25519_carry_wide(out, wide);

	/* The product is secret. */
	cryc_wipe(wide, sizeof(wide));
}

/*
 * out = a^(p - 2) = a^-1 (0 for a = 0), a fixed chain of squarings and
 * multiplications (RFC 7748 5).
 *
 * p - 2 = 2^255 - 21 = (2^250 - 1) * 2^5 + 11, so the chain builds
 * a^(2^250 - 1), squares it five times and multiplies by a^11.
 */
void
cryc_f25519_invert(
	struct cryc_f25519 *out,
	const struct cryc_f25519 *a)
{
	struct cryc_f25519 x250;
	struct cryc_f25519 a2;
	struct cryc_f25519 a3;
	struct cryc_f25519 a8;
	struct cryc_f25519 a11;
	struct cryc_f25519 result;
	struct cryc_f25519 copy;

	/* Builds a^(2^250 - 1); copy receives a copy of the input. */
	f25519_chain_250(&x250, &copy, a);

	/* Builds a^2, a^3, a^8 and a^11 = a^8 * a^3. */
	cryc_f25519_square(&a2, &copy);
	cryc_f25519_mul(&a3, &a2, &copy);
	f25519_square_times(&a8, &a2, 2U);
	cryc_f25519_mul(&a11, &a8, &a3);

	/* Raises a^(2^250 - 1) to the 32nd power and multiplies by a^11. */
	f25519_square_times(&result, &x250, 5U);
	cryc_f25519_mul(out, &result, &a11);

	/* The intermediate powers are secret. */
	cryc_wipe(&x250, sizeof(x250));
	cryc_wipe(&a2, sizeof(a2));
	cryc_wipe(&a3, sizeof(a3));
	cryc_wipe(&a8, sizeof(a8));
	cryc_wipe(&a11, sizeof(a11));
	cryc_wipe(&result, sizeof(result));
	cryc_wipe(&copy, sizeof(copy));
}

/*
 * out = a^((p - 5) / 8) = a^(2^252 - 3), the exponentiation of RFC 8032
 * 5.1.3 step 2's square root candidate, a fixed chain.
 *
 * 2^252 - 3 = (2^250 - 1) * 4 + 1.
 */
void
cryc_f25519_pow_p58(
	struct cryc_f25519 *out,
	const struct cryc_f25519 *a)
{
	struct cryc_f25519 x250;
	struct cryc_f25519 result;
	struct cryc_f25519 copy;

	/* Builds a^(2^250 - 1); copy receives a copy of the input. */
	f25519_chain_250(&x250, &copy, a);

	/* Raises it to the 4th power and multiplies by a. */
	f25519_square_times(&result, &x250, 2U);
	cryc_f25519_mul(out, &result, &copy);

	/* The intermediate powers are secret. */
	cryc_wipe(&x250, sizeof(x250));
	cryc_wipe(&result, sizeof(result));
	cryc_wipe(&copy, sizeof(copy));
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
	uint32_t mask;
	uint32_t difference;
	unsigned int index;

	/* All ones when swap is 1, all zeros when it is 0 (hidden from the optimizer). */
	mask = f25519_opaque(0U - swap);

	/*
	 * Exchanges the limbs under the mask: the masked difference of the
	 * two values is exclusive-ored into both, or zero is.
	 */
	for (index = 0; index < F25519_LIMBS; index++) {
		difference = mask & ((uint32_t)a->limb[index] ^ (uint32_t)b->limb[index]);
		a->limb[index] = (int32_t)((uint32_t)a->limb[index] ^ difference);
		b->limb[index] = (int32_t)((uint32_t)b->limb[index] ^ difference);
	}
}

/*
 * Returns 0xffffffff when a (reduced or loose) is 0 modulo p, else 0.
 */
uint32_t
cryc_f25519_is_zero(
	const struct cryc_f25519 *a)
{
	uint8_t bytes[32];
	uint32_t folded;
	uint32_t mask;
	unsigned int index;

	/* The canonical encoding is all zeros exactly for a multiple of p. */
	cryc_f25519_encode(bytes, a);

	/* Ors every byte, so the loop reads all 32 whatever they hold. */
	folded = 0U;
	for (index = 0; index < 32U; index++)
		folded |= (uint32_t)bytes[index];

	/* All ones when nothing was set. */
	mask = cryc_ct_mask_zero32(folded);

	/* The encoding is secret. */
	cryc_wipe(bytes, sizeof(bytes));

	/* Reports the mask of "is zero". */
	return mask;
}

/*
 * Returns the low bit of a's canonical value (RFC 8032 5.1.2's x_0,
 * "negative").
 */
uint32_t
cryc_f25519_is_negative(
	const struct cryc_f25519 *a)
{
	uint8_t bytes[32];
	uint32_t low_bit;

	/* The canonical encoding's lowest byte holds the lowest bit. */
	cryc_f25519_encode(bytes, a);
	low_bit = (uint32_t)bytes[0] & 1U;

	/* The encoding is secret. */
	cryc_wipe(bytes, sizeof(bytes));

	/* Reports the low bit. */
	return low_bit;
}

/*
 * Brings ten wide positions down to a reduced element and stores it.
 *
 * Every position must be below 2^63.  The first pass carries each
 * position into the next with the exact widths; the carry out of
 * limb 9 stands for 2^255 and returns as 19 times itself to limb 0.  That
 * carry is below 2^63 / 2^25 = 2^38, so limb 0 gains less than 2^43.  The
 * second step carries limb 0 into limb 1 (at most 2^17) and limb 1 into
 * limb 2 (at most 1): limbs 0 and 1 are then below 2^26 and 2^25, limb 2
 * is at most 2^26, and every other limb is below its width, which is the
 * reduced form.
 */
static void
f25519_carry_wide(
	struct cryc_f25519 *out,
	uint64_t *wide)
{
	uint64_t carry;
	unsigned int index;

	/* Runs the first pass with the wrap of the top carry. */
	f25519_carry_strict(wide);

	/* Carries limb 0 into limb 1 and limb 1 into limb 2. */
	carry = wide[0] >> 26;
	wide[0] &= 0x3ffffffU;
	wide[1] += carry;
	carry = wide[1] >> 25;
	wide[1] &= 0x1ffffffU;
	wide[2] += carry;

	/* Stores the limbs; each is at most 2^26 and fits. */
	for (index = 0; index < F25519_LIMBS; index++)
		out->limb[index] = (int32_t)(uint32_t)wide[index];
}

/*
 * One pass of exact carries over ten wide positions, ending with the top
 * carry folded back into limb 0 as 19 times itself (2^255 = 19).
 *
 * Positions 1 to 9 end below their width.  Position 0 ends below
 * 2^26 + 19 * (the top carry); the callers handle that.
 */
static void
f25519_carry_strict(
	uint64_t *wide)
{
	uint64_t carry;
	unsigned int index;

	/* Carries from limb 0 up to limb 9. */
	carry = 0;
	for (index = 0; index < F25519_LIMBS; index++) {
		wide[index] += carry;
		carry = wide[index] >> limb_bits[index];
		wide[index] &= (uint64_t)limb_mask[index];
	}

	/* Folds the carry out of limb 9 back into limb 0. */
	wide[0] += carry * 19U;
}

/*
 * Finds the canonical value of an element as ten strict limbs.
 *
 * Two exact carry passes bring any reduced or loose element to a value
 * below 2^255 with every limb below its width (the first leaves at most
 * 19 * 3 = 57 extra in limb 0, the second absorbs that, and a carry out of
 * limb 9 then means the rest of the value is below 57 so adding 19 to it
 * never carries again).  The value v is below 2^255, so v >= p exactly
 * when v + 19 reaches 2^255: the carry chain of v + 19 gives that bit,
 * 19 times it is added, and the carry out of limb 9 (the 2^255 bit) is
 * dropped.
 */
static void
f25519_canonical(
	uint32_t *limbs,
	const struct cryc_f25519 *in)
{
	uint64_t wide[F25519_LIMBS];
	uint64_t carry;
	unsigned int index;

	/* Widens the limbs. */
	for (index = 0; index < F25519_LIMBS; index++)
		wide[index] = (uint64_t)(uint32_t)in->limb[index];

	/* Runs two exact passes. */
	f25519_carry_strict(wide);
	f25519_carry_strict(wide);

	/* Finds whether the value reaches p: the carry out of value + 19. */
	carry = (wide[0] + 19U) >> limb_bits[0];
	for (index = 1; index < F25519_LIMBS; index++)
		carry = (wide[index] + carry) >> limb_bits[index];

	/* Adds 19 when the value reached p, which wraps it below p. */
	wide[0] += carry * 19U;

	/* Carries exactly once more, dropping the carry out of limb 9. */
	carry = 0;
	for (index = 0; index < F25519_LIMBS; index++) {
		wide[index] += carry;
		carry = wide[index] >> limb_bits[index];
		wide[index] &= (uint64_t)limb_mask[index];
	}

	/* Hands over the strict limbs. */
	for (index = 0; index < F25519_LIMBS; index++)
		limbs[index] = (uint32_t)wide[index];

	/* The limbs are secret. */
	cryc_wipe(wide, sizeof(wide));
}

/*
 * out = in^(2^count), by count squarings.
 *
 * count is at least 1.  out may be the same as in.
 */
static void
f25519_square_times(
	struct cryc_f25519 *out,
	const struct cryc_f25519 *in,
	unsigned int count)
{
	unsigned int index;

	/* The first squaring reads in. */
	cryc_f25519_square(out, in);

	/* The others square the running value. */
	for (index = 1; index < count; index++)
		cryc_f25519_square(out, out);
}

/*
 * Builds a^(2^250 - 1) with a fixed addition chain.
 *
 * With x_k = a^(2^k - 1), x_(m + n) = x_m^(2^n) * x_n, because
 * (2^m - 1) * 2^n + 2^n - 1 = 2^(m + n) - 1.  The chain is
 * x1, x2, x4, x5, x10, x20, x40, x50, x100, x200, x250, and the steps
 * are x2 = x1^2 * x1, x4 = x2^4 * x2, x5 = x4^2 * x1, x10 = x5^32 * x5,
 * x20 = x10^1024 * x10, x40 = x20^(2^20) * x20, x50 = x40^1024 * x10,
 * x100 = x50^(2^50) * x50, x200 = x100^(2^100) * x100 and
 * x250 = x200^(2^50) * x50.
 *
 * x1_out receives a copy of the input (a may alias an output); the
 * caller wipes it.
 */
static void
f25519_chain_250(
	struct cryc_f25519 *x250,
	struct cryc_f25519 *x1_out,
	const struct cryc_f25519 *a)
{
	struct cryc_f25519 x2;
	struct cryc_f25519 x4;
	struct cryc_f25519 x5;
	struct cryc_f25519 x10;
	struct cryc_f25519 x20;
	struct cryc_f25519 x40;
	struct cryc_f25519 x50;
	struct cryc_f25519 x100;
	struct cryc_f25519 x200;
	struct cryc_f25519 work;

	/* Keeps a copy of the input. */
	*x1_out = *a;

	/* x2 = a^3, x4 = a^15 and x5 = a^31. */
	cryc_f25519_square(&work, x1_out);
	cryc_f25519_mul(&x2, &work, x1_out);
	f25519_square_times(&work, &x2, 2U);
	cryc_f25519_mul(&x4, &work, &x2);
	cryc_f25519_square(&work, &x4);
	cryc_f25519_mul(&x5, &work, x1_out);

	/* x10, x20, x40 and x50. */
	f25519_square_times(&work, &x5, 5U);
	cryc_f25519_mul(&x10, &work, &x5);
	f25519_square_times(&work, &x10, 10U);
	cryc_f25519_mul(&x20, &work, &x10);
	f25519_square_times(&work, &x20, 20U);
	cryc_f25519_mul(&x40, &work, &x20);
	f25519_square_times(&work, &x40, 10U);
	cryc_f25519_mul(&x50, &work, &x10);

	/* x100, x200 and x250. */
	f25519_square_times(&work, &x50, 50U);
	cryc_f25519_mul(&x100, &work, &x50);
	f25519_square_times(&work, &x100, 100U);
	cryc_f25519_mul(&x200, &work, &x100);
	f25519_square_times(&work, &x200, 50U);
	cryc_f25519_mul(x250, &work, &x50);

	/* The intermediate powers are secret. */
	cryc_wipe(&x2, sizeof(x2));
	cryc_wipe(&x4, sizeof(x4));
	cryc_wipe(&x5, sizeof(x5));
	cryc_wipe(&x10, sizeof(x10));
	cryc_wipe(&x20, sizeof(x20));
	cryc_wipe(&x40, sizeof(x40));
	cryc_wipe(&x50, sizeof(x50));
	cryc_wipe(&x100, sizeof(x100));
	cryc_wipe(&x200, sizeof(x200));
	cryc_wipe(&work, sizeof(work));
}

/*
 * Returns value through a volatile object, so that the compiler cannot
 * know which of the values of a mask (0 or 0xffffffff) it holds and turn
 * a selection by it into a branch or a choice of load.
 */
static uint32_t
f25519_opaque(
	uint32_t value)
{
	volatile uint32_t hidden;

	/* Stored and loaded again. */
	hidden = value;
	return hidden;
}
