/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The P-256 and P-384 groups (NIST SP 800-186, SEC 1).
 *
 * A point is (X, Y, Z) in Jacobian coordinates, standing for the affine
 * point (X / Z^2, Y / Z^3).  The addition and doubling below are derived
 * from the affine group law of SP 800-186 Appendix A.1.1 by putting
 * x = X / Z^2 and y = Y / Z^3 and multiplying out the denominators.
 * Every field operation is in Montgomery form modulo p, and takes the same
 * time whatever the values.
 */

#include "cryc-ecp.h"

#include <string.h>

/* The number of points of the window table of a multiplication: every 4-bit digit. */
#define ECP_TABLE_SIZE	16U

/* The bits of a window of a multiplication. */
#define ECP_WINDOW_BITS	4U

/* The first byte of an uncompressed point (SEC 1 2.3.3). */
#define ECP_TAG_UNCOMPRESSED	0x04U

/* The first bytes of a compressed point, which are refused (SEC 1 2.3.4). */
#define ECP_TAG_COMPRESSED_EVEN	0x02U
#define ECP_TAG_COMPRESSED_ODD	0x03U

/* The parameters of one curve: the byte length of a field element and the five values. */
struct ecp_params {
	enum cryc_curve_id id;
	size_t size;
	const uint8_t *prime;
	const uint8_t *order;
	const uint8_t *coefficient;
	const uint8_t *base_x;
	const uint8_t *base_y;
};

/*
 * The parameters of P-256: the prime p, the order n of the base point, the
 * coefficient b (a is -3) and the base point G, each as 32 big-endian
 * bytes.
 *
 * Values referenced: NIST SP 800-186 (February 2023) 3.2.1.3, the
 * hexadecimal forms of p, n, b, Gx and Gy.
 */
static const uint8_t ecp_p256_p[32] = {
	0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x01,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff
};

/* P-256: the order n of the base point. */
static const uint8_t ecp_p256_n[32] = {
	0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00,
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	0xbc, 0xe6, 0xfa, 0xad, 0xa7, 0x17, 0x9e, 0x84,
	0xf3, 0xb9, 0xca, 0xc2, 0xfc, 0x63, 0x25, 0x51
};

/* P-256: the coefficient b. */
static const uint8_t ecp_p256_b[32] = {
	0x5a, 0xc6, 0x35, 0xd8, 0xaa, 0x3a, 0x93, 0xe7,
	0xb3, 0xeb, 0xbd, 0x55, 0x76, 0x98, 0x86, 0xbc,
	0x65, 0x1d, 0x06, 0xb0, 0xcc, 0x53, 0xb0, 0xf6,
	0x3b, 0xce, 0x3c, 0x3e, 0x27, 0xd2, 0x60, 0x4b
};

/* P-256: the x coordinate of the base point. */
static const uint8_t ecp_p256_gx[32] = {
	0x6b, 0x17, 0xd1, 0xf2, 0xe1, 0x2c, 0x42, 0x47,
	0xf8, 0xbc, 0xe6, 0xe5, 0x63, 0xa4, 0x40, 0xf2,
	0x77, 0x03, 0x7d, 0x81, 0x2d, 0xeb, 0x33, 0xa0,
	0xf4, 0xa1, 0x39, 0x45, 0xd8, 0x98, 0xc2, 0x96
};

/* P-256: the y coordinate of the base point. */
static const uint8_t ecp_p256_gy[32] = {
	0x4f, 0xe3, 0x42, 0xe2, 0xfe, 0x1a, 0x7f, 0x9b,
	0x8e, 0xe7, 0xeb, 0x4a, 0x7c, 0x0f, 0x9e, 0x16,
	0x2b, 0xce, 0x33, 0x57, 0x6b, 0x31, 0x5e, 0xce,
	0xcb, 0xb6, 0x40, 0x68, 0x37, 0xbf, 0x51, 0xf5
};

/*
 * The parameters of P-384: the prime p, the order n of the base point, the
 * coefficient b (a is -3) and the base point G, each as 48 big-endian
 * bytes.
 *
 * Values referenced: NIST SP 800-186 (February 2023) 3.2.1.4, the
 * hexadecimal forms of p, n, b, Gx and Gy.
 */
static const uint8_t ecp_p384_p[48] = {
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xfe,
	0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff
};

/* P-384: the order n of the base point. */
static const uint8_t ecp_p384_n[48] = {
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	0xc7, 0x63, 0x4d, 0x81, 0xf4, 0x37, 0x2d, 0xdf,
	0x58, 0x1a, 0x0d, 0xb2, 0x48, 0xb0, 0xa7, 0x7a,
	0xec, 0xec, 0x19, 0x6a, 0xcc, 0xc5, 0x29, 0x73
};

