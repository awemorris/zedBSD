/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The P-256 and P-384 groups (NIST SP 800-186, SEC 1).
 */

#include "cryc-ecp.h"

/*
 * Prepares the curve id.
 */
int
cryc_ecp_init(
	struct cryc_ecp *ec,
	enum cryc_curve_id id)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(ec);
	UNUSED_PARAMETER(id);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
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
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(ec);
	UNUSED_PARAMETER(point);
	UNUSED_PARAMETER(in);
	UNUSED_PARAMETER(size);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
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
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(ec);
	UNUSED_PARAMETER(point);
	UNUSED_PARAMETER(out);
	UNUSED_PARAMETER(size);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
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
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(ec);
	UNUSED_PARAMETER(point);
	UNUSED_PARAMETER(out);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
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
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(ec);
	UNUSED_PARAMETER(out);
	UNUSED_PARAMETER(point);
	UNUSED_PARAMETER(scalar);
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
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(ec);
	UNUSED_PARAMETER(out);
	UNUSED_PARAMETER(scalar);
}

/*
 * out = u1 * G + u2 * q, for ECDSA verification (FIPS 186-5 6.4.2 step 5).
 */
void
cryc_ecp_mul_add_public(
	const struct cryc_ecp *ec,
	struct cryc_ecp_point *out,
	const uint32_t *u1,
	const uint32_t *u2,
	const struct cryc_ecp_point *q)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(ec);
	UNUSED_PARAMETER(out);
	UNUSED_PARAMETER(u1);
	UNUSED_PARAMETER(u2);
	UNUSED_PARAMETER(q);
}
