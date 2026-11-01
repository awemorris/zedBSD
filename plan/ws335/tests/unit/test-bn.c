/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Checks the fixed-width integers and the Montgomery arithmetic
 * (cryc-bn.h).
 *
 * Expected values come from a reference written here from the definitions
 * alone: numbers of 16-bit digits, a schoolbook sum, difference and
 * product, and a remainder taken bit by bit (shift in one bit, subtract
 * the modulus when the remainder reaches it).  A Montgomery product is
 * checked by its defining property: r * R = a * b (mod m) with r below m,
 * which has exactly one solution since m is odd.  Powers are checked
 * against square-and-multiply on plain numbers.  Hand-computed cases: the
 * modulus 7 (R = 2^32 = 4, R^2 = 2, -7^-1 = 0x49249249 mod 2^32) and the
 * modulus 2^32 + 1 (2^32 = -1, so R = 2^64 = 1 and Montgomery form is the
 * plain value).  The primes of P-256 and P-384 (p and n) are built from
 * their defining formulas or hex values and checked by Fermat's relation
 * a^(p-1) = 1 and by a * a^(p-2) = 1, which a mistyped or composite number
 * fails.
 */

#include "check.h"

#include "userland/base/libcrypto-compat/cryc-bn.h"

/* The digits of a reference number: enough for the product of two 8192-bit numbers and a carry. */
#define REF_DIGITS	1100U

/* A fill byte that shows later that an output was not written. */
#define SENTINEL_WORD	0xa5a5a5a5U

/* A reference number: little-endian digits of 16 bits, kept in uint32_t. */
struct ref {
	uint32_t digit[REF_DIGITS];
};

/* The state of the pseudo-random generator of the tests (xorshift32, Marsaglia 2003). */
static uint32_t rng_state = 0x2545f491U;

/* Returns the next pseudo-random 32-bit value. */
static uint32_t
random32(void)
{
	/* Marsaglia's xorshift32 with the shifts 13, 17, 5. */
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 17;
	rng_state ^= rng_state << 5;
	return rng_state;
}

/* Sets a reference number to zero. */
static void
ref_zero(
	struct ref *r)
{
	unsigned i;

	/* Every digit. */
	for (i = 0; i < REF_DIGITS; i++)
		r->digit[i] = 0;
}

/* Loads n limbs into a reference number (two digits per limb). */
static void
ref_set_limbs(
	struct ref *r,
	const uint32_t *limbs,
	size_t n)
{
	size_t i;

	/* Zero, then each limb as two digits. */
	ref_zero(r);
	for (i = 0; i < n; i++) {
		r->digit[2U * i] = limbs[i] & 0xffffU;
		r->digit[2U * i + 1U] = limbs[i] >> 16;
	}
}

/* Reads the low n limbs of a reference number. */
static void
ref_get_limbs(
	uint32_t *out,
	size_t n,
	const struct ref *r)
{
	size_t i;

	/* Two digits per limb. */
	for (i = 0; i < n; i++)
		out[i] = r->digit[2U * i] | (r->digit[2U * i + 1U] << 16);
}

/* Returns whether every digit at or above 2n is zero (the number fits in n limbs). */
static int
ref_fits(
	const struct ref *r,
	size_t n)
{
	size_t i;

	/* The high digits. */
	for (i = 2U * n; i < REF_DIGITS; i++) {
		if (r->digit[i] != 0)
			return 0;
	}
	return 1;
}

/* Compares the low `digits` digits (the rest are zero): -1, 0 or 1. */
static int
ref_cmp(
	const struct ref *a,
	const struct ref *b,
	size_t digits)
{
	size_t i;

	/* From the top digit down. */
	for (i = digits; i > 0; i--) {
		if (a->digit[i - 1U] < b->digit[i - 1U])
			return -1;
		if (a->digit[i - 1U] > b->digit[i - 1U])
			return 1;
	}
	return 0;
}

/* a = a - b over `digits` digits, for a >= b. */
static void
ref_sub_in(
	struct ref *a,
	const struct ref *b,
	size_t digits)
{
	size_t i;
	uint32_t borrow;
	uint32_t value;

	/* Digit by digit with a borrow. */
	borrow = 0;
	for (i = 0; i < digits; i++) {
		value = a->digit[i] - b->digit[i] - borrow;
		borrow = (value >> 31) & 1U;
		a->digit[i] = value & 0xffffU;
	}
}

/* r = a + b over `digits` digits; the carry goes to digit `digits`. */
static void
ref_add(
	struct ref *r,
	const struct ref *a,
	const struct ref *b,
	size_t digits)
{
	size_t i;
	uint32_t carry;
	uint32_t value;

	/* Digit by digit with a carry. */
	carry = 0;
	for (i = 0; i < digits; i++) {
		value = a->digit[i] + b->digit[i] + carry;
		r->digit[i] = value & 0xffffU;
		carry = value >> 16;
	}
	r->digit[digits] = carry;
}

/* p = a * b, a of na digits and b of nb digits (schoolbook). */
static void
ref_mul(
	struct ref *p,
	const struct ref *a,
	size_t na,
	const struct ref *b,
	size_t nb)
{
	size_t i;
	size_t j;
	uint32_t carry;
	uint32_t value;

	/* Zero, then each row of a * b[j] added in with a carry. */
	ref_zero(p);
	for (j = 0; j < nb; j++) {
		carry = 0;
		for (i = 0; i < na; i++) {
			value = p->digit[i + j] + a->digit[i] * b->digit[j] + carry;
			p->digit[i + j] = value & 0xffffU;
			carry = value >> 16;
		}
		p->digit[na + j] = carry;
	}
}

/*
 * r = x mod m, x of xdigits digits and m of mdigits digits, bit by bit:
 * the remainder takes the next bit of x from the top, and m is subtracted
 * when the remainder reaches m.
 */
static void
ref_mod(
	struct ref *r,
	const struct ref *x,
	size_t xdigits,
	const struct ref *m,
	size_t mdigits)
{
	size_t i;
	size_t j;
	size_t top;
	int bit;
	uint32_t carry;
	uint32_t value;

	/* The remainder starts at zero; leading zero digits of x add nothing. */
	ref_zero(r);
	top = xdigits;
	while (top > 0 && x->digit[top - 1U] == 0)
		top--;

	/* Each bit of x from the top. */
	for (i = top * 16U; i > 0; i--) {
		bit = (int)((x->digit[(i - 1U) / 16U] >> ((i - 1U) % 16U)) & 1U);

		/* Doubles the remainder and adds the bit. */
		carry = (uint32_t)bit;
		for (j = 0; j <= mdigits; j++) {
			value = (r->digit[j] << 1) | carry;
			r->digit[j] = value & 0xffffU;
			carry = value >> 16;
		}

		/* Subtracts m when the remainder reached it. */
		if (ref_cmp(r, m, mdigits + 1U) >= 0)
			ref_sub_in(r, m, mdigits + 1U);
	}
}

/* r = a * b mod m, with a and b of mdigits digits. */
static void
ref_mulmod(
	struct ref *r,
	const struct ref *a,
	const struct ref *b,
	const struct ref *m,
	size_t mdigits)
{
	static struct ref product;

	/* The product, then its remainder. */
	ref_mul(&product, a, mdigits, b, mdigits);
	ref_mod(r, &product, 2U * mdigits, m, mdigits);
}

/* r = x * R mod m with R = 2^(32 * limbs) (x of `limbs` limbs below m). */
static void
ref_to_mont(
	struct ref *r,
	const struct ref *x,
	const struct ref *m,
	size_t limbs)
{
	static struct ref shifted;
	size_t i;

	/* x shifted left by the width of the modulus, then its remainder. */
	ref_zero(&shifted);
	for (i = 0; i < 2U * limbs; i++)
		shifted.digit[2U * limbs + i] = x->digit[i];
	ref_mod(r, &shifted, 4U * limbs, m, 2U * limbs);
}