/* P-384: the coefficient b. */
static const uint8_t ecp_p384_b[48] = {
	0xb3, 0x31, 0x2f, 0xa7, 0xe2, 0x3e, 0xe7, 0xe4,
	0x98, 0x8e, 0x05, 0x6b, 0xe3, 0xf8, 0x2d, 0x19,
	0x18, 0x1d, 0x9c, 0x6e, 0xfe, 0x81, 0x41, 0x12,
	0x03, 0x14, 0x08, 0x8f, 0x50, 0x13, 0x87, 0x5a,
	0xc6, 0x56, 0x39, 0x8d, 0x8a, 0x2e, 0xd1, 0x9d,
	0x2a, 0x85, 0xc8, 0xed, 0xd3, 0xec, 0x2a, 0xef
};

/* P-384: the x coordinate of the base point. */
static const uint8_t ecp_p384_gx[48] = {
	0xaa, 0x87, 0xca, 0x22, 0xbe, 0x8b, 0x05, 0x37,
	0x8e, 0xb1, 0xc7, 0x1e, 0xf3, 0x20, 0xad, 0x74,
	0x6e, 0x1d, 0x3b, 0x62, 0x8b, 0xa7, 0x9b, 0x98,
	0x59, 0xf7, 0x41, 0xe0, 0x82, 0x54, 0x2a, 0x38,
	0x55, 0x02, 0xf2, 0x5d, 0xbf, 0x55, 0x29, 0x6c,
	0x3a, 0x54, 0x5e, 0x38, 0x72, 0x76, 0x0a, 0xb7
};

/* P-384: the y coordinate of the base point. */
static const uint8_t ecp_p384_gy[48] = {
	0x36, 0x17, 0xde, 0x4a, 0x96, 0x26, 0x2c, 0x6f,
	0x5d, 0x9e, 0x98, 0xbf, 0x92, 0x92, 0xdc, 0x29,
	0xf8, 0xf4, 0x1d, 0xbd, 0x28, 0x9a, 0x14, 0x7c,
	0xe9, 0xda, 0x31, 0x13, 0xb5, 0xf0, 0xb8, 0xc0,
	0x0a, 0x60, 0xb1, 0xce, 0x1d, 0x7e, 0x81, 0x9d,
	0x7a, 0x43, 0x1d, 0x7c, 0x90, 0xea, 0x0e, 0x5f
};

/* The parameters of the curves, found by id. */
static const struct ecp_params ecp_curves[2] = {
	{
		CRYC_CURVE_P256, 32U, ecp_p256_p, ecp_p256_n, ecp_p256_b,
		ecp_p256_gx, ecp_p256_gy
	},
	{
		CRYC_CURVE_P384, 48U, ecp_p384_p, ecp_p384_n, ecp_p384_b,
		ecp_p384_gx, ecp_p384_gy
	}
};

/* Forward declarations. */
static void ecp_infinity(struct cryc_ecp_point *point);
static void ecp_base_point(const struct cryc_ecp *ec, struct cryc_ecp_point *point);
static void ecp_select(struct cryc_ecp_point *out, const struct cryc_ecp_point *when_set, const struct cryc_ecp_point *when_clear, uint32_t mask);
static void ecp_double(const struct cryc_ecp *ec, struct cryc_ecp_point *out, const struct cryc_ecp_point *in);
static void ecp_add(const struct cryc_ecp *ec, struct cryc_ecp_point *out, const struct cryc_ecp_point *left, const struct cryc_ecp_point *right);
static void ecp_invert(const struct cryc_ecp *ec, uint32_t *out, const uint32_t *value);
static uint32_t ecp_to_affine(const struct cryc_ecp *ec, uint32_t *x, uint32_t *y, const struct cryc_ecp_point *point);
static uint32_t ecp_bit(const uint32_t *scalar, size_t index);

/*
 * Prepares the curve id.
 */
int
cryc_ecp_init(
	struct cryc_ecp *ec,
	enum cryc_curve_id id)
{
	const struct ecp_params *params;
	uint32_t value[CRYC_ECP_MAX_LIMBS];
	size_t index;
	int error;

	/* Starts from a zeroed result, which is what an error leaves. */
	cryc_wipe(ec, sizeof(*ec));

	/* Finds the curve's parameters, refusing an id that names none. */
	params = NULL;
	for (index = 0; index < 2U; index++) {
		if (ecp_curves[index].id == id)
			params = &ecp_curves[index];
	}
	if (params == NULL)
		return ENOTSUP;

	/* Records the sizes, which are the same for the prime and the order. */
	ec->id = id;
	ec->size = params->size;
	ec->limbs = params->size / 4U;

	/* Prepares the prime and the order for Montgomery arithmetic. */
	error = cryc_bn_mont_init(&ec->field, params->prime, params->size);
	if (error != 0) {
		cryc_wipe(ec, sizeof(*ec));
		return error;
	}
	error = cryc_bn_mont_init(&ec->order, params->order, params->size);
	if (error != 0) {
		cryc_wipe(ec, sizeof(*ec));
		return error;
	}

