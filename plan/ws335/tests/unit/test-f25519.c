/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Checks the field arithmetic modulo 2^255 - 19 (cryc-f25519.h).
 *
 * Expected values come from a reference written here from the definition
 * of the field alone: numbers of 16-bit digits (a schoolbook sum,
 * difference and product, and a reduction that folds 2^256 as 38 and then
 * subtracts p), the value of an element as the sum of limb * 2^(position
 * of the limb), and square-and-multiply for the exponents p - 2 and
 * (p - 5) / 8 (RFC 7748 4.1, 5; RFC 8032 5.1.3).  The inputs are random
 * limbs within the stated ranges (reduced: at most 2^width; loose: below
 * 3 * 2^width) and the boundary values: zero, one, p - 1, p, p + 1,
 * 2^255 - 1, and every limb at its largest.  Each result is checked for
 * its value and for its range.
 */

#include "check.h"

#include "userland/base/libcrypto-compat/cryc-f25519.h"

/* The digits of a reference number (16 bits each), enough for a 512-bit product and the carries. */
#define BIG_DIGITS	36U

/* The number of random cases of each operation. */
#define RANDOM_CASES	400U

/* A reference number: little-endian digits of 16 bits, kept in uint64_t (a row of products does not overflow). */
struct big {
	uint64_t digit[BIG_DIGITS];
};

/* The state of the pseudo-random generator of the tests. */
static uint32_t rng_state = 0x2545f491U;

/* The width in bits of limb i. */
static unsigned
limb_width(
	unsigned i)
{
	/* Even limbs carry 26 bits, odd limbs 25. */
	return (i & 1U) == 0U ? 26U : 25U;
}

/* The bit position of limb i: ceil(25.5 * i). */
static unsigned
limb_position(
	unsigned i)
{
	/* 26 * ceil(i / 2) + 25 * floor(i / 2). */
	return 26U * ((i + 1U) / 2U) + 25U * (i / 2U);
}

/* Returns the next pseudo-random 32-bit value (xorshift). */
static uint32_t
random32(void)
{
	/* Marsaglia's xorshift32 with the shifts 13, 17, 5. */
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 17;
	rng_state ^= rng_state << 5;
	return rng_state;
}

/* Sets a number to zero. */
static void
big_zero(
	struct big *n)
{
	unsigned i;

	/* Every digit. */
	for (i = 0; i < BIG_DIGITS; i++)
		n->digit[i] = 0;
}

/* Sets n to the value of a small number. */
static void
big_set(
	struct big *n,
	uint32_t value)
{
	/* Two digits. */
	big_zero(n);
	n->digit[0] = value & 0xffffU;
	n->digit[1] = value >> 16;
}

/* Normalizes the carries so each digit is below 2^16. */
static void
big_normalize(
	struct big *n)
{
	uint64_t carry;
	unsigned i;

	/* One pass from the bottom. */
	carry = 0;
	for (i = 0; i < BIG_DIGITS; i++) {
		n->digit[i] += carry;
		carry = n->digit[i] >> 16;
		n->digit[i] &= 0xffffU;
	}
}

/* Returns 1 when the digits from first up are all zero. */
static int
big_high_is_zero(
	const struct big *n,
	unsigned first)
{
	unsigned i;

	/* Any non-zero digit. */
	for (i = first; i < BIG_DIGITS; i++)
		if (n->digit[i] != 0U)
			return 0;
	return 1;
}

/* Sets n to p = 2^255 - 19. */
static void
big_p(
	struct big *n)
{
	unsigned i;

	/* 2^255 - 19: digit 0 is 0xffed, digits 1 to 14 are 0xffff, digit 15 is 0x7fff. */
	big_zero(n);
	for (i = 1; i < 15U; i++)
		n->digit[i] = 0xffffU;
	n->digit[0] = 0xffedU;
	n->digit[15] = 0x7fffU;
}

