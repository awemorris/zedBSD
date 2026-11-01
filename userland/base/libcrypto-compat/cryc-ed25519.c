/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Ed25519 (RFC 8032, January 2017).
 *
 * The section numbers in the comments of this file are those of RFC 8032.
 * A point is held in extended homogeneous coordinates (X, Y, Z, T) with
 * x = X / Z, y = Y / Z and x * y = T / Z (5.1.4).  Every value that is
 * not a function's input is reduced (cryc_f25519_carry) before it is the
 * operand of an addition or a subtraction, so a field element given to
 * cryc_f25519_mul is never looser than the sum or difference of two
 * reduced elements.
 */

#include <string.h>

#include "cryc-ed25519.h"

/* The number of 32-bit limbs of a scalar below 2^256. */
#define ED_SCALAR_LIMBS	8U

/* The number of 32-bit limbs of a 512-bit hash read as an integer. */
#define ED_WIDE_LIMBS	16U

/* The number of multiples [0]P .. [15]P a 4-bit window selects from. */
#define ED_TABLE_SIZE	16U

/* The window count of a 256-bit scalar (4 bits each). */
#define ED_WINDOWS_256	64U

/* The window count of a 512-bit scalar (4 bits each). */
#define ED_WINDOWS_512	128U

/*
 * A point of the curve in extended homogeneous coordinates (5.1.4).
 *
 * An instance lives on the stack of the function that computes with it;
 * it is secret whenever the scalar that produced it is.
 */
struct ed_point {
	struct cryc_f25519 x;
	struct cryc_f25519 y;
	struct cryc_f25519 z;
	struct cryc_f25519 t;
};

/*
 * The curve constant d = -121665 / 121666 mod p, 32 bytes little-endian.
 *
 * Values referenced: RFC 8032 (January 2017) 5.1, Table 1, d of
 * edwards25519 (-121665/121666 modulo p = 2^255 - 19).
 */
static const uint8_t ed_constant_d[32] = {
	0xa3U, 0x78U, 0x59U, 0x13U, 0xcaU, 0x4dU, 0xebU, 0x75U,
	0xabU, 0xd8U, 0x41U, 0x41U, 0x4dU, 0x0aU, 0x70U, 0x00U,
	0x98U, 0xe8U, 0x79U, 0x77U, 0x79U, 0x40U, 0xc7U, 0x8cU,
	0x73U, 0xfeU, 0x6fU, 0x2bU, 0xeeU, 0x6cU, 0x03U, 0x52U
};

/*
 * A square root of -1 modulo p, 2^((p - 1) / 4), 32 bytes little-endian.
 *
 * Values referenced: RFC 8032 (January 2017) 5.1.1 and 5.1.3 step 3.2,
 * the factor 2^((p-1)/4) that turns a candidate root x with x^2 = -a into
 * a root of a.
 */
static const uint8_t ed_constant_sqrt_m1[32] = {
	0xb0U, 0xa0U, 0x0eU, 0x4aU, 0x27U, 0x1bU, 0xeeU, 0xc4U,
	0x78U, 0xe4U, 0x2fU, 0xadU, 0x06U, 0x18U, 0x43U, 0x2fU,
	0xa7U, 0xd7U, 0xfbU, 0x3dU, 0x99U, 0x00U, 0x4dU, 0x2bU,
	0x0bU, 0xdfU, 0xc1U, 0x4fU, 0x80U, 0x24U, 0x83U, 0x2bU
};

/*
 * The x-coordinate of the base point B, 32 bytes little-endian.
 *
 * Values referenced: RFC 8032 (January 2017) 5.1, Table 1, B =
 * (X(P), Y(P)) of edwards25519: X(P) =
 * 15112221349535400772501151409588531511454012693041857206046113283949847762202.
 */
static const uint8_t ed_base_x[32] = {
	0x1aU, 0xd5U, 0x25U, 0x8fU, 0x60U, 0x2dU, 0x56U, 0xc9U,
	0xb2U, 0xa7U, 0x25U, 0x95U, 0x60U, 0xc7U, 0x2cU, 0x69U,
	0x5cU, 0xdcU, 0xd6U, 0xfdU, 0x31U, 0xe2U, 0xa4U, 0xc0U,
	0xfeU, 0x53U, 0x6eU, 0xcdU, 0xd3U, 0x36U, 0x69U, 0x21U
};