	/* Reads b and puts it into Montgomery form; each value is below p. */
	memset(value, 0, sizeof(value));
	error = cryc_bn_decode(value, ec->limbs, params->coefficient, params->size);
	if (error != 0) {
		cryc_wipe(ec, sizeof(*ec));
		return error;
	}
	cryc_bn_to_mont(ec->b_mont, value, &ec->field);

	/* Reads the base point and puts both coordinates into Montgomery form. */
	error = cryc_bn_decode(value, ec->limbs, params->base_x, params->size);
	if (error != 0) {
		cryc_wipe(ec, sizeof(*ec));
		return error;
	}
	cryc_bn_to_mont(ec->base_x, value, &ec->field);
	error = cryc_bn_decode(value, ec->limbs, params->base_y, params->size);
	if (error != 0) {
		cryc_wipe(ec, sizeof(*ec));
		return error;
	}
	cryc_bn_to_mont(ec->base_y, value, &ec->field);

	/* Succeeded: the curve is ready. */
	return 0;
}

/*
 * Decodes a point from its uncompressed encoding (SEC 1 2.3.4 with 0x04 ||
 * x || y) and validates it (SEC 1 3.2.2.1; NIST SP 800-56A Rev.
 */
int
cryc_ecp_decode(
	const struct cryc_ecp *ec,
	struct cryc_ecp_point *point,
	const uint8_t *in,
	size_t size)
{
	uint32_t x[CRYC_ECP_MAX_LIMBS];
	uint32_t y[CRYC_ECP_MAX_LIMBS];
	uint32_t x_mont[CRYC_ECP_MAX_LIMBS];
	uint32_t y_mont[CRYC_ECP_MAX_LIMBS];
	uint32_t left[CRYC_ECP_MAX_LIMBS];
	uint32_t right[CRYC_ECP_MAX_LIMBS];
	uint32_t mask;
	size_t index;
	int error;

	/* Starts from a zeroed point, which is what an error leaves. */
	cryc_wipe(point, sizeof(*point));
	memset(x, 0, sizeof(x));
	memset(y, 0, sizeof(y));
	memset(x_mont, 0, sizeof(x_mont));
	memset(y_mont, 0, sizeof(y_mont));
	memset(left, 0, sizeof(left));
	memset(right, 0, sizeof(right));

	/* Refuses an empty encoding. */
	if (size == 0U)
		return EINVAL;

	/* Refuses the compressed forms, which are valid but unsupported. */
	if (in[0] == ECP_TAG_COMPRESSED_EVEN || in[0] == ECP_TAG_COMPRESSED_ODD)
		return ENOTSUP;

	/* Refuses any other first byte, the point at infinity (0x00) included. */
	if (in[0] != ECP_TAG_UNCOMPRESSED)
		return EINVAL;

	/* Refuses an encoding of the wrong length. */
	if (size != 1U + 2U * ec->size)
		return EINVAL;

	/* Reads the coordinates, which fit since the length is exact. */
	error = cryc_bn_decode(x, ec->limbs, in + 1U, ec->size);
	if (error != 0)
		return EINVAL;
	error = cryc_bn_decode(y, ec->limbs, in + 1U + ec->size, ec->size);
	if (error != 0)
		return EINVAL;

	/* Refuses a coordinate that is not below p (SEC 1 3.2.2.1 step 2). */
	mask = cryc_bn_lt(x, ec->field.modulus, ec->limbs);
	if (mask == 0U)
		return EINVAL;
	mask = cryc_bn_lt(y, ec->field.modulus, ec->limbs);
	if (mask == 0U)
		return EINVAL;

	/* Puts the coordinates into Montgomery form. */
	cryc_bn_to_mont(x_mont, x, &ec->field);
	cryc_bn_to_mont(y_mont, y, &ec->field);

	/* Forms the right side x^3 - 3x + b of the curve equation (SP 800-186 3.2.1.3 and 3.2.1.4: a = -3). */
	cryc_bn_mont_mul(left, x_mont, x_mont, &ec->field);
	cryc_bn_mont_mul(right, left, x_mont, &ec->field);
	cryc_bn_mod_add(left, x_mont, x_mont, &ec->field);
	cryc_bn_mod_add(left, left, x_mont, &ec->field);
	cryc_bn_mod_sub(right, right, left, &ec->field);
	cryc_bn_mod_add(right, right, ec->b_mont, &ec->field);

	/* Forms the left side y^2. */
	cryc_bn_mont_mul(left, y_mont, y_mont, &ec->field);

	/* Refuses a point that is not on the curve (SEC 1 3.2.2.1 step 3). */
	for (index = 0; index < ec->limbs; index++) {
		if (left[index] != right[index])
			return EINVAL;
	}

	/* Makes the point, with Z = 1. */
	memcpy(point->x, x_mont, sizeof(point->x));
	memcpy(point->y, y_mont, sizeof(point->y));
	cryc_bn_mont_one(point->z, &ec->field);

	/* Succeeded: the point is valid. */
	return 0;
}

