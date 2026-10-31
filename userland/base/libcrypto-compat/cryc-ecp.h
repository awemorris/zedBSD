/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The groups of points of the curves P-256 and P-384: their parameters,
 * the encoding of points, and scalar multiplication.  ECDH, key
 * generation and ECDSA verification are built on this in cryc-ecc.h.
 *
 * Representation.  A point is held in Jacobian coordinates (X, Y, Z),
 * standing for the affine point (X / Z^2, Y / Z^3), with the point at
 * infinity being any triple with Z = 0; the coordinates are in Montgomery
 * form modulo p (cryc-bn.h).  The doubling and addition formulas are
 * derived by the implementer from the affine law of A.1.1 (the standard
 * Jacobian formulas, with the a = -3 shortcut in doubling); they are not
 * copied from another implementation.
 *
 * Constant time.  Scalars are secret in cryc_ecp_mul and
 * cryc_ecp_mul_base: the multiplication is a fixed sequence of doublings
 * and additions whatever the scalar (a fixed window of 4 bits, the
 * multiple of the window read from a table of 16 points by scanning every
 * entry and selecting with masks), and the exceptional cases of the
 * addition (an input at infinity, P = Q, P = -Q) are handled by computing
 * the alternatives and selecting with masks, never by branching.  The
 * conversion to affine coordinates uses an inversion by Fermat's little
 * theorem (exponent p - 2, public).  Points decoded from the network are
 * public; cryc_ecp_decode may branch on them.  cryc_ecp_mul_add_public
 * handles only public values and may be variable time.
 *
 * The parameters (p, b, n, G) are data from SP 800-186 3.2.1.3 and
 * 3.2.1.4, cited at their table in the .c file.
 */

#ifndef LIBCRYPTO_COMPAT_CRYC_ECP_H
#define LIBCRYPTO_COMPAT_CRYC_ECP_H

#include "cryc-bn.h"

/*
 * The curves are those of NIST SP 800-186 (February 2023) 3.2.1.3
 * (P-256) and 3.2.1.4 (P-384): y^2 = x^3 - 3x + b over GF(p), prime
 * order n, cofactor 1.  The group law is that of SP 800-186 Appendix
 * A.1.1; the point encodings are those of SEC 1 version 2.0 (May 2009)
 * 2.3.3 and 2.3.4.
 */

/* The curves.  The values are libssl-compat's own, not TLS's group numbers. */
enum cryc_curve_id {
	CRYC_CURVE_NONE = 0,
	CRYC_CURVE_P256 = 1,
	CRYC_CURVE_P384 = 2
};

/* The largest field element and scalar: P-384's 384 bits, in limbs and bytes. */
#define CRYC_ECP_MAX_LIMBS	12U
#define CRYC_ECP_MAX_BYTES	48U

/* The largest uncompressed point encoding: 0x04, x and y (SEC 1 2.3.3). */
#define CRYC_ECP_MAX_POINT	(1U + 2U * CRYC_ECP_MAX_BYTES)

/*
 * A curve prepared for arithmetic.
 *
 * id names it; size is the byte length of a field element and of a
 * scalar (32 or 48: for both curves p and n have the same bit length);
 * limbs is size / 4.  field is p prepared for Montgomery arithmetic,
 * order is n.  b_mont is b in Montgomery form modulo p.  base_x and base_y
 * are G's affine coordinates in Montgomery form modulo p.  Public; made by
 * cryc_ecp_init (a few microseconds: the two moduli are prepared) and not
 * changed after, so it may be shared read-only.  About 4 KiB (two
 * struct cryc_bn_mont); a caller keeps it in a heap object or a
 * long-lived structure rather than on a deep stack.
 */
struct cryc_ecp {
	enum cryc_curve_id id;
	size_t size;
	size_t limbs;
	struct cryc_bn_mont field;
	struct cryc_bn_mont order;
	uint32_t b_mont[CRYC_ECP_MAX_LIMBS];
	uint32_t base_x[CRYC_ECP_MAX_LIMBS];
	uint32_t base_y[CRYC_ECP_MAX_LIMBS];
};