/*
 * The y-coordinate of the base point B, 32 bytes little-endian (4/5 mod p).
 *
 * Values referenced: RFC 8032 (January 2017) 5.1, Table 1, B =
 * (X(P), Y(P)) of edwards25519: Y(P) =
 * 46316835694926478169428394003475163141307993866256225615783033603165251855960.
 */
static const uint8_t ed_base_y[32] = {
	0x58U, 0x66U, 0x66U, 0x66U, 0x66U, 0x66U, 0x66U, 0x66U,
	0x66U, 0x66U, 0x66U, 0x66U, 0x66U, 0x66U, 0x66U, 0x66U,
	0x66U, 0x66U, 0x66U, 0x66U, 0x66U, 0x66U, 0x66U, 0x66U,
	0x66U, 0x66U, 0x66U, 0x66U, 0x66U, 0x66U, 0x66U, 0x66U
};

/*
 * The group order L = 2^252 + 27742317777372353535851937790883648493 as
 * eight 32-bit limbs, least significant first.
 *
 * Values referenced: RFC 8032 (January 2017) 5.1, Table 1, L of
 * edwards25519.
 */
static const uint32_t ed_order[ED_SCALAR_LIMBS] = {
	0x5cf5d3edU, 0x5812631aU, 0xa2f79cd6U, 0x14def9deU,
	0x00000000U, 0x00000000U, 0x00000000U, 0x10000000U
};

/* Forward declarations. */
static void ed_add_reduced(struct cryc_f25519 *out, const struct cryc_f25519 *a, const struct cryc_f25519 *b);
static void ed_sub_reduced(struct cryc_f25519 *out, const struct cryc_f25519 *a, const struct cryc_f25519 *b);
static void ed_identity(struct ed_point *out);
static void ed_base_point(struct ed_point *out);
static void ed_point_add(struct ed_point *out, const struct ed_point *left, const struct ed_point *right);
static void ed_point_double(struct ed_point *out, const struct ed_point *in);
static void ed_point_negate(struct ed_point *out, const struct ed_point *in);
static void ed_point_select(struct ed_point *out, const struct ed_point *table, uint32_t index);
static void ed_scalar_mul(struct ed_point *out, const uint8_t *scalar, unsigned int windows, const struct ed_point *base);
static void ed_point_encode(uint8_t *out, const struct ed_point *in);
static int ed_point_decode(struct ed_point *out, const uint8_t *in);
static void ed_reduce_order(uint32_t *out, const uint32_t *in);
static void ed_scalar_muladd(uint32_t *out, const uint32_t *k, const uint32_t *s, const uint32_t *r);
static int ed_scalar_below_order(const uint32_t *scalar);
static void ed_load_limbs(uint32_t *out, const uint8_t *in, unsigned int count);
static void ed_store_limbs(uint8_t *out, const uint32_t *in, unsigned int count);
static void ed_expand(uint8_t *scalar, uint8_t *prefix, const uint8_t *seed);
static void ed_hash(uint8_t *out, const uint8_t *first, size_t first_size, const uint8_t *second, size_t second_size, const uint8_t *message, size_t message_size);

/*
 * Computes the public key of a 32-byte private key (seed) (5.1.5).
 */
int
cryc_ed25519_public(
	uint8_t *public_key,
	const uint8_t *seed)
{
	uint8_t scalar[32];
	uint8_t prefix[32];
	struct ed_point base;
	struct ed_point product;

	/* Hashes the seed and prunes the first half into the secret scalar (steps 1 and 2). */
	ed_expand(scalar, prefix, seed);

	/* Multiplies the base point by the scalar and encodes it (steps 3 and 4). */
	ed_base_point(&base);
	ed_scalar_mul(&product, scalar, ED_WINDOWS_256, &base);
	ed_point_encode(public_key, &product);

	/* The scalar, the prefix and the point it made are secret. */
	cryc_wipe(scalar, sizeof(scalar));
	cryc_wipe(prefix, sizeof(prefix));
	cryc_wipe(&product, sizeof(product));

	/* Succeeded: the public key is written. */
	return 0;
}

/*
 * Signs message with the private key seed (the public key is derived
 * inside).
 */