/*
 * Encodes a point uncompressed (SEC 1 2.3.3) into 1 + 2 * ec->size bytes.
 */
int
cryc_ecp_encode(
	const struct cryc_ecp *ec,
	const struct cryc_ecp_point *point,
	uint8_t *out,
	size_t size)
{
	uint32_t x[CRYC_ECP_MAX_LIMBS];
	uint32_t y[CRYC_ECP_MAX_LIMBS];
	uint32_t infinity;
	uint32_t keep;
	size_t total;
	size_t index;

	/* Refuses a buffer that is too small, writing nothing. */
	total = 1U + 2U * ec->size;
	if (size < total)
		return ENOBUFS;

	/* Converts to affine coordinates, out of Montgomery form. */
	infinity = ecp_to_affine(ec, x, y, point);

	/* Writes the tag and the coordinates. */
	out[0] = (uint8_t)ECP_TAG_UNCOMPRESSED;
	cryc_bn_encode(x, ec->limbs, out + 1U, ec->size);
	cryc_bn_encode(y, ec->limbs, out + 1U + ec->size, ec->size);

	/* Zeroes the whole encoding for the point at infinity, with a mask. */
	keep = ~infinity;
	for (index = 0; index < total; index++)
		out[index] = (uint8_t)(out[index] & (keep & 0xffU));

	/* Wipes the affine coordinates, which may be secret. */
	cryc_wipe(x, sizeof(x));
	cryc_wipe(y, sizeof(y));

	/* Reports the point at infinity as an error, chosen with a mask. */
	return (int)((uint32_t)EINVAL & infinity);
}

/*
 * Writes the affine x coordinate of point (not in Montgomery form) as
 * ec->size big-endian bytes into out: the shared secret of ECDH (NIST SP
 * 800-56A Rev.
 */
int
cryc_ecp_affine_x(
	const struct cryc_ecp *ec,
	const struct cryc_ecp_point *point,
	uint8_t *out)
{
	uint32_t x[CRYC_ECP_MAX_LIMBS];
	uint32_t y[CRYC_ECP_MAX_LIMBS];
	uint32_t infinity;
	uint32_t keep;
	size_t index;

	/* Converts to affine coordinates, out of Montgomery form. */
	infinity = ecp_to_affine(ec, x, y, point);

	/* Writes x, zeroed for the point at infinity with a mask. */
	cryc_bn_encode(x, ec->limbs, out, ec->size);
	keep = ~infinity;
	for (index = 0; index < ec->size; index++)
		out[index] = (uint8_t)(out[index] & (keep & 0xffU));

	/* Wipes the affine coordinates, which may be secret. */
	cryc_wipe(x, sizeof(x));
	cryc_wipe(y, sizeof(y));

	/* Reports the point at infinity as an error, chosen with a mask. */
	return (int)((uint32_t)EINVAL & infinity);
}

/*
 * out = scalar * point.
 */
void
cryc_ecp_mul(
	const struct cryc_ecp *ec,
	struct cryc_ecp_point *out,
	const struct cryc_ecp_point *point,
	const uint32_t *scalar)
{
	struct cryc_ecp_point table[ECP_TABLE_SIZE];
	struct cryc_ecp_point accumulator;
	struct cryc_ecp_point picked;
	size_t window;
	size_t index;
	size_t step;
	uint32_t digit;
	uint32_t mask;

	/*
	 * Builds the multiples 0 * P to 15 * P.  An even multiple is the
	 * double of the half; an odd one adds P to the one before.
	 */
	ecp_infinity(&table[0]);
	table[1] = *point;
	for (index = 2; index < ECP_TABLE_SIZE; index++) {
		if (index % 2U == 0U)
			ecp_double(ec, &table[index], &table[index / 2U]);
		else
			ecp_add(ec, &table[index], &table[index - 1U], &table[1]);
	}

	/*
	 * Runs the same steps whatever the scalar: from the highest 4-bit
	 * digit down, four doublings, then the multiple the digit names,
	 * found by looking at every entry of the table and selecting the
	 * wanted one with a mask, then an addition (of infinity for a zero
	 * digit, which the addition handles by masks too).
	 */
	ecp_infinity(&accumulator);
	for (window = ec->size * 2U; window > 0U; window--) {
		for (step = 0; step < ECP_WINDOW_BITS; step++)
			ecp_double(ec, &accumulator, &accumulator);

		/* Reads the digit; the shift is by a public amount. */
		digit = (scalar[(window - 1U) / 8U] >> (4U * (uint32_t)((window - 1U) % 8U))) & 15U;

		/* Selects the entry of the digit without an index that depends on it. */
		ecp_infinity(&picked);
		for (index = 0; index < ECP_TABLE_SIZE; index++) {
			mask = cryc_ct_mask_eq32((uint32_t)index, digit);
			ecp_select(&picked, &table[index], &picked, mask);
		}
		ecp_add(ec, &accumulator, &accumulator, &picked);
	}

	/* Delivers the result and wipes everything derived from the scalar. */
	*out = accumulator;
	cryc_wipe(table, sizeof(table));
	cryc_wipe(&accumulator, sizeof(accumulator));
	cryc_wipe(&picked, sizeof(picked));
}