/* r = x ^ e mod m by square-and-multiply on plain numbers (e of esize big-endian bytes). */
static void
ref_modexp(
	struct ref *r,
	const struct ref *x,
	const uint8_t *e,
	size_t esize,
	const struct ref *m,
	size_t mdigits)
{
	static struct ref held;
	size_t i;
	int bit;

	/* The result starts at 1. */
	ref_zero(r);
	r->digit[0] = 1;

	/* Each bit of the exponent from the top. */
	for (i = 0; i < esize; i++) {
		for (bit = 7; bit >= 0; bit--) {
			ref_mulmod(&held, r, r, m, mdigits);
			*r = held;
			if (((e[i] >> bit) & 1U) != 0U) {
				ref_mulmod(&held, r, x, m, mdigits);
				*r = held;
			}
		}
	}
}

/* Checks that n limbs equal the reference number r (and that r fits). */
static void
check_limbs_ref(
	const uint32_t *got,
	size_t n,
	const struct ref *r)
{
	uint32_t expected[CRYC_BN_MAX_LIMBS];

	/* The reference must fit, then the limbs must match. */
	CHECK(ref_fits(r, n));
	ref_get_limbs(expected, n, r);
	CHECK_BYTES(got, expected, n * sizeof(uint32_t));
}

/* Allocates n limbs of exactly that size (so a stray access is caught by the sanitizer). */
static uint32_t *
limbs_alloc(
	size_t n)
{
	uint32_t *p;

	/* Exactly the size asked for; a test cannot go on without memory. */
	p = (uint32_t *)malloc(n * sizeof(uint32_t));
	if (p == NULL) {
		printf("FAIL: out of memory\n");
		exit(EXIT_FAILURE);
	}
	return p;
}

/* Makes a copy of n limbs in a buffer of exactly that size. */
static uint32_t *
limbs_dup(
	const uint32_t *p,
	size_t n)
{
	uint32_t *copy;
	size_t i;

	/* Allocates and copies. */
	copy = limbs_alloc(n);
	for (i = 0; i < n; i++)
		copy[i] = p[i];
	return copy;
}

/* Fills n limbs with pseudo-random values. */
static void
limbs_random(
	uint32_t *p,
	size_t n)
{
	size_t i;

	/* Each limb. */
	for (i = 0; i < n; i++)
		p[i] = random32();
}

/* Writes the big-endian bytes of n limbs (4n bytes) into out. */
static void
limbs_to_bytes(
	const uint32_t *p,
	size_t n,
	uint8_t *out)
{
	size_t i;

	/* The most significant limb first, each limb's high byte first. */
	for (i = 0; i < n; i++) {
		out[4U * (n - 1U - i)] = (uint8_t)(p[i] >> 24);
		out[4U * (n - 1U - i) + 1U] = (uint8_t)(p[i] >> 16);
		out[4U * (n - 1U - i) + 2U] = (uint8_t)(p[i] >> 8);
		out[4U * (n - 1U - i) + 3U] = (uint8_t)p[i];
	}
}

/* p = m - w for a small w (m is at least w), over n limbs. */
static void
limbs_sub_word(
	uint32_t *out,
	const uint32_t *m,
	size_t n,
	uint32_t w)
{
	size_t i;
	uint32_t borrow;
	uint32_t value;

	/* The word comes off the lowest limb and the borrow runs up. */
	borrow = w;
	for (i = 0; i < n; i++) {
		value = m[i] - borrow;
		borrow = m[i] < borrow ? 1U : 0U;
		out[i] = value;
	}
}

/* Returns the number of significant bits of n limbs. */
static size_t
limbs_bits(
	const uint32_t *p,
	size_t n)
{
	size_t i;
	size_t bits;
	uint32_t top;

	/* The top non-zero limb and the position of its top bit. */
	for (i = n; i > 0; i--) {
		if (p[i - 1U] != 0) {
			top = p[i - 1U];
			bits = 0;
			while (top != 0) {
				bits++;
				top >>= 1;
			}
			return (i - 1U) * 32U + bits;
		}
	}
	return 0;
}

/*
 * Makes a modulus of `limbs` limbs of the given kind: 0 random with the
 * top bit set, 1 random with a random non-zero top limb, 2 all ones, 3
 * 2^(32 * (limbs - 1)) + 1 (the smallest of its width; 3 for one limb),
 * 4 random with a top limb of 1.  It is odd and at least 3.
 */
static void
make_modulus(
	uint32_t *m,
	size_t limbs,
	int kind)
{
	size_t i;

	/* The limbs by kind. */
	limbs_random(m, limbs);
	if (kind == 0) {
		m[limbs - 1U] |= 0x80000000U;
	} else if (kind == 1) {
		m[limbs - 1U] >>= random32() % 32U;
		if (m[limbs - 1U] == 0)
			m[limbs - 1U] = 1;
	} else if (kind == 2) {
		for (i = 0; i < limbs; i++)
			m[i] = 0xffffffffU;
	} else if (kind == 3) {
		for (i = 0; i < limbs; i++)
			m[i] = 0;
		m[limbs - 1U] = 1;
	} else {
		m[limbs - 1U] = 1;
	}

	/* Odd, and at least 3. */
	m[0] |= 1U;
	if (limbs == 1U && m[0] < 3U)
		m[0] = 3U;
}

/* Makes a value below the modulus: kind 0 random, 1 zero, 2 one, 3 m - 1, 4 m - 2, 5 near m, 6 small. */
static void
make_value(
	uint32_t *a,
	const uint32_t *m,
	size_t limbs,
	int kind)
{
	size_t i;
	uint32_t top;

	/* The value by kind; a top limb below the modulus' keeps it below m. */
	top = m[limbs - 1U];
	limbs_random(a, limbs);
	if (kind == 0) {
		a[limbs - 1U] = random32() % top;
	} else if (kind == 1) {
		for (i = 0; i < limbs; i++)
			a[i] = 0;
	} else if (kind == 2) {
		for (i = 0; i < limbs; i++)
			a[i] = 0;
		a[0] = 1;
	} else if (kind == 3) {
		limbs_sub_word(a, m, limbs, 1U);
	} else if (kind == 4) {
		limbs_sub_word(a, m, limbs, 2U);
	} else if (kind == 5) {
		for (i = 0; i < limbs; i++)
			a[i] = 0xffffffffU;
		a[limbs - 1U] = top - 1U;
	} else {
		for (i = 0; i < limbs; i++)
			a[i] = 0;
		a[0] = random32() % 100U;

		/* A one-limb modulus below 100 is smaller than that: reduces the value into range. */
		if (limbs == 1U)
			a[0] = a[0] % m[0];
	}
}

/*
 * Checks one prepared modulus against the reference: the fields of the
 * structure, the helpers, and every arithmetic function on a set of values.
 * `pairs` is the number of random value pairs on top of the fixed ones;
 * `exp_bytes` the number of random exponent bytes.
 */
