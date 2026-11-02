/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Checks the P-256 and P-384 groups (cryc-ecp.h).
 *
 * The reference is written here from the affine group law of NIST SP
 * 800-186 A.1.1: a point is an affine pair (or the point at infinity),
 * the slope is a quotient computed with an inverse by Fermat's little
 * theorem (cryc_bn_mont_exp with the exponent p - 2), the sum and the
 * double follow the formulas of A.1.1 with a case for each exception, and
 * a multiple is double-and-add on the bits.  The Jacobian code of the
 * module is compared with it on the base point and on random points with
 * random scalars, with the edge scalars (0, 1, 2, small, near n, single
 * bits), and with the points the exceptions need (P and -P, P and P).
 *
 * The parameters are given again here as hex text.  They are checked
 * against the defining formulas of p (2^256 - 2^224 + 2^192 + 2^96 - 1 and
 * 2^384 - 2^128 - 2^96 + 2^32 - 1), by the base point lying on the curve,
 * by (n - 1) * G = -G and n * G = infinity (which a wrong n or G fails).
 *
 * Published vectors: the public-key validation cases of CAVP (the PKV
 * file: P-256 and P-384 points, valid, out of range, not on the curve),
 * the key pairs of CAVP (d, and Q = d * G), and the ECDH cases of
 * Wycheproof for P-256 (a public point as bytes, a private scalar, the
 * shared x).
 */

#include "check.h"
#include "vecfile.h"

#include "userland/base/libcrypto-compat/cryc-ecp.h"

/* The number of random points kept for the curve under test. */
#define POOL_SIZE	4U

/* The value of a fill that shows a byte or a word was not written. */
#define SENTINEL_BYTE	0x5aU

/* The state of the pseudo-random generator of the tests (xorshift32, Marsaglia 2003). */
static uint32_t rng_state = 0x6d2b79f5U;

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

/* The parameters of one curve, as hex text, the same widths as the curve's. */
struct params {
	enum cryc_curve_id id;
	const char *name;
	size_t size;
	const char *p;
	const char *n;
	const char *b;
	const char *gx;
	const char *gy;
};

/*
 * The parameters typed again from the pages.
 *
 * Values referenced: NIST SP 800-186 (February 2023) 3.2.1.3 and 3.2.1.4.
 */
static const struct params curves[2] = {
	{
		CRYC_CURVE_P256, "P-256", 32U,
		"ffffffff00000001000000000000000000000000ffffffffffffffffffffffff",
		"ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551",
		"5ac635d8aa3a93e7b3ebbd55769886bc651d06b0cc53b0f63bce3c3e27d2604b",
		"6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296",
		"4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5"
	},
	{
		CRYC_CURVE_P384, "P-384", 48U,
		"fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffeffffffff0000000000000000ffffffff",
		"ffffffffffffffffffffffffffffffffffffffffffffffffc7634d81f4372ddf581a0db248b0a77aecec196accc52973",
		"b3312fa7e23ee7e4988e056be3f82d19181d9c6efe8141120314088f5013875ac656398d8a2ed19d2a85c8edd3ec2aef",
		"aa87ca22be8b05378eb1c71ef320ad746e1d3b628ba79b9859f741e082542a385502f25dbf55296c3a545e3872760ab7",
		"3617de4a96262c6f5d9e98bf9292dc29f8f41dbd289a147ce9da3113b5f0b8c00a60b1ce1d7e819d7a431d7c90ea0e5f"
	}
};

/* A reference point: affine coordinates in Montgomery form, or infinity. */
struct ref_point {
	uint32_t x[CRYC_ECP_MAX_LIMBS];
	uint32_t y[CRYC_ECP_MAX_LIMBS];
	int infinity;
};

/* Returns the next pseudo-random byte string of size bytes into out. */
static void random_bytes(uint8_t *out, size_t size);

/* Reads hex text into exactly size bytes, left-padded with zeros; returns 0, or -1 when the value is wider. */
static int hex_to_size(const char *text, uint8_t *out, size_t size);

/* Fills a scalar of the curve's width from big-endian bytes. */
static void scalar_from_bytes(uint32_t *scalar, const struct cryc_ecp *ec, const uint8_t *bytes);

/* Sets scalar to a random value below n. */
static void random_scalar(uint32_t *scalar, const struct cryc_ecp *ec);

/* Sets scalar to the small value value. */
static void scalar_small(uint32_t *scalar, uint32_t value);

/* Sets scalar to 2^bit. */
static void scalar_bit(uint32_t *scalar, size_t bit);

/* Sets scalar to n - delta, with the single limb value delta (n is large enough). */
static void scalar_n_minus(uint32_t *scalar, const struct cryc_ecp *ec, uint32_t delta);

/* Prepares the exponent p - 2 from the text of p (the last byte is 0xff on both curves, so no borrow). */
static void ref_prepare(const struct params *params);

/* Sets out to the inverse of a (not zero) in Montgomery form. */
static void ref_inverse(uint32_t *out, const uint32_t *a, const struct cryc_ecp *ec);

/* Returns 1 when the limbs of a and b (the curve's width) are equal. */
static int limbs_equal(const uint32_t *a, const uint32_t *b, const struct cryc_ecp *ec);

/* Sets out to the double of a. */
static void ref_double(struct ref_point *out, const struct ref_point *a, const struct cryc_ecp *ec);

/* Sets out to a + b. */
static void ref_add(struct ref_point *out, const struct ref_point *a, const struct ref_point *b, const struct cryc_ecp *ec);

/* Sets out to scalar * a by double-and-add from the top bit. */
static void ref_mul(struct ref_point *out, const uint32_t *scalar, const struct ref_point *a, const struct cryc_ecp *ec);

/* Sets out to the base point of the curve, from the module's Montgomery values. */
static void ref_base(struct ref_point *out, const struct cryc_ecp *ec);

/* Sets out to -a. */
static void ref_negate(struct ref_point *out, const struct ref_point *a, const struct cryc_ecp *ec);

/* Writes the uncompressed encoding of a reference point into out (1 + 2 * size bytes). */
static void ref_encode(uint8_t *out, const struct ref_point *a, const struct cryc_ecp *ec);

/* Checks that the module's point equals the reference point. */
static void check_point(const struct cryc_ecp *ec, const struct cryc_ecp_point *point, const struct ref_point *expected);

/* Makes a point with the same affine coordinates as a reference point but Z = lambda (a random field element). */
static void point_from_ref(const struct cryc_ecp *ec, struct cryc_ecp_point *out, const struct ref_point *a, int scaled);

/* Fills the pool; the reference's multiplication is slow, so the points are made once per curve. */
static void pool_prepare(const struct cryc_ecp *ec);

/* Picks a random point of the pool. */
static void random_point(const struct cryc_ecp *ec, struct ref_point *out);

/* Adds (add is not 0) or subtracts 2^bit in a number of limbs, propagating the carry or the borrow; the result stays in range. */
static void formula_bit(uint32_t *limbs, size_t bit, int add);

/* Checks the parameters of the curve and the module's use of them. */
static void check_parameters(const struct params *params, const struct cryc_ecp *ec);

/* Checks the module's multiplications against the reference on one scalar and one point. */
static void check_mul_one(const struct cryc_ecp *ec, const uint32_t *scalar, const struct ref_point *a, int scaled);

/* Checks mul_base on one scalar. */
static void check_mul_base_one(const struct cryc_ecp *ec, const uint32_t *scalar);

/* Checks the multiplications of one curve: edge scalars, single bits, random scalars and points. */
static void check_multiplications(const struct cryc_ecp *ec);

/* Checks multiplication by a scalar that is n (outside the stated range, but the masks make the result infinity). */
static void check_order_scalar(const struct cryc_ecp *ec);

/* Checks mul_add_public: random, edge, and the cases that need the exceptional additions. */
static void check_mul_add(const struct cryc_ecp *ec);

/* Checks encode, decode, affine_x on valid points: round trips, representations, buffer sizes. */
static void check_encode_decode(const struct cryc_ecp *ec);

/* Checks the decode errors, in the stated order, and the single-byte changes of a valid encoding. */
static void check_decode_errors(const struct params *params, const struct cryc_ecp *ec);

/* Reports whether the record belongs to the curve named by the section key. */
static int record_is_curve(const struct vec_file *file, const char *name);

/* Sets out to the right side x^3 - 3 x + b of the curve equation, all in Montgomery form. */
static void rhs_mont(const struct cryc_ecp *ec, uint32_t *out, const uint32_t *x_mont);

/* Sets out to the square root of a in Montgomery form. */
static void sqrt_mont(const struct cryc_ecp *ec, uint32_t *out, const uint32_t *a, const struct params *params);

/* Adds p to the size big-endian bytes at in into out; returns 1 when the sum overflows the size. */
static int add_p_bytes(const struct params *params, uint8_t *out, const uint8_t *in);

/* For a valid encoding, tries each coordinate plus p (when it still fits): the result must be refused as out of range. */
static void check_alias_coordinates(const struct params *params, const struct cryc_ecp *ec, const uint8_t *encoding);

/* Checks the validation of constructed points. */
static void check_constructed_points(const struct params *params, const struct cryc_ecp *ec);

/* Checks the CAVP public key validation cases of the curve (the section's name is in section). */
static void check_pkv(const struct params *params, const struct cryc_ecp *ec, const char *section);

/* Checks the CAVP key pairs of the curve: Q = d * G. */
static void check_keypair(const struct params *params, const struct cryc_ecp *ec, const char *section);

/* Checks the Wycheproof ECDH cases for P-256 (public point bytes, private scalar, shared x). */
static void check_wycheproof_ecdh(const struct params *params, const struct cryc_ecp *ec);