/* Returns 1 when the number is at least p (both normalized, the number below 2^256). */
static int
big_ge_p(
	const struct big *n)
{
	struct big p;
	unsigned i;

	/* Compares from the top. */
	big_p(&p);
	for (i = BIG_DIGITS; i > 0; i--) {
		if (n->digit[i - 1U] > p.digit[i - 1U])
			return 1;
		if (n->digit[i - 1U] < p.digit[i - 1U])
			return 0;
	}
	return 1;
}

/* Subtracts p from a normalized number that is at least p. */
static void
big_sub_p(
	struct big *n)
{
	struct big p;
	int32_t borrow;
	int32_t value;
	unsigned i;

	/* Digit by digit with a borrow. */
	big_p(&p);
	borrow = 0;
	for (i = 0; i < BIG_DIGITS; i++) {
		value = (int32_t)n->digit[i] - (int32_t)p.digit[i] - borrow;
		borrow = 0;
		if (value < 0) {
			value += 0x10000;
			borrow = 1;
		}
		n->digit[i] = (uint32_t)value;
	}
}

/* Reduces a number modulo p to [0, p - 1]. */
static void
big_reduce(
	struct big *n)
{
	struct big fold;
	unsigned i;

	/* Folds 2^256 = 38 (mod p) until nothing is left above 256 bits. */
	big_normalize(n);
	while (!big_high_is_zero(n, 16U)) {
		fold = *n;
		for (i = 16U; i < BIG_DIGITS; i++)
			n->digit[i] = 0;
		for (i = 16U; i < BIG_DIGITS; i++)
			n->digit[i - 16U] += fold.digit[i] * 38U;
		big_normalize(n);
	}

	/* 2^256 is 2p + 38, so at most two subtractions. */
	while (big_ge_p(n))
		big_sub_p(n);
}

/* out = a * b modulo p (a and b normalized, below 2^256). */
static void
big_mul(
	struct big *out,
	const struct big *a,
	const struct big *b)
{
	struct big product;
	unsigned i;
	unsigned j;

	/* Schoolbook: every partial product below 2^32, accumulated with a normalize per row. */
	big_zero(&product);
	for (i = 0; i < 16U; i++) {
		for (j = 0; j < 16U; j++)
			product.digit[i + j] += a->digit[i] * b->digit[j];
		big_normalize(&product);
	}
	big_reduce(&product);
	*out = product;
}

/* out = a + b modulo p. */
static void
big_add(
	struct big *out,
	const struct big *a,
	const struct big *b)
{
	struct big sum;
	unsigned i;

	/* Digit sums, then the reduction. */
	big_zero(&sum);
	for (i = 0; i < 16U; i++)
		sum.digit[i] = a->digit[i] + b->digit[i];
	big_reduce(&sum);
	*out = sum;
}

/* out = a - b modulo p (a + p - b). */
static void
big_sub(
	struct big *out,
	const struct big *a,
	const struct big *b)
{
	struct big p;
	struct big result;
	int32_t borrow;
	int32_t value;
	unsigned i;

	/* a + p first (below 2^257), then the subtraction with a borrow: never negative. */
	big_p(&p);
	big_zero(&result);
	for (i = 0; i < 16U; i++)
		result.digit[i] = a->digit[i] + p.digit[i];
	big_normalize(&result);
	borrow = 0;
	for (i = 0; i < BIG_DIGITS; i++) {
		value = (int32_t)result.digit[i] - (int32_t)(i < 16U ? b->digit[i] : 0U) - borrow;
		borrow = 0;
		if (value < 0) {
			value += 0x10000;
			borrow = 1;
		}
		result.digit[i] = (uint32_t)value;
	}
	big_reduce(&result);
	*out = result;
}

/* Returns 1 when two reduced numbers are equal. */
static int
big_equal(
	const struct big *a,
	const struct big *b)
{
	unsigned i;

	/* Every digit. */
	for (i = 0; i < 16U; i++)
		if (a->digit[i] != b->digit[i])
			return 0;
	return 1;
}