int
cryc_ed25519_sign(
	uint8_t *signature,
	const uint8_t *message,
	size_t message_size,
	const uint8_t *seed)
{
	uint8_t scalar[32];
	uint8_t prefix[32];
	uint8_t public_key[32];
	uint8_t digest[64];
	uint8_t nonce_bytes[32];
	uint32_t wide[ED_WIDE_LIMBS];
	uint32_t nonce[ED_SCALAR_LIMBS];
	uint32_t challenge[ED_SCALAR_LIMBS];
	uint32_t secret[ED_SCALAR_LIMBS];
	uint32_t response[ED_SCALAR_LIMBS];
	struct ed_point base;
	struct ed_point product;

	/* Hashes the seed: the secret scalar s and the prefix (step 1). */
	ed_expand(scalar, prefix, seed);

	/* The public key A, derived here and never taken from the caller (step 1). */
	ed_base_point(&base);
	ed_scalar_mul(&product, scalar, ED_WINDOWS_256, &base);
	ed_point_encode(public_key, &product);

	/* The nonce r = SHA-512(prefix || M) mod L (steps 2 and 3). */
	ed_hash(digest, prefix, sizeof(prefix), NULL, 0, message, message_size);
	ed_load_limbs(wide, digest, ED_WIDE_LIMBS);
	ed_reduce_order(nonce, wide);
	ed_store_limbs(nonce_bytes, nonce, ED_SCALAR_LIMBS);

	/* R = encode([r]B), the first half of the signature (step 3). */
	ed_scalar_mul(&product, nonce_bytes, ED_WINDOWS_256, &base);
	ed_point_encode(signature, &product);

	/* The challenge k = SHA-512(R || A || M) mod L (step 4). */
	ed_hash(digest, signature, 32U, public_key, sizeof(public_key), message, message_size);
	ed_load_limbs(wide, digest, ED_WIDE_LIMBS);
	ed_reduce_order(challenge, wide);

	/* S = (r + k * s) mod L, the second half of the signature (steps 5 and 6). */
	ed_load_limbs(secret, scalar, ED_SCALAR_LIMBS);
	ed_scalar_muladd(response, challenge, secret, nonce);
	ed_store_limbs(signature + 32, response, ED_SCALAR_LIMBS);

	/* Everything that came from the seed is secret. */
	cryc_wipe(scalar, sizeof(scalar));
	cryc_wipe(prefix, sizeof(prefix));
	cryc_wipe(digest, sizeof(digest));
	cryc_wipe(nonce_bytes, sizeof(nonce_bytes));
	cryc_wipe(wide, sizeof(wide));
	cryc_wipe(nonce, sizeof(nonce));
	cryc_wipe(challenge, sizeof(challenge));
	cryc_wipe(secret, sizeof(secret));
	cryc_wipe(response, sizeof(response));
	cryc_wipe(&product, sizeof(product));

	/* Succeeded: the 64-byte signature is written. */
	return 0;
}

/*
 * Verifies a signature (5.1.7) with the cofactorless equation [S]B = R +
 * [k]A' of step 3, rejecting S not less than L (step 1) and a public key
 * or R that does not decode (5.1.3).
 */
int
cryc_ed25519_verify(
	const uint8_t *signature,
	const uint8_t *message,
	size_t message_size,
	const uint8_t *public_key)
{
	uint8_t digest[64];
	uint8_t encoded[32];
	uint32_t response[ED_SCALAR_LIMBS];
	struct ed_point base;
	struct ed_point key;
	struct ed_point negative_key;
	struct ed_point left;
	struct ed_point right;
	struct ed_point difference;
	int decoded;
	int same;

	/* S is an integer in 0 <= S < L (step 1). */
	ed_load_limbs(response, signature + 32, ED_SCALAR_LIMBS);
	same = ed_scalar_below_order(response);
	if (same == 0)
		return EBADMSG;

	/* The public key must decode to a point A' (step 1, 5.1.3). */
	decoded = ed_point_decode(&key, public_key);
	if (decoded != 0)
		return EBADMSG;

	/* The challenge k = SHA-512(R || A || M), kept as the full 512-bit integer (step 2). */
	ed_hash(digest, signature, 32U, public_key, 32U, message, message_size);

	/*
	 * Computes [S]B - [k]A' and compares its encoding with the 32 bytes of
	 * R (step 3).  R decodes to a point exactly when its bytes are the
	 * canonical encoding of one, so one equal encoding is the equality of
	 * [S]B with R + [k]A', and a R that does not decode or is not
	 * canonical never matches.
	 */
	ed_base_point(&base);
	ed_point_negate(&negative_key, &key);
	ed_scalar_mul(&left, signature + 32, ED_WINDOWS_256, &base);
	ed_scalar_mul(&right, digest, ED_WINDOWS_512, &negative_key);
	ed_point_add(&difference, &left, &right);
	ed_point_encode(encoded, &difference);
	same = cryc_ct_equal(encoded, signature, sizeof(encoded));
	if (same == 0)
		return EBADMSG;

	/* Succeeded: the signature verifies. */
	return 0;
}