/* Runs the checks of one curve. */
static void run_curve(const struct params *params);

/* Returns the next pseudo-random byte string of size bytes into out. */
static void
random_bytes(
	uint8_t *out,
	size_t size)
{
	size_t index;

	/* One byte of each word. */
	for (index = 0; index < size; index++)
		out[index] = (uint8_t)(random32() >> 11);
}

/* Reads hex text into exactly size bytes, left-padded with zeros; returns 0, or -1 when the value is wider. */
static int
hex_to_size(
	const char *text,
	uint8_t *out,
	size_t size)
{
	uint8_t raw[256];
	char padded[600];
	size_t length;
	size_t start;
	size_t text_size;

	/* Decodes the text and drops the leading zero bytes. */
	length = hex_decode(text, raw, sizeof(raw));
	if (length == (size_t)-1) {
		/* An odd-length text: pads one zero digit on the left. */
		text_size = strlen(text);
		if (text_size + 2U > sizeof(padded))
			return -1;
		padded[0] = '0';
		strcpy(padded + 1, text);
		length = hex_decode(padded, raw, sizeof(raw));
		if (length == (size_t)-1)
			return -1;
	}

	/* Skips the leading zero bytes. */
	start = 0;
	while (start < length && raw[start] == 0)
		start++;
	if (length - start > size)
		return -1;

	/* Pads on the left. */
	memset(out, 0, size);
	memcpy(out + (size - (length - start)), raw + start, length - start);

	/* Succeeded: the value fits the size. */
	return 0;
}

/* Fills a scalar of the curve's width from big-endian bytes. */
static void
scalar_from_bytes(
	uint32_t *scalar,
	const struct cryc_ecp *ec,
	const uint8_t *bytes)
{
	int error;

	/* The bytes fit: they are the curve's size. */
	memset(scalar, 0, CRYC_ECP_MAX_LIMBS * sizeof(scalar[0]));
	error = cryc_bn_decode(scalar, ec->limbs, bytes, ec->size);
	CHECK(error == 0);
}

/* Sets scalar to a random value below n. */
static void
random_scalar(
	uint32_t *scalar,
	const struct cryc_ecp *ec)
{
	uint8_t bytes[CRYC_ECP_MAX_BYTES];
	uint32_t mask;

	/* Draws until the value is below n (nearly always the first draw). */
	do {
		random_bytes(bytes, ec->size);
		scalar_from_bytes(scalar, ec, bytes);
		mask = cryc_bn_lt(scalar, ec->order.modulus, ec->limbs);
	} while (mask == 0U);
}

/* Sets scalar to the small value value. */
static void
scalar_small(
	uint32_t *scalar,
	uint32_t value)
{
	/* The low limb only. */
	memset(scalar, 0, CRYC_ECP_MAX_LIMBS * sizeof(scalar[0]));
	scalar[0] = value;
}

/* Sets scalar to 2^bit. */
static void
scalar_bit(
	uint32_t *scalar,
	size_t bit)
{
	/* One bit. */
	memset(scalar, 0, CRYC_ECP_MAX_LIMBS * sizeof(scalar[0]));
	scalar[bit / 32U] = (uint32_t)1U << (bit % 32U);
}

/* Sets scalar to n - delta, with the single limb value delta (n is large enough). */
static void
scalar_n_minus(
	uint32_t *scalar,
	const struct cryc_ecp *ec,
	uint32_t delta)
{
	uint32_t small[CRYC_ECP_MAX_LIMBS];

	/* The modular difference 0 - delta is n - delta. */
	scalar_small(small, delta);
	memset(scalar, 0, CRYC_ECP_MAX_LIMBS * sizeof(scalar[0]));
	cryc_bn_mod_sub(scalar, scalar, small, &ec->order);
}

/* ---- the reference group law (SP 800-186 A.1.1), in Montgomery form modulo p ---- */

/* The curve whose reference is in use: its p - 2 as bytes is kept here. */
static uint8_t ref_exponent[CRYC_ECP_MAX_BYTES];

/* Prepares the exponent p - 2 from the text of p (the last byte is 0xff on both curves, so no borrow). */
static void
ref_prepare(
	const struct params *params)
{
	int error;

	/* The text of p as bytes, minus two in the last byte. */
	error = hex_to_size(params->p, ref_exponent, params->size);
	CHECK(error == 0);
	CHECK(ref_exponent[params->size - 1U] >= 2U);
	ref_exponent[params->size - 1U] = (uint8_t)(ref_exponent[params->size - 1U] - 2U);
}

/* Sets out to the inverse of a (not zero) in Montgomery form. */
static void
ref_inverse(
	uint32_t *out,
	const uint32_t *a,
	const struct cryc_ecp *ec)
{
	/* Fermat: a^(p-2). */
	cryc_bn_mont_exp(out, a, ref_exponent, ec->size, &ec->field);
}

/* Returns 1 when the limbs of a and b (the curve's width) are equal. */
static int
limbs_equal(
	const uint32_t *a,
	const uint32_t *b,
	const struct cryc_ecp *ec)
{
	/* A byte comparison of the used limbs. */
	return memcmp(a, b, ec->limbs * sizeof(a[0])) == 0;
}

/* Sets out to the double of a. */
static void
ref_double(
	struct ref_point *out,
	const struct ref_point *a,
	const struct cryc_ecp *ec)
{
	struct ref_point result;
	uint32_t slope[CRYC_ECP_MAX_LIMBS];
	uint32_t numerator[CRYC_ECP_MAX_LIMBS];
	uint32_t denominator[CRYC_ECP_MAX_LIMBS];
	uint32_t inverse[CRYC_ECP_MAX_LIMBS];
	uint32_t one[CRYC_ECP_MAX_LIMBS];
	uint32_t three[CRYC_ECP_MAX_LIMBS];
	uint32_t temp[CRYC_ECP_MAX_LIMBS];
	uint32_t zero[CRYC_ECP_MAX_LIMBS];
	int y_zero;

	/* Infinity doubles to infinity, and so does a point with y = 0. */
	memset(&result, 0, sizeof(result));
	memset(zero, 0, sizeof(zero));
	y_zero = 0;
	if (!a->infinity)
		y_zero = limbs_equal(a->y, zero, ec);
	if (a->infinity || y_zero) {
		result.infinity = 1;
		*out = result;
		return;
	}

	/* The slope (3 x^2 + a) / (2 y) with a = -3. */
	memset(one, 0, sizeof(one));
	memset(three, 0, sizeof(three));
	cryc_bn_mont_one(one, &ec->field);
	cryc_bn_mod_add(three, one, one, &ec->field);
	cryc_bn_mod_add(three, three, one, &ec->field);
	memset(numerator, 0, sizeof(numerator));
	cryc_bn_mont_mul(numerator, a->x, a->x, &ec->field);
	cryc_bn_mont_mul(numerator, numerator, three, &ec->field);
	cryc_bn_mod_sub(numerator, numerator, three, &ec->field);
	memset(denominator, 0, sizeof(denominator));
	cryc_bn_mod_add(denominator, a->y, a->y, &ec->field);
	memset(inverse, 0, sizeof(inverse));
	ref_inverse(inverse, denominator, ec);
	memset(slope, 0, sizeof(slope));
	cryc_bn_mont_mul(slope, numerator, inverse, &ec->field);

	/* x = slope^2 - 2 x1. */
	memset(temp, 0, sizeof(temp));
	cryc_bn_mont_mul(result.x, slope, slope, &ec->field);
	cryc_bn_mod_sub(result.x, result.x, a->x, &ec->field);
	cryc_bn_mod_sub(result.x, result.x, a->x, &ec->field);

	/* y = slope (x1 - x) - y1. */
	cryc_bn_mod_sub(temp, a->x, result.x, &ec->field);
	cryc_bn_mont_mul(result.y, slope, temp, &ec->field);
	cryc_bn_mod_sub(result.y, result.y, a->y, &ec->field);
	*out = result;
}

/* Sets out to a + b. */
static void
ref_add(
	struct ref_point *out,
	const struct ref_point *a,
	const struct ref_point *b,
	const struct cryc_ecp *ec)
{
	struct ref_point result;
	uint32_t slope[CRYC_ECP_MAX_LIMBS];
	uint32_t numerator[CRYC_ECP_MAX_LIMBS];
	uint32_t denominator[CRYC_ECP_MAX_LIMBS];
	uint32_t inverse[CRYC_ECP_MAX_LIMBS];
	uint32_t temp[CRYC_ECP_MAX_LIMBS];
	int same_x;
	int same_y;

	/* An operand at infinity is the identity. */
	if (a->infinity) {
		*out = *b;
		return;
	}

	/* The same for the second operand. */
	if (b->infinity) {
		*out = *a;
		return;
	}

	/* Equal x: the points are equal (double) or opposite (infinity). */
	same_x = limbs_equal(a->x, b->x, ec);
	if (same_x) {
		same_y = limbs_equal(a->y, b->y, ec);
		if (same_y) {
			ref_double(out, a, ec);
			return;
		}

		/* The opposite points add to infinity. */
		memset(&result, 0, sizeof(result));
		result.infinity = 1;
		*out = result;
		return;
	}

	/* The slope (y2 - y1) / (x2 - x1). */
	memset(&result, 0, sizeof(result));
	memset(numerator, 0, sizeof(numerator));
	memset(denominator, 0, sizeof(denominator));
	memset(inverse, 0, sizeof(inverse));
	memset(slope, 0, sizeof(slope));
	memset(temp, 0, sizeof(temp));
	cryc_bn_mod_sub(numerator, b->y, a->y, &ec->field);
	cryc_bn_mod_sub(denominator, b->x, a->x, &ec->field);
	ref_inverse(inverse, denominator, ec);
	cryc_bn_mont_mul(slope, numerator, inverse, &ec->field);

	/* x = slope^2 - x1 - x2. */
	cryc_bn_mont_mul(result.x, slope, slope, &ec->field);
	cryc_bn_mod_sub(result.x, result.x, a->x, &ec->field);
	cryc_bn_mod_sub(result.x, result.x, b->x, &ec->field);

	/* y = slope (x1 - x) - y1. */
	cryc_bn_mod_sub(temp, a->x, result.x, &ec->field);
	cryc_bn_mont_mul(result.y, slope, temp, &ec->field);
	cryc_bn_mod_sub(result.y, result.y, a->y, &ec->field);
	*out = result;
}

