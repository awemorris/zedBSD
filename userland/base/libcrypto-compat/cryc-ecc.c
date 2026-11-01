/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Key generation, ECDH and ECDSA verification on P-256 and P-384.
 */

#include "cryc-ecc.h"

#include <string.h>

/* Forward declarations. */
static int ecc_curve_usable(const struct cryc_ecp *ec);
static void ecc_order_minus_two(const struct cryc_ecp *ec, uint32_t *out);
static void ecc_add_one(const struct cryc_ecp *ec, uint32_t *out, const uint32_t *value);

/*
 * Makes a key pair by rejection sampling (FIPS 186-5 A.2.2): c is bits(n)
 * random bits; c > n - 2 is rejected; d = c + 1; Q = d * G.
 *
 * Each draw asks the random source for ec->size bytes, which are bits(n)
 * bits for both curves.  Whether a candidate is rejected is decided by a
 * mask comparison and then used to leave the loop: that one bit is public
 * by design, because a rejected candidate is discarded and never becomes
 * part of the key.
 */
int
cryc_ecc_keygen(
	const struct cryc_ecp *ec,
	uint8_t *private_key,
	uint8_t *public_key,
	cryc_random_fn random,
	void *random_context)
{
	struct cryc_ecp_point point;
	uint8_t candidate[CRYC_ECP_MAX_BYTES];
	uint32_t c[CRYC_ECP_MAX_LIMBS];
	uint32_t d[CRYC_ECP_MAX_LIMBS];
	uint32_t limit[CRYC_ECP_MAX_LIMBS];
	uint8_t zeros[1U + 2U * CRYC_ECP_MAX_BYTES];
	uint32_t rejected;
	uint32_t encoded;
	unsigned tries;
	int accepted;
	int error;

	/* Refuses a curve that was never prepared and a missing source of randomness. */
	if (!ecc_curve_usable(ec))
		return EINVAL;
	if (random == NULL)
		return EINVAL;

	/* Starts the working values at zero, so the limbs above the width stay zero. */
	memset(&point, 0, sizeof(point));
	memset(candidate, 0, sizeof(candidate));
	memset(c, 0, sizeof(c));
	memset(d, 0, sizeof(d));
	memset(limit, 0, sizeof(limit));
	memset(zeros, 0, sizeof(zeros));

	/* The largest candidate that is accepted is n - 2: then d is at most n - 1. */
	ecc_order_minus_two(ec, limit);

	/* Draws candidates until one is in range, giving up after CRYC_ECC_KEYGEN_TRIES. */
	accepted = 0;
	for (tries = 0; tries < CRYC_ECC_KEYGEN_TRIES; tries++) {
		/* Asks the random source for bits(n) bits. */
		error = random(random_context, candidate, ec->size);
		if (error != 0) {
			cryc_wipe(candidate, sizeof(candidate));
			memset(private_key, 0, ec->size);
			memset(public_key, 0, 1U + 2U * ec->size);
			return EIO;
		}

		/* Reads the candidate as an integer (it always fits: it is as wide as n). */
		error = cryc_bn_decode(c, ec->limbs, candidate, ec->size);
		if (error != 0) {
			cryc_wipe(candidate, sizeof(candidate));
			memset(private_key, 0, ec->size);
			memset(public_key, 0, 1U + 2U * ec->size);
			return EIO;
		}

		/* Rejects a candidate above n - 2. */
		rejected = cryc_bn_lt(limit, c, ec->limbs);
		if (rejected == 0U) {
			accepted = 1;
			break;
		}
	}

	/* Gives up when every candidate was rejected: the random source is broken. */
	if (!accepted) {
		cryc_wipe(candidate, sizeof(candidate));
		cryc_wipe(c, sizeof(c));
		memset(private_key, 0, ec->size);
		memset(public_key, 0, 1U + 2U * ec->size);
		return EIO;
	}

	/* Forms the private key d = c + 1 and the public point Q = d * G. */
	ecc_add_one(ec, d, c);
	cryc_ecp_mul_base(ec, &point, d);

	/*
	 * Writes the private key and the uncompressed public key.  The
	 * encoding fails only for the point at infinity, which d in [1, n - 1]
	 * cannot give; the failure is still handled by masks, so that nothing
	 * branches on a value derived from d.
	 */
	cryc_bn_encode(d, ec->limbs, private_key, ec->size);
	error = cryc_ecp_encode(ec, &point, public_key, 1U + 2U * ec->size);
	encoded = cryc_ct_mask_zero32((uint32_t)error);
	cryc_ct_select_bytes(private_key, private_key, zeros, ec->size, encoded);
	cryc_ct_select_bytes(public_key, public_key, zeros, 1U + 2U * ec->size, encoded);

	/* Wipes everything derived from the secret. */
	cryc_wipe(candidate, sizeof(candidate));
	cryc_wipe(c, sizeof(c));
	cryc_wipe(d, sizeof(d));
	cryc_wipe(&point, sizeof(point));

	/* Succeeded, or EIO when the encoding failed (the outputs are then zero). */
	error = (int)((uint32_t)EIO & ~encoded);
	return error;
}

