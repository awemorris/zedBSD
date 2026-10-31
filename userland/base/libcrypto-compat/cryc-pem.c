/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Base64 (RFC 4648) and PEM certificate blocks (RFC 7468).
 */

#include "cryc-pem.h"

/*
 * Decodes base64 (RFC 4648 section 4, the standard alphabet of Table 1)
 * from size characters at text into out.
 */
int
cryc_base64_decode(
	const char *text,
	size_t size,
	uint8_t *out,
	size_t room,
	size_t *written)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(text);
	UNUSED_PARAMETER(size);
	UNUSED_PARAMETER(out);
	UNUSED_PARAMETER(room);
	UNUSED_PARAMETER(written);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}

/*
 * Finds the next CERTIFICATE block in a PEM text, starting at *position,
 * and decodes it.
 */
int
cryc_pem_next_certificate(
	const char *text,
	size_t size,
	size_t *position,
	uint8_t **der,
	size_t *der_size)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(text);
	UNUSED_PARAMETER(size);
	UNUSED_PARAMETER(position);
	UNUSED_PARAMETER(der);
	UNUSED_PARAMETER(der_size);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}