/*
 * out = scalar * G; as cryc_ecp_mul with the base point.
 */
void
cryc_ecp_mul_base(
	const struct cryc_ecp *ec,
	struct cryc_ecp_point *out,
	const uint32_t *scalar)
{
	struct cryc_ecp_point base;

	/* Multiplies the base point, which is public, by the scalar. */
	ecp_base_point(ec, &base);
	cryc_ecp_mul(ec, out, &base, scalar);
}

/*
 * out = u1 * G + u2 * q, for ECDSA verification (FIPS 186-5 6.4.2 step 6).
 */
void
cryc_ecp_mul_add_public(
	const struct cryc_ecp *ec,
	struct cryc_ecp_point *out,
	const uint32_t *u1,
	const uint32_t *u2,
	const struct cryc_ecp_point *q)
{
	struct cryc_ecp_point base;
	struct cryc_ecp_point both;
	struct cryc_ecp_point accumulator;
	size_t bit;
	uint32_t bit_one;
	uint32_t bit_two;

	/* Prepares G, q and their sum, the three non-zero choices of a bit pair. */
	ecp_base_point(ec, &base);
	ecp_add(ec, &both, &base, q);

	/*
	 * Goes from the highest bit down: a doubling, then the addition of the
	 * point the two scalars' bits name.  The values are public, so the
	 * choice may branch.
	 */
	ecp_infinity(&accumulator);
	for (bit = ec->size * 8U; bit > 0U; bit--) {
		ecp_double(ec, &accumulator, &accumulator);
		bit_one = ecp_bit(u1, bit - 1U);
		bit_two = ecp_bit(u2, bit - 1U);
		if (bit_one != 0U && bit_two != 0U)
			ecp_add(ec, &accumulator, &accumulator, &both);
		else if (bit_one != 0U)
			ecp_add(ec, &accumulator, &accumulator, &base);
		else if (bit_two != 0U)
			ecp_add(ec, &accumulator, &accumulator, q);
	}

	/* Delivers the result. */
	*out = accumulator;
}

/*
 * Sets point to the point at infinity: every coordinate zero, so Z = 0.
 */
static void
ecp_infinity(
	struct cryc_ecp_point *point)
{
	/* Zeroes the three coordinates. */
	memset(point, 0, sizeof(*point));
}

/*
 * Sets point to the base point, in Montgomery form with Z = 1.
 */
static void
ecp_base_point(
	const struct cryc_ecp *ec,
	struct cryc_ecp_point *point)
{
	/* Copies the coordinates and sets Z to one. */
	memset(point, 0, sizeof(*point));
	memcpy(point->x, ec->base_x, sizeof(point->x));
	memcpy(point->y, ec->base_y, sizeof(point->y));
	cryc_bn_mont_one(point->z, &ec->field);
}

/*
 * Sets out to when_set where mask is 0xffffffff and to when_clear where it
 * is 0, in constant time.
 */
static void
ecp_select(
	struct cryc_ecp_point *out,
	const struct cryc_ecp_point *when_set,
	const struct cryc_ecp_point *when_clear,
	uint32_t mask)
{
	/* Selects each coordinate over the full width. */
	cryc_bn_select(out->x, when_set->x, when_clear->x, CRYC_ECP_MAX_LIMBS, mask);
	cryc_bn_select(out->y, when_set->y, when_clear->y, CRYC_ECP_MAX_LIMBS, mask);
	cryc_bn_select(out->z, when_set->z, when_clear->z, CRYC_ECP_MAX_LIMBS, mask);
}

/*
 * Doubles a point (for a = -3).
 *
 * With x = X / Z^2 and y = Y / Z^3, the doubling of A.1.1 has the slope
 * lambda = (3 x^2 - 3) / (2 y) = 3 (X - Z^2) (X + Z^2) / (2 Y Z).  Put
 * M = 3 (X - Z^2) (X + Z^2), S = 4 X Y^2 and Z3 = 2 Y Z, so lambda = M / Z3
 * and Z3^2 = 4 Y^2 Z^2.  Then x3 = lambda^2 - 2x gives X3 = M^2 - 2 S, and
 * y3 = lambda (x - x3) - y gives Y3 = M (S - X3) - 8 Y^4.  A point with
 * Z = 0 gives Z3 = 0, so infinity doubles to infinity.  out may alias in.
 */