/*
 * Computes the ECDH shared secret (NIST SP 800-56A Rev. 3 5.7.1.2 with
 * cofactor 1): the x coordinate of d * Q.
 *
 * The peer's point is public and is decoded first, so its errors come
 * before a private key error.  Nothing branches on the private key: its
 * range check, and the check that the product is not infinity, are masks
 * combined into the error value and into the wiping of the output.
 */
int
cryc_ecc_ecdh(
	const struct cryc_ecp *ec,
	const uint8_t *private_key,
	const uint8_t *peer,
	size_t peer_size,
	uint8_t *shared)
{
	struct cryc_ecp_point peer_point;
	struct cryc_ecp_point product;
	uint8_t zeros[CRYC_ECP_MAX_BYTES];
	uint32_t d[CRYC_ECP_MAX_LIMBS];
	uint32_t zero;
	uint32_t below_order;
	uint32_t valid;
	uint32_t total;
	uint32_t ok;
	int product_error;
	int error;

	/* Refuses a curve that was never prepared. */
	if (!ecc_curve_usable(ec))
		return EINVAL;

	/* Starts the working values at zero, so the limbs above the width stay zero. */
	memset(&peer_point, 0, sizeof(peer_point));
	memset(&product, 0, sizeof(product));
	memset(zeros, 0, sizeof(zeros));
	memset(d, 0, sizeof(d));
	memset(shared, 0, ec->size);

	/* Decodes and validates the peer's public point; its errors are the caller's. */
	error = cryc_ecp_decode(ec, &peer_point, peer, peer_size);
	if (error != 0)
		return error;

	/* Reads the private key as an integer (it always fits: it is as wide as n). */
	error = cryc_bn_decode(d, ec->limbs, private_key, ec->size);
	if (error != 0)
		return EINVAL;

	/* Finds out by masks whether 1 <= d <= n - 1. */
	zero = cryc_bn_is_zero(d, ec->limbs);
	below_order = cryc_bn_lt(d, ec->order.modulus, ec->limbs);
	valid = below_order & ~zero;

	/* Forms d * Q, the point whose x coordinate is the shared secret, and takes its x coordinate. */
	cryc_ecp_mul(ec, &product, &peer_point, d);
	product_error = cryc_ecp_affine_x(ec, &product, shared);

	/* Combines both failures (an out-of-range key, a product at infinity) into one error value. */
	total = (uint32_t)product_error | ((uint32_t)EINVAL & ~valid);

	/* Wipes the output when anything failed. */
	ok = cryc_ct_mask_zero32(total);
	cryc_ct_select_bytes(shared, shared, zeros, ec->size, ok);

	/* Wipes the working values derived from the private key. */
	cryc_wipe(d, sizeof(d));
	cryc_wipe(&product, sizeof(product));

	/* Succeeded when the total is 0; otherwise EINVAL. */
	error = (int)total;
	return error;
}

