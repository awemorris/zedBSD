/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Signature verification by algorithm.
 */

#include "cryc-sigver.h"

/*
 * Decodes an ECDSA signature in its DER form, Ecdsa-Sig-Value (RFC 5480
 * 2.2 via RFC 3279 2.2.3; the same form TLS uses, RFC 8446 4.2.3 and RFC
 * 8422 5.4): SEQUENCE { r INTEGER, s INTEGER }, nothing after it.
 */
int
cryc_ecdsa_signature_decode(
	const uint8_t *der,
	size_t der_size,
	size_t size,
	uint8_t *r,
	uint8_t *s)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(der);
	UNUSED_PARAMETER(der_size);
	UNUSED_PARAMETER(size);
	UNUSED_PARAMETER(r);
	UNUSED_PARAMETER(s);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}

/*
 * Verifies signature over message with key under params.
 */
int
cryc_signature_verify(
	const struct cryc_pubkey *key,
	const struct cryc_sig_params *params,
	const uint8_t *message,
	size_t message_size,
	const uint8_t *signature,
	size_t signature_size)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(key);
	UNUSED_PARAMETER(params);
	UNUSED_PARAMETER(message);
	UNUSED_PARAMETER(message_size);
	UNUSED_PARAMETER(signature);
	UNUSED_PARAMETER(signature_size);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}