static void
ecp_double(
	const struct cryc_ecp *ec,
	struct cryc_ecp_point *out,
	const struct cryc_ecp_point *in)
{
	uint32_t zz[CRYC_ECP_MAX_LIMBS];
	uint32_t left[CRYC_ECP_MAX_LIMBS];
	uint32_t right[CRYC_ECP_MAX_LIMBS];
	uint32_t m[CRYC_ECP_MAX_LIMBS];
	uint32_t yy[CRYC_ECP_MAX_LIMBS];
	uint32_t s[CRYC_ECP_MAX_LIMBS];
	uint32_t x3[CRYC_ECP_MAX_LIMBS];
	uint32_t y3[CRYC_ECP_MAX_LIMBS];
	uint32_t z3[CRYC_ECP_MAX_LIMBS];
	uint32_t t[CRYC_ECP_MAX_LIMBS];

	/* Starts the temporaries at zero, so the limbs above the width stay zero. */
	memset(zz, 0, sizeof(zz));
	memset(left, 0, sizeof(left));
	memset(right, 0, sizeof(right));
	memset(m, 0, sizeof(m));
	memset(yy, 0, sizeof(yy));
	memset(s, 0, sizeof(s));
	memset(x3, 0, sizeof(x3));
	memset(y3, 0, sizeof(y3));
	memset(z3, 0, sizeof(z3));
	memset(t, 0, sizeof(t));

	/* Forms M = 3 (X - Z^2) (X + Z^2). */
	cryc_bn_mont_mul(zz, in->z, in->z, &ec->field);
	cryc_bn_mod_sub(left, in->x, zz, &ec->field);
	cryc_bn_mod_add(right, in->x, zz, &ec->field);
	cryc_bn_mont_mul(t, left, right, &ec->field);
	cryc_bn_mod_add(m, t, t, &ec->field);
	cryc_bn_mod_add(m, m, t, &ec->field);

	/* Forms S = 4 X Y^2, keeping Y^2. */
	cryc_bn_mont_mul(yy, in->y, in->y, &ec->field);
	cryc_bn_mont_mul(t, in->x, yy, &ec->field);
	cryc_bn_mod_add(t, t, t, &ec->field);
	cryc_bn_mod_add(s, t, t, &ec->field);

	/* Forms X3 = M^2 - 2 S. */
	cryc_bn_mont_mul(x3, m, m, &ec->field);
	cryc_bn_mod_sub(x3, x3, s, &ec->field);
	cryc_bn_mod_sub(x3, x3, s, &ec->field);

	/* Forms Y3 = M (S - X3) - 8 Y^4, with Y^4 = (Y^2)^2. */
	cryc_bn_mod_sub(t, s, x3, &ec->field);
	cryc_bn_mont_mul(y3, m, t, &ec->field);
	cryc_bn_mont_mul(t, yy, yy, &ec->field);
	cryc_bn_mod_add(t, t, t, &ec->field);
	cryc_bn_mod_add(t, t, t, &ec->field);
	cryc_bn_mod_add(t, t, t, &ec->field);
	cryc_bn_mod_sub(y3, y3, t, &ec->field);

	/* Forms Z3 = 2 Y Z. */
	cryc_bn_mont_mul(z3, in->y, in->z, &ec->field);
	cryc_bn_mod_add(z3, z3, z3, &ec->field);

	/* Delivers the result, which may alias the input, then wipes the temporaries. */
	memcpy(out->x, x3, sizeof(out->x));
	memcpy(out->y, y3, sizeof(out->y));
	memcpy(out->z, z3, sizeof(out->z));
	cryc_wipe(zz, sizeof(zz));
	cryc_wipe(left, sizeof(left));
	cryc_wipe(right, sizeof(right));
	cryc_wipe(m, sizeof(m));
	cryc_wipe(yy, sizeof(yy));
	cryc_wipe(s, sizeof(s));
	cryc_wipe(x3, sizeof(x3));
	cryc_wipe(y3, sizeof(y3));
	cryc_wipe(z3, sizeof(z3));
	cryc_wipe(t, sizeof(t));
}

/*
 * Adds two points, whatever they are.
 *
 * Put U1 = X1 Z2^2, U2 = X2 Z1^2, S1 = Y1 Z2^3, S2 = Y2 Z1^3, H = U2 - U1
 * and R = S2 - S1.  The two affine points are then x1 = U1 / (Z1 Z2)^2,
 * x2 = U2 / (Z1 Z2)^2, y1 = S1 / (Z1 Z2)^3, and the slope of A.1.1 is
 * lambda = R / (H Z1 Z2).  With Z3 = H Z1 Z2, x3 = lambda^2 - x1 - x2 gives
 * X3 = R^2 - H^3 - 2 U1 H^2 (using U2 = U1 + H), and y3 = lambda (x1 - x3)
 * - y1 gives Y3 = R (U1 H^2 - X3) - S1 H^3.
 *
 * The cases the formula does not cover are found by masks and the result
 * chosen after the formula and the doubling were both computed: the left
 * point at infinity (the result is the right point), the right point at
 * infinity (the left point), the two points equal (H = 0 and R = 0: the
 * double of the left point).  Opposite points (H = 0, R not 0) give
 * Z3 = 0, which is infinity.  out may alias left or right.
 */