/*
 * Verifies an ECDSA signature (FIPS 186-5 6.4.2) over a message whose hash
 * is given.
 *
 * The public key is decoded first, so that a key the curve refuses is
 * reported as such rather than as a bad signature.  The hash is cut to
 * its leftmost bits(n) bits (B.2.1): for both curves that is ec->size
 * bytes, and a shorter hash is read as the shorter integer it is.  With
 * w = s^-1 mod n, the point R = (w E) G + (w r) Q is formed and its x
 * coordinate, reduced modulo n, is compared with r.
 */
int
cryc_ecdsa_verify(
	const struct cryc_ecp *ec,
	const uint8_t *public_key,
	size_t public_key_size,
	const uint8_t *hash,
	size_t hash_size,
	const uint8_t *r,
	const uint8_t *s)
{
	struct cryc_ecp_point q;
	struct cryc_ecp_point sum;
	uint8_t exponent[CRYC_ECP_MAX_BYTES];
	uint8_t x_bytes[CRYC_ECP_MAX_BYTES];
	uint32_t limit[CRYC_ECP_MAX_LIMBS];
	uint32_t r_value[CRYC_ECP_MAX_LIMBS];
	uint32_t s_value[CRYC_ECP_MAX_LIMBS];
	uint32_t e[CRYC_ECP_MAX_LIMBS];
	uint32_t s_mont[CRYC_ECP_MAX_LIMBS];
	uint32_t w_mont[CRYC_ECP_MAX_LIMBS];
	uint32_t u1[CRYC_ECP_MAX_LIMBS];
	uint32_t u2[CRYC_ECP_MAX_LIMBS];
	uint32_t v[CRYC_ECP_MAX_LIMBS];
	size_t hash_used;
	uint32_t below_order;
	uint32_t zero;
	int error;

	/* Refuses a curve that was never prepared and a hash of no bytes. */
	if (!ecc_curve_usable(ec))
		return EINVAL;
	if (hash_size == 0U)
		return EINVAL;

	/* Starts the working values at zero, so the limbs above the width stay zero. */
	memset(&q, 0, sizeof(q));
	memset(&sum, 0, sizeof(sum));
	memset(limit, 0, sizeof(limit));
	memset(r_value, 0, sizeof(r_value));
	memset(s_value, 0, sizeof(s_value));
	memset(e, 0, sizeof(e));
	memset(s_mont, 0, sizeof(s_mont));
	memset(w_mont, 0, sizeof(w_mont));
	memset(u1, 0, sizeof(u1));
	memset(u2, 0, sizeof(u2));
	memset(v, 0, sizeof(v));

	/* Decodes and validates the public key; its errors are the caller's. */
	error = cryc_ecp_decode(ec, &q, public_key, public_key_size);
	if (error != 0)
		return error;

	/* Reads r and s (they always fit: they are as wide as n). */
	error = cryc_bn_decode(r_value, ec->limbs, r, ec->size);
	if (error != 0)
		return EBADMSG;
	error = cryc_bn_decode(s_value, ec->limbs, s, ec->size);
	if (error != 0)
		return EBADMSG;

	/* Step 1: requires 1 <= r <= n - 1. */
	zero = cryc_bn_is_zero(r_value, ec->limbs);
	below_order = cryc_bn_lt(r_value, ec->order.modulus, ec->limbs);
	if (zero != 0U || below_order == 0U)
		return EBADMSG;

	/* Step 1: requires 1 <= s <= n - 1. */
	zero = cryc_bn_is_zero(s_value, ec->limbs);
	below_order = cryc_bn_lt(s_value, ec->order.modulus, ec->limbs);
	if (zero != 0U || below_order == 0U)
		return EBADMSG;

	/* Step 3: takes the leftmost bits(n) bits of the hash as the integer E and reduces it modulo n. */
	hash_used = hash_size;
	if (hash_used > ec->size)
		hash_used = ec->size;
	error = cryc_bn_decode(e, ec->limbs, hash, hash_used);
	if (error != 0)
		return EINVAL;
	cryc_bn_reduce_once(e, e, &ec->order);

	/* Step 4: forms w = s^-1 mod n by Fermat's little theorem (the exponent n - 2 is public). */
	ecc_order_minus_two(ec, limit);
	cryc_bn_encode(limit, ec->limbs, exponent, ec->size);
	cryc_bn_to_mont(s_mont, s_value, &ec->order);
	cryc_bn_mont_exp(w_mont, s_mont, exponent, ec->size, &ec->order);

	/* Step 5: forms u1 = E w and u2 = r w (a product with a Montgomery form is a plain product). */
	cryc_bn_mont_mul(u1, e, w_mont, &ec->order);
	cryc_bn_mont_mul(u2, r_value, w_mont, &ec->order);

	/* Step 6: forms R = u1 G + u2 Q. */
	cryc_ecp_mul_add_public(ec, &sum, u1, u2, &q);

	/* Step 6: rejects R at infinity (the affine x of infinity is an error). */
	error = cryc_ecp_affine_x(ec, &sum, x_bytes);
	if (error != 0)
		return EBADMSG;

	/* Step 7: reduces the x coordinate of R modulo n (it is below p, which is below 2n). */
	error = cryc_bn_decode(v, ec->limbs, x_bytes, ec->size);
	if (error != 0)
		return EBADMSG;
	cryc_bn_reduce_once(v, v, &ec->order);

	/* Step 8: accepts when the reduced x coordinate equals r. */
	if (memcmp(v, r_value, ec->limbs * sizeof(v[0])) != 0)
		return EBADMSG;

	/* Succeeded: the signature verifies. */
	return 0;
}

