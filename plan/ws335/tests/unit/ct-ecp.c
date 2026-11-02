/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The constant-time checks of the P-256 and P-384 groups: the scalar of a
 * multiplication is secret, and so is the point it multiplies (and every
 * point derived from them), so memcheck reports any branch or address that
 * depends on them.  The result of each call (the point, the encoding, the
 * x coordinate, the error value) is made public only after the call, as a
 * caller does when it sends or compares it.
 */

#include "check.h"
#include "ct.h"

#include "userland/base/libcrypto-compat/cryc-ecp.h"

/* Runs the operations on one curve. */
static unsigned run(enum cryc_curve_id id);

/* Runs the operations on one curve. */
static unsigned
run(
	enum cryc_curve_id id)
{
	struct cryc_ecp ec;
	struct cryc_ecp_point base;
	struct cryc_ecp_point secret_point;
	struct cryc_ecp_point result;
	uint8_t scalar_bytes[CRYC_ECP_MAX_BYTES];
	uint8_t encoded[CRYC_ECP_MAX_POINT];
	uint8_t x_only[CRYC_ECP_MAX_BYTES];
	uint32_t scalar[CRYC_ECP_MAX_LIMBS];
	unsigned total;
	size_t index;
	int error;

	/* The curve is public. */
	total = 0;
	error = cryc_ecp_init(&ec, id);
	CHECK(error == 0);

	/* A scalar below n (the top byte is small): secret. */
	for (index = 0; index < ec.size; index++)
		scalar_bytes[index] = (uint8_t)(0x6bU + index * 29U);
	scalar_bytes[0] = 0x3cU;
	memset(scalar, 0, sizeof(scalar));
	error = cryc_bn_decode(scalar, ec.limbs, scalar_bytes, ec.size);
	CHECK(error == 0);

	/* The multiple of the base point: the scalar is secret, and so is the result. */
	CT_SECRET(scalar, ec.limbs * sizeof(scalar[0]));
	cryc_ecp_mul_base(&ec, &base, scalar);
	CT_PUBLIC(scalar, ec.limbs * sizeof(scalar[0]));

	/* A multiplication of a secret point by a secret scalar. */
	secret_point = base;
	scalar[0] ^= 0x5a5aU;
	CT_SECRET(scalar, ec.limbs * sizeof(scalar[0]));
	CT_SECRET(&secret_point, sizeof(secret_point));
	cryc_ecp_mul(&ec, &result, &secret_point, scalar);

	/* The encoding and the x coordinate of a secret point: the bytes and the verdict are public afterwards. */
	error = cryc_ecp_encode(&ec, &result, encoded, sizeof(encoded));
	CT_PUBLIC(&error, sizeof(error));
	CT_PUBLIC(encoded, 1U + 2U * ec.size);
	CHECK(error == 0);
	total += encoded[1] & 1U;
	error = cryc_ecp_affine_x(&ec, &result, x_only);
	CT_PUBLIC(&error, sizeof(error));
	CT_PUBLIC(x_only, ec.size);
	CHECK(error == 0);
	total += x_only[0] & 1U;

	/* The same on a result of the base multiplication. */
	CT_SECRET(&base, sizeof(base));
	error = cryc_ecp_encode(&ec, &base, encoded, sizeof(encoded));
	CT_PUBLIC(&error, sizeof(error));
	CT_PUBLIC(encoded, 1U + 2U * ec.size);
	CHECK(error == 0);
	total += encoded[2] & 1U;

	/* The point at infinity as the result of a secret multiplication (scalar 0): the verdict is public afterwards. */
	memset(scalar, 0, sizeof(scalar));
	CT_SECRET(scalar, ec.limbs * sizeof(scalar[0]));
	cryc_ecp_mul_base(&ec, &result, scalar);
	error = cryc_ecp_encode(&ec, &result, encoded, sizeof(encoded));
	CT_PUBLIC(&error, sizeof(error));
	CT_PUBLIC(encoded, 1U + 2U * ec.size);
	CHECK(error == EINVAL);
	total += encoded[0] & 1U;
	CT_PUBLIC(scalar, ec.limbs * sizeof(scalar[0]));
	CT_PUBLIC(&result, sizeof(result));

	/* Everything public again. */
	CT_PUBLIC(&base, sizeof(base));
	CT_PUBLIC(&secret_point, sizeof(secret_point));

	/* Reports the folded outputs of this curve, so no call is dropped. */
	return total;
}

/* Runs the checks. */
int
main(void)
{
	unsigned total;

	/* Both curves. */
	total = run(CRYC_CURVE_P256);
	total += run(CRYC_CURVE_P384);

	/* The folded value is used so that no call can be dropped. */
	CHECK(total < 1000U);

	/* The result. */
	return check_finish("ct-ecp");
}