static void
check_modulus(
	const uint32_t *m_limbs,
	size_t limbs,
	unsigned pairs,
	size_t exp_bytes,
	int heavy)
{
	static struct cryc_bn_mont mont;
	static struct ref m;
	static struct ref ra;
	static struct ref rb;
	static struct ref expected;
	static struct ref scratch;
	static struct ref one;
	uint8_t bytes[CRYC_BN_MAX_LIMBS * 4U + 4U];
	uint8_t exponent[40];
	uint32_t *a;
	uint32_t *b;
	uint32_t *out;
	uint32_t *copy;
	uint32_t *am;
	uint32_t *bm;
	uint32_t carry_probe;
	size_t md;
	size_t i;
	size_t extra;
	size_t bits;
	unsigned pair;
	int kind_a;
	int kind_b;
	int error;
	int round;

	/* The modulus with a random number of leading zero bytes, which init must accept. */
	extra = random32() % 7U;
	memset(bytes, 0, extra);
	limbs_to_bytes(m_limbs, limbs, bytes + extra);
	memset(&mont, 0x5a, sizeof(mont));
	error = cryc_bn_mont_init(&mont, bytes, limbs * 4U + extra);
	CHECK(error == 0);

	/* The fields: the width, the bit length, the zero high limbs and the modulus itself. */
	bits = limbs_bits(m_limbs, limbs);
	CHECK(mont.limbs == (bits + 31U) / 32U);
	CHECK(mont.bits == bits);
	CHECK(mont.limbs == limbs);
	CHECK_BYTES(mont.modulus, m_limbs, limbs * sizeof(uint32_t));
	for (i = limbs; i < CRYC_BN_MAX_LIMBS; i++) {
		CHECK(mont.modulus[i] == 0);
		CHECK(mont.r2[i] == 0);
	}

	/* -m^-1 mod 2^32: m * m0inv = -1. */
	CHECK((uint32_t)(m_limbs[0] * mont.m0inv) == 0xffffffffU);

	/* R^2 mod m, from the definition. */
	md = 2U * limbs;
	ref_set_limbs(&m, m_limbs, limbs);
	ref_zero(&scratch);
	scratch.digit[4U * limbs] = 1;
	ref_mod(&expected, &scratch, 4U * limbs + 1U, &m, md);
	check_limbs_ref(mont.r2, limbs, &expected);

	/* R mod m: the Montgomery form of 1. */
	out = limbs_alloc(limbs);
	for (i = 0; i < limbs; i++)
		out[i] = SENTINEL_WORD;
	cryc_bn_mont_one(out, &mont);
	ref_zero(&scratch);
	scratch.digit[2U * limbs] = 1;
	ref_mod(&expected, &scratch, 2U * limbs + 1U, &m, md);
	check_limbs_ref(out, limbs, &expected);
	ref_zero(&one);
	one.digit[0] = 1;

	/* The fixed and random pairs of values. */
	a = limbs_alloc(limbs);
	b = limbs_alloc(limbs);
	am = limbs_alloc(limbs);
	bm = limbs_alloc(limbs);
	for (pair = 0; pair < 7U + pairs; pair++) {
		/* The first pairs pair the edge values, the rest are random. */
		if (pair < 7U) {
			kind_a = (int)pair;
			kind_b = (int)((pair * 3U + 2U) % 7U);
		} else {
			kind_a = 0;
			kind_b = 0;
		}
		make_value(a, m_limbs, limbs, kind_a);
		make_value(b, m_limbs, limbs, kind_b);
		CHECK(cryc_bn_lt(a, m_limbs, limbs) == 0xffffffffU);
		CHECK(cryc_bn_lt(b, m_limbs, limbs) == 0xffffffffU);
		ref_set_limbs(&ra, a, limbs);
		ref_set_limbs(&rb, b, limbs);

		/* a + b mod m, with out apart, equal to a, equal to b, and a = b. */
		ref_add(&scratch, &ra, &rb, md);
		ref_mod(&expected, &scratch, md + 1U, &m, md);
		for (i = 0; i < limbs; i++)
			out[i] = SENTINEL_WORD;
		cryc_bn_mod_add(out, a, b, &mont);
		check_limbs_ref(out, limbs, &expected);
		copy = limbs_dup(a, limbs);
		cryc_bn_mod_add(copy, copy, b, &mont);
		check_limbs_ref(copy, limbs, &expected);
		free(copy);
		copy = limbs_dup(b, limbs);
		cryc_bn_mod_add(copy, a, copy, &mont);
		check_limbs_ref(copy, limbs, &expected);
		free(copy);

		/* a - b mod m: (a + m - b) mod m. */
		ref_add(&scratch, &ra, &m, md);
		ref_sub_in(&scratch, &rb, md + 1U);
		ref_mod(&expected, &scratch, md + 1U, &m, md);
		for (i = 0; i < limbs; i++)
			out[i] = SENTINEL_WORD;
		cryc_bn_mod_sub(out, a, b, &mont);
		check_limbs_ref(out, limbs, &expected);
		copy = limbs_dup(a, limbs);
		cryc_bn_mod_sub(copy, copy, b, &mont);
		check_limbs_ref(copy, limbs, &expected);
		free(copy);
		copy = limbs_dup(b, limbs);
		cryc_bn_mod_sub(copy, a, copy, &mont);
		check_limbs_ref(copy, limbs, &expected);
		free(copy);

		/* Montgomery product: below m, and r * R = a * b (mod m). */
		for (i = 0; i < limbs; i++)
			out[i] = SENTINEL_WORD;
		cryc_bn_mont_mul(out, a, b, &mont);
		CHECK(cryc_bn_lt(out, m_limbs, limbs) == 0xffffffffU);
		ref_mulmod(&expected, &ra, &rb, &m, md);
		ref_set_limbs(&scratch, out, limbs);
		ref_to_mont(&ra, &scratch, &m, limbs);
		CHECK(ref_cmp(&ra, &expected, md) == 0);
		ref_set_limbs(&ra, a, limbs);

		/* The same product with out equal to a, to b, and a = b = out. */
		copy = limbs_dup(a, limbs);
		cryc_bn_mont_mul(copy, copy, b, &mont);
		CHECK_BYTES(copy, out, limbs * sizeof(uint32_t));
		free(copy);
		copy = limbs_dup(b, limbs);
		cryc_bn_mont_mul(copy, a, copy, &mont);
		CHECK_BYTES(copy, out, limbs * sizeof(uint32_t));
		free(copy);
		copy = limbs_dup(a, limbs);
		cryc_bn_mont_mul(copy, copy, copy, &mont);
		cryc_bn_mont_mul(out, a, a, &mont);
		CHECK_BYTES(copy, out, limbs * sizeof(uint32_t));
		free(copy);

		/* a * R mod m. */
		for (i = 0; i < limbs; i++)
			am[i] = SENTINEL_WORD;
		cryc_bn_to_mont(am, a, &mont);
		ref_to_mont(&expected, &ra, &m, limbs);
		check_limbs_ref(am, limbs, &expected);
		copy = limbs_dup(a, limbs);
		cryc_bn_to_mont(copy, copy, &mont);
		check_limbs_ref(copy, limbs, &expected);
		free(copy);

		/* Out of Montgomery form: the value times R is a, and the round trip returns a. */
		for (i = 0; i < limbs; i++)
			out[i] = SENTINEL_WORD;
		cryc_bn_from_mont(out, a, &mont);
		CHECK(cryc_bn_lt(out, m_limbs, limbs) == 0xffffffffU);
		ref_set_limbs(&scratch, out, limbs);
		ref_to_mont(&expected, &scratch, &m, limbs);
		CHECK(ref_cmp(&expected, &ra, md) == 0);
		cryc_bn_from_mont(out, am, &mont);
		CHECK_BYTES(out, a, limbs * sizeof(uint32_t));
		copy = limbs_dup(am, limbs);
		cryc_bn_from_mont(copy, copy, &mont);
		CHECK_BYTES(copy, a, limbs * sizeof(uint32_t));
		free(copy);

		/* The product of Montgomery forms is the form of the product. */
		cryc_bn_to_mont(bm, b, &mont);
		cryc_bn_mont_mul(out, am, bm, &mont);
		cryc_bn_from_mont(out, out, &mont);
		ref_mulmod(&expected, &ra, &rb, &m, md);
		check_limbs_ref(out, limbs, &expected);
	}

	/* reduce_once: a below m is kept, m reduces to 0, m + a reduces to a when it fits. */
	carry_probe = 0;
	make_value(a, m_limbs, limbs, 0);
	copy = limbs_dup(a, limbs);
	cryc_bn_reduce_once(out, a, &mont);
	CHECK_BYTES(out, a, limbs * sizeof(uint32_t));
	cryc_bn_reduce_once(copy, copy, &mont);
	CHECK_BYTES(copy, a, limbs * sizeof(uint32_t));
	free(copy);
	cryc_bn_reduce_once(out, m_limbs, &mont);
	CHECK(cryc_bn_is_zero(out, limbs) == 0xffffffffU);
	ref_set_limbs(&ra, a, limbs);
	ref_add(&scratch, &ra, &m, md);
	if (ref_fits(&scratch, limbs) != 0) {
		/* m + a fits the width, so it is in the range of the function. */
		ref_get_limbs(b, limbs, &scratch);
		cryc_bn_reduce_once(out, b, &mont);
		CHECK_BYTES(out, a, limbs * sizeof(uint32_t));
		carry_probe = 1;
	}
	if (limbs_bits(m_limbs, limbs) < 32U * limbs) {
		/* A modulus below the full width: 2m - 1 fits, and reduces to m - 1. */
		ref_add(&scratch, &m, &m, md);
		ref_sub_in(&scratch, &one, md + 1U);
		CHECK(ref_fits(&scratch, limbs));
		ref_get_limbs(b, limbs, &scratch);
		cryc_bn_reduce_once(out, b, &mont);
		limbs_sub_word(a, m_limbs, limbs, 1U);
		CHECK_BYTES(out, a, limbs * sizeof(uint32_t));
		carry_probe = 1;
	}
	CHECK(carry_probe <= 1U);

	/* Powers: a small exponent, 0, and random bytes (with leading zero bytes), against plain square-and-multiply. */
	if (heavy != 0) {
		for (round = 0; round < 4; round++) {
			memset(exponent, 0, sizeof(exponent));
			if (round == 0) {
				exponent[0] = 0;
				ref_zero(&scratch);
				exp_bytes = 1;
			} else if (round == 1) {
				exponent[0] = 0x01U;
				exponent[1] = 0x00U;
				exponent[2] = 0x01U;
				exp_bytes = 3;
			} else {
				exponent[0] = 0;
				exponent[1] = 0;
				for (i = 2; i < exp_bytes + 2U && i < sizeof(exponent); i++)
					exponent[i] = (uint8_t)random32();
				exp_bytes = exp_bytes + 2U < sizeof(exponent) ? exp_bytes + 2U : sizeof(exponent);
			}
			make_value(a, m_limbs, limbs, 0);
			ref_set_limbs(&ra, a, limbs);
			ref_to_mont(&scratch, &ra, &m, limbs);
			ref_get_limbs(am, limbs, &scratch);
			ref_modexp(&expected, &ra, exponent, exp_bytes, &m, md);
			ref_to_mont(&scratch, &expected, &m, limbs);
			for (i = 0; i < limbs; i++)
				out[i] = SENTINEL_WORD;
			cryc_bn_mont_exp(out, am, exponent, exp_bytes, &mont);
			check_limbs_ref(out, limbs, &scratch);
			copy = limbs_dup(am, limbs);
			cryc_bn_mont_exp(copy, copy, exponent, exp_bytes, &mont);
			check_limbs_ref(copy, limbs, &scratch);
			free(copy);
			if (round >= 2)
				exp_bytes = exp_bytes - 2U;
		}
	}
	free(a);
	free(b);
	free(am);
	free(bm);
	free(out);
}