/* Sets a number from 32 little-endian bytes (unreduced). */
static void
big_from_bytes(
	struct big *n,
	const uint8_t *bytes)
{
	unsigned i;

	/* Two bytes per digit. */
	big_zero(n);
	for (i = 0; i < 16U; i++)
		n->digit[i] = (uint64_t)bytes[2U * i] | ((uint64_t)bytes[2U * i + 1U] << 8);
}

/* Writes a reduced number as 32 little-endian bytes. */
static void
big_to_bytes(
	uint8_t *bytes,
	const struct big *n)
{
	unsigned i;

	/* Two bytes per digit. */
	for (i = 0; i < 16U; i++) {
		bytes[2U * i] = (uint8_t)(n->digit[i] & 0xffU);
		bytes[2U * i + 1U] = (uint8_t)(n->digit[i] >> 8);
	}
}

/* Returns the value of an element (any limbs) modulo p. */
static void
big_from_element(
	struct big *out,
	const struct cryc_f25519 *e)
{
	struct big total;
	uint64_t shifted;
	unsigned i;
	unsigned digit;
	unsigned shift;
	uint32_t part;

	/*
	 * Adds limb * 2^position for each limb: the limb is below 2^31, so
	 * shifted by the position within a digit it spans at most three
	 * digits.
	 */
	big_zero(&total);
	for (i = 0; i < 10U; i++) {
		digit = limb_position(i) / 16U;
		shift = limb_position(i) % 16U;
		shifted = (uint64_t)(uint32_t)e->limb[i] << shift;
		part = (uint32_t)(shifted & 0xffffU);
		total.digit[digit] += part;
		part = (uint32_t)((shifted >> 16) & 0xffffU);
		total.digit[digit + 1U] += part;
		part = (uint32_t)((shifted >> 32) & 0xffffU);
		total.digit[digit + 2U] += part;
		big_normalize(&total);
	}
	big_reduce(&total);
	*out = total;
}

/* Returns 1 when every limb of the element is within 0 to bound_factor * 2^width (inclusive or below). */
static int
element_within(
	const struct cryc_f25519 *e,
	unsigned factor,
	int inclusive)
{
	uint64_t bound;
	uint64_t value;
	unsigned i;

	/* Each limb against its width. */
	for (i = 0; i < 10U; i++) {
		if (e->limb[i] < 0)
			return 0;
		value = (uint64_t)(uint32_t)e->limb[i];
		bound = (uint64_t)factor << limb_width(i);
		if (inclusive ? value > bound : value >= bound)
			return 0;
	}
	return 1;
}

/* Returns 1 when the element is reduced: every limb at most 2^width. */
static int
is_reduced(
	const struct cryc_f25519 *e)
{
	/* Inclusive bound 2^width. */
	return element_within(e, 1U, 1);
}

/* Returns 1 when the element is loose: every limb below 3 * 2^width. */
static int
is_loose(
	const struct cryc_f25519 *e)
{
	/* Strict bound 3 * 2^width. */
	return element_within(e, 3U, 0);
}

/*
 * Makes a random element whose limbs are at most factor * 2^width (just
 * below it when strict), with a bias to the boundary values.
 */
static void
random_element(
	struct cryc_f25519 *e,
	unsigned factor,
	int inclusive)
{
	uint64_t bound;
	uint32_t pick;
	unsigned i;

	/* Each limb: sometimes zero, sometimes the largest, sometimes the largest minus one, else random. */
	for (i = 0; i < 10U; i++) {
		bound = (uint64_t)factor << limb_width(i);
		if (!inclusive)
			bound -= 1U;
		pick = random32() % 8U;
		if (pick == 0U)
			e->limb[i] = 0;
		else if (pick == 1U)
			e->limb[i] = (int32_t)bound;
		else if (pick == 2U)
			e->limb[i] = (int32_t)(bound - 1U);
		else
			e->limb[i] = (int32_t)(random32() % (uint32_t)(bound + 1U));
	}
}