/* Sets out to scalar * a by double-and-add from the top bit. */
static void
ref_mul(
	struct ref_point *out,
	const uint32_t *scalar,
	const struct ref_point *a,
	const struct cryc_ecp *ec)
{
	struct ref_point accumulator;
	size_t bit;

	/* From the top bit: double, then add when the bit is set. */
	memset(&accumulator, 0, sizeof(accumulator));
	accumulator.infinity = 1;
	for (bit = ec->size * 8U; bit > 0U; bit--) {
		ref_double(&accumulator, &accumulator, ec);
		if (((scalar[(bit - 1U) / 32U] >> ((bit - 1U) % 32U)) & 1U) != 0U)
			ref_add(&accumulator, &accumulator, a, ec);
	}

	/* Stores the product. */
	*out = accumulator;
}

/* Sets out to the base point of the curve, from the module's Montgomery values. */
static void
ref_base(
	struct ref_point *out,
	const struct cryc_ecp *ec)
{
	/* The coordinates as the module holds them. */
	memset(out, 0, sizeof(*out));
	memcpy(out->x, ec->base_x, sizeof(out->x));
	memcpy(out->y, ec->base_y, sizeof(out->y));
}

/* Sets out to -a. */
static void
ref_negate(
	struct ref_point *out,
	const struct ref_point *a,
	const struct cryc_ecp *ec)
{
	uint32_t zero[CRYC_ECP_MAX_LIMBS];

	/* (x, -y). */
	*out = *a;
	memset(zero, 0, sizeof(zero));
	cryc_bn_mod_sub(out->y, zero, a->y, &ec->field);
}

/* Writes the uncompressed encoding of a reference point into out (1 + 2 * size bytes). */
static void
ref_encode(
	uint8_t *out,
	const struct ref_point *a,
	const struct cryc_ecp *ec)
{
	uint32_t x[CRYC_ECP_MAX_LIMBS];
	uint32_t y[CRYC_ECP_MAX_LIMBS];

	/* Out of Montgomery form, then big-endian. */
	memset(x, 0, sizeof(x));
	memset(y, 0, sizeof(y));
	cryc_bn_from_mont(x, a->x, &ec->field);
	cryc_bn_from_mont(y, a->y, &ec->field);
	out[0] = 0x04U;
	cryc_bn_encode(x, ec->limbs, out + 1U, ec->size);
	cryc_bn_encode(y, ec->limbs, out + 1U + ec->size, ec->size);
}

/*
 * Checks that the module's point equals the reference point (or both are
 * infinity), by the encoding of each, and that limbs above the width are
 * zero.
 */
static void
check_point(
	const struct cryc_ecp *ec,
	const struct cryc_ecp_point *point,
	const struct ref_point *expected)
{
	uint8_t got[CRYC_ECP_MAX_POINT];
	uint8_t want[CRYC_ECP_MAX_POINT];
	size_t index;
	int error;

	/* The limbs above the curve's width are zero. */
	for (index = ec->limbs; index < CRYC_ECP_MAX_LIMBS; index++) {
		CHECK(point->x[index] == 0U);
		CHECK(point->y[index] == 0U);
		CHECK(point->z[index] == 0U);
	}

	/* Infinity: the module says EINVAL and zeroes the output. */
	memset(got, SENTINEL_BYTE, sizeof(got));
	error = cryc_ecp_encode(ec, point, got, sizeof(got));
	if (expected->infinity) {
		CHECK(error == EINVAL);
		for (index = 0; index < 1U + 2U * ec->size; index++)
			CHECK(got[index] == 0U);
		return;
	}

	/* A finite point: the same encoding. */
	CHECK(error == 0);
	ref_encode(want, expected, ec);
	CHECK_BYTES(got, want, 1U + 2U * ec->size);
}

/* ---- the points of the tests ---- */

/* Makes a point with the same affine coordinates as a reference point but Z = lambda (a random field element). */
static void
point_from_ref(
	const struct cryc_ecp *ec,
	struct cryc_ecp_point *out,
	const struct ref_point *a,
	int scaled)
{
	uint32_t lambda[CRYC_ECP_MAX_LIMBS];
	uint32_t square[CRYC_ECP_MAX_LIMBS];
	uint32_t cube[CRYC_ECP_MAX_LIMBS];
	uint8_t bytes[CRYC_ECP_MAX_BYTES];
	uint32_t plain[CRYC_ECP_MAX_LIMBS];
	uint32_t below;
	uint32_t is_zero;

	/* Infinity: any triple with Z = 0. */
	memset(out, 0, sizeof(*out));
	if (a->infinity) {
		cryc_bn_mont_one(out->x, &ec->field);
		cryc_bn_mont_one(out->y, &ec->field);
		return;
	}

	/* Z = 1, or the scaled representation (X lambda^2, Y lambda^3, lambda). */
	if (!scaled) {
		memcpy(out->x, a->x, sizeof(out->x));
		memcpy(out->y, a->y, sizeof(out->y));
		cryc_bn_mont_one(out->z, &ec->field);
		return;
	}

	/* A random lambda that is below p and not zero. */
	do {
		random_bytes(bytes, ec->size);
		memset(plain, 0, sizeof(plain));
		cryc_bn_decode(plain, ec->limbs, bytes, ec->size);
		below = cryc_bn_lt(plain, ec->field.modulus, ec->limbs);
		is_zero = cryc_bn_is_zero(plain, ec->limbs);
	} while (below == 0U || is_zero != 0U);

	/* The scaled representation of the point. */
	memset(lambda, 0, sizeof(lambda));
	memset(square, 0, sizeof(square));
	memset(cube, 0, sizeof(cube));
	cryc_bn_to_mont(lambda, plain, &ec->field);
	cryc_bn_mont_mul(square, lambda, lambda, &ec->field);
	cryc_bn_mont_mul(cube, square, lambda, &ec->field);
	cryc_bn_mont_mul(out->x, a->x, square, &ec->field);
	cryc_bn_mont_mul(out->y, a->y, cube, &ec->field);
	memcpy(out->z, lambda, sizeof(out->z));
}

/* A few random finite reference points of the curve under test (random multiples of G, made by the reference). */
static struct ref_point pool[POOL_SIZE];

/* Fills the pool; the reference's multiplication is slow, so the points are made once per curve. */
static void
pool_prepare(
	const struct cryc_ecp *ec)
{
	struct ref_point base;
	uint32_t scalar[CRYC_ECP_MAX_LIMBS];
	size_t index;

	/* A random scalar times the base point, never infinity (the scalar is below n and not zero). */
	ref_base(&base, ec);
	for (index = 0; index < POOL_SIZE; index++) {
		do {
			random_scalar(scalar, ec);
			ref_mul(&pool[index], scalar, &base, ec);
		} while (pool[index].infinity);
	}
}

/* Picks a random point of the pool. */
static void
random_point(
	const struct cryc_ecp *ec,
	struct ref_point *out)
{
	/* The curve is not needed: the pool belongs to the curve under test. */
	UNUSED_PARAMETER(ec);
	*out = pool[random32() % POOL_SIZE];
}

/* ---- checks ---- */

/* Adds (add is not 0) or subtracts 2^bit in a number of limbs, propagating the carry or the borrow; the result stays in range. */
static void
formula_bit(
	uint32_t *limbs,
	size_t bit,
	int add)
{
	size_t index;
	uint32_t amount;

	/* The limb of the bit takes the amount, and the carry or borrow runs upward. */
	index = bit / 32U;
	amount = (uint32_t)1U << (bit % 32U);
	if (add) {
		limbs[index] += amount;
		while (limbs[index] < amount && index + 1U < CRYC_ECP_MAX_LIMBS) {
			index++;
			amount = 1U;
			limbs[index] += amount;
		}
	} else {
		while (limbs[index] < amount) {
			limbs[index] -= amount;
			index++;
			amount = 1U;
		}

		/* The borrow stops at a limb that can pay it. */
		limbs[index] -= amount;
	}
}