/* The hand-computed cases of the modulus 7 and the modulus 2^32 + 1. */
static void
test_hand(void)
{
	struct cryc_bn_mont mont;
	uint8_t modulus7[1];
	uint8_t modulus_f[5];
	uint32_t a[2];
	uint32_t b[2];
	uint32_t out[2];
	uint8_t exponent[3];
	int error;

	/* The modulus 7: one limb, three bits, R = 4, R^2 = 2, -7^-1 = 0x49249249. */
	modulus7[0] = 7;
	error = cryc_bn_mont_init(&mont, modulus7, 1);
	CHECK(error == 0);
	CHECK(mont.limbs == 1);
	CHECK(mont.bits == 3);
	CHECK(mont.modulus[0] == 7U);
	CHECK(mont.modulus[1] == 0U);
	CHECK(mont.r2[0] == 2U);
	CHECK(mont.r2[1] == 0U);
	CHECK(mont.m0inv == 0x49249249U);

	/* R mod 7 = 4. */
	cryc_bn_mont_one(out, &mont);
	CHECK(out[0] == 4U);

	/* 3 into Montgomery form is 3 * 4 = 12 = 5, and back to 3. */
	a[0] = 3;
	cryc_bn_to_mont(out, a, &mont);
	CHECK(out[0] == 5U);
	a[0] = 5;
	cryc_bn_from_mont(out, a, &mont);
	CHECK(out[0] == 3U);

	/* mont_mul(5, 6) = 30 * R^-1 = 30 * 2 = 60 = 4 (R^-1 = 2 since 4 * 2 = 8 = 1). */
	a[0] = 5;
	b[0] = 6;
	cryc_bn_mont_mul(out, a, b, &mont);
	CHECK(out[0] == 4U);

	/* 3^5 = 243 = 5 (mod 7): in Montgomery form 5 * 4 = 20 = 6 gives 6; 3^6 = 1 gives R mod 7 = 4. */
	a[0] = 5;
	exponent[0] = 5;
	cryc_bn_mont_exp(out, a, exponent, 1, &mont);
	CHECK(out[0] == 6U);
	exponent[0] = 6;
	cryc_bn_mont_exp(out, a, exponent, 1, &mont);
	CHECK(out[0] == 4U);
	exponent[0] = 0;
	exponent[1] = 0;
	exponent[2] = 5;
	cryc_bn_mont_exp(out, a, exponent, 3, &mont);
	CHECK(out[0] == 6U);

	/* An exponent of 0 (size 0, and a zero byte) is the Montgomery form of 1, which is 4. */
	cryc_bn_mont_exp(out, a, exponent, 0, &mont);
	CHECK(out[0] == 4U);
	exponent[0] = 0;
	cryc_bn_mont_exp(out, a, exponent, 1, &mont);
	CHECK(out[0] == 4U);

	/* Modular sum and difference below 7: 5 + 4 = 9 = 2, 2 - 5 = -3 = 4, 6 + 1 = 0, 0 - 1 = 6. */
	a[0] = 5;
	b[0] = 4;
	cryc_bn_mod_add(out, a, b, &mont);
	CHECK(out[0] == 2U);
	a[0] = 2;
	b[0] = 5;
	cryc_bn_mod_sub(out, a, b, &mont);
	CHECK(out[0] == 4U);
	a[0] = 6;
	b[0] = 1;
	cryc_bn_mod_add(out, a, b, &mont);
	CHECK(out[0] == 0U);
	a[0] = 0;
	cryc_bn_mod_sub(out, a, b, &mont);
	CHECK(out[0] == 6U);

	/* reduce_once: 13 = 6, 7 = 0, 3 = 3, 0xffffffff - 7 + 1 is not below 2m: not asked. */
	a[0] = 13;
	cryc_bn_reduce_once(out, a, &mont);
	CHECK(out[0] == 6U);
	a[0] = 7;
	cryc_bn_reduce_once(out, a, &mont);
	CHECK(out[0] == 0U);

	/* The modulus 2^32 + 1 = 01 00 00 00 01: R = 2^64 = (2^32)^2 = (-1)^2 = 1, so R^2 = 1 and -m^-1 = -1. */
	modulus_f[0] = 1;
	modulus_f[1] = 0;
	modulus_f[2] = 0;
	modulus_f[3] = 0;
	modulus_f[4] = 1;
	error = cryc_bn_mont_init(&mont, modulus_f, 5);
	CHECK(error == 0);
	CHECK(mont.limbs == 2);
	CHECK(mont.bits == 33);
	CHECK(mont.modulus[0] == 1U);
	CHECK(mont.modulus[1] == 1U);
	CHECK(mont.m0inv == 0xffffffffU);
	CHECK(mont.r2[0] == 1U);
	CHECK(mont.r2[1] == 0U);
	cryc_bn_mont_one(out, &mont);
	CHECK(out[0] == 1U && out[1] == 0U);

	/* With R = 1 Montgomery form is the plain value: 2^32 * 2^32 = 2^64 = 1 and 2^32 * 3 = -3 = m - 3 = 2^32 - 2. */
	a[0] = 0;
	a[1] = 1;
	cryc_bn_mont_mul(out, a, a, &mont);
	CHECK(out[0] == 1U && out[1] == 0U);
	b[0] = 3;
	b[1] = 0;
	cryc_bn_mont_mul(out, a, b, &mont);
	CHECK(out[0] == 0xfffffffeU && out[1] == 0U);
	cryc_bn_to_mont(out, a, &mont);
	CHECK(out[0] == 0U && out[1] == 1U);
	cryc_bn_from_mont(out, a, &mont);
	CHECK(out[0] == 0U && out[1] == 1U);

	/* (2^32)^3 = 2^96 = 2^64 * 2^32 = 2^32: exponent 3 of 2^32 in Montgomery form is itself. */
	exponent[0] = 3;
	cryc_bn_mont_exp(out, a, exponent, 1, &mont);
	CHECK(out[0] == 0U && out[1] == 1U);

	/* 2^32 + 1 as a sum: (2^32 + 0) + (1) = 2^32 + 1 = 0 (mod m); 2^32 as a difference 0 - 1 = m - 1 = 2^32. */
	a[0] = 0;
	a[1] = 1;
	b[0] = 1;
	b[1] = 0;
	cryc_bn_mod_add(out, a, b, &mont);
	CHECK(out[0] == 0U && out[1] == 0U);
	a[0] = 0;
	a[1] = 0;
	cryc_bn_mod_sub(out, a, b, &mont);
	CHECK(out[0] == 0U && out[1] == 1U);
}