/*
 * Adds two reduced field elements into a reduced element.
 */
static void
ed_add_reduced(
	struct cryc_f25519 *out,
	const struct cryc_f25519 *a,
	const struct cryc_f25519 *b)
{
	/* The sum is loose; the carry brings it back. */
	cryc_f25519_add(out, a, b);
	cryc_f25519_carry(out, out);
}

/*
 * Subtracts one reduced field element from another into a reduced element.
 */
static void
ed_sub_reduced(
	struct cryc_f25519 *out,
	const struct cryc_f25519 *a,
	const struct cryc_f25519 *b)
{
	/* The difference is loose; the carry brings it back. */
	cryc_f25519_sub(out, a, b);
	cryc_f25519_carry(out, out);
}

/*
 * Sets a point to the neutral element (0, 1) (5.1.4).
 */
static void
ed_identity(
	struct ed_point *out)
{
	/* (0, 1, 1, 0) in extended coordinates. */
	cryc_f25519_zero(&out->x);
	cryc_f25519_one(&out->y);
	cryc_f25519_one(&out->z);
	cryc_f25519_zero(&out->t);
}

/*
 * Sets a point to the base point B (5.1, Table 1).
 */
static void
ed_base_point(
	struct ed_point *out)
{
	/* The affine coordinates of Table 1, with Z = 1. */
	cryc_f25519_decode(&out->x, ed_base_x);
	cryc_f25519_decode(&out->y, ed_base_y);
	cryc_f25519_one(&out->z);

	/* T = x * y. */
	cryc_f25519_mul(&out->t, &out->x, &out->y);
}

/*
 * Adds two points with the complete formulas of 5.1.4 (they also double).
 */
static void
ed_point_add(
	struct ed_point *out,
	const struct ed_point *left,
	const struct ed_point *right)
{
	struct cryc_f25519 curve_d;
	struct cryc_f25519 twice_d;
	struct cryc_f25519 a;
	struct cryc_f25519 b;
	struct cryc_f25519 c;
	struct cryc_f25519 d;
	struct cryc_f25519 e;
	struct cryc_f25519 f;
	struct cryc_f25519 g;
	struct cryc_f25519 h;
	struct cryc_f25519 first;
	struct cryc_f25519 second;

	/* 2 * d. */
	cryc_f25519_decode(&curve_d, ed_constant_d);
	ed_add_reduced(&twice_d, &curve_d, &curve_d);

	/* A = (Y1 - X1) * (Y2 - X2). */
	ed_sub_reduced(&first, &left->y, &left->x);
	ed_sub_reduced(&second, &right->y, &right->x);
	cryc_f25519_mul(&a, &first, &second);

	/* B = (Y1 + X1) * (Y2 + X2). */
	ed_add_reduced(&first, &left->y, &left->x);
	ed_add_reduced(&second, &right->y, &right->x);
	cryc_f25519_mul(&b, &first, &second);

	/* C = T1 * 2 * d * T2. */
	cryc_f25519_mul(&first, &left->t, &right->t);
	cryc_f25519_mul(&c, &first, &twice_d);

	/* D = Z1 * 2 * Z2. */
	cryc_f25519_mul(&first, &left->z, &right->z);
	ed_add_reduced(&d, &first, &first);

	/* E = B - A, F = D - C, G = D + C, H = B + A. */
	ed_sub_reduced(&e, &b, &a);
	ed_sub_reduced(&f, &d, &c);
	ed_add_reduced(&g, &d, &c);
	ed_add_reduced(&h, &b, &a);

	/* X3 = E * F, Y3 = G * H, T3 = E * H, Z3 = F * G. */
	cryc_f25519_mul(&out->x, &e, &f);
	cryc_f25519_mul(&out->y, &g, &h);
	cryc_f25519_mul(&out->t, &e, &h);
	cryc_f25519_mul(&out->z, &f, &g);

	/* The operands may be secret. */
	cryc_wipe(&a, sizeof(a));
	cryc_wipe(&b, sizeof(b));
	cryc_wipe(&c, sizeof(c));
	cryc_wipe(&d, sizeof(d));
	cryc_wipe(&e, sizeof(e));
	cryc_wipe(&f, sizeof(f));
	cryc_wipe(&g, sizeof(g));
	cryc_wipe(&h, sizeof(h));
	cryc_wipe(&first, sizeof(first));
	cryc_wipe(&second, sizeof(second));
}