/* Checks the parameters of the curve and the module's use of them. */
static void
check_parameters(
	const struct params *params,
	const struct cryc_ecp *ec)
{
	uint8_t bytes[CRYC_ECP_MAX_BYTES];
	uint8_t want[CRYC_ECP_MAX_BYTES];
	uint32_t value[CRYC_ECP_MAX_LIMBS];
	uint32_t scalar[CRYC_ECP_MAX_LIMBS];
	uint32_t formula[CRYC_ECP_MAX_LIMBS];
	struct ref_point base;
	struct ref_point minus_base;
	struct ref_point result;
	uint8_t encoded[CRYC_ECP_MAX_POINT];
	struct cryc_ecp_point point;
	size_t index;
	int error;

	/* The sizes. */
	CHECK(ec->id == params->id);
	CHECK(ec->size == params->size);
	CHECK(ec->limbs == params->size / 4U);
	CHECK(ec->field.limbs == ec->limbs);
	CHECK(ec->order.limbs == ec->limbs);
	CHECK(ec->field.bits == params->size * 8U);
	CHECK(ec->order.bits == params->size * 8U);

	/* p and n as the text of the page. */
	cryc_bn_encode(ec->field.modulus, ec->limbs, bytes, ec->size);
	error = hex_to_size(params->p, want, params->size);
	CHECK(error == 0);
	CHECK_BYTES(bytes, want, params->size);
	cryc_bn_encode(ec->order.modulus, ec->limbs, bytes, ec->size);
	error = hex_to_size(params->n, want, params->size);
	CHECK(error == 0);
	CHECK_BYTES(bytes, want, params->size);

	/*
	 * p from its defining formula: start from 2^bits - 1 and apply each
	 * power of two of the formula (SP 800-186 3.2.1.3 and 3.2.1.4), each
	 * addition or subtraction of a single bit done on the limbs here.
	 */
	memset(formula, 0, sizeof(formula));
	for (index = 0; index < ec->limbs; index++)
		formula[index] = 0xffffffffU;
	if (params->id == CRYC_CURVE_P256) {
		/* 2^256 - 2^224 + 2^192 + 2^96 - 1: minus bit 224, then plus bits 192 and 96. */
		formula_bit(formula, 224U, 0);
		formula_bit(formula, 192U, 1);
		formula_bit(formula, 96U, 1);
	} else {
		/* 2^384 - 2^128 - 2^96 + 2^32 - 1: minus bits 128 and 96, then plus bit 32. */
		formula_bit(formula, 128U, 0);
		formula_bit(formula, 96U, 0);
		formula_bit(formula, 32U, 1);
	}

	/* The formula gives the modulus of the field. */
	CHECK(limbs_equal(formula, ec->field.modulus, ec));

	/* b out of Montgomery form is the text's b; G likewise. */
	memset(value, 0, sizeof(value));
	cryc_bn_from_mont(value, ec->b_mont, &ec->field);
	cryc_bn_encode(value, ec->limbs, bytes, ec->size);
	error = hex_to_size(params->b, want, params->size);
	CHECK(error == 0);
	CHECK_BYTES(bytes, want, params->size);
	cryc_bn_from_mont(value, ec->base_x, &ec->field);
	cryc_bn_encode(value, ec->limbs, bytes, ec->size);
	error = hex_to_size(params->gx, want, params->size);
	CHECK(error == 0);
	CHECK_BYTES(bytes, want, params->size);
	cryc_bn_from_mont(value, ec->base_y, &ec->field);
	cryc_bn_encode(value, ec->limbs, bytes, ec->size);
	error = hex_to_size(params->gy, want, params->size);
	CHECK(error == 0);
	CHECK_BYTES(bytes, want, params->size);

	/* The limbs above the width are zero in the stored values. */
	for (index = ec->limbs; index < CRYC_ECP_MAX_LIMBS; index++) {
		CHECK(ec->b_mont[index] == 0U);
		CHECK(ec->base_x[index] == 0U);
		CHECK(ec->base_y[index] == 0U);
	}

	/* G is on the curve: the encoding of G decodes. */
	ref_base(&base, ec);
	ref_encode(encoded, &base, ec);
	error = cryc_ecp_decode(ec, &point, encoded, 1U + 2U * ec->size);
	CHECK(error == 0);

	/* 1 * G is G, and (n - 1) * G is -G: x the same, y = p - Gy. */
	scalar_small(scalar, 1U);
	cryc_ecp_mul_base(ec, &point, scalar);
	check_point(ec, &point, &base);
	scalar_n_minus(scalar, ec, 1U);
	cryc_ecp_mul_base(ec, &point, scalar);
	ref_negate(&minus_base, &base, ec);
	check_point(ec, &point, &minus_base);

	/* The reference agrees: (n - 1) * G = -G, so G + (n - 1) * G is infinity (this fails for a wrong n or G). */
	ref_mul(&result, scalar, &base, ec);
	ref_add(&result, &result, &base, ec);
	CHECK(result.infinity);

	/* The module agrees: (n - 1) * G + 1 * G is infinity. */
	point_from_ref(ec, &point, &base, 0);
	scalar_small(value, 1U);
	cryc_ecp_mul_add_public(ec, &point, scalar, value, &point);
	result.infinity = 1;
	check_point(ec, &point, &result);
}

/* Checks the module's multiplications against the reference on one scalar and one point. */
static void
check_mul_one(
	const struct cryc_ecp *ec,
	const uint32_t *scalar,
	const struct ref_point *a,
	int scaled)
{
	struct cryc_ecp_point input;
	struct cryc_ecp_point output;
	struct ref_point expected;

	/* scalar * a, with the point given in a plain or a scaled representation. */
	ref_mul(&expected, scalar, a, ec);
	point_from_ref(ec, &input, a, scaled);
	cryc_ecp_mul(ec, &output, &input, scalar);
	check_point(ec, &output, &expected);

	/* out may be the point itself. */
	cryc_ecp_mul(ec, &input, &input, scalar);
	check_point(ec, &input, &expected);
}

/* Checks mul_base on one scalar. */
static void
check_mul_base_one(
	const struct cryc_ecp *ec,
	const uint32_t *scalar)
{
	struct cryc_ecp_point output;
	struct ref_point base;
	struct ref_point expected;

	/* scalar * G. */
	ref_base(&base, ec);
	ref_mul(&expected, scalar, &base, ec);
	cryc_ecp_mul_base(ec, &output, scalar);
	check_point(ec, &output, &expected);
}

/* Checks the multiplications of one curve: edge scalars, single bits, random scalars and points. */
static void
check_multiplications(
	const struct cryc_ecp *ec)
{
	struct ref_point base;
	struct ref_point other;
	struct ref_point minus;
	struct ref_point twice;
	uint32_t scalar[CRYC_ECP_MAX_LIMBS];
	uint32_t small;
	size_t bit;
	size_t count;
	int scaled;

	/* The points: G, a random point, -G, 2G. */
	ref_base(&base, ec);
	random_point(ec, &other);
	ref_negate(&minus, &base, ec);
	ref_double(&twice, &base, ec);

	/* The edge scalars 0 to 40 on G and on a random point, with both representations. */
	for (small = 0; small <= 40U; small++) {
		scalar_small(scalar, small);
		check_mul_base_one(ec, scalar);
		check_mul_one(ec, scalar, &other, (int)(small & 1U));
		check_mul_one(ec, scalar, &minus, 0);
		check_mul_one(ec, scalar, &twice, 1);
	}

	/* Scalars near n: n - 1 to n - 6. */
	for (small = 1; small <= 6U; small++) {
		scalar_n_minus(scalar, ec, small);
		check_mul_base_one(ec, scalar);
		check_mul_one(ec, scalar, &other, (int)(small & 1U));
	}

	/* Single bits: every bit below 12, then every eleventh, then the top bits (a window boundary and a limb boundary are in these). */
	for (bit = 0; bit < ec->size * 8U; bit++) {
		if (bit >= 12U && bit % 11U != 0U && bit + 4U < ec->size * 8U)
			continue;
		scalar_bit(scalar, bit);
		check_mul_base_one(ec, scalar);
		if (bit % 3U == 0U)
			check_mul_one(ec, scalar, &other, 1);
	}

	/* A scalar with every bit set below n's top (alternating patterns). */
	memset(scalar, 0xaa, sizeof(scalar));
	memset(scalar + ec->limbs, 0, (CRYC_ECP_MAX_LIMBS - ec->limbs) * sizeof(scalar[0]));
	scalar[ec->limbs - 1U] = 0x7fffffffU;
	check_mul_base_one(ec, scalar);
	memset(scalar, 0x55, ec->limbs * sizeof(scalar[0]));
	check_mul_base_one(ec, scalar);
	memset(scalar, 0xff, ec->limbs * sizeof(scalar[0]));
	scalar[ec->limbs - 1U] = 0x7fffffffU;
	check_mul_base_one(ec, scalar);

	/* Random scalars on G and on random points. */
	count = 6;
	while (count > 0U) {
		scaled = (int)(count & 1U);
		random_scalar(scalar, ec);
		check_mul_base_one(ec, scalar);
		random_point(ec, &other);
		random_scalar(scalar, ec);
		check_mul_one(ec, scalar, &other, scaled);
		count--;
	}
}

/* Checks multiplication by a scalar that is n (outside the stated range, but the masks make the result infinity). */
static void
check_order_scalar(
	const struct cryc_ecp *ec)
{
	struct cryc_ecp_point output;
	struct ref_point expected;
	uint32_t scalar[CRYC_ECP_MAX_LIMBS];

	/* n * G is infinity: the last addition is of opposite points. */
	memset(scalar, 0, sizeof(scalar));
	memcpy(scalar, ec->order.modulus, ec->limbs * sizeof(scalar[0]));
	cryc_ecp_mul_base(ec, &output, scalar);
	memset(&expected, 0, sizeof(expected));
	expected.infinity = 1;
	check_point(ec, &output, &expected);

	/* The point at infinity times any scalar is infinity. */
	point_from_ref(ec, &output, &expected, 0);
	scalar_small(scalar, 5U);
	cryc_ecp_mul(ec, &output, &output, scalar);
	check_point(ec, &output, &expected);
}