/* Sets an element with every limb at the largest the range allows. */
static void
max_element(
	struct cryc_f25519 *e,
	unsigned factor,
	int inclusive)
{
	uint64_t bound;
	unsigned i;

	/* All at the bound. */
	for (i = 0; i < 10U; i++) {
		bound = (uint64_t)factor << limb_width(i);
		if (!inclusive)
			bound -= 1U;
		e->limb[i] = (int32_t)bound;
	}
}

/* Checks that the element has the value of the number. */
static int
value_is(
	const struct cryc_f25519 *e,
	const struct big *expected)
{
	struct big got;

	/* The element's value against the number. */
	big_from_element(&got, e);
	return big_equal(&got, expected);
}

/* Checks decode and encode on the 32-byte input. */
static void
check_codec(
	const uint8_t *bytes)
{
	struct cryc_f25519 e;
	struct big n;
	struct big expected;
	uint8_t masked[32];
	uint8_t out[32];

	/* decode ignores the top bit and reduces; the result is reduced. */
	memcpy(masked, bytes, 32U);
	masked[31] &= 0x7fU;
	big_from_bytes(&n, masked);
	big_reduce(&n);
	expected = n;
	cryc_f25519_decode(&e, bytes);
	CHECK(value_is(&e, &expected));
	CHECK(is_reduced(&e));

	/* encode writes the canonical value. */
	cryc_f25519_encode(out, &e);
	big_to_bytes(masked, &expected);
	CHECK_BYTES(out, masked, 32U);
}

/* Adds a small signed amount to the 32 bytes, as a little-endian number (no carry out of the top byte for the values used). */
static void
bytes_add(
	uint8_t *bytes,
	int delta)
{
	int carry;
	int value;
	unsigned i;

	/* Byte by byte with a carry that may be negative. */
	carry = delta;
	for (i = 0; i < 32U; i++) {
		value = (int)bytes[i] + carry;
		carry = 0;
		while (value < 0) {
			value += 256;
			carry -= 1;
		}
		while (value > 255) {
			value -= 256;
			carry += 1;
		}
		bytes[i] = (uint8_t)value;
	}
}

/* The 32-byte boundary inputs of the codec. */
static void
check_codec_boundaries(void)
{
	uint8_t bytes[32];
	int delta;

	/* 0, 1, 2^255 - 1 (top bit clear), and the same with the top bit set. */
	memset(bytes, 0, sizeof(bytes));
	check_codec(bytes);
	bytes[0] = 1U;
	check_codec(bytes);
	memset(bytes, 0xff, sizeof(bytes));
	bytes[31] = 0x7fU;
	check_codec(bytes);
	bytes[31] = 0xffU;
	check_codec(bytes);

	/* p - 24 to p + 39: every encoding near p is accepted, including p to 2^255 - 1 and the ones that carry into the top bit. */
	for (delta = -24; delta < 40; delta++) {
		memset(bytes, 0xff, sizeof(bytes));
		bytes[0] = 0xedU;
		bytes[31] = 0x7fU;
		bytes_add(bytes, delta);
		check_codec(bytes);
	}

	/* 2^255 - 19 + 19 = 2^255: the top bit alone is ignored, giving 0. */
	memset(bytes, 0, sizeof(bytes));
	bytes[31] = 0x80U;
	check_codec(bytes);
}