/*
 * Doubles a point with the doubling formulas of 5.1.4.
 */
static void
ed_point_double(
	struct ed_point *out,
	const struct ed_point *in)
{
	struct cryc_f25519 a;
	struct cryc_f25519 b;
	struct cryc_f25519 c;
	struct cryc_f25519 e;
	struct cryc_f25519 f;
	struct cryc_f25519 g;
	struct cryc_f25519 h;
	struct cryc_f25519 sum;

	/* A = X1^2, B = Y1^2, C = 2 * Z1^2. */
	cryc_f25519_square(&a, &in->x);
	cryc_f25519_square(&b, &in->y);
	cryc_f25519_square(&c, &in->z);
	ed_add_reduced(&c, &c, &c);

	/* H = A + B, E = H - (X1 + Y1)^2, G = A - B, F = C + G. */
	ed_add_reduced(&h, &a, &b);
	ed_add_reduced(&sum, &in->x, &in->y);
	cryc_f25519_square(&sum, &sum);
	ed_sub_reduced(&e, &h, &sum);
	ed_sub_reduced(&g, &a, &b);
	ed_add_reduced(&f, &c, &g);

	/* X3 = E * F, Y3 = G * H, T3 = E * H, Z3 = F * G. */
	cryc_f25519_mul(&out->x, &e, &f);
	cryc_f25519_mul(&out->y, &g, &h);
	cryc_f25519_mul(&out->t, &e, &h);
	cryc_f25519_mul(&out->z, &f, &g);

	/* The operands may be secret. */
	cryc_wipe(&a, sizeof(a));
	cryc_wipe(&b, sizeof(b));
	cryc_wipe(&c, sizeof(c));
	cryc_wipe(&e, sizeof(e));
	cryc_wipe(&f, sizeof(f));
	cryc_wipe(&g, sizeof(g));
	cryc_wipe(&h, sizeof(h));
	cryc_wipe(&sum, sizeof(sum));
}

/*
 * Negates a point: (x, y) becomes (-x, y), so X and T change sign.
 */
static void
ed_point_negate(
	struct ed_point *out,
	const struct ed_point *in)
{
	/* The sign of x and of x * y. */
	cryc_f25519_neg(&out->x, &in->x);
	cryc_f25519_carry(&out->x, &out->x);
	cryc_f25519_neg(&out->t, &in->t);
	cryc_f25519_carry(&out->t, &out->t);

	/* y and Z are the same. */
	out->y = in->y;
	out->z = in->z;
}

/*
 * Copies table[index] into out, reading every one of the 16 entries and
 * choosing the wanted one with masks, so the memory touched does not
 * depend on index.
 */
static void
ed_point_select(
	struct ed_point *out,
	const struct ed_point *table,
	uint32_t index)
{
	struct ed_point candidate;
	uint32_t swap;
	unsigned int entry;

	/* Starts from entry 0; every later entry replaces it when its number is index. */
	*out = table[0];
	for (entry = 1; entry < ED_TABLE_SIZE; entry++) {
		candidate = table[entry];
		swap = cryc_ct_mask_eq32((uint32_t)entry, index) & 1U;
		cryc_f25519_cswap(&out->x, &candidate.x, swap);
		cryc_f25519_cswap(&out->y, &candidate.y, swap);
		cryc_f25519_cswap(&out->z, &candidate.z, swap);
		cryc_f25519_cswap(&out->t, &candidate.t, swap);
	}

	/* The discarded copy holds table entries that may be secret multiples. */
	cryc_wipe(&candidate, sizeof(candidate));
}

/*
 * Computes [scalar] * base for a little-endian scalar of windows * 4 bits
 * (5.1.5 step 3, 5.1.6 steps 3 and 5.1.7 step 3), by a fixed 4-bit window
 * from the top: four doublings and one addition of a table entry per
 * window, the entry read by ed_point_select.  The same work is done for
 * every scalar of the length; the scalar may be secret.
 */