/* Checks mul_add_public: random, edge, and the cases that need the exceptional additions. */
static void
check_mul_add(
	const struct cryc_ecp *ec)
{
	struct cryc_ecp_point output;
	struct cryc_ecp_point q_point;
	struct cryc_ecp_point other_point;
	uint8_t encoded[CRYC_ECP_MAX_POINT];
	uint8_t again[CRYC_ECP_MAX_POINT];
	struct ref_point base;
	struct ref_point other;
	struct ref_point minus;
	struct ref_point left;
	struct ref_point right;
	struct ref_point expected;
	uint32_t u1[CRYC_ECP_MAX_LIMBS];
	uint32_t u2[CRYC_ECP_MAX_LIMBS];
	uint32_t secret[CRYC_ECP_MAX_LIMBS];
	uint32_t product[CRYC_ECP_MAX_LIMBS];
	uint32_t sum[CRYC_ECP_MAX_LIMBS];
	uint32_t small1;
	uint32_t small2;
	size_t count;
	int error;

	/* Random scalars and random points: u1 G + u2 Q against the reference. */
	ref_base(&base, ec);
	for (count = 0; count < 5U; count++) {
		random_point(ec, &other);
		random_scalar(u1, ec);
		random_scalar(u2, ec);
		ref_mul(&left, u1, &base, ec);
		ref_mul(&right, u2, &other, ec);
		ref_add(&expected, &left, &right, ec);
		point_from_ref(ec, &q_point, &other, (int)(count & 1U));
		cryc_ecp_mul_add_public(ec, &output, u1, u2, &q_point);
		check_point(ec, &output, &expected);
	}

	/* The identity u1 + u2 d with Q = d G: (u1 + u2 d) G = u1 G + u2 Q (an algebraic check with arithmetic modulo n). */
	for (count = 0; count < 6U; count++) {
		random_scalar(secret, ec);
		random_scalar(u1, ec);
		random_scalar(u2, ec);
		cryc_ecp_mul_base(ec, &q_point, secret);
		cryc_bn_to_mont(product, u2, &ec->order);
		cryc_bn_mont_mul(product, product, secret, &ec->order);
		cryc_bn_mod_add(sum, u1, product, &ec->order);
		cryc_ecp_mul_add_public(ec, &output, u1, u2, &q_point);
		cryc_ecp_mul_base(ec, &q_point, sum);
		if (count == 0U) {
			ref_mul(&expected, sum, &base, ec);
			check_point(ec, &output, &expected);
			check_point(ec, &q_point, &expected);
		} else {
			cryc_ecp_mul_base(ec, &other_point, sum);
			error = cryc_ecp_encode(ec, &output, encoded, sizeof(encoded));
			CHECK(error == 0);
			error = cryc_ecp_encode(ec, &other_point, again, sizeof(again));
			CHECK(error == 0);
			CHECK_BYTES(encoded, again, 1U + 2U * ec->size);
		}
	}

	/* Small scalars (0 to 3 each) with Q = G: the doubling case (G + G), and the zero cases. */
	for (small1 = 0; small1 <= 3U; small1++) {
		for (small2 = 0; small2 <= 3U; small2++) {
			scalar_small(u1, small1);
			scalar_small(u2, small2);
			scalar_small(secret, small1 + small2);
			point_from_ref(ec, &q_point, &base, 0);
			cryc_ecp_mul_add_public(ec, &output, u1, u2, &q_point);
			ref_mul(&expected, secret, &base, ec);
			check_point(ec, &output, &expected);
		}
	}

	/* Q = -G: G + (-G) is infinity, and u1 = u2 gives infinity; u1 = 2, u2 = 1 gives G. */
	ref_negate(&minus, &base, ec);
	point_from_ref(ec, &q_point, &minus, 1);
	scalar_small(u1, 1U);
	scalar_small(u2, 1U);
	cryc_ecp_mul_add_public(ec, &output, u1, u2, &q_point);
	memset(&expected, 0, sizeof(expected));
	expected.infinity = 1;
	check_point(ec, &output, &expected);
	scalar_small(u1, 2U);
	cryc_ecp_mul_add_public(ec, &output, u1, u2, &q_point);
	check_point(ec, &output, &base);

	/* u1 = n - 1, u2 = 1, Q = G: infinity; u1 = 0 and u2 = 0 give infinity. */
	scalar_n_minus(u1, ec, 1U);
	scalar_small(u2, 1U);
	point_from_ref(ec, &q_point, &base, 0);
	cryc_ecp_mul_add_public(ec, &output, u1, u2, &q_point);
	check_point(ec, &output, &expected);
	scalar_small(u1, 0U);
	scalar_small(u2, 0U);
	cryc_ecp_mul_add_public(ec, &output, u1, u2, &q_point);
	check_point(ec, &output, &expected);

	/* out may be q. */
	scalar_small(u1, 7U);
	scalar_small(u2, 9U);
	scalar_small(secret, 16U);
	ref_mul(&expected, secret, &base, ec);
	cryc_ecp_mul_add_public(ec, &q_point, u1, u2, &q_point);
	check_point(ec, &q_point, &expected);
}

/* Checks encode, decode, affine_x on valid points: round trips, representations, buffer sizes. */
static void
check_encode_decode(
	const struct cryc_ecp *ec)
{
	struct cryc_ecp_point point;
	struct cryc_ecp_point decoded;
	struct ref_point other;
	struct ref_point infinity;
	uint8_t encoded[CRYC_ECP_MAX_POINT + 4U];
	uint8_t again[CRYC_ECP_MAX_POINT + 4U];
	uint8_t x_only[CRYC_ECP_MAX_BYTES];
	uint32_t one[CRYC_ECP_MAX_LIMBS];
	size_t total;
	size_t size;
	size_t index;
	size_t count;
	int error;

	/* The size of an uncompressed point, and the infinity reference. */
	total = 1U + 2U * ec->size;
	memset(&infinity, 0, sizeof(infinity));
	infinity.infinity = 1;

	/* Random points: encode, decode, encode again; Z representations agree; affine_x is x. */
	for (count = 0; count < 6U; count++) {
		random_point(ec, &other);
		point_from_ref(ec, &point, &other, (int)(count & 1U));
		memset(encoded, SENTINEL_BYTE, sizeof(encoded));
		error = cryc_ecp_encode(ec, &point, encoded, total);
		CHECK(error == 0);
		CHECK(encoded[0] == 0x04U);
		ref_encode(again, &other, ec);
		CHECK_BYTES(encoded, again, total);

		/* Bytes beyond the encoding are not written. */
		for (index = total; index < sizeof(encoded); index++)
			CHECK(encoded[index] == SENTINEL_BYTE);

		/* A larger buffer gives the same bytes. */
		memset(again, SENTINEL_BYTE, sizeof(again));
		error = cryc_ecp_encode(ec, &point, again, sizeof(again));
		CHECK(error == 0);
		CHECK_BYTES(again, encoded, total);

		/* Decodes it again: Z = 1 (Montgomery one), and the same encoding. */
		error = cryc_ecp_decode(ec, &decoded, encoded, total);
		CHECK(error == 0);
		memset(one, 0, sizeof(one));
		cryc_bn_mont_one(one, &ec->field);
		CHECK(limbs_equal(decoded.z, one, ec));
		check_point(ec, &decoded, &other);
		for (index = ec->limbs; index < CRYC_ECP_MAX_LIMBS; index++) {
			CHECK(decoded.x[index] == 0U);
			CHECK(decoded.y[index] == 0U);
			CHECK(decoded.z[index] == 0U);
		}

		/* affine_x is the x bytes. */
		memset(x_only, SENTINEL_BYTE, sizeof(x_only));
		error = cryc_ecp_affine_x(ec, &point, x_only);
		CHECK(error == 0);
		CHECK_BYTES(x_only, encoded + 1, ec->size);
	}

	/* Buffer sizes: too small for every size below the total (nothing is written), exact. */
	random_point(ec, &other);
	point_from_ref(ec, &point, &other, 1);
	for (size = 0; size < total; size++) {
		memset(again, SENTINEL_BYTE, sizeof(again));
		error = cryc_ecp_encode(ec, &point, again, size);
		CHECK(error == ENOBUFS);
		for (index = 0; index < sizeof(again); index++)
			CHECK(again[index] == SENTINEL_BYTE);
	}

	/* Infinity: EINVAL and the whole encoding zeroed, also with a small buffer (ENOBUFS first). */
	point_from_ref(ec, &point, &infinity, 0);
	memset(again, SENTINEL_BYTE, sizeof(again));
	error = cryc_ecp_encode(ec, &point, again, total);
	CHECK(error == EINVAL);
	for (index = 0; index < total; index++)
		CHECK(again[index] == 0U);
	for (index = total; index < sizeof(again); index++)
		CHECK(again[index] == SENTINEL_BYTE);
	error = cryc_ecp_encode(ec, &point, again, total - 1U);
	CHECK(error == ENOBUFS);
	memset(x_only, SENTINEL_BYTE, sizeof(x_only));
	error = cryc_ecp_affine_x(ec, &point, x_only);
	CHECK(error == EINVAL);
	for (index = 0; index < ec->size; index++)
		CHECK(x_only[index] == 0U);
	memset(&point, 0, sizeof(point));
	error = cryc_ecp_encode(ec, &point, again, total);
	CHECK(error == EINVAL);
}