/* Checks encode of elements, loose and reduced, including p and 2p as limbs. */
static void
check_encode_elements(void)
{
	struct cryc_f25519 e;
	struct big expected;
	uint8_t out[32];
	uint8_t want[32];
	unsigned i;
	unsigned round;

	/* Random reduced and loose elements. */
	for (round = 0; round < RANDOM_CASES; round++) {
		random_element(&e, round % 2U == 0U ? 1U : 3U, round % 2U == 0U);
		big_from_element(&expected, &e);
		big_to_bytes(want, &expected);
		cryc_f25519_encode(out, &e);
		CHECK_BYTES(out, want, 32U);
	}

	/* Every limb at its largest, reduced and loose. */
	max_element(&e, 1U, 1);
	big_from_element(&expected, &e);
	big_to_bytes(want, &expected);
	cryc_f25519_encode(out, &e);
	CHECK_BYTES(out, want, 32U);
	max_element(&e, 3U, 0);
	big_from_element(&expected, &e);
	big_to_bytes(want, &expected);
	cryc_f25519_encode(out, &e);
	CHECK_BYTES(out, want, 32U);

	/* The limbs of p itself (all full, limb 0 short by 19): the encoding is zero. */
	for (i = 0; i < 10U; i++)
		e.limb[i] = (int32_t)((1U << limb_width(i)) - 1U);
	e.limb[0] -= 18;
	memset(want, 0, sizeof(want));
	cryc_f25519_encode(out, &e);
	CHECK_BYTES(out, want, 32U);

	/* p + 1 and p - 1 as limbs. */
	e.limb[0] += 1;
	want[0] = 1U;
	cryc_f25519_encode(out, &e);
	CHECK_BYTES(out, want, 32U);
	e.limb[0] -= 2;
	memset(want, 0xff, sizeof(want));
	want[0] = 0xecU;
	want[31] = 0x7fU;
	cryc_f25519_encode(out, &e);
	CHECK_BYTES(out, want, 32U);

	/* 2p as limbs (a loose zero). */
	for (i = 0; i < 10U; i++)
		e.limb[i] = (int32_t)((2U << limb_width(i)) - 2U);
	e.limb[0] -= 36;
	memset(want, 0, sizeof(want));
	cryc_f25519_encode(out, &e);
	CHECK_BYTES(out, want, 32U);
}

/* Checks zero, one, is_zero and is_negative. */
static void
check_predicates(void)
{
	struct cryc_f25519 e;
	struct cryc_f25519 f;
	struct big n;
	uint8_t bytes[32];
	unsigned round;
	unsigned i;

	/* zero and one. */
	cryc_f25519_zero(&e);
	CHECK(cryc_f25519_is_zero(&e) == 0xffffffffU);
	CHECK(cryc_f25519_is_negative(&e) == 0U);
	cryc_f25519_one(&e);
	CHECK(cryc_f25519_is_zero(&e) == 0U);
	CHECK(cryc_f25519_is_negative(&e) == 1U);
	big_set(&n, 1U);
	CHECK(value_is(&e, &n));

	/* p and 2p written as limbs are zero; p - 1 is not, and is "positive" (even). */
	for (i = 0; i < 10U; i++)
		e.limb[i] = (int32_t)((1U << limb_width(i)) - 1U);
	e.limb[0] -= 18;
	CHECK(cryc_f25519_is_zero(&e) == 0xffffffffU);
	CHECK(cryc_f25519_is_negative(&e) == 0U);
	e.limb[0] -= 1;
	CHECK(cryc_f25519_is_zero(&e) == 0U);
	CHECK(cryc_f25519_is_negative(&e) == 0U);
	for (i = 0; i < 10U; i++)
		e.limb[i] = (int32_t)((2U << limb_width(i)) - 2U);
	e.limb[0] -= 36;
	CHECK(cryc_f25519_is_zero(&e) == 0xffffffffU);

	/* Random elements: the low bit of the canonical value. */
	for (round = 0; round < RANDOM_CASES; round++) {
		random_element(&f, 3U, 0);
		big_from_element(&n, &f);
		big_to_bytes(bytes, &n);
		CHECK(cryc_f25519_is_negative(&f) == (uint32_t)(bytes[0] & 1U));
		CHECK(cryc_f25519_is_zero(&f) == (big_high_is_zero(&n, 0U) ? 0xffffffffU : 0U));
	}
}

