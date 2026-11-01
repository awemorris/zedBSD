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
 * The Random of a HelloRetryRequest: the SHA-256 value of the string
 * "HelloRetryRequest", 32 bytes.  A ServerHello whose random equals it is a
 * HelloRetryRequest.
 *
 * Values referenced: RFC 8446 section 4.1.3 (the byte string
 * CF 21 AD 74 E5 9A 61 11 BE 1D 8C 02 1E 65 B8 91 C2 A2 11 16 7A BB 8C 5E
 * 07 9E 09 E2 C8 A8 33 9C).
 */
const uint8_t sslc_tls_hrr_random[32] = {
	0xcf, 0x21, 0xad, 0x74, 0xe5, 0x9a, 0x61, 0x11,
	0xbe, 0x1d, 0x8c, 0x02, 0x1e, 0x65, 0xb8, 0x91,
	0xc2, 0xa2, 0x11, 0x16, 0x7a, 0xbb, 0x8c, 0x5e,
	0x07, 0x9e, 0x09, 0xe2, 0xc8, 0xa8, 0x33, 0x9c
};

/*
 * The last eight bytes of a ServerHello.random of a server that fell back
 * to TLS 1.2: "DOWNGRD" and the byte 0x01.
 *
 * Values referenced: RFC 8446 section 4.1.3 (44 4F 57 4E 47 52 44 01).
 */
const uint8_t sslc_tls_downgrade12[8] = {
	0x44, 0x4f, 0x57, 0x4e, 0x47, 0x52, 0x44, 0x01
};

/*
 * The same for a fallback to TLS 1.1 or below: "DOWNGRD" and the byte
 * 0x00.
 *
 * Values referenced: RFC 8446 section 4.1.3 (44 4F 57 4E 47 52 44 00).
 */
const uint8_t sslc_tls_downgrade11[8] = {
	0x44, 0x4f, 0x57, 0x4e, 0x47, 0x52, 0x44, 0x00
};

/*
 * The suites libssl-compat supports.
 *
 * Each row is the number, the protocol version, the AEAD, the hash of the
 * HKDF (TLS 1.3) or of the PRF and the transcript (TLS 1.2), how a 1.2
 * suite signs, the AEAD key length in bytes, the IV length in bytes, and the
 * IANA name.  The TLS 1.3 suites take a 12-byte per-record nonce base; the
 * TLS 1.2 AES-GCM suites take a 4-byte implicit salt; the TLS 1.2
 * ChaCha20-Poly1305 suites take a 12-byte nonce base.  The key lengths are
 * those of the AEAD (AES-128: 16, AES-256: 32, ChaCha20: 32).  The
 * ECDHE_ECDSA suites need an ECDSA server certificate and the ECDHE_RSA
 * suites an RSA one.
 *
 * Values referenced: RFC 8446 Appendix B.4 (TLS_AES_128_GCM_SHA256 {0x13,
 * 0x01}, TLS_AES_256_GCM_SHA384 {0x13,0x02}, TLS_CHACHA20_POLY1305_SHA256
 * {0x13,0x03}) and section 5.3 (the 12-byte nonce), RFC 5289 section 3
 * (ECDSA with AES-128-GCM and SHA-256 {0xC0,0x2B}, with AES-256-GCM and
 * SHA-384 {0xC0,0x2C}; RSA {0xC0,0x2F} and {0xC0,0x30}), RFC 7905 section 2
 * (ECDHE_RSA {0xCC,0xA8}, ECDHE_ECDSA {0xCC,0xA9}, with SHA-256 as the PRF
 * hash), RFC 5288 section 3 (the 4-byte implicit salt of AES-GCM).
 */