static void
ecp_add(
	const struct cryc_ecp *ec,
	struct cryc_ecp_point *out,
	const struct cryc_ecp_point *left,
	const struct cryc_ecp_point *right)
{
	struct cryc_ecp_point sum;
	struct cryc_ecp_point twice;
	uint32_t z1z1[CRYC_ECP_MAX_LIMBS];
	uint32_t z2z2[CRYC_ECP_MAX_LIMBS];
	uint32_t u1[CRYC_ECP_MAX_LIMBS];
	uint32_t u2[CRYC_ECP_MAX_LIMBS];
	uint32_t s1[CRYC_ECP_MAX_LIMBS];
	uint32_t s2[CRYC_ECP_MAX_LIMBS];
	uint32_t h[CRYC_ECP_MAX_LIMBS];
	uint32_t r[CRYC_ECP_MAX_LIMBS];
	uint32_t hh[CRYC_ECP_MAX_LIMBS];
	uint32_t hhh[CRYC_ECP_MAX_LIMBS];
	uint32_t v[CRYC_ECP_MAX_LIMBS];
	uint32_t t[CRYC_ECP_MAX_LIMBS];
	uint32_t same;
	uint32_t left_infinity;
	uint32_t right_infinity;

	/* Starts the temporaries at zero, so the limbs above the width stay zero. */
	memset(&sum, 0, sizeof(sum));
	memset(z1z1, 0, sizeof(z1z1));
	memset(z2z2, 0, sizeof(z2z2));
	memset(u1, 0, sizeof(u1));
	memset(u2, 0, sizeof(u2));
	memset(s1, 0, sizeof(s1));
	memset(s2, 0, sizeof(s2));
	memset(h, 0, sizeof(h));
	memset(r, 0, sizeof(r));
	memset(hh, 0, sizeof(hh));
	memset(hhh, 0, sizeof(hhh));
	memset(v, 0, sizeof(v));
	memset(t, 0, sizeof(t));

	/* Forms U1 = X1 Z2^2 and U2 = X2 Z1^2. */
	cryc_bn_mont_mul(z1z1, left->z, left->z, &ec->field);
	cryc_bn_mont_mul(z2z2, right->z, right->z, &ec->field);
	cryc_bn_mont_mul(u1, left->x, z2z2, &ec->field);
	cryc_bn_mont_mul(u2, right->x, z1z1, &ec->field);

	/* Forms S1 = Y1 Z2^3 and S2 = Y2 Z1^3. */
	cryc_bn_mont_mul(t, right->z, z2z2, &ec->field);
	cryc_bn_mont_mul(s1, left->y, t, &ec->field);
	cryc_bn_mont_mul(t, left->z, z1z1, &ec->field);
	cryc_bn_mont_mul(s2, right->y, t, &ec->field);

	/* Forms H and R, and the powers of H. */
	cryc_bn_mod_sub(h, u2, u1, &ec->field);
	cryc_bn_mod_sub(r, s2, s1, &ec->field);
	cryc_bn_mont_mul(hh, h, h, &ec->field);
	cryc_bn_mont_mul(hhh, h, hh, &ec->field);
	cryc_bn_mont_mul(v, u1, hh, &ec->field);

	/* Forms X3 = R^2 - H^3 - 2 U1 H^2. */
	cryc_bn_mont_mul(sum.x, r, r, &ec->field);
	cryc_bn_mod_sub(sum.x, sum.x, hhh, &ec->field);
	cryc_bn_mod_sub(sum.x, sum.x, v, &ec->field);
	cryc_bn_mod_sub(sum.x, sum.x, v, &ec->field);

	/* Forms Y3 = R (U1 H^2 - X3) - S1 H^3. */
	cryc_bn_mod_sub(t, v, sum.x, &ec->field);
	cryc_bn_mont_mul(sum.y, r, t, &ec->field);
	cryc_bn_mont_mul(t, s1, hhh, &ec->field);
	cryc_bn_mod_sub(sum.y, sum.y, t, &ec->field);

	/* Forms Z3 = H Z1 Z2. */
	cryc_bn_mont_mul(t, left->z, right->z, &ec->field);
	cryc_bn_mont_mul(sum.z, h, t, &ec->field);

	/* Computes the doubling of the left point, for the case of equal points. */
	ecp_double(ec, &twice, left);

	/* Finds the special cases by masks. */
	same = cryc_bn_is_zero(h, ec->limbs) & cryc_bn_is_zero(r, ec->limbs);
	left_infinity = cryc_bn_is_zero(left->z, ec->limbs);
	right_infinity = cryc_bn_is_zero(right->z, ec->limbs);

	/*
	 * Chooses the result: the doubling for equal points, then the left
	 * point when the right one is at infinity, then the right point when
	 * the left one is (which wins when both are).
	 */
	ecp_select(&sum, &twice, &sum, same);
	ecp_select(&sum, left, &sum, right_infinity);
	ecp_select(out, right, &sum, left_infinity);

	/* Wipes the temporaries. */
	cryc_wipe(&sum, sizeof(sum));
	cryc_wipe(&twice, sizeof(twice));
	cryc_wipe(z1z1, sizeof(z1z1));
	cryc_wipe(z2z2, sizeof(z2z2));
	cryc_wipe(u1, sizeof(u1));
	cryc_wipe(u2, sizeof(u2));
	cryc_wipe(s1, sizeof(s1));
	cryc_wipe(s2, sizeof(s2));
	cryc_wipe(h, sizeof(h));
	cryc_wipe(r, sizeof(r));
	cryc_wipe(hh, sizeof(hh));
	cryc_wipe(hhh, sizeof(hhh));
	cryc_wipe(v, sizeof(v));
	cryc_wipe(t, sizeof(t));
}