/* Checks the additive operations and the carry. */
static void
check_additive(void)
{
	struct cryc_f25519 a;
	struct cryc_f25519 b;
	struct cryc_f25519 r;
	struct cryc_f25519 copy;
	struct big na;
	struct big nb;
	struct big expected;
	struct big zero;
	unsigned round;

	big_zero(&zero);

	/* Random reduced operands, plus every limb at its largest. */
	for (round = 0; round < RANDOM_CASES; round++) {
		if (round == 0U) {
			max_element(&a, 1U, 1);
			max_element(&b, 1U, 1);
		} else if (round == 1U) {
			max_element(&a, 1U, 1);
			cryc_f25519_zero(&b);
		} else if (round == 2U) {
			cryc_f25519_zero(&a);
			max_element(&b, 1U, 1);
		} else {
			random_element(&a, 1U, 1);
			random_element(&b, 1U, 1);
		}
		big_from_element(&na, &a);
		big_from_element(&nb, &b);

		/* add: value and loose range; the carry then makes it reduced. */
		cryc_f25519_add(&r, &a, &b);
		big_add(&expected, &na, &nb);
		CHECK(value_is(&r, &expected));
		CHECK(is_loose(&r));
		copy = r;
		cryc_f25519_carry(&r, &copy);
		CHECK(value_is(&r, &expected));
		CHECK(is_reduced(&r));

		/* sub. */
		cryc_f25519_sub(&r, &a, &b);
		big_sub(&expected, &na, &nb);
		CHECK(value_is(&r, &expected));
		CHECK(is_loose(&r));
		copy = r;
		cryc_f25519_carry(&r, &copy);
		CHECK(value_is(&r, &expected));
		CHECK(is_reduced(&r));

		/* neg. */
		cryc_f25519_neg(&r, &a);
		big_sub(&expected, &zero, &na);
		CHECK(value_is(&r, &expected));
		CHECK(is_loose(&r));

		/* Aliases: out is a, out is b. */
		copy = a;
		cryc_f25519_add(&copy, &copy, &b);
		big_add(&expected, &na, &nb);
		CHECK(value_is(&copy, &expected));
		copy = b;
		cryc_f25519_add(&copy, &a, &copy);
		CHECK(value_is(&copy, &expected));
		copy = a;
		cryc_f25519_sub(&copy, &copy, &b);
		big_sub(&expected, &na, &nb);
		CHECK(value_is(&copy, &expected));
		copy = b;
		cryc_f25519_sub(&copy, &a, &copy);
		CHECK(value_is(&copy, &expected));
	}

	/* carry of loose elements: random and the largest. */
	for (round = 0; round < RANDOM_CASES; round++) {
		if (round == 0U)
			max_element(&a, 3U, 0);
		else
			random_element(&a, 3U, 0);
		big_from_element(&na, &a);
		cryc_f25519_carry(&r, &a);
		CHECK(value_is(&r, &na));
		CHECK(is_reduced(&r));
	}
}

