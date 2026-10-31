/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The TLS constants that are data, and the table of cipher suites.
 */

#include "sslc-tls.h"

/*
 * The Random of a HelloRetryRequest (RFC 8446 4.1.3); its 32 bytes are
 * written with their citation when the module is implemented.
 */
const uint8_t sslc_tls_hrr_random[32] = { 0 };

/* The last eight bytes of a ServerHello.random of a server that fell back to TLS 1.2 (RFC 8446 4.1.3). */
const uint8_t sslc_tls_downgrade12[8] = { 0 };

/* The same for a fallback to TLS 1.1 or below (RFC 8446 4.1.3). */
const uint8_t sslc_tls_downgrade11[8] = { 0 };

/*
 * Returns the suite numbered id when libssl-compat supports it, else NULL.
 */
const struct sslc_suite *
sslc_tls_suite(
	uint16_t id)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(id);

	/* Not implemented: the failure value. */
	return NULL;
}

/*
 * The suites a ClientHello offers, in preference order, for the versions
 * allowed: TLS 1.3 suites first (AES-128-GCM, ChaCha20-Poly1305,
 * AES-256-GCM), then the TLS 1.2 suites (ECDSA before RSA; AES-128-GCM,
 * ChaCha20-Poly1305, AES-256-GCM).
 */
size_t
sslc_tls_offered_suites(
	uint16_t min_version,
	uint16_t max_version,
	uint16_t *out,
	size_t room)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(min_version);
	UNUSED_PARAMETER(max_version);
	UNUSED_PARAMETER(out);
	UNUSED_PARAMETER(room);

	/* Not implemented: the failure value. */
	return 0;
}