/*
 * Sets out to the inverse of value modulo p, for a value in Montgomery form
 * (the result is in Montgomery form); zero gives zero.
 *
 * Fermat's little theorem: value^(p - 2) = value^-1.  The exponent p - 2 is
 * public.
 */
static void
ecp_invert(
	const struct cryc_ecp *ec,
	uint32_t *out,
	const uint32_t *value)
{
	uint8_t exponent[CRYC_ECP_MAX_BYTES];
	size_t index;
	uint32_t borrow;
	uint32_t difference;

	/* Writes p as bytes and subtracts 2, from the last byte with a borrow. */
	cryc_bn_encode(ec->field.modulus, ec->limbs, exponent, ec->size);
	borrow = 2U;
	for (index = ec->size; index > 0U; index--) {
		difference = (uint32_t)exponent[index - 1U] - borrow;
		exponent[index - 1U] = (uint8_t)(difference & 0xffU);
		borrow = (difference >> 31) & 1U;
	}

	/* Raises to the power p - 2. */
	cryc_bn_mont_exp(out, value, exponent, ec->size, &ec->field);
}

/*
 * Converts point to affine coordinates, out of Montgomery form, into x and
 * y (ec->limbs limbs each; both zero for the point at infinity), and
 * returns 0xffffffff for the point at infinity and 0 otherwise.
 */
static uint32_t
ecp_to_affine(
	const struct cryc_ecp *ec,
	uint32_t *x,
	uint32_t *y,
	const struct cryc_ecp_point *point)
{
	uint32_t inverse[CRYC_ECP_MAX_LIMBS];
	uint32_t square[CRYC_ECP_MAX_LIMBS];
	uint32_t cube[CRYC_ECP_MAX_LIMBS];
	uint32_t x_mont[CRYC_ECP_MAX_LIMBS];
	uint32_t y_mont[CRYC_ECP_MAX_LIMBS];
	uint32_t infinity;

	/* Starts the temporaries at zero, so the limbs above the width stay zero. */
	memset(inverse, 0, sizeof(inverse));
	memset(square, 0, sizeof(square));
	memset(cube, 0, sizeof(cube));
	memset(x_mont, 0, sizeof(x_mont));
	memset(y_mont, 0, sizeof(y_mont));

	/* Finds the point at infinity, whose inverse of Z is zero, which makes both coordinates zero. */
	infinity = cryc_bn_is_zero(point->z, ec->limbs);

	/* Forms x = X / Z^2 and y = Y / Z^3. */
	ecp_invert(ec, inverse, point->z);
	cryc_bn_mont_mul(square, inverse, inverse, &ec->field);
	cryc_bn_mont_mul(cube, square, inverse, &ec->field);
	cryc_bn_mont_mul(x_mont, point->x, square, &ec->field);
	cryc_bn_mont_mul(y_mont, point->y, cube, &ec->field);

	/* Takes the coordinates out of Montgomery form. */
	cryc_bn_from_mont(x, x_mont, &ec->field);
	cryc_bn_from_mont(y, y_mont, &ec->field);

	/* Wipes the temporaries. */
	cryc_wipe(inverse, sizeof(inverse));
	cryc_wipe(square, sizeof(square));
	cryc_wipe(cube, sizeof(cube));
	cryc_wipe(x_mont, sizeof(x_mont));
	cryc_wipe(y_mont, sizeof(y_mont));

	/* Reports whether the point was at infinity. */
	return infinity;
}

/*
 * Returns bit index (0 is the least significant) of scalar, 0 or 1.
 */
static uint32_t
ecp_bit(
	const uint32_t *scalar,
	size_t index)
{
	/* Picks the limb and shifts the bit down. */
	return (scalar[index / 32U] >> (index % 32U)) & 1U;
}