static void
ed_scalar_mul(
	struct ed_point *out,
	const uint8_t *scalar,
	unsigned int windows,
	const struct ed_point *base)
{
	struct ed_point table[ED_TABLE_SIZE];
	struct ed_point accumulator;
	struct ed_point picked;
	uint32_t digit;
	unsigned int window;
	unsigned int entry;
	unsigned int step;

	/* The table of the multiples [0]P to [15]P of the base. */
	ed_identity(&table[0]);
	table[1] = *base;
	for (entry = 2; entry < ED_TABLE_SIZE; entry++)
		ed_point_add(&table[entry], &table[entry - 1U], base);

	/* From the top window down: shifts the accumulator by 4 bits and adds the digit's multiple. */
	ed_identity(&accumulator);
	picked = accumulator;
	for (window = windows; window > 0U; window--) {
		for (step = 0; step < 4U; step++)
			ed_point_double(&accumulator, &accumulator);
		digit = ((uint32_t)scalar[(window - 1U) >> 1] >> (((window - 1U) & 1U) * 4U)) & 15U;
		ed_point_select(&picked, table, digit);
		ed_point_add(&accumulator, &accumulator, &picked);
	}

	/* Takes the result. */
	*out = accumulator;

	/* The table and the accumulator hold multiples of the base, secret when the base is. */
	cryc_wipe(table, sizeof(table));
	cryc_wipe(&accumulator, sizeof(accumulator));
	cryc_wipe(&picked, sizeof(picked));
}

/*
 * Encodes a point as 32 bytes (5.1.2): y, with the low bit of x in the top
 * bit of the last byte.
 */
static void
ed_point_encode(
	uint8_t *out,
	const struct ed_point *in)
{
	struct cryc_f25519 inverse;
	struct cryc_f25519 x;
	struct cryc_f25519 y;
	uint32_t negative;

	/* The affine coordinates: x = X / Z, y = Y / Z. */
	cryc_f25519_invert(&inverse, &in->z);
	cryc_f25519_mul(&x, &in->x, &inverse);
	cryc_f25519_mul(&y, &in->y, &inverse);

	/* y, then the low bit of x in bit 255. */
	cryc_f25519_encode(out, &y);
	negative = cryc_f25519_is_negative(&x);
	out[31] = (uint8_t)(out[31] | (uint8_t)(negative << 7));

	/* The coordinates may be secret. */
	cryc_wipe(&inverse, sizeof(inverse));
	cryc_wipe(&x, sizeof(x));
	cryc_wipe(&y, sizeof(y));
}

/*
 * Decodes 32 bytes into a point (5.1.3); returns 0, or EINVAL when the
 * bytes are not the encoding of a point.  Handles public values only.
 */
static int
ed_point_decode(
	struct ed_point *out,
	const uint8_t *in)
{
	uint8_t bytes[32];
	struct cryc_f25519 y;
	struct cryc_f25519 y_squared;
	struct cryc_f25519 curve_d;
	struct cryc_f25519 one;
	struct cryc_f25519 u;
	struct cryc_f25519 v;
	struct cryc_f25519 v3;
	struct cryc_f25519 v7;
	struct cryc_f25519 power;
	struct cryc_f25519 x;
	struct cryc_f25519 check;
	struct cryc_f25519 root_of_minus_one;
	uint32_t sign;
	uint32_t is_square;
	uint32_t is_minus;
	uint32_t negative;
	uint32_t zero;
	unsigned int index;
	int at_least_p;

	/* Bit 255 is x_0; the rest is the integer y (step 1). */
	memcpy(bytes, in, sizeof(bytes));
	sign = (uint32_t)bytes[31] >> 7;
	bytes[31] = (uint8_t)(bytes[31] & 0x7fU);

	/* y >= p fails: p is 0x7f, 0xff x 30, 0xed from the top (step 1). */
	at_least_p = 0;
	if (bytes[31] == 0x7fU && bytes[0] >= 0xedU) {
		at_least_p = 1;
		for (index = 1; index < 31U; index++) {
			if (bytes[index] != 0xffU)
				at_least_p = 0;
		}
	}
	if (at_least_p != 0)
		return EINVAL;

	/* u = y^2 - 1 and v = d * y^2 + 1 (step 2). */
	cryc_f25519_decode(&y, bytes);
	cryc_f25519_decode(&curve_d, ed_constant_d);
	cryc_f25519_one(&one);
	cryc_f25519_square(&y_squared, &y);
	ed_sub_reduced(&u, &y_squared, &one);
	cryc_f25519_mul(&v, &curve_d, &y_squared);
	ed_add_reduced(&v, &v, &one);

	/* The candidate root x = u * v^3 * (u * v^7)^((p - 5) / 8) (step 2). */
	cryc_f25519_square(&v3, &v);
	cryc_f25519_mul(&v3, &v3, &v);
	cryc_f25519_square(&v7, &v3);
	cryc_f25519_mul(&v7, &v7, &v);
	cryc_f25519_mul(&v7, &v7, &u);
	cryc_f25519_pow_p58(&power, &v7);
	cryc_f25519_mul(&x, &u, &v3);
	cryc_f25519_mul(&x, &x, &power);

	/* Tells v * x^2 = u (a root) from v * x^2 = -u (a root after one more factor) (step 3). */
	cryc_f25519_square(&check, &x);
	cryc_f25519_mul(&check, &check, &v);
	ed_sub_reduced(&v3, &check, &u);
	is_square = cryc_f25519_is_zero(&v3);
	ed_add_reduced(&v3, &check, &u);
	is_minus = cryc_f25519_is_zero(&v3);
	if (is_square == 0U && is_minus == 0U)
		return EINVAL;

	/* Multiplies x by 2^((p - 1) / 4) when only -u matched (step 3.2). */
	if (is_square == 0U) {
		cryc_f25519_decode(&root_of_minus_one, ed_constant_sqrt_m1);
		cryc_f25519_mul(&x, &x, &root_of_minus_one);
	}

	/* x = 0 with x_0 = 1 fails (step 4). */
	zero = cryc_f25519_is_zero(&x);
	if (zero != 0U && sign != 0U)
		return EINVAL;

	/* Chooses the root whose low bit is x_0 (step 4). */
	negative = cryc_f25519_is_negative(&x);
	if (negative != sign) {
		cryc_f25519_neg(&x, &x);
		cryc_f25519_carry(&x, &x);
	}

	/* The point with Z = 1 and T = x * y. */
	out->x = x;
	out->y = y;
	cryc_f25519_one(&out->z);
	cryc_f25519_mul(&out->t, &x, &y);

	/* Succeeded: the point is decoded. */
	return 0;
}