/* The modulus checks of mont_init: its errors, its accepted forms, and the zeroed result. */
static void
test_init(void)
{
	struct cryc_bn_mont mont;
	struct cryc_bn_mont zero;
	uint8_t modulus[CRYC_BN_MAX_LIMBS * 4U + 8U];
	uint8_t *exact;
	size_t index;
	size_t size;
	int error;

	/* A zeroed structure to compare with, and the sizes of the largest modulus. */
	memset(&zero, 0, sizeof(zero));

	/* Zero in any length, and the value 1: EINVAL, with the structure zeroed. */
	for (size = 0; size <= 5; size++) {
		memset(modulus, 0, sizeof(modulus));
		memset(&mont, 0x5a, sizeof(mont));
		error = cryc_bn_mont_init(&mont, modulus, size);
		CHECK(error == EINVAL);
		CHECK(memcmp(&mont, &zero, sizeof(mont)) == 0);
	}
	memset(modulus, 0, sizeof(modulus));
	modulus[4] = 1;
	memset(&mont, 0x5a, sizeof(mont));
	error = cryc_bn_mont_init(&mont, modulus, 5);
	CHECK(error == EINVAL);
	CHECK(memcmp(&mont, &zero, sizeof(mont)) == 0);

	/* A NULL pointer with size 0 is zero too. */
	memset(&mont, 0x5a, sizeof(mont));
	error = cryc_bn_mont_init(&mont, NULL, 0);
	CHECK(error == EINVAL);
	CHECK(memcmp(&mont, &zero, sizeof(mont)) == 0);

	/* Even moduli of several widths (2, 4, 1024 with high bits, and 0x100 with leading zeros). */
	for (size = 1; size <= 12; size++) {
		memset(modulus, 0, sizeof(modulus));
		modulus[0] = 0x80U;
		modulus[size - 1U] = (uint8_t)(modulus[size - 1U] | 0x02U);
		memset(&mont, 0x5a, sizeof(mont));
		error = cryc_bn_mont_init(&mont, modulus, size);
		CHECK(error == EINVAL);
		CHECK(memcmp(&mont, &zero, sizeof(mont)) == 0);
	}

	/* The modulus 3 and 5, the smallest accepted; leading zero bytes do not change the result. */
	memset(modulus, 0, sizeof(modulus));
	modulus[3] = 3;
	error = cryc_bn_mont_init(&mont, modulus, 4);
	CHECK(error == 0);
	CHECK(mont.limbs == 1 && mont.bits == 2 && mont.modulus[0] == 3U);
	CHECK((uint32_t)(3U * mont.m0inv) == 0xffffffffU);
	CHECK(mont.r2[0] == 1U);

	/* The modulus 0xffffffff (one limb, 32 bits) and 0x100000001 (two limbs, 33 bits). */
	memset(modulus, 0xff, 4);
	error = cryc_bn_mont_init(&mont, modulus, 4);
	CHECK(error == 0);
	CHECK(mont.limbs == 1 && mont.bits == 32 && mont.modulus[0] == 0xffffffffU);
	memset(modulus, 0, sizeof(modulus));
	modulus[0] = 1;
	modulus[4] = 1;
	error = cryc_bn_mont_init(&mont, modulus, 5);
	CHECK(error == 0);
	CHECK(mont.limbs == 2 && mont.bits == 33);

	/* The widest modulus: 8192 bits (all ones) is accepted; with 1000 leading zero bytes too. */
	memset(modulus, 0xff, 1024);
	error = cryc_bn_mont_init(&mont, modulus, 1024);
	CHECK(error == 0);
	CHECK(mont.limbs == CRYC_BN_MAX_LIMBS && mont.bits == CRYC_BN_MAX_BITS);
	exact = (uint8_t *)malloc(2024);
	if (exact == NULL) {
		printf("FAIL: out of memory\n");
		exit(EXIT_FAILURE);
	}
	memset(exact, 0, 1000);
	memset(exact + 1000, 0xff, 1024);
	error = cryc_bn_mont_init(&mont, exact, 2024);
	CHECK(error == 0);
	CHECK(mont.limbs == CRYC_BN_MAX_LIMBS && mont.bits == CRYC_BN_MAX_BITS);
	free(exact);

	/* One bit more (2^8192 + 1, odd): ENOTSUP, with the structure zeroed. */
	memset(modulus, 0, sizeof(modulus));
	modulus[0] = 1;
	modulus[1024] = 1;
	memset(&mont, 0x5a, sizeof(mont));
	error = cryc_bn_mont_init(&mont, modulus, 1025);
	CHECK(error == ENOTSUP);
	CHECK(memcmp(&mont, &zero, sizeof(mont)) == 0);

	/* A 8193-bit modulus with a top byte of 1 and one with a top byte of 0xff of more bytes: ENOTSUP. */
	memset(modulus, 0xff, 1025);
	memset(&mont, 0x5a, sizeof(mont));
	error = cryc_bn_mont_init(&mont, modulus, 1025);
	CHECK(error == ENOTSUP);
	CHECK(memcmp(&mont, &zero, sizeof(mont)) == 0);
	memset(modulus, 0xff, 1030);
	error = cryc_bn_mont_init(&mont, modulus, 1030);
	CHECK(error == ENOTSUP);
	CHECK(memcmp(&mont, &zero, sizeof(mont)) == 0);

	/* An even modulus that is also too wide is malformed (EINVAL), not out of range. */
	memset(modulus, 0xff, 1030);
	modulus[1029] = 0xfe;
	error = cryc_bn_mont_init(&mont, modulus, 1030);
	CHECK(error == EINVAL);
	CHECK(memcmp(&mont, &zero, sizeof(mont)) == 0);

	/* A modulus of 1025 bytes with a zero first byte is 8192 bits: accepted. */
	memset(modulus, 0xff, 1025);
	modulus[0] = 0;
	error = cryc_bn_mont_init(&mont, modulus, 1025);
	CHECK(error == 0);
	CHECK(mont.bits == CRYC_BN_MAX_BITS);

	/* Random byte strings of random lengths (with and without leading zeros): always 0, EINVAL or ENOTSUP. */
	for (index = 0; index < 400; index++) {
		size = random32() % (CRYC_BN_MAX_LIMBS * 4U + 8U);
		if (index % 4U == 0)
			size = random32() % 12U;
		for (error = 0; (size_t)error < size; error++)
			modulus[error] = (uint8_t)random32();
		if (random32() % 2U == 0U && size > 0)
			memset(modulus, 0, random32() % size);
		exact = (uint8_t *)malloc(size == 0 ? 1U : size);
		if (exact == NULL) {
			printf("FAIL: out of memory\n");
			exit(EXIT_FAILURE);
		}
		memcpy(exact, modulus, size);
		memset(&mont, 0x5a, sizeof(mont));
		error = cryc_bn_mont_init(&mont, exact, size);
		CHECK(error == 0 || error == EINVAL || error == ENOTSUP);
		if (error != 0) {
			CHECK(memcmp(&mont, &zero, sizeof(mont)) == 0);
		} else {
			CHECK(mont.limbs >= 1 && mont.limbs <= CRYC_BN_MAX_LIMBS);
			CHECK((uint32_t)(mont.modulus[0] * mont.m0inv) == 0xffffffffU);
			CHECK(mont.modulus[mont.limbs - 1U] != 0U);
		}
		free(exact);
	}
}