/*
 * Reports whether ec is a prepared curve whose width the arrays here can
 * hold (1) or not (0).
 */
static int
ecc_curve_usable(
	const struct cryc_ecp *ec)
{
	/* A curve that was never prepared, or whose width the arrays cannot hold, is unusable. */
	if (ec == NULL)
		return 0;
	if (ec->size == 0U)
		return 0;
	if (ec->size > CRYC_ECP_MAX_BYTES)
		return 0;
	if (ec->limbs != ec->size / 4U)
		return 0;

	/* Usable. */
	return 1;
}

/*
 * Sets out to n - 2, the largest key-generation candidate (ec->limbs limbs;
 * the limbs above stay as the caller left them).
 */
static void
ecc_order_minus_two(
	const struct cryc_ecp *ec,
	uint32_t *out)
{
	size_t index;
	uint64_t borrow;
	uint64_t difference;
	uint64_t amount;

	/* Subtracts 2 from the lowest limb, then carries the borrow upward. */
	borrow = 0;
	amount = 2U;
	for (index = 0; index < ec->limbs; index++) {
		difference = (uint64_t)ec->order.modulus[index] - amount - borrow;
		out[index] = (uint32_t)(difference & 0xffffffffU);
		borrow = (difference >> 32) & 1U;
		amount = 0;
	}
}

/*
 * Sets out to value + 1 (ec->limbs limbs; the carry out of the top limb is
 * dropped, which only happens for a candidate that is then discarded).
 */
static void
ecc_add_one(
	const struct cryc_ecp *ec,
	uint32_t *out,
	const uint32_t *value)
{
	size_t index;
	uint64_t carry;
	uint64_t sum;

	/* Adds 1 to the lowest limb, then carries upward. */
	carry = 1U;
	for (index = 0; index < ec->limbs; index++) {
		sum = (uint64_t)value[index] + carry;
		out[index] = (uint32_t)(sum & 0xffffffffU);
		carry = sum >> 32;
	}
}