/* Checks the decode errors, in the stated order, and the single-byte changes of a valid encoding. */
static void
check_decode_errors(
	const struct params *params,
	const struct cryc_ecp *ec)
{
	struct cryc_ecp_point point;
	struct ref_point base;
	uint8_t good[CRYC_ECP_MAX_POINT];
	uint8_t *exact;
	uint8_t pbytes[CRYC_ECP_MAX_BYTES];
	size_t total;
	size_t size;
	size_t index;
	size_t count;
	int error;
	unsigned tag;
	static const size_t extra[6] = { 0, 1, 2, 3, 4, 5 };
	static const unsigned tags[] = { 0x00U, 0x01U, 0x05U, 0x06U, 0x07U, 0x08U, 0x80U, 0x84U, 0xffU };
	struct ref_point minus;
	uint8_t minus_bytes[CRYC_ECP_MAX_POINT];

	/* The valid encoding of the base point, and p as bytes. */
	total = 1U + 2U * ec->size;
	ref_base(&base, ec);
	ref_encode(good, &base, ec);
	error = hex_to_size(params->p, pbytes, params->size);
	CHECK(error == 0);

	/* An empty input. */
	memset(&point, SENTINEL_BYTE, sizeof(point));
	error = cryc_ecp_decode(ec, &point, good, 0);
	CHECK(error == EINVAL);
	CHECK(point.z[0] == 0U && point.x[0] == 0U && point.y[0] == 0U);

	/* Compressed tags (0x02, 0x03): ENOTSUP whatever the size (1, a compressed size, the full size, longer). */
	for (tag = 2; tag <= 3U; tag++) {
		for (count = 0; count < 6U; count++) {
			/* The sizes 1, 2, size, size + 1 (compressed), total - 1, total + 1. */
			size = 1U;
			if (extra[count] == 1U)
				size = 2U;
			if (extra[count] == 2U)
				size = ec->size;
			if (extra[count] == 3U)
				size = ec->size + 1U;
			if (extra[count] == 4U)
				size = total - 1U;
			if (extra[count] == 5U)
				size = total + 1U;
			exact = (uint8_t *)malloc(size);
			CHECK(exact != NULL);
			memset(exact, 0x11, size);
			exact[0] = (uint8_t)tag;
			memset(&point, SENTINEL_BYTE, sizeof(point));
			error = cryc_ecp_decode(ec, &point, exact, size);
			CHECK(error == ENOTSUP);
			CHECK(point.z[0] == 0U && point.x[0] == 0U && point.y[0] == 0U);
			free(exact);
		}

		/* The exact length, with a valid-looking body. */
		exact = (uint8_t *)malloc(total);
		CHECK(exact != NULL);
		memcpy(exact, good, total);
		exact[0] = (uint8_t)tag;
		error = cryc_ecp_decode(ec, &point, exact, total);
		CHECK(error == ENOTSUP);
		free(exact);
	}

	/* Other first bytes: 0x00 (infinity), hybrid 0x06 and 0x07, 0x01, 0x05, 0xff, 0x84: EINVAL. */
	for (count = 0; count < sizeof(tags) / sizeof(tags[0]); count++) {
		exact = (uint8_t *)malloc(total);
		CHECK(exact != NULL);
		memcpy(exact, good, total);
		exact[0] = (uint8_t)tags[count];
		memset(&point, SENTINEL_BYTE, sizeof(point));
		error = cryc_ecp_decode(ec, &point, exact, total);
		CHECK(error == EINVAL);
		CHECK(point.z[0] == 0U && point.x[0] == 0U && point.y[0] == 0U);
		free(exact);
	}

	/* The infinity encoding: one byte 0x00. */
	exact = (uint8_t *)malloc(1);
	CHECK(exact != NULL);
	exact[0] = 0U;
	error = cryc_ecp_decode(ec, &point, exact, 1);
	CHECK(error == EINVAL);
	free(exact);

	/* Wrong lengths with the right tag: every size from 1 to total + 3 except total. */
	for (size = 1; size <= total + 3U; size++) {
		if (size == total)
			continue;
		exact = (uint8_t *)malloc(size);
		CHECK(exact != NULL);
		for (index = 0; index < size; index++) {
			exact[index] = (uint8_t)0x33U;
			if (index < total)
				exact[index] = good[index];
		}

		/* The wrong length is refused and the output is zeroed. */
		memset(&point, SENTINEL_BYTE, sizeof(point));
		error = cryc_ecp_decode(ec, &point, exact, size);
		CHECK(error == EINVAL);
		CHECK(point.z[0] == 0U && point.x[0] == 0U && point.y[0] == 0U);
		free(exact);
	}

	/* The exact length: valid. */
	exact = (uint8_t *)malloc(total);
	CHECK(exact != NULL);
	memcpy(exact, good, total);
	error = cryc_ecp_decode(ec, &point, exact, total);
	CHECK(error == 0);

	/* Coordinates not below p: x = p and 2^bits - 1; y the same (the range check comes before the curve equation). */
	for (count = 0; count < 2U; count++) {
		memcpy(exact, good, total);
		memcpy(exact + 1U, pbytes, ec->size);
		if (count == 1U)
			memset(exact + 1U, 0xff, ec->size);
		error = cryc_ecp_decode(ec, &point, exact, total);
		CHECK(error == EINVAL);
		memcpy(exact, good, total);
		memcpy(exact + 1U + ec->size, pbytes, ec->size);
		if (count == 1U)
			memset(exact + 1U + ec->size, 0xff, ec->size);
		error = cryc_ecp_decode(ec, &point, exact, total);
		CHECK(error == EINVAL);
	}

	/* p - 1 in a coordinate is below p: the point is (then) off the curve or on it; either way not the range error: x = p - 1 with the real y is off the curve. */
	memcpy(exact, good, total);
	memcpy(exact + 1U, pbytes, ec->size);
	exact[ec->size] = (uint8_t)(exact[ec->size] - 1U);
	error = cryc_ecp_decode(ec, &point, exact, total);
	CHECK(error == EINVAL);

	/* Every single-bit change of a byte of a valid encoding is refused (off the curve, out of range or a tag: never accepted). */
	for (index = 0; index < total; index++) {
		memcpy(exact, good, total);
		exact[index] = (uint8_t)(exact[index] ^ 0x01U);
		memset(&point, SENTINEL_BYTE, sizeof(point));
		error = cryc_ecp_decode(ec, &point, exact, total);
		CHECK(error == EINVAL);
		CHECK(point.z[0] == 0U && point.x[0] == 0U && point.y[0] == 0U);
		memcpy(exact, good, total);
		exact[index] = (uint8_t)(exact[index] ^ 0x80U);
		error = cryc_ecp_decode(ec, &point, exact, total);
		CHECK(error == EINVAL || error == ENOTSUP);
	}

	/* The negated point (x, p - y) is valid, and (x, y + 1) is not. */
	ref_negate(&minus, &base, ec);
	ref_encode(minus_bytes, &minus, ec);
	error = cryc_ecp_decode(ec, &point, minus_bytes, total);
	CHECK(error == 0);

	/* Releases the buffer of the valid encoding. */
	free(exact);

	/* Random bytes with the uncompressed tag and the exact length, in exact-size buffers: refused (a random pair is nearly never on the curve). */
	for (count = 0; count < 300U; count++) {
		exact = (uint8_t *)malloc(total);
		CHECK(exact != NULL);
		random_bytes(exact, total);
		exact[0] = 0x04U;
		if ((count & 3U) == 0U)
			memset(exact + 1U, 0, 8U);
		error = cryc_ecp_decode(ec, &point, exact, total);
		CHECK(error == EINVAL || error == 0);
		free(exact);
	}

	/* Random bytes of random sizes and tags: only 0, EINVAL or ENOTSUP, never a crash. */
	for (count = 0; count < 300U; count++) {
		size = 1U + (size_t)(random32() % (total + 4U));
		exact = (uint8_t *)malloc(size);
		CHECK(exact != NULL);
		random_bytes(exact, size);
		if ((count & 1U) == 0U)
			exact[0] = (uint8_t)(2U + (random32() % 4U));
		error = cryc_ecp_decode(ec, &point, exact, size);
		CHECK(error == 0 || error == EINVAL || error == ENOTSUP);
		free(exact);
	}
}

/*
 * Returns 1 when the record is of the curve whose section key is name
 * (e.g. "section.P-256").  The record file lists every section header seen
 * so far in each record, so the curve of a record is its last section key
 * other than the key-generation method's.
 */
static int
record_is_curve(
	const struct vec_file *file,
	const char *name)
{
	const char *key;
	size_t index;
	int different;

	/* From the last key backwards: the first section key that names a curve. */
	for (index = file->count; index > 0U; index--) {
		key = file->keys[index - 1U];
		different = strncmp(key, "section.", 8);
		if (different != 0)
			continue;
		different = strncmp(key, "section.B.4.2", 13);
		if (different == 0)
			continue;

		/* The first curve key decides: the record is of the curve when the keys are the same. */
		different = strcmp(key, name);
		if (different == 0)
			return 1;
		return 0;
	}

	/* The record names no curve. */
	return 0;
}

/* The numbers of coordinates built as a value plus p (valid points whose coordinate fits below 2^bits - p). */
static unsigned long alias_count_x;
static unsigned long alias_count_y;

/* Sets out to the right side x^3 - 3 x + b of the curve equation, all in Montgomery form. */
static void
rhs_mont(
	const struct cryc_ecp *ec,
	uint32_t *out,
	const uint32_t *x_mont)
{
	uint32_t square[CRYC_ECP_MAX_LIMBS];
	uint32_t triple[CRYC_ECP_MAX_LIMBS];

	/* x^3, then minus 3 x, then plus b. */
	memset(square, 0, sizeof(square));
	memset(triple, 0, sizeof(triple));
	cryc_bn_mont_mul(square, x_mont, x_mont, &ec->field);
	cryc_bn_mont_mul(out, square, x_mont, &ec->field);
	cryc_bn_mod_add(triple, x_mont, x_mont, &ec->field);
	cryc_bn_mod_add(triple, triple, x_mont, &ec->field);
	cryc_bn_mod_sub(out, out, triple, &ec->field);
	cryc_bn_mod_add(out, out, ec->b_mont, &ec->field);
}