/* decode and encode: positions by hand, widths, overflow, round trips. */
static void
test_codec(void)
{
	uint32_t out[4];
	uint8_t bytes[40];
	uint8_t back[40];
	uint32_t *limbs;
	uint8_t *in;
	size_t size;
	size_t width;
	size_t i;
	int error;

	/* Five bytes 01 02 03 04 05 into two limbs: the low limb takes the last four bytes. */
	bytes[0] = 1;
	bytes[1] = 2;
	bytes[2] = 3;
	bytes[3] = 4;
	bytes[4] = 5;
	memset(out, 0xff, sizeof(out));
	error = cryc_bn_decode(out, 2, bytes, 5);
	CHECK(error == 0);
	CHECK(out[0] == 0x02030405U);
	CHECK(out[1] == 0x00000001U);
	CHECK(out[2] == 0xffffffffU);

	/* The same five bytes into one limb do not fit: EINVAL, zeroed. */
	memset(out, 0xff, sizeof(out));
	error = cryc_bn_decode(out, 1, bytes, 5);
	CHECK(error == EINVAL);
	CHECK(out[0] == 0U);
	CHECK(out[1] == 0xffffffffU);

	/* A zero byte above the width is allowed: 00 00 00 01 02 03 04 into one limb. */
	bytes[0] = 0;
	bytes[1] = 0;
	bytes[2] = 0;
	bytes[3] = 1;
	bytes[4] = 2;
	bytes[5] = 3;
	bytes[6] = 4;
	error = cryc_bn_decode(out, 1, bytes, 7);
	CHECK(error == 0);
	CHECK(out[0] == 0x01020304U);

	/* Size 0 is zero, into any width; width 0 with size 0 is fine; width 0 with a non-zero byte is EINVAL. */
	error = cryc_bn_decode(out, 3, bytes, 0);
	CHECK(error == 0);
	CHECK(out[0] == 0U && out[1] == 0U && out[2] == 0U);
	error = cryc_bn_decode(out, 0, bytes, 0);
	CHECK(error == 0);
	bytes[0] = 0;
	error = cryc_bn_decode(out, 0, bytes, 1);
	CHECK(error == 0);
	bytes[0] = 1;
	error = cryc_bn_decode(out, 0, bytes, 1);
	CHECK(error == EINVAL);

	/* A single byte in the lowest position and in the highest position of a width. */
	bytes[0] = 0x80;
	error = cryc_bn_decode(out, 2, bytes, 1);
	CHECK(error == 0 && out[0] == 0x80U && out[1] == 0U);
	memset(bytes, 0, 8);
	bytes[0] = 0x80;
	error = cryc_bn_decode(out, 2, bytes, 8);
	CHECK(error == 0 && out[0] == 0U && out[1] == 0x80000000U);

	/* encode by hand: two limbs 0x02030405, 1 into 5 bytes, 8 bytes (padded), 3 bytes (truncated), 0 bytes. */
	out[0] = 0x02030405U;
	out[1] = 1U;
	memset(bytes, 0xee, sizeof(bytes));
	cryc_bn_encode(out, 2, bytes, 5);
	CHECK(bytes[0] == 1 && bytes[1] == 2 && bytes[2] == 3 && bytes[3] == 4 && bytes[4] == 5);
	CHECK(bytes[5] == 0xee);
	cryc_bn_encode(out, 2, bytes, 8);
	CHECK(bytes[0] == 0 && bytes[1] == 0 && bytes[2] == 0 && bytes[3] == 1 && bytes[4] == 2);
	CHECK(bytes[5] == 3 && bytes[6] == 4 && bytes[7] == 5);
	memset(bytes, 0xee, sizeof(bytes));
	cryc_bn_encode(out, 2, bytes, 3);
	CHECK(bytes[0] == 3 && bytes[1] == 4 && bytes[2] == 5 && bytes[3] == 0xee);
	cryc_bn_encode(out, 2, bytes, 0);
	CHECK(bytes[0] == 3);

	/* Round trips for widths 1 to 40 limbs-ish and byte sizes around the width: decode then encode gives the bytes back. */
	for (width = 1; width <= 9; width++) {
		for (size = 0; size <= width * 4U + 3U; size++) {
			in = (uint8_t *)malloc(size == 0 ? 1U : size);
			limbs = limbs_alloc(width);
			if (in == NULL) {
				printf("FAIL: out of memory\n");
				exit(EXIT_FAILURE);
			}
			for (i = 0; i < size; i++)
				in[i] = (uint8_t)random32();
			if (size > width * 4U)
				memset(in, 0, size - width * 4U);
			error = cryc_bn_decode(limbs, width, in, size);
			CHECK(error == 0);
			cryc_bn_encode(limbs, width, back, size);
			CHECK_BYTES(back, in, size);
			free(in);
			free(limbs);
		}
	}

	/* A non-zero byte above the width in each position is EINVAL and the result is zero. */
	for (width = 1; width <= 3; width++) {
		for (i = width * 4U; i < width * 4U + 5U; i++) {
			memset(bytes, 0, sizeof(bytes));
			bytes[(width * 4U + 5U) - 1U - i] = 0x01;
			limbs = limbs_alloc(width);
			error = cryc_bn_decode(limbs, width, bytes, width * 4U + 5U);
			CHECK(error == EINVAL);
			CHECK(cryc_bn_is_zero(limbs, width) == 0xffffffffU);
			free(limbs);
		}
	}
}