/*
 * Reduces a 512-bit integer (16 limbs) modulo L into 8 limbs, bit by bit
 * from the top: shifts the running value left, adds the bit, and subtracts
 * L when that leaves it at least L, the subtraction chosen by a mask.  The
 * input may be secret (the nonce comes from a hash of the secret prefix).
 */
static void
ed_reduce_order(
	uint32_t *out,
	const uint32_t *in)
{
	uint32_t value[ED_SCALAR_LIMBS];
	uint32_t trial[ED_SCALAR_LIMBS];
	uint64_t difference;
	uint32_t carry;
	uint32_t borrow;
	uint32_t mask;
	uint32_t next;
	unsigned int limb;
	unsigned int bit;

	/* Starts from zero. */
	for (limb = 0; limb < ED_SCALAR_LIMBS; limb++)
		value[limb] = 0U;

	/* From bit 511 down to bit 0. */
	for (bit = ED_WIDE_LIMBS * 32U; bit > 0U; bit--) {
		/* value = 2 * value + bit; value is below L, so the result is below 2 * L < 2^254. */
		carry = (in[(bit - 1U) >> 5] >> ((bit - 1U) & 31U)) & 1U;
		for (limb = 0; limb < ED_SCALAR_LIMBS; limb++) {
			next = (value[limb] << 1) | carry;
			carry = value[limb] >> 31;
			value[limb] = next;
		}

		/* trial = value - L, with the borrow of the whole subtraction. */
		borrow = 0U;
		for (limb = 0; limb < ED_SCALAR_LIMBS; limb++) {
			difference = (uint64_t)value[limb] - (uint64_t)ed_order[limb] - (uint64_t)borrow;
			trial[limb] = (uint32_t)difference;
			borrow = (uint32_t)(difference >> 63);
		}

		/* A final borrow of 1 means value < L: keep value; otherwise take trial. */
		mask = borrow - 1U;
		for (limb = 0; limb < ED_SCALAR_LIMBS; limb++)
			value[limb] = (trial[limb] & mask) | (value[limb] & ~mask);
	}

	/* The result. */
	for (limb = 0; limb < ED_SCALAR_LIMBS; limb++)
		out[limb] = value[limb];

	/* The scratch holds secret values. */
	cryc_wipe(value, sizeof(value));
	cryc_wipe(trial, sizeof(trial));
}

/*
 * Computes (r + k * s) mod L from three 8-limb integers, k and r below L
 * and s below 2^256, so that k * s + r fits 512 bits.  Constant time.
 */