static const struct sslc_suite sslc_suites[] = {
	{
		SSLC_SUITE_AES_128_GCM_SHA256, SSLC_TLS13, SSLC_AEAD_AES_128_GCM,
		CRYC_HASH_SHA256, SSLC_AUTH_ANY, 16, 12, "TLS_AES_128_GCM_SHA256"
	},
	{
		SSLC_SUITE_AES_256_GCM_SHA384, SSLC_TLS13, SSLC_AEAD_AES_256_GCM,
		CRYC_HASH_SHA384, SSLC_AUTH_ANY, 32, 12, "TLS_AES_256_GCM_SHA384"
	},
	{
		SSLC_SUITE_CHACHA20_POLY1305_SHA256, SSLC_TLS13, SSLC_AEAD_CHACHA20_POLY1305,
		CRYC_HASH_SHA256, SSLC_AUTH_ANY, 32, 12, "TLS_CHACHA20_POLY1305_SHA256"
	},
	{
		SSLC_SUITE_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256, SSLC_TLS12, SSLC_AEAD_AES_128_GCM,
		CRYC_HASH_SHA256, SSLC_AUTH_ECDSA, 16, 4, "TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256"
	},
	{
		SSLC_SUITE_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384, SSLC_TLS12, SSLC_AEAD_AES_256_GCM,
		CRYC_HASH_SHA384, SSLC_AUTH_ECDSA, 32, 4, "TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384"
	},
	{
		SSLC_SUITE_ECDHE_RSA_WITH_AES_128_GCM_SHA256, SSLC_TLS12, SSLC_AEAD_AES_128_GCM,
		CRYC_HASH_SHA256, SSLC_AUTH_RSA, 16, 4, "TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256"
	},
	{
		SSLC_SUITE_ECDHE_RSA_WITH_AES_256_GCM_SHA384, SSLC_TLS12, SSLC_AEAD_AES_256_GCM,
		CRYC_HASH_SHA384, SSLC_AUTH_RSA, 32, 4, "TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384"
	},
	{
		SSLC_SUITE_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256, SSLC_TLS12, SSLC_AEAD_CHACHA20_POLY1305,
		CRYC_HASH_SHA256, SSLC_AUTH_RSA, 32, 12, "TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256"
	},
	{
		SSLC_SUITE_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256, SSLC_TLS12, SSLC_AEAD_CHACHA20_POLY1305,
		CRYC_HASH_SHA256, SSLC_AUTH_ECDSA, 32, 12, "TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256"
	}
};

/*
 * The suites a ClientHello offers, most preferred first: the TLS 1.3
 * suites (AES-128-GCM, ChaCha20-Poly1305, AES-256-GCM), then the TLS 1.2
 * suites with an ECDSA signature (same AEAD order), then those with an RSA
 * signature.  The renegotiation SCSV is not a suite and is not listed.
 * The list is filtered by the versions the caller allows.
 */
static const uint16_t sslc_suite_preference[] = {
	SSLC_SUITE_AES_128_GCM_SHA256,
	SSLC_SUITE_CHACHA20_POLY1305_SHA256,
	SSLC_SUITE_AES_256_GCM_SHA384,
	SSLC_SUITE_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
	SSLC_SUITE_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256,
	SSLC_SUITE_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,
	SSLC_SUITE_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
	SSLC_SUITE_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
	SSLC_SUITE_ECDHE_RSA_WITH_AES_256_GCM_SHA384
};

/*
 * Returns the suite numbered id when libssl-compat supports it, else NULL.
 */
const struct sslc_suite *
sslc_tls_suite(
	uint16_t id)
{
	size_t index;

	/* Looks for the row of the number. */
	for (index = 0;
	     index < sizeof(sslc_suites) / sizeof(sslc_suites[0]);
	     index++) {
		if (sslc_suites[index].id == id)
			return &sslc_suites[index];
	}

	/* The number is not a supported suite. */
	return NULL;
}

/*
 * Lists the suites a ClientHello offers for the versions allowed, in
 * preference order, up to room numbers.
 *
 * A suite is offered when its version lies between min_version and
 * max_version.  The list stops when room numbers are written; a NULL out
 * writes nothing.
 */
size_t
sslc_tls_offered_suites(
	uint16_t min_version,
	uint16_t max_version,
	uint16_t *out,
	size_t room)
{
	const struct sslc_suite *suite;
	size_t index;
	size_t count;

	/* A caller with nowhere to write gets an empty list. */
	if (out == NULL)
		return 0;

	/* Walks the preference order and keeps the suites of an allowed version. */
	count = 0;
	for (index = 0;
	     index < sizeof(sslc_suite_preference) / sizeof(sslc_suite_preference[0]);
	     index++) {
		/* The list is full. */
		if (count >= room)
			break;

		/* Every preferred number is in the table. */
		suite = sslc_tls_suite(sslc_suite_preference[index]);
		if (suite == NULL)
			continue;

		/* The suite belongs to a version below the range. */
		if (suite->version < min_version)
			continue;

		/* The suite belongs to a version above the range. */
		if (suite->version > max_version)
			continue;

		/* Offers the suite. */
		out[count] = suite->id;
		count++;
	}

	/* Reports how many suites were written. */
	return count;
}