/* lt, is_zero, select on boundary and random values. */
static void
test_compare(void)
{
	uint32_t a[6];
	uint32_t b[6];
	uint32_t out[6];
	uint32_t *ea;
	uint32_t *eb;
	size_t limbs;
	size_t position;
	size_t i;
	unsigned round;
	int expected_lt;
	struct ref ra;
	struct ref rb;

	/* Width 0: nothing is less than nothing, and an empty number is zero. */
	memset(a, 0, sizeof(a));
	memset(b, 0, sizeof(b));
	CHECK(cryc_bn_lt(a, b, 0) == 0U);
	CHECK(cryc_bn_is_zero(a, 0) == 0xffffffffU);

	/* Each width: equal, and differing in only one limb, each way. */
	for (limbs = 1; limbs <= 6; limbs++) {
		for (position = 0; position < limbs; position++) {
			for (i = 0; i < limbs; i++) {
				a[i] = random32();
				b[i] = a[i];
			}
			ea = limbs_dup(a, limbs);
			eb = limbs_dup(b, limbs);
			CHECK(cryc_bn_lt(ea, eb, limbs) == 0U);
			eb[position] = a[position] + 1U;
			if (eb[position] != 0U) {
				CHECK(cryc_bn_lt(ea, eb, limbs) == 0xffffffffU);
				CHECK(cryc_bn_lt(eb, ea, limbs) == 0U);
			}
			eb[position] = 0xffffffffU;
			ea[position] = 0xfffffffeU;
			CHECK(cryc_bn_lt(ea, eb, limbs) == 0xffffffffU);
			CHECK(cryc_bn_lt(eb, ea, limbs) == 0U);
			free(ea);
			free(eb);
		}
	}

	/* Random pairs, against the reference comparison. */
	for (round = 0; round < 300U; round++) {
		limbs = 1U + random32() % 6U;
		limbs_random(a, limbs);
		limbs_random(b, limbs);
		if (round % 3U == 0U) {
			for (i = 0; i < limbs; i++)
				b[i] = a[i];
			b[random32() % limbs] ^= (uint32_t)1U << (random32() % 32U);
		}
		ref_set_limbs(&ra, a, limbs);
		ref_set_limbs(&rb, b, limbs);
		expected_lt = ref_cmp(&ra, &rb, 2U * limbs) < 0;
		ea = limbs_dup(a, limbs);
		eb = limbs_dup(b, limbs);
		CHECK(cryc_bn_lt(ea, eb, limbs) == (expected_lt ? 0xffffffffU : 0U));
		free(ea);
		free(eb);
	}

	/* is_zero: zero, and a single bit anywhere. */
	for (limbs = 1; limbs <= 6; limbs++) {
		for (i = 0; i < limbs; i++)
			a[i] = 0;
		ea = limbs_dup(a, limbs);
		CHECK(cryc_bn_is_zero(ea, limbs) == 0xffffffffU);
		for (position = 0; position < limbs * 32U; position++) {
			ea[position / 32U] = (uint32_t)1U << (position % 32U);
			CHECK(cryc_bn_is_zero(ea, limbs) == 0U);
			ea[position / 32U] = 0;
		}
		free(ea);
	}

	/* select: mask all ones takes a, mask 0 takes b, and out may be a or b. */
	for (limbs = 1; limbs <= 6; limbs++) {
		limbs_random(a, limbs);
		limbs_random(b, limbs);
		ea = limbs_dup(a, limbs);
		eb = limbs_dup(b, limbs);
		memset(out, 0, sizeof(out));
		cryc_bn_select(out, ea, eb, limbs, 0xffffffffU);
		CHECK_BYTES(out, a, limbs * sizeof(uint32_t));
		cryc_bn_select(out, ea, eb, limbs, 0U);
		CHECK_BYTES(out, b, limbs * sizeof(uint32_t));
		cryc_bn_select(ea, ea, eb, limbs, 0U);
		CHECK_BYTES(ea, b, limbs * sizeof(uint32_t));
		cryc_bn_select(eb, ea, eb, limbs, 0xffffffffU);
		CHECK_BYTES(eb, b, limbs * sizeof(uint32_t));
		free(ea);
		free(eb);
	}
	for (limbs = 0; limbs < 3; limbs++)
		cryc_bn_select(out, a, b, 0, 0xffffffffU);
}

/* The widths: every modulus kind at widths from 1 limb up to the largest. */
static void
test_widths(void)
{
	static const size_t widths[] = {
		1, 2, 3, 4, 5, 6, 7, 8, 9, 12, 16, 24, 32, 64, 128, 256
	};
	uint32_t m[CRYC_BN_MAX_LIMBS];
	size_t count;
	size_t index;
	int kind;
	unsigned round;
	unsigned pairs;
	size_t exp_bytes;
	int heavy;

	/* Each width, each kind, and a number of random moduli that falls as the width grows. */
	count = sizeof(widths) / sizeof(widths[0]);
	for (index = 0; index < count; index++) {
		pairs = 6;
		exp_bytes = 8;
		heavy = 1;
		if (widths[index] >= 24U) {
			pairs = 2;
			exp_bytes = 2;
		}
		if (widths[index] >= 64U) {
			pairs = 1;
			exp_bytes = 0;
		}
		for (kind = 0; kind < 5; kind++) {
			if (widths[index] == 256U && kind != 2 && kind != 0)
				continue;
			if (widths[index] == 128U && kind == 4)
				continue;
			make_modulus(m, widths[index], kind);
			if (widths[index] >= 128U) {
				/* The heavy exponent work only on the all-ones and the random full modulus. */
				heavy = kind == 2 || widths[index] == 128U;
			}
			check_modulus(m, widths[index], pairs, exp_bytes, heavy);
		}
		if (widths[index] <= 16U) {
			for (round = 0; round < 12U; round++) {
				make_modulus(m, widths[index], (int)(round % 5U));
				check_modulus(m, widths[index], pairs, exp_bytes, 1);
			}
		}
	}
}

/* Reads a hex string of a prime into limbs and bytes. */
static size_t
prime_bytes(
	const char *hex,
	uint8_t *out)
{
	size_t length;
	unsigned value;
	size_t i;
	int digit;
	int half;

	/* Two hex digits per byte. */
	length = strlen(hex) / 2U;
	for (i = 0; i < length; i++) {
		value = 0;
		for (half = 0; half < 2; half++) {
			digit = hex[i * 2U + (size_t)half];
			if (digit >= '0' && digit <= '9')
				value = value * 16U + (unsigned)(digit - '0');
			else
				value = value * 16U + (unsigned)(digit - 'a' + 10);
		}
		out[i] = (uint8_t)value;
	}
	return length;
}

/*
 * Checks one prime of the curves: the number equals its definition (when a
 * formula builds it), Fermat's relation a^(p-1) = 1 for several bases,
 * a * a^(p-2) = 1, the round trip through Montgomery form, and
 * associativity.
 */