static void
ed_scalar_muladd(
	uint32_t *out,
	const uint32_t *k,
	const uint32_t *s,
	const uint32_t *r)
{
	uint32_t product[ED_WIDE_LIMBS];
	uint64_t accumulator;
	uint32_t carry;
	unsigned int left;
	unsigned int right;
	unsigned int limb;

	/* Starts the 512-bit product at zero. */
	for (limb = 0; limb < ED_WIDE_LIMBS; limb++)
		product[limb] = 0U;

	/* The schoolbook product k * s: each step is below 2^64. */
	for (left = 0; left < ED_SCALAR_LIMBS; left++) {
		carry = 0U;
		for (right = 0; right < ED_SCALAR_LIMBS; right++) {
			accumulator = (uint64_t)k[left] * (uint64_t)s[right];
			accumulator += (uint64_t)product[left + right];
			accumulator += (uint64_t)carry;
			product[left + right] = (uint32_t)accumulator;
			carry = (uint32_t)(accumulator >> 32);
		}
		product[left + ED_SCALAR_LIMBS] = carry;
	}

	/* Adds r, carrying through the high limbs. */
	carry = 0U;
	for (limb = 0; limb < ED_WIDE_LIMBS; limb++) {
		accumulator = (uint64_t)product[limb] + (uint64_t)carry;
		if (limb < ED_SCALAR_LIMBS)
			accumulator += (uint64_t)r[limb];
		product[limb] = (uint32_t)accumulator;
		carry = (uint32_t)(accumulator >> 32);
	}

	/* Reduces modulo L. */
	ed_reduce_order(out, product);

	/* The product holds a secret. */
	cryc_wipe(product, sizeof(product));
}

/*
 * Returns 1 when the 8-limb integer is less than L, else 0 (5.1.7 step 1).
 * Handles public values only.
 */
static int
ed_scalar_below_order(
	const uint32_t *scalar)
{
	unsigned int limb;

	/* From the top limb: the first limb that differs decides. */
	for (limb = ED_SCALAR_LIMBS; limb > 0U; limb--) {
		if (scalar[limb - 1U] < ed_order[limb - 1U])
			return 1;
		if (scalar[limb - 1U] > ed_order[limb - 1U])
			return 0;
	}

	/* Equal to L is not less than L. */
	return 0;
}

/*
 * Reads count little-endian 32-bit limbs from bytes.
 */
static void
ed_load_limbs(
	uint32_t *out,
	const uint8_t *in,
	unsigned int count)
{
	unsigned int limb;

	/* Four bytes per limb. */
	for (limb = 0; limb < count; limb++)
		out[limb] = cryc_load_le32(in + limb * 4U);
}

/*
 * Writes count 32-bit limbs as little-endian bytes.
 */
static void
ed_store_limbs(
	uint8_t *out,
	const uint32_t *in,
	unsigned int count)
{
	unsigned int limb;

	/* Four bytes per limb. */
	for (limb = 0; limb < count; limb++)
		cryc_store_le32(out + limb * 4U, in[limb]);
}

/*
 * Hashes the seed with SHA-512 and splits the digest (5.1.5 steps 1 and 2,
 * 5.1.6 step 1): the first half is pruned into the secret scalar (three
 * low bits of the first byte cleared, the top bit of the last byte
 * cleared, the next one set); the second half is the prefix.
 */
static void
ed_expand(
	uint8_t *scalar,
	uint8_t *prefix,
	const uint8_t *seed)
{
	struct cryc_sha512 hash;
	uint8_t digest[64];

	/* SHA-512 of the 32-byte seed. */
	cryc_sha512_init(&hash);
	cryc_sha512_update(&hash, seed, CRYC_ED25519_SEED);
	cryc_sha512_final(&hash, digest);

	/* The two halves. */
	memcpy(scalar, digest, 32U);
	memcpy(prefix, digest + 32, 32U);

	/* Prunes the scalar. */
	scalar[0] = (uint8_t)(scalar[0] & 248U);
	scalar[31] = (uint8_t)(scalar[31] & 127U);
	scalar[31] = (uint8_t)(scalar[31] | 64U);

	/* The digest is secret. */
	cryc_wipe(digest, sizeof(digest));
}

/*
 * Computes SHA-512(first || second || message); second may be NULL with
 * second_size 0, and message may be NULL with message_size 0.
 */
static void
ed_hash(
	uint8_t *out,
	const uint8_t *first,
	size_t first_size,
	const uint8_t *second,
	size_t second_size,
	const uint8_t *message,
	size_t message_size)
{
	struct cryc_sha512 hash;

	/* The three parts in order. */
	cryc_sha512_init(&hash);
	cryc_sha512_update(&hash, first, first_size);
	if (second_size > 0U)
		cryc_sha512_update(&hash, second, second_size);
	if (message_size > 0U)
		cryc_sha512_update(&hash, message, message_size);
	cryc_sha512_final(&hash, out);
}