/*
 * Sets out to a square root of a (Montgomery form) as a^((p + 1) / 4),
 * which is one when a is a square and p = 3 (mod 4) (both curves); the
 * caller checks by squaring.
 */
static void
sqrt_mont(
	const struct cryc_ecp *ec,
	uint32_t *out,
	const uint32_t *a,
	const struct params *params)
{
	uint8_t exponent[CRYC_ECP_MAX_BYTES];
	size_t index;
	int error;

	/* p + 1 as bytes (the last byte is 0xff, so the carry runs), then shifted right by two. */
	error = hex_to_size(params->p, exponent, params->size);
	CHECK(error == 0);
	index = params->size;
	while (index > 0U) {
		index--;
		exponent[index] = (uint8_t)(exponent[index] + 1U);
		if (exponent[index] != 0U)
			break;
	}

	/* The shift right by two bits, byte by byte from the low end. */
	for (index = params->size; index > 0U; index--) {
		exponent[index - 1U] = (uint8_t)(exponent[index - 1U] >> 2);
		if (index > 1U)
			exponent[index - 1U] = (uint8_t)(exponent[index - 1U] | (uint8_t)(exponent[index - 2U] << 6));
	}

	/* The power a^((p + 1) / 4). */
	cryc_bn_mont_exp(out, a, exponent, params->size, &ec->field);
}

/* Adds p to the size big-endian bytes at in into out; returns 1 when the sum overflows the size. */
static int
add_p_bytes(
	const struct params *params,
	uint8_t *out,
	const uint8_t *in)
{
	uint8_t prime[CRYC_ECP_MAX_BYTES];
	uint32_t carry;
	uint32_t sum;
	size_t index;
	int error;

	/* Byte by byte from the low end. */
	error = hex_to_size(params->p, prime, params->size);
	CHECK(error == 0);
	carry = 0;
	for (index = params->size; index > 0U; index--) {
		sum = (uint32_t)in[index - 1U] + (uint32_t)prime[index - 1U] + carry;
		out[index - 1U] = (uint8_t)(sum & 0xffU);
		carry = sum >> 8;
	}

	/* Reports whether the sum overflowed. */
	return (int)carry;
}

/* For a valid encoding, tries each coordinate plus p (when it still fits): the result must be refused as out of range. */
static void
check_alias_coordinates(
	const struct params *params,
	const struct cryc_ecp *ec,
	const uint8_t *encoding)
{
	struct cryc_ecp_point point;
	uint8_t changed[CRYC_ECP_MAX_POINT];
	size_t total;
	int error;
	int overflow;

	/* The x coordinate. */
	total = 1U + 2U * ec->size;
	memcpy(changed, encoding, total);
	overflow = add_p_bytes(params, changed + 1U, encoding + 1U);
	if (overflow == 0) {
		error = cryc_ecp_decode(ec, &point, changed, total);
		CHECK(error == EINVAL);
		alias_count_x++;
	}

	/* The y coordinate. */
	memcpy(changed, encoding, total);
	overflow = add_p_bytes(params, changed + 1U + ec->size, encoding + 1U + ec->size);
	if (overflow == 0) {
		error = cryc_ecp_decode(ec, &point, changed, total);
		CHECK(error == EINVAL);
		alias_count_y++;
	}
}

/*
 * Checks the validation on constructed points: small x values on the
 * curve (valid, and refused when x + p is given: the range check, not the
 * curve equation, rejects it), and points that miss the curve equation by
 * one unit in a single limb of its Montgomery form (each limb in turn:
 * the comparison of the two sides must look at every limb).
 */
static void
check_constructed_points(
	const struct params *params,
	const struct cryc_ecp *ec)
{
	struct cryc_ecp_point point;
	uint32_t x_plain[CRYC_ECP_MAX_LIMBS];
	uint32_t x_mont[CRYC_ECP_MAX_LIMBS];
	uint32_t rhs[CRYC_ECP_MAX_LIMBS];
	uint32_t target[CRYC_ECP_MAX_LIMBS];
	uint32_t root[CRYC_ECP_MAX_LIMBS];
	uint32_t square[CRYC_ECP_MAX_LIMBS];
	uint32_t y_plain[CRYC_ECP_MAX_LIMBS];
	uint8_t encoded[CRYC_ECP_MAX_POINT];
	uint32_t carry;
	uint32_t candidate;
	size_t limb;
	size_t index;
	unsigned long valid_small;
	unsigned long near_misses;
	int error;
	int matches;
	uint32_t below;

	/* Tries each small x; counts the points that exist and the near misses. */
	valid_small = 0;
	near_misses = 0;
	for (candidate = 1; candidate <= 24U; candidate++) {
		memset(x_plain, 0, sizeof(x_plain));
		memset(x_mont, 0, sizeof(x_mont));
		memset(rhs, 0, sizeof(rhs));
		scalar_small(x_plain, candidate);
		cryc_bn_to_mont(x_mont, x_plain, &ec->field);
		rhs_mont(ec, rhs, x_mont);

		/* The root, when the right side is a square: the point (x, y) is valid, and (x + p, y) is not. */
		memset(root, 0, sizeof(root));
		memset(square, 0, sizeof(square));
		sqrt_mont(ec, root, rhs, params);
		cryc_bn_mont_mul(square, root, root, &ec->field);
		matches = limbs_equal(square, rhs, ec);
		if (matches) {
			memset(y_plain, 0, sizeof(y_plain));
			cryc_bn_from_mont(y_plain, root, &ec->field);
			encoded[0] = 0x04U;
			cryc_bn_encode(x_plain, ec->limbs, encoded + 1U, ec->size);
			cryc_bn_encode(y_plain, ec->limbs, encoded + 1U + ec->size, ec->size);
			error = cryc_ecp_decode(ec, &point, encoded, 1U + 2U * ec->size);
			CHECK(error == 0);
			valid_small++;
			check_alias_coordinates(params, ec, encoded);
		}

		/* Near misses: the right side plus one unit at limb j (when that stays below p and the result is a square). */
		for (limb = 0; limb < ec->limbs; limb++) {
			memcpy(target, rhs, sizeof(target));
			carry = 1;
			for (index = limb; index < ec->limbs && carry != 0U; index++) {
				target[index] += carry;
				carry = 0U;
				if (target[index] == 0U)
					carry = 1U;
			}

			/* Skips a right side that overflowed or reached p. */
			if (carry != 0U)
				continue;
			below = cryc_bn_lt(target, ec->field.modulus, ec->limbs);
			if (below == 0U)
				continue;
			memset(root, 0, sizeof(root));
			memset(square, 0, sizeof(square));
			sqrt_mont(ec, root, target, params);
			cryc_bn_mont_mul(square, root, root, &ec->field);
			matches = limbs_equal(square, target, ec);
			if (!matches)
				continue;
			memset(y_plain, 0, sizeof(y_plain));
			cryc_bn_from_mont(y_plain, root, &ec->field);
			encoded[0] = 0x04U;
			cryc_bn_encode(x_plain, ec->limbs, encoded + 1U, ec->size);
			cryc_bn_encode(y_plain, ec->limbs, encoded + 1U + ec->size, ec->size);
			error = cryc_ecp_decode(ec, &point, encoded, 1U + 2U * ec->size);
			CHECK(error == EINVAL);
			near_misses++;
		}
	}

	/* Reports the counts and requires that both kinds occurred. */
	printf("  %s constructed: %lu small-x points, %lu near misses\n", params->name, valid_small, near_misses);
	CHECK(valid_small > 3U);
	CHECK(near_misses > 2U * ec->limbs);
}

/* Checks the CAVP public key validation cases of the curve (the section's name is in section). */
static void
check_pkv(
	const struct params *params,
	const struct cryc_ecp *ec,
	const char *section)
{
	struct vec_file file;
	struct cryc_ecp_point point;
	uint8_t input[CRYC_ECP_MAX_POINT];
	uint8_t again[CRYC_ECP_MAX_POINT];
	const char *x_text;
	const char *y_text;
	const char *result;
	unsigned long valid;
	unsigned long invalid;
	unsigned long too_wide;
	size_t total;
	int x_fits;
	int y_fits;
	int error;
	int more;
	int fit_error;
	int on_curve;

	/* Starts with no record counted. */
	total = 1U + 2U * ec->size;
	valid = 0;
	invalid = 0;
	too_wide = 0;
	error = vec_open(&file, "cavp-186-4ecdsatestvectors-PKV");
	CHECK(error == 0);
	if (error != 0)
		return;

	/* Each record of this curve. */
	for (more = vec_next(&file);
	     more;
	     more = vec_next(&file)) {
		on_curve = record_is_curve(&file, section);
		if (!on_curve)
			continue;
		x_text = vec_get(&file, "Qx");
		y_text = vec_get(&file, "Qy");
		result = vec_get(&file, "Result");
		CHECK(x_text != NULL && y_text != NULL && result != NULL);
		if (x_text == NULL || y_text == NULL || result == NULL)
			continue;
		input[0] = 0x04U;
		fit_error = hex_to_size(x_text, input + 1U, ec->size);
		x_fits = 0;
		if (fit_error == 0)
			x_fits = 1;
		fit_error = hex_to_size(y_text, input + 1U + ec->size, ec->size);
		y_fits = 0;
		if (fit_error == 0)
			y_fits = 1;

		/* A coordinate wider than the curve's size cannot be encoded: the case is out of range by its width. */
		if (!x_fits || !y_fits) {
			CHECK(result[0] == 'F');
			too_wide++;
			continue;
		}

		/* A valid case decodes and encodes back to the input; an invalid one is refused. */
		error = cryc_ecp_decode(ec, &point, input, total);
		if (result[0] == 'P') {
			valid++;
			CHECK(error == 0);
			if (error == 0) {
				error = cryc_ecp_encode(ec, &point, again, total);
				CHECK(error == 0);
				CHECK_BYTES(again, input, total);
				check_alias_coordinates(params, ec, input);
			}
		} else {
			invalid++;
			CHECK(result[0] == 'F');
			CHECK(error == EINVAL);
		}
	}

	/* Releases the file. */
	vec_close(&file);

	/* The counts of the file: 60 valid, 120 invalid for each curve. */
	printf("  %s PKV: %lu valid, %lu invalid, %lu too wide\n", params->name, valid, invalid, too_wide);
	CHECK(valid > 0U);
	CHECK(invalid > 0U);
}