/*
 * A point in Jacobian coordinates, each in Montgomery form modulo p, in
 * the curve's limbs (limbs above them are zero).  Z = 0 is the point at
 * infinity.  A point that came from a secret scalar is secret.
 */
struct cryc_ecp_point {
	uint32_t x[CRYC_ECP_MAX_LIMBS];
	uint32_t y[CRYC_ECP_MAX_LIMBS];
	uint32_t z[CRYC_ECP_MAX_LIMBS];
};

/*
 * Prepares the curve id.
 *
 * Returns 0, or ENOTSUP for CRYC_CURVE_NONE or an unknown id (*ec is then
 * zeroed).
 */
int cryc_ecp_init(struct cryc_ecp *ec, enum cryc_curve_id id);

/*
 * Decodes a point from its uncompressed encoding (SEC 1 2.3.4 with
 * 0x04 || x || y) and validates it (SEC 1 3.2.2.1; NIST SP 800-56A Rev. 3
 * 5.6.2.3.3 steps 1 to 3: coordinates in [0, p - 1], on the curve; step 4,
 * n * Q = infinity, holds for every point on these curves because the
 * cofactor is 1).
 *
 * The checks, in this order: size 0 is EINVAL; a first byte of 0x02 or
 * 0x03 (compressed, whatever the size) is ENOTSUP (TLS 1.3 and RFC 8422
 * use only the uncompressed form); a first byte other than 0x04 (0x00,
 * infinity, included: it is not a public key) is EINVAL; a size other
 * than 1 + 2 * ec->size is EINVAL; a coordinate not less than p is
 * EINVAL; a point not on the curve is EINVAL.  On an error *point is
 * zeroed.  Variable time (public input).  Returns 0, EINVAL or ENOTSUP.
 */
int cryc_ecp_decode(const struct cryc_ecp *ec, struct cryc_ecp_point *point, const uint8_t *in, size_t size);

/*
 * Encodes a point uncompressed (SEC 1 2.3.3) into 1 + 2 * ec->size bytes.
 *
 * size is the room at out.  Returns 0; ENOBUFS when size is too small;
 * EINVAL when the point is at infinity (out is then zeroed).  Constant
 * time in the point (the infinity test is a mask, and the result is
 * chosen after the conversion).
 */
int cryc_ecp_encode(const struct cryc_ecp *ec, const struct cryc_ecp_point *point, uint8_t *out, size_t size);

/*
 * Writes the affine x coordinate of point (not in Montgomery form) as
 * ec->size big-endian bytes into out: the shared secret of ECDH (NIST SP
 * 800-56A Rev. 3 5.7.1.2, Z = x_P).
 *
 * Returns 0, or EINVAL when the point is at infinity (out is zeroed).
 * Constant time.
 */
int cryc_ecp_affine_x(const struct cryc_ecp *ec, const struct cryc_ecp_point *point, uint8_t *out);

/*
 * out = scalar * point.
 *
 * scalar is ec->limbs limbs, any value less than n (0 gives infinity);
 * point is any point of the group, including infinity.  out may be point.
 * Constant time in scalar and point.
 */
void cryc_ecp_mul(const struct cryc_ecp *ec, struct cryc_ecp_point *out, const struct cryc_ecp_point *point, const uint32_t *scalar);

/* out = scalar * G; as cryc_ecp_mul with the base point.  Constant time in scalar. */
void cryc_ecp_mul_base(const struct cryc_ecp *ec, struct cryc_ecp_point *out, const uint32_t *scalar);

/*
 * out = u1 * G + u2 * q, for ECDSA verification (FIPS 186-5 6.4.2 step 6).
 *
 * u1 and u2 are ec->limbs limbs, less than n; q is a validated public
 * point.  The result may be infinity.  All inputs are public; the time may
 * depend on them.
 */
void cryc_ecp_mul_add_public(const struct cryc_ecp *ec, struct cryc_ecp_point *out, const uint32_t *u1, const uint32_t *u2, const struct cryc_ecp_point *q);

#endif