static void
check_prime(
	const char *hex,
	size_t bytes_expected)
{
	static struct cryc_bn_mont mont;
	uint8_t modulus[64];
	uint8_t exponent[64];
	uint32_t a[12];
	uint32_t b[12];
	uint32_t c[12];
	uint32_t am[12];
	uint32_t bm[12];
	uint32_t cm[12];
	uint32_t left[12];
	uint32_t right[12];
	uint32_t one[12];
	uint32_t inverse[12];
	uint32_t power[12];
	uint32_t m_minus[12];
	size_t size;
	size_t limbs;
	size_t i;
	unsigned base;
	int error;

	/* The prime into the modulus, and its Montgomery setup. */
	size = prime_bytes(hex, modulus);
	CHECK(size == bytes_expected);
	error = cryc_bn_mont_init(&mont, modulus, size);
	CHECK(error == 0);
	limbs = mont.limbs;
	CHECK(limbs == size / 4U);
	cryc_bn_mont_one(one, &mont);

	/* The exponent p - 1 (p is odd, so only the last byte changes). */
	memcpy(exponent, modulus, size);
	exponent[size - 1U] = (uint8_t)(exponent[size - 1U] - 1U);
	limbs_sub_word(m_minus, mont.modulus, limbs, 2U);

	/* Fermat: base^(p-1) = 1 for the bases 2, 3, 5, 7, 11, 13 and random. */
	for (base = 0; base < 12U; base++) {
		limbs_random(a, limbs);
		a[limbs - 1U] = 0;
		if (base < 6U) {
			static const unsigned small[6] = { 2, 3, 5, 7, 11, 13 };

			for (i = 0; i < limbs; i++)
				a[i] = 0;
			a[0] = small[base];
		}
		cryc_bn_to_mont(am, a, &mont);
		cryc_bn_mont_exp(power, am, exponent, size, &mont);
		CHECK_BYTES(power, one, limbs * sizeof(uint32_t));
	}

	/* The inverse by p - 2: a * a^(p-2) = 1, and a round trip a -> aR -> a. */
	exponent[size - 1U] = (uint8_t)(exponent[size - 1U] - 1U);
	for (base = 0; base < 6U; base++) {
		limbs_random(a, limbs);
		a[limbs - 1U] = 0;
		a[0] |= 1U;
		cryc_bn_to_mont(am, a, &mont);
		cryc_bn_mont_exp(inverse, am, exponent, size, &mont);
		cryc_bn_mont_mul(power, am, inverse, &mont);
		CHECK_BYTES(power, one, limbs * sizeof(uint32_t));
		cryc_bn_from_mont(power, am, &mont);
		CHECK_BYTES(power, a, limbs * sizeof(uint32_t));
	}

	/* Associativity (a * b) * c = a * (b * c), and distributivity over the modular sum. */
	for (base = 0; base < 6U; base++) {
		limbs_random(a, limbs);
		limbs_random(b, limbs);
		limbs_random(c, limbs);
		a[limbs - 1U] = 0;
		b[limbs - 1U] = 0;
		c[limbs - 1U] = 0;
		cryc_bn_to_mont(am, a, &mont);
		cryc_bn_to_mont(bm, b, &mont);
		cryc_bn_to_mont(cm, c, &mont);
		cryc_bn_mont_mul(left, am, bm, &mont);
		cryc_bn_mont_mul(left, left, cm, &mont);
		cryc_bn_mont_mul(right, bm, cm, &mont);
		cryc_bn_mont_mul(right, am, right, &mont);
		CHECK_BYTES(left, right, limbs * sizeof(uint32_t));
		cryc_bn_mod_add(left, bm, cm, &mont);
		cryc_bn_mont_mul(left, am, left, &mont);
		cryc_bn_mont_mul(right, am, bm, &mont);
		cryc_bn_mont_mul(power, am, cm, &mont);
		cryc_bn_mod_add(right, right, power, &mont);
		CHECK_BYTES(left, right, limbs * sizeof(uint32_t));
	}

	/* (p - 1)^2 = 1: -1 squared. */
	limbs_sub_word(a, mont.modulus, limbs, 1U);
	cryc_bn_to_mont(am, a, &mont);
	cryc_bn_mont_mul(power, am, am, &mont);
	CHECK_BYTES(power, one, limbs * sizeof(uint32_t));
	cryc_bn_from_mont(power, power, &mont);
	CHECK(power[0] == 1U);
	CHECK(m_minus[0] == mont.modulus[0] - 2U);
}

/* The primes of the curves, from their definitions and as hex. */
static void
test_primes(void)
{
	static struct ref p256;
	static struct ref term;
	static struct ref scratch;
	uint32_t limbs[8];
	uint8_t built[32];
	uint8_t hex_bytes[32];

	/* P-256's p = 2^256 - 2^224 + 2^192 + 2^96 - 1 from the formula, against the hex value. */
	ref_zero(&p256);
	p256.digit[16] = 1;
	ref_zero(&term);
	term.digit[14] = 1;
	ref_sub_in(&p256, &term, 17);
	ref_zero(&term);
	term.digit[12] = 1;
	ref_add(&scratch, &p256, &term, 17);
	p256 = scratch;
	ref_zero(&term);
	term.digit[6] = 1;
	ref_add(&scratch, &p256, &term, 17);
	p256 = scratch;
	ref_zero(&term);
	term.digit[0] = 1;
	ref_sub_in(&p256, &term, 17);
	ref_get_limbs(limbs, 8, &p256);
	limbs_to_bytes(limbs, 8, built);
	prime_bytes("ffffffff00000001000000000000000000000000ffffffffffffffffffffffff", hex_bytes);
	CHECK_BYTES(built, hex_bytes, 32);

	/* P-384's p = 2^384 - 2^128 - 2^96 + 2^32 - 1 against its hex value. */
	{
		uint32_t limbs384[12];
		uint8_t built384[48];
		uint8_t hex384[48];

		ref_zero(&p256);
		p256.digit[24] = 1;
		ref_zero(&term);
		term.digit[8] = 1;
		ref_sub_in(&p256, &term, 25);
		ref_zero(&term);
		term.digit[6] = 1;
		ref_sub_in(&p256, &term, 25);
		ref_zero(&term);
		term.digit[2] = 1;
		ref_add(&scratch, &p256, &term, 25);
		p256 = scratch;
		ref_zero(&term);
		term.digit[0] = 1;
		ref_sub_in(&p256, &term, 25);
		ref_get_limbs(limbs384, 12, &p256);
		limbs_to_bytes(limbs384, 12, built384);
		prime_bytes("fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffeffffffff0000000000000000ffffffff", hex384);
		CHECK_BYTES(built384, hex384, 48);
	}

	/* The four primes: Fermat, inverses, associativity. */
	check_prime("ffffffff00000001000000000000000000000000ffffffffffffffffffffffff", 32);
	check_prime("ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551", 32);
	check_prime("fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffeffffffff0000000000000000ffffffff", 48);
	check_prime("ffffffffffffffffffffffffffffffffffffffffffffffffc7634d81f4372ddf581a0db248b0a77aecec196accc52973", 48);
}

/*
 * A composite modulus fails Fermat's relation, and an even-cofactor
 * modulus still gives the defining property of the product: the checks
 * above do not assume a prime.
 */
static void
test_composite(void)
{
	struct cryc_bn_mont mont;
	uint8_t modulus[4];
	uint8_t exponent[1];
	uint32_t a[1];
	uint32_t am[1];
	uint32_t one[1];
	uint32_t power[1];
	int error;

	/* 15 = 3 * 5: 2^14 = 16384 = 4 (mod 15), not 1. */
	modulus[0] = 15;
	error = cryc_bn_mont_init(&mont, modulus, 1);
	CHECK(error == 0);
	a[0] = 2;
	cryc_bn_to_mont(am, a, &mont);
	cryc_bn_mont_one(one, &mont);
	exponent[0] = 14;
	cryc_bn_mont_exp(power, am, exponent, 1, &mont);
	CHECK(power[0] != one[0]);
	cryc_bn_from_mont(power, power, &mont);
	CHECK(power[0] == 4U);
}

/* Runs the checks. */
int
main(void)
{
	/* Hand-computed cases first. */
	test_hand();
	test_init();
	test_codec();
	test_compare();
	test_composite();
	test_primes();
	test_widths();

	/* The result. */
	return check_finish("test-bn");
}