/* Checks mul, square and mul_small. */
static void
check_multiplicative(void)
{
	struct cryc_f25519 a;
	struct cryc_f25519 b;
	struct cryc_f25519 r;
	struct cryc_f25519 copy;
	struct big na;
	struct big nb;
	struct big expected;
	struct big small;
	uint32_t smalls[8];
	unsigned round;
	unsigned factor;
	unsigned k;

	/* Operands reduced (factor 1) and loose (factor 3), random and at their largest. */
	for (round = 0; round < RANDOM_CASES * 2U; round++) {
		factor = round % 2U == 0U ? 1U : 3U;
		if (round < 4U) {
			max_element(&a, factor, factor == 1U);
			max_element(&b, factor, factor == 1U);
		} else {
			random_element(&a, factor, factor == 1U);
			random_element(&b, factor, factor == 1U);
		}
		big_from_element(&na, &a);
		big_from_element(&nb, &b);

		/* mul. */
		cryc_f25519_mul(&r, &a, &b);
		big_mul(&expected, &na, &nb);
		CHECK(value_is(&r, &expected));
		CHECK(is_reduced(&r));

		/* mul with out aliasing a, then b. */
		copy = a;
		cryc_f25519_mul(&copy, &copy, &b);
		CHECK(value_is(&copy, &expected));
		copy = b;
		cryc_f25519_mul(&copy, &a, &copy);
		CHECK(value_is(&copy, &expected));

		/* square, with out aliasing a. */
		cryc_f25519_square(&r, &a);
		big_mul(&expected, &na, &na);
		CHECK(value_is(&r, &expected));
		CHECK(is_reduced(&r));
		copy = a;
		cryc_f25519_square(&copy, &copy);
		CHECK(value_is(&copy, &expected));
	}

	/* mul_small for small below 2^17: 0, 1, 121665, 2^17 - 1 and random. */
	smalls[0] = 0U;
	smalls[1] = 1U;
	smalls[2] = 121665U;
	smalls[3] = 131071U;
	smalls[4] = 65536U;
	smalls[5] = 2U;
	for (round = 0; round < RANDOM_CASES; round++) {
		factor = round % 2U == 0U ? 1U : 3U;
		if (round < 4U)
			max_element(&a, factor, factor == 1U);
		else
			random_element(&a, factor, factor == 1U);
		smalls[6] = random32() % 131072U;
		smalls[7] = random32() % 131072U;
		big_from_element(&na, &a);
		for (k = 0; k < 8U; k++) {
			big_set(&small, smalls[k]);
			cryc_f25519_mul_small(&r, &a, smalls[k]);
			big_mul(&expected, &na, &small);
			CHECK(value_is(&r, &expected));
			CHECK(is_reduced(&r));
		}
		copy = a;
		cryc_f25519_mul_small(&copy, &copy, 121665U);
		big_set(&small, 121665U);
		big_mul(&expected, &na, &small);
		CHECK(value_is(&copy, &expected));
	}
}

/* Sets n to the exponent 2^252 - 3 = (p - 5) / 8. */
static void
exponent_p58(
	struct big *n)
{
	unsigned i;

	/* 2^252 - 3: digit 0 is 0xfffd, digits 1 to 14 are 0xffff, digit 15 is 0x0fff. */
	big_zero(n);
	for (i = 1; i < 15U; i++)
		n->digit[i] = 0xffffU;
	n->digit[0] = 0xfffdU;
	n->digit[15] = 0x0fffU;
}

/* Sets n to the exponent p - 2. */
static void
exponent_inverse(
	struct big *n)
{
	/* p - 2 = p with digit 0 lowered by 2. */
	big_p(n);
	n->digit[0] -= 2U;
}

/* out = base^exponent modulo p, square and multiply from the top bit. */
static void
big_pow(
	struct big *out,
	const struct big *base,
	const struct big *exponent)
{
	struct big result;
	int bit;

	/* Left to right over 256 bits. */
	big_set(&result, 1U);
	for (bit = 255; bit >= 0; bit--) {
		big_mul(&result, &result, &result);
		if (((exponent->digit[bit / 16] >> (bit % 16)) & 1U) != 0U)
			big_mul(&result, &result, base);
	}
	*out = result;
}

/* Checks invert and pow_p58. */
static void
check_powers(void)
{
	struct cryc_f25519 a;
	struct cryc_f25519 r;
	struct cryc_f25519 product;
	struct cryc_f25519 copy;
	struct big na;
	struct big expected;
	struct big exponent;
	struct big one;
	unsigned round;

	big_set(&one, 1U);
	for (round = 0; round < 40U; round++) {
		if (round == 0U)
			cryc_f25519_zero(&a);
		else if (round == 1U)
			cryc_f25519_one(&a);
		else if (round == 2U)
			max_element(&a, 1U, 1);
		else if (round == 3U)
			max_element(&a, 3U, 0);
		else
			random_element(&a, round % 2U == 0U ? 1U : 3U, round % 2U == 0U);
		big_from_element(&na, &a);

		/* invert: a^(p - 2), zero for zero, and a * a^-1 = 1 otherwise. */
		exponent_inverse(&exponent);
		big_pow(&expected, &na, &exponent);
		cryc_f25519_invert(&r, &a);
		CHECK(value_is(&r, &expected));
		CHECK(is_reduced(&r));
		cryc_f25519_mul(&product, &a, &r);
		if (cryc_f25519_is_zero(&a) == 0U)
			CHECK(value_is(&product, &one));
		else
			CHECK(cryc_f25519_is_zero(&product) == 0xffffffffU);
		copy = a;
		cryc_f25519_invert(&copy, &copy);
		CHECK(value_is(&copy, &expected));

		/* pow_p58: a^(2^252 - 3). */
		exponent_p58(&exponent);
		big_pow(&expected, &na, &exponent);
		cryc_f25519_pow_p58(&r, &a);
		CHECK(value_is(&r, &expected));
		CHECK(is_reduced(&r));
		copy = a;
		cryc_f25519_pow_p58(&copy, &copy);
		CHECK(value_is(&copy, &expected));
	}
}