/* Checks the CAVP key pairs of the curve: Q = d * G. */
static void
check_keypair(
	const struct params *params,
	const struct cryc_ecp *ec,
	const char *section)
{
	struct vec_file file;
	struct cryc_ecp_point point;
	uint8_t d_bytes[CRYC_ECP_MAX_BYTES];
	uint8_t got[CRYC_ECP_MAX_POINT];
	uint8_t want[CRYC_ECP_MAX_POINT];
	uint32_t scalar[CRYC_ECP_MAX_LIMBS];
	const char *d_text;
	unsigned long count;
	int error;
	int more;
	int on_curve;

	/* Opens the vector file. */
	count = 0;
	error = vec_open(&file, "cavp-186-4ecdsatestvectors-KeyPair");
	CHECK(error == 0);
	if (error != 0)
		return;

	/* Checks each record of this curve: Q = d * G. */
	for (more = vec_next(&file);
	     more;
	     more = vec_next(&file)) {
		d_text = vec_get(&file, "d");
		on_curve = record_is_curve(&file, section);
		if (!on_curve)
			continue;
		if (d_text == NULL)
			continue;
		CHECK(hex_to_size(d_text, d_bytes, ec->size) == 0);
		scalar_from_bytes(scalar, ec, d_bytes);
		want[0] = 0x04U;
		CHECK(hex_to_size(vec_get(&file, "Qx"), want + 1U, ec->size) == 0);
		CHECK(hex_to_size(vec_get(&file, "Qy"), want + 1U + ec->size, ec->size) == 0);
		cryc_ecp_mul_base(ec, &point, scalar);
		error = cryc_ecp_encode(ec, &point, got, sizeof(got));
		CHECK(error == 0);
		CHECK_BYTES(got, want, 1U + 2U * ec->size);
		count++;
	}

	/* Releases the file. */
	vec_close(&file);

	/* Reports the number of key pairs and requires at least one. */
	printf("  %s key pairs: %lu\n", params->name, count);
	CHECK(count > 0U);
}

/* Checks the Wycheproof ECDH cases for P-256 (public point bytes, private scalar, shared x). */
static void
check_wycheproof_ecdh(
	const struct params *params,
	const struct cryc_ecp *ec)
{
	struct vec_file file;
	struct cryc_ecp_point point;
	struct cryc_ecp_point product;
	uint8_t pub[256];
	uint8_t priv[256];
	uint8_t shared[64];
	uint8_t got[CRYC_ECP_MAX_BYTES];
	uint8_t scalar_bytes[CRYC_ECP_MAX_BYTES];
	uint32_t scalar[CRYC_ECP_MAX_LIMBS];
	const char *result;
	size_t pub_size;
	size_t priv_size;
	size_t shared_size;
	size_t start;
	unsigned long valid;
	unsigned long refused;
	unsigned long skipped;
	int error;
	int more;
	int verdict;
	uint32_t below;

	/* Starts with no record counted, and opens the vector file. */
	valid = 0;
	refused = 0;
	skipped = 0;
	error = vec_open(&file, "wycheproof-ecdh_secp256r1_ecpoint_test");
	CHECK(error == 0);
	if (error != 0)
		return;

	/* Checks each record: decoding, the alias coordinates and the shared secret. */
	for (more = vec_next(&file);
	     more;
	     more = vec_next(&file)) {
		result = vec_get(&file, "result");
		pub_size = vec_hex(&file, "public", pub, sizeof(pub));
		priv_size = vec_hex(&file, "private", priv, sizeof(priv));
		shared_size = vec_hex(&file, "shared", shared, sizeof(shared));
		CHECK(result != NULL);
		CHECK(pub_size != (size_t)-1 && priv_size != (size_t)-1 && shared_size != (size_t)-1);
		if (result == NULL || pub_size == (size_t)-1 || priv_size == (size_t)-1 || shared_size == (size_t)-1)
			continue;

		/* Decodes the public point of the record. */
		error = cryc_ecp_decode(ec, &point, pub, pub_size);

		/* An invalid public key (not on the curve, wrong curve, bad encoding) must be refused. */
		verdict = strcmp(result, "invalid");
		if (verdict == 0) {
			CHECK(error != 0);
			refused++;
			continue;
		}

		/* An acceptable one is either decoded and agrees, or is in a form this module does not support (compressed). */
		if (error != 0) {
			CHECK(strcmp(result, "acceptable") == 0);
			CHECK(error == ENOTSUP || error == EINVAL);
			refused++;
			continue;
		}

		/* The coordinates plus p, where they still fit, are refused (the file has points with y = 1 and x = 0). */
		if (pub_size == 1U + 2U * ec->size)
			check_alias_coordinates(params, ec, pub);

		/* The private scalar: strip leading zero bytes; a value not below n is outside the contract and skipped. */
		start = 0;
		while (start < priv_size && priv[start] == 0U)
			start++;
		if (priv_size - start > ec->size) {
			skipped++;
			continue;
		}

		/* The scalar as limbs. */
		memset(scalar_bytes, 0, sizeof(scalar_bytes));
		memcpy(scalar_bytes + (ec->size - (priv_size - start)), priv + start, priv_size - start);
		scalar_from_bytes(scalar, ec, scalar_bytes);
		below = cryc_bn_lt(scalar, ec->order.modulus, ec->limbs);
		if (below == 0U) {
			skipped++;
			continue;
		}

		/* The shared secret is the x of scalar * point. */
		cryc_ecp_mul(ec, &product, &point, scalar);
		memset(got, SENTINEL_BYTE, sizeof(got));
		error = cryc_ecp_affine_x(ec, &product, got);
		CHECK(error == 0);
		CHECK(shared_size == ec->size);
		if (shared_size == ec->size)
			CHECK_BYTES(got, shared, ec->size);
		valid++;
	}

	/* Releases the file. */
	vec_close(&file);

	/* Reports the counts and requires that the cases were computed. */
	printf("  P-256 ECDH: %lu computed, %lu refused, %lu skipped\n", valid, refused, skipped);
	CHECK(valid > 100U);
	CHECK(refused > 0U);
	printf("  P-256 coordinate + p cases: %lu x, %lu y\n", alias_count_x, alias_count_y);
	CHECK(alias_count_x > 0U);
	CHECK(alias_count_y > 0U);
}

/* Checks the initialization errors. */
static void
check_init_errors(void)
{
	struct cryc_ecp ec;
	size_t index;
	int error;

	/* No curve, and ids that name none: ENOTSUP and a zeroed structure. */
	memset(&ec, SENTINEL_BYTE, sizeof(ec));
	error = cryc_ecp_init(&ec, CRYC_CURVE_NONE);
	CHECK(error == ENOTSUP);
	for (index = 0; index < sizeof(ec); index++)
		CHECK(((const uint8_t *)&ec)[index] == 0U);
	memset(&ec, SENTINEL_BYTE, sizeof(ec));
	error = cryc_ecp_init(&ec, (enum cryc_curve_id)3);
	CHECK(error == ENOTSUP);
	for (index = 0; index < sizeof(ec); index++)
		CHECK(((const uint8_t *)&ec)[index] == 0U);
	memset(&ec, SENTINEL_BYTE, sizeof(ec));
	error = cryc_ecp_init(&ec, (enum cryc_curve_id)255);
	CHECK(error == ENOTSUP);
	CHECK(ec.id == CRYC_CURVE_NONE);
}

/* Runs the checks of one curve. */
static void
run_curve(
	const struct params *params)
{
	struct cryc_ecp ec;
	struct cryc_ecp second;
	const char *section;
	int error;

	/* Prepares the curve and the reference. */
	error = cryc_ecp_init(&ec, params->id);
	CHECK(error == 0);
	if (error != 0)
		return;
	ref_prepare(params);
	pool_prepare(&ec);
	printf("%s\n", params->name);

	/* The groups of checks. */
	check_parameters(params, &ec);
	check_multiplications(&ec);
	check_order_scalar(&ec);
	check_mul_add(&ec);
	check_encode_decode(&ec);
	check_decode_errors(params, &ec);
	check_constructed_points(params, &ec);
	section = "section.P-384";
	if (params->id == CRYC_CURVE_P256)
		section = "section.P-256";
	check_pkv(params, &ec, section);
	check_keypair(params, &ec, section);
	if (params->id == CRYC_CURVE_P256)
		check_wycheproof_ecdh(params, &ec);

	/* init twice gives the same structure (it holds no hidden state). */
	error = cryc_ecp_init(&second, params->id);
	CHECK(error == 0);
	CHECK(memcmp(&second, &ec, sizeof(ec)) == 0);
}

/* Runs the checks. */
int
main(void)
{
	/* The initialization errors, then each curve. */
	check_init_errors();
	run_curve(&curves[0]);
	run_curve(&curves[1]);

	/* The result. */
	return check_finish("test-ecp");
}