/* Checks cswap. */
static void
check_cswap(void)
{
	struct cryc_f25519 a;
	struct cryc_f25519 b;
	struct cryc_f25519 a0;
	struct cryc_f25519 b0;
	unsigned round;

	/* 0 leaves, 1 exchanges. */
	for (round = 0; round < 50U; round++) {
		random_element(&a0, 3U, 0);
		random_element(&b0, 3U, 0);
		a = a0;
		b = b0;
		cryc_f25519_cswap(&a, &b, 0U);
		CHECK_BYTES(&a, &a0, sizeof(a));
		CHECK_BYTES(&b, &b0, sizeof(b));
		cryc_f25519_cswap(&a, &b, 1U);
		CHECK_BYTES(&a, &b0, sizeof(a));
		CHECK_BYTES(&b, &a0, sizeof(b));
	}
}

/* Chains many operations, feeding each result back, against the reference. */
static void
check_chains(void)
{
	struct cryc_f25519 x;
	struct cryc_f25519 y;
	struct cryc_f25519 t;
	struct big nx;
	struct big ny;
	struct big nt;
	unsigned round;

	/* x = (x + y) * (x - y) + 121665 * y, y = x * y, 1000 steps; limbs never leave their ranges. */
	random_element(&x, 1U, 1);
	random_element(&y, 1U, 1);
	big_from_element(&nx, &x);
	big_from_element(&ny, &y);
	for (round = 0; round < 1000U; round++) {
		struct cryc_f25519 sum;
		struct cryc_f25519 diff;
		struct cryc_f25519 prod;
		struct cryc_f25519 small;
		struct big nsum;
		struct big ndiff;
		struct big nsmall;
		struct big k;

		cryc_f25519_add(&sum, &x, &y);
		cryc_f25519_sub(&diff, &x, &y);
		cryc_f25519_mul(&prod, &sum, &diff);
		cryc_f25519_mul_small(&small, &y, 121665U);
		cryc_f25519_add(&t, &prod, &small);
		cryc_f25519_mul(&y, &x, &y);
		cryc_f25519_carry(&x, &t);
		big_add(&nsum, &nx, &ny);
		big_sub(&ndiff, &nx, &ny);
		big_mul(&nt, &nsum, &ndiff);
		big_set(&k, 121665U);
		big_mul(&nsmall, &ny, &k);
		big_add(&nt, &nt, &nsmall);
		big_mul(&ny, &nx, &ny);
		nx = nt;
		if (!CHECK(value_is(&x, &nx) && value_is(&y, &ny) && is_reduced(&x) && is_reduced(&y)))
			break;
	}
}

/* Runs the checks. */
int
main(void)
{
	unsigned round;
	uint8_t bytes[32];
	unsigned i;

	/* Decode and encode: boundaries, then random bytes (the top bit random too). */
	check_codec_boundaries();
	for (round = 0; round < RANDOM_CASES; round++) {
		for (i = 0; i < 32U; i++)
			bytes[i] = (uint8_t)random32();
		check_codec(bytes);
	}
	check_encode_elements();

	/* The rest. */
	check_predicates();
	check_additive();
	check_multiplicative();
	check_powers();
	check_cswap();
	check_chains();

	/* The result. */
	return check_finish("test-f25519");
}
