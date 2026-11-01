/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Checks the TLS constants and the table of cipher suites (sslc-tls.h).
 * The expected numbers are written out here from the tables of RFC 8446
 * (Appendix B and sections 4.1.3, 4.2, 5.1, 5.2, 5.3), RFC 5246 (the
 * record limits, the alert numbers), RFC 5288 (the AES-GCM salt), RFC
 * 5289 section 3, RFC 7905 section 2, RFC 5746 section 3.3, RFC 7627
 * section 5.1 and RFC 8422 (the extension number), each pair of bytes as
 * the RFC prints it.  The HelloRetryRequest random is also checked as the
 * SHA-256 of the string "HelloRetryRequest" (RFC 8446 4.1.3) with
 * libcrypto-compat's hash, and the suite order against the ClientHello
 * of RFC 8448.
 */

#include "check.h"

#include "userland/base/libssl-compat/sslc-tls.h"

/* Joins the two bytes of an IANA number as the RFCs print them, {0xAA,0xBB}. */
static uint32_t
pair(
	unsigned high,
	unsigned low)
{
	/* The first byte is the high byte (network order). */
	return (uint32_t)(high * 256U + low);
}

/* Checks that one macro equals the number the RFC gives. */
#define CHECK_VALUE(macro, expected) CHECK((uint32_t)(macro) == (uint32_t)(expected))

/* Checks the record, handshake and alert numbers. */
static void
test_numbers(void)
{
	/* The versions (RFC 8446 B.3.1.1: TLS 1.2 is 0x0303 and TLS 1.3 is 0x0304). */
	CHECK_VALUE(SSLC_TLS12, pair(3, 3));
	CHECK_VALUE(SSLC_TLS13, pair(3, 4));

	/* The content types (RFC 8446 B.1). */
	CHECK_VALUE(SSLC_CT_CHANGE_CIPHER_SPEC, 20);
	CHECK_VALUE(SSLC_CT_ALERT, 21);
	CHECK_VALUE(SSLC_CT_HANDSHAKE, 22);
	CHECK_VALUE(SSLC_CT_APPLICATION_DATA, 23);

	/* The handshake types (RFC 8446 B.3 and RFC 5246 A.4). */
	CHECK_VALUE(SSLC_HS_HELLO_REQUEST, 0);
	CHECK_VALUE(SSLC_HS_CLIENT_HELLO, 1);
	CHECK_VALUE(SSLC_HS_SERVER_HELLO, 2);
	CHECK_VALUE(SSLC_HS_NEW_SESSION_TICKET, 4);
	CHECK_VALUE(SSLC_HS_END_OF_EARLY_DATA, 5);
	CHECK_VALUE(SSLC_HS_ENCRYPTED_EXTENSIONS, 8);
	CHECK_VALUE(SSLC_HS_CERTIFICATE, 11);
	CHECK_VALUE(SSLC_HS_SERVER_KEY_EXCHANGE, 12);
	CHECK_VALUE(SSLC_HS_CERTIFICATE_REQUEST, 13);
	CHECK_VALUE(SSLC_HS_SERVER_HELLO_DONE, 14);
	CHECK_VALUE(SSLC_HS_CERTIFICATE_VERIFY, 15);
	CHECK_VALUE(SSLC_HS_CLIENT_KEY_EXCHANGE, 16);
	CHECK_VALUE(SSLC_HS_FINISHED, 20);
	CHECK_VALUE(SSLC_HS_KEY_UPDATE, 24);
	CHECK_VALUE(SSLC_HS_MESSAGE_HASH, 254);

	/* The alert levels (RFC 8446 6) and descriptions (RFC 8446 B.2; no_renegotiation is RFC 5246 7.2). */
	CHECK_VALUE(SSLC_ALERT_WARNING, 1);
	CHECK_VALUE(SSLC_ALERT_FATAL, 2);
	CHECK_VALUE(SSLC_ALERT_CLOSE_NOTIFY, 0);
	CHECK_VALUE(SSLC_ALERT_UNEXPECTED_MESSAGE, 10);
	CHECK_VALUE(SSLC_ALERT_BAD_RECORD_MAC, 20);
	CHECK_VALUE(SSLC_ALERT_RECORD_OVERFLOW, 22);
	CHECK_VALUE(SSLC_ALERT_HANDSHAKE_FAILURE, 40);
	CHECK_VALUE(SSLC_ALERT_BAD_CERTIFICATE, 42);
	CHECK_VALUE(SSLC_ALERT_UNSUPPORTED_CERTIFICATE, 43);
	CHECK_VALUE(SSLC_ALERT_CERTIFICATE_REVOKED, 44);
	CHECK_VALUE(SSLC_ALERT_CERTIFICATE_EXPIRED, 45);
	CHECK_VALUE(SSLC_ALERT_CERTIFICATE_UNKNOWN, 46);
	CHECK_VALUE(SSLC_ALERT_ILLEGAL_PARAMETER, 47);
	CHECK_VALUE(SSLC_ALERT_UNKNOWN_CA, 48);
	CHECK_VALUE(SSLC_ALERT_ACCESS_DENIED, 49);
	CHECK_VALUE(SSLC_ALERT_DECODE_ERROR, 50);
	CHECK_VALUE(SSLC_ALERT_DECRYPT_ERROR, 51);
	CHECK_VALUE(SSLC_ALERT_PROTOCOL_VERSION, 70);
	CHECK_VALUE(SSLC_ALERT_INSUFFICIENT_SECURITY, 71);
	CHECK_VALUE(SSLC_ALERT_INTERNAL_ERROR, 80);
	CHECK_VALUE(SSLC_ALERT_INAPPROPRIATE_FALLBACK, 86);
	CHECK_VALUE(SSLC_ALERT_USER_CANCELED, 90);
	CHECK_VALUE(SSLC_ALERT_NO_RENEGOTIATION, 100);
	CHECK_VALUE(SSLC_ALERT_MISSING_EXTENSION, 109);
	CHECK_VALUE(SSLC_ALERT_UNSUPPORTED_EXTENSION, 110);
	CHECK_VALUE(SSLC_ALERT_UNRECOGNIZED_NAME, 112);
	CHECK_VALUE(SSLC_ALERT_BAD_CERTIFICATE_STATUS_RESPONSE, 113);
	CHECK_VALUE(SSLC_ALERT_UNKNOWN_PSK_IDENTITY, 115);
	CHECK_VALUE(SSLC_ALERT_CERTIFICATE_REQUIRED, 116);
	CHECK_VALUE(SSLC_ALERT_NO_APPLICATION_PROTOCOL, 120);
}

/* Checks the group, scheme, extension and signalling numbers. */
static void
test_registries(void)
{
	/* The named groups (RFC 8446 4.2.7). */
	CHECK_VALUE(SSLC_GROUP_SECP256R1, pair(0x00, 0x17));
	CHECK_VALUE(SSLC_GROUP_SECP384R1, pair(0x00, 0x18));
	CHECK_VALUE(SSLC_GROUP_X25519, pair(0x00, 0x1d));

	/* The signature schemes (RFC 8446 4.2.3). */
	CHECK_VALUE(SSLC_SCHEME_RSA_PKCS1_SHA256, pair(0x04, 0x01));
	CHECK_VALUE(SSLC_SCHEME_RSA_PKCS1_SHA384, pair(0x05, 0x01));
	CHECK_VALUE(SSLC_SCHEME_RSA_PKCS1_SHA512, pair(0x06, 0x01));
	CHECK_VALUE(SSLC_SCHEME_ECDSA_SECP256R1_SHA256, pair(0x04, 0x03));
	CHECK_VALUE(SSLC_SCHEME_ECDSA_SECP384R1_SHA384, pair(0x05, 0x03));
	CHECK_VALUE(SSLC_SCHEME_RSA_PSS_RSAE_SHA256, pair(0x08, 0x04));
	CHECK_VALUE(SSLC_SCHEME_RSA_PSS_RSAE_SHA384, pair(0x08, 0x05));
	CHECK_VALUE(SSLC_SCHEME_RSA_PSS_RSAE_SHA512, pair(0x08, 0x06));

	/* The extension types (RFC 8446 4.2; RFC 8422 5.1.2; RFC 7627 5.1; RFC 5746 3.2). */
	CHECK_VALUE(SSLC_EXT_SERVER_NAME, 0);
	CHECK_VALUE(SSLC_EXT_SUPPORTED_GROUPS, 10);
	CHECK_VALUE(SSLC_EXT_EC_POINT_FORMATS, 11);
	CHECK_VALUE(SSLC_EXT_SIGNATURE_ALGORITHMS, 13);
	CHECK_VALUE(SSLC_EXT_ALPN, 16);
	CHECK_VALUE(SSLC_EXT_EXTENDED_MASTER_SECRET, pair(0x00, 0x17));
	CHECK_VALUE(SSLC_EXT_SESSION_TICKET, 35);
	CHECK_VALUE(SSLC_EXT_PRE_SHARED_KEY, 41);
	CHECK_VALUE(SSLC_EXT_EARLY_DATA, 42);
	CHECK_VALUE(SSLC_EXT_SUPPORTED_VERSIONS, 43);
	CHECK_VALUE(SSLC_EXT_COOKIE, 44);
	CHECK_VALUE(SSLC_EXT_PSK_KEY_EXCHANGE_MODES, 45);
	CHECK_VALUE(SSLC_EXT_KEY_SHARE, 51);
	CHECK_VALUE(SSLC_EXT_RENEGOTIATION_INFO, pair(0xff, 0x01));

	/* The renegotiation SCSV {0x00,0xFF} (RFC 5746 3.3). */
	CHECK_VALUE(SSLC_SUITE_EMPTY_RENEGOTIATION_INFO_SCSV, pair(0x00, 0xff));

	/* The record limits: 2^14, 2^14 + 256 (RFC 8446 5.2), 2^14 + 2048 (RFC 5246 6.2.3), the 5-byte header (5.1). */
	CHECK_VALUE(SSLC_RECORD_HEADER, 5);
	CHECK_VALUE(SSLC_RECORD_PLAINTEXT_MAX, 16384);
	CHECK_VALUE(SSLC_RECORD13_CIPHERTEXT_MAX, 16640);
	CHECK_VALUE(SSLC_RECORD12_CIPHERTEXT_MAX, 18432);
	CHECK_VALUE(SSLC_RECORD_BUFFER, 5 + 18432);
	CHECK_VALUE(SSLC_HANDSHAKE_MAX, 102404);

	/* The AEAD and authentication enumerations start at "none" and are distinct. */
	CHECK(SSLC_AEAD_NONE == 0);
	CHECK(SSLC_AEAD_AES_128_GCM != SSLC_AEAD_AES_256_GCM);
	CHECK(SSLC_AEAD_AES_128_GCM != SSLC_AEAD_CHACHA20_POLY1305);
	CHECK(SSLC_AEAD_AES_256_GCM != SSLC_AEAD_CHACHA20_POLY1305);
	CHECK(SSLC_AUTH_ANY == 0);
	CHECK(SSLC_AUTH_ECDSA != SSLC_AUTH_RSA);
}

/* Checks the three special byte strings. */
static void
test_special_bytes(void)
{
	/* The HelloRetryRequest random as RFC 8446 4.1.3 prints it. */
	static const uint8_t retry_random[32] = {
		0xcf, 0x21, 0xad, 0x74, 0xe5, 0x9a, 0x61, 0x11, 0xbe, 0x1d, 0x8c, 0x02, 0x1e, 0x65, 0xb8, 0x91,
		0xc2, 0xa2, 0x11, 0x16, 0x7a, 0xbb, 0x8c, 0x5e, 0x07, 0x9e, 0x09, 0xe2, 0xc8, 0xa8, 0x33, 0x9c
	};
	uint8_t digest[32];
	char text[9];
	size_t index;
	int status;

	/* The bytes of the table. */
	CHECK_BYTES(sslc_tls_hrr_random, retry_random, 32);

	/* The same bytes as the SHA-256 of "HelloRetryRequest" (RFC 8446 4.1.3), by the hash of libcrypto-compat. */
	status = cryc_hash_digest(CRYC_HASH_SHA256, (const uint8_t *)"HelloRetryRequest", 17, digest);
	CHECK(status == 0);
	CHECK_BYTES(digest, sslc_tls_hrr_random, 32);

	/* The downgrade marks: 44 4F 57 4E 47 52 44 01 and ... 00, which is "DOWNGRD" and a byte. */
	CHECK_BYTES(sslc_tls_downgrade12, "\x44\x4f\x57\x4e\x47\x52\x44\x01", 8);
	CHECK_BYTES(sslc_tls_downgrade11, "\x44\x4f\x57\x4e\x47\x52\x44\x00", 8);
	CHECK_BYTES(sslc_tls_downgrade12, "DOWNGRD", 7);
	CHECK_BYTES(sslc_tls_downgrade11, "DOWNGRD", 7);
	CHECK(sslc_tls_downgrade12[7] == 1U);
	CHECK(sslc_tls_downgrade11[7] == 0U);

	/* The three strings differ from each other and from zeros. */
	CHECK(memcmp(sslc_tls_downgrade12, sslc_tls_downgrade11, 8) != 0);
	for (index = 0; index < 8U; index++)
		text[index] = (char)sslc_tls_downgrade12[index];
	text[8] = '\0';
	CHECK(strncmp(text, "DOWNGRD", 7) == 0);
}

/* The expected row of the suite table, written from the RFCs. */
struct expected_suite {
	uint32_t id;
	uint32_t version;
	int aead;
	int hash;
	int auth;
	unsigned key_size;
	unsigned fixed_iv_size;
	const char *name;
};

/* Checks the suite table row by row, and that no other number is a suite. */
static void
test_suite_table(void)
{
	/*
	 * Key sizes: AES-128 16 bytes, AES-256 32, ChaCha20 32.  IV sizes:
	 * the 1.3 nonce base (RFC 8446 5.3) and the 1.2 ChaCha20 nonce
	 * (RFC 7905 2) are 12 bytes, the 1.2 AES-GCM salt (RFC 5288 3) is
	 * 4.  Hashes: SHA-256 for AES-128 and ChaCha20 suites, SHA-384 for
	 * AES-256 (RFC 8446 B.4; RFC 5289 3; RFC 7905 2).
	 */
	static const struct expected_suite expected[] = {
		{ 0x1301U, 0x0304U, SSLC_AEAD_AES_128_GCM, CRYC_HASH_SHA256, SSLC_AUTH_ANY, 16U, 12U, "TLS_AES_128_GCM_SHA256" },
		{ 0x1302U, 0x0304U, SSLC_AEAD_AES_256_GCM, CRYC_HASH_SHA384, SSLC_AUTH_ANY, 32U, 12U, "TLS_AES_256_GCM_SHA384" },
		{ 0x1303U, 0x0304U, SSLC_AEAD_CHACHA20_POLY1305, CRYC_HASH_SHA256, SSLC_AUTH_ANY, 32U, 12U, "TLS_CHACHA20_POLY1305_SHA256" },
		{ 0xc02bU, 0x0303U, SSLC_AEAD_AES_128_GCM, CRYC_HASH_SHA256, SSLC_AUTH_ECDSA, 16U, 4U, "TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256" },
		{ 0xc02cU, 0x0303U, SSLC_AEAD_AES_256_GCM, CRYC_HASH_SHA384, SSLC_AUTH_ECDSA, 32U, 4U, "TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384" },
		{ 0xc02fU, 0x0303U, SSLC_AEAD_AES_128_GCM, CRYC_HASH_SHA256, SSLC_AUTH_RSA, 16U, 4U, "TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256" },
		{ 0xc030U, 0x0303U, SSLC_AEAD_AES_256_GCM, CRYC_HASH_SHA384, SSLC_AUTH_RSA, 32U, 4U, "TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384" },
		{ 0xcca8U, 0x0303U, SSLC_AEAD_CHACHA20_POLY1305, CRYC_HASH_SHA256, SSLC_AUTH_RSA, 32U, 12U, "TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256" },
		{ 0xcca9U, 0x0303U, SSLC_AEAD_CHACHA20_POLY1305, CRYC_HASH_SHA256, SSLC_AUTH_ECDSA, 32U, 12U, "TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256" }
	};
	const struct sslc_suite *suite;
	const struct sslc_suite *again;
	size_t index;
	size_t other;
	unsigned long supported;
	unsigned long number;

	/* Each supported suite and every one of its fields. */
	for (index = 0; index < sizeof(expected) / sizeof(expected[0]); index++) {
		suite = sslc_tls_suite((uint16_t)expected[index].id);
		CHECK(suite != NULL);
		if (suite == NULL)
			continue;
		CHECK(suite->id == expected[index].id);
		CHECK((uint32_t)suite->version == expected[index].version);
		CHECK((int)suite->aead == expected[index].aead);
		CHECK((int)suite->hash == expected[index].hash);
		CHECK((int)suite->auth == expected[index].auth);
		CHECK(suite->key_size == expected[index].key_size);
		CHECK(suite->fixed_iv_size == expected[index].fixed_iv_size);
		CHECK(suite->name != NULL);
		if (suite->name != NULL)
			CHECK(strcmp(suite->name, expected[index].name) == 0);

		/* A second lookup gives the same row (the table is constant). */
		again = sslc_tls_suite((uint16_t)expected[index].id);
		CHECK(again == suite);

		/* A TLS 1.3 suite does not say how it signs; a 1.2 suite does. */
		if (suite->version == SSLC_TLS13)
			CHECK(suite->auth == SSLC_AUTH_ANY);
		else
			CHECK(suite->auth != SSLC_AUTH_ANY);

		/* The names of different suites differ. */
		for (other = 0; other < index; other++)
			CHECK(strcmp(expected[other].name, expected[index].name) != 0);
	}

	/* Every 16-bit number: exactly the nine above are suites, and each returns a row with its own number. */
	supported = 0;
	for (number = 0; number <= 0xffffUL; number++) {
		suite = sslc_tls_suite((uint16_t)number);
		if (suite == NULL)
			continue;
		supported++;
		CHECK(suite->id == number);
	}
	CHECK(supported == 9UL);

	/* Numbers a reader might expect: the SCSV, the CCM suites, CBC suites, RSA key transport, DHE, PSK and static ECDH are not supported. */
	CHECK(sslc_tls_suite(0x0000U) == NULL);
	CHECK(sslc_tls_suite(0x00ffU) == NULL);
	CHECK(sslc_tls_suite(0x1304U) == NULL);
	CHECK(sslc_tls_suite(0x1305U) == NULL);
	CHECK(sslc_tls_suite(0xc023U) == NULL);
	CHECK(sslc_tls_suite(0xc024U) == NULL);
	CHECK(sslc_tls_suite(0xc027U) == NULL);
	CHECK(sslc_tls_suite(0xc028U) == NULL);
	CHECK(sslc_tls_suite(0xc02aU) == NULL);
	CHECK(sslc_tls_suite(0xc02dU) == NULL);
	CHECK(sslc_tls_suite(0xc02eU) == NULL);
	CHECK(sslc_tls_suite(0xc031U) == NULL);
	CHECK(sslc_tls_suite(0xcca7U) == NULL);
	CHECK(sslc_tls_suite(0xccaaU) == NULL);
	CHECK(sslc_tls_suite(0xccacU) == NULL);
	CHECK(sslc_tls_suite(0x009cU) == NULL);
	CHECK(sslc_tls_suite(0x009eU) == NULL);
	CHECK(sslc_tls_suite(0xffffU) == NULL);
}

/* Checks the offered list for one pair of versions against the expected numbers. */
static void
check_offered(
	uint16_t min_version,
	uint16_t max_version,
	const uint16_t *expected,
	size_t expected_count)
{
	uint16_t out[16];
	size_t count;
	size_t index;
	size_t room;

	/* A list with room to spare is the whole list, in order, and nothing after it is touched. */
	for (index = 0; index < 16U; index++)
		out[index] = 0xeeeeU;
	count = sslc_tls_offered_suites(min_version, max_version, out, 16);
	CHECK(count == expected_count);
	for (index = 0; index < expected_count && index < 16U; index++)
		CHECK(out[index] == expected[index]);
	for (index = expected_count; index < 16U; index++)
		CHECK(out[index] == 0xeeeeU);

	/* A smaller room gives the first numbers only and writes nothing past the room. */
	for (room = 0; room <= expected_count + 2U; room++) {
		for (index = 0; index < 4U; index++)
		for (index = 0; index < 16U; index++)
			out[index] = 0xeeeeU;
		count = sslc_tls_offered_suites(min_version, max_version, out, room);
		CHECK(count == (room < expected_count ? room : expected_count));
		for (index = 0; index < count; index++)
			CHECK(out[index] == expected[index]);
		for (index = count; index < 16U; index++)
			CHECK(out[index] == 0xeeeeU);
	}

	/* An exact-size block: a write outside it is an AddressSanitizer error. */
	if (expected_count > 0U) {
		uint16_t *block;

		block = (uint16_t *)malloc(expected_count * sizeof(uint16_t));
		if (block == NULL)
			abort();
		count = sslc_tls_offered_suites(min_version, max_version, block, expected_count);
		CHECK(count == expected_count);
		for (index = 0; index < count; index++)
			CHECK(block[index] == expected[index]);
		free(block);
	}
}

/* Checks sslc_tls_offered_suites. */
static void
test_offered_suites(void)
{
	/* The TLS 1.3 suites: AES-128-GCM, ChaCha20-Poly1305, AES-256-GCM (the order of the header, and of the ClientHello of RFC 8448). */
	static const uint16_t only13[] = { 0x1301U, 0x1303U, 0x1302U };

	/* The TLS 1.2 suites: ECDSA before RSA, each in the order AES-128-GCM, ChaCha20-Poly1305, AES-256-GCM. */
	static const uint16_t only12[] = { 0xc02bU, 0xcca9U, 0xc02cU, 0xc02fU, 0xcca8U, 0xc030U };

	/* Both: the 1.3 suites first. */
	static const uint16_t both[] = {
		0x1301U, 0x1303U, 0x1302U, 0xc02bU, 0xcca9U, 0xc02cU, 0xc02fU, 0xcca8U, 0xc030U
	};
	static const uint8_t rfc8448_suites[6] = { 0x13, 0x01, 0x13, 0x03, 0x13, 0x02 };
	uint16_t out[16];
	size_t count;
	size_t index;
	size_t other;
	const struct sslc_suite *suite;

	/* The four combinations of the two versions. */
	check_offered(0x0303U, 0x0304U, both, 9);
	check_offered(0x0304U, 0x0304U, only13, 3);
	check_offered(0x0303U, 0x0303U, only12, 6);

	/* Ranges wider than the supported versions include what falls inside them. */
	check_offered(0x0000U, 0xffffU, both, 9);
	check_offered(0x0300U, 0x0304U, both, 9);
	check_offered(0x0303U, 0xffffU, both, 9);
	check_offered(0x0304U, 0xffffU, only13, 3);
	check_offered(0x0000U, 0x0303U, only12, 6);

	/* Ranges with no supported version, and the minimum above the maximum, give an empty list. */
	check_offered(0x0304U, 0x0303U, NULL, 0);
	check_offered(0x0300U, 0x0302U, NULL, 0);
	check_offered(0x0305U, 0xffffU, NULL, 0);
	check_offered(0xffffU, 0xffffU, NULL, 0);
	check_offered(0x0000U, 0x0000U, NULL, 0);

	/* A NULL out writes nothing and the count is 0. */
	CHECK(sslc_tls_offered_suites(0x0303U, 0x0304U, NULL, 0) == 0U);
	CHECK(sslc_tls_offered_suites(0x0303U, 0x0304U, NULL, 9) == 0U);

	/* Every offered number is a supported suite of an allowed version, no number repeats, and the SCSV is not in the list. */
	count = sslc_tls_offered_suites(0x0303U, 0x0304U, out, 16);
	CHECK(count == 9U);
	for (index = 0; index < count; index++) {
		suite = sslc_tls_suite(out[index]);
		CHECK(suite != NULL);
		if (suite != NULL)
			CHECK(suite->version == 0x0303U || suite->version == 0x0304U);
		CHECK(out[index] != SSLC_SUITE_EMPTY_RENEGOTIATION_INFO_SCSV);
		for (other = 0; other < index; other++)
			CHECK(out[other] != out[index]);
	}

	/* All the 1.3 suites come before all the 1.2 suites; the ECDSA suites before the RSA suites. */
	for (index = 0; index < count; index++) {
		suite = sslc_tls_suite(out[index]);
		if (suite == NULL)
			continue;
		if (index < 3U)
			CHECK(suite->version == SSLC_TLS13);
		else
			CHECK(suite->version == SSLC_TLS12);
		if (index >= 3U && index < 6U)
			CHECK(suite->auth == SSLC_AUTH_ECDSA);
		if (index >= 6U)
			CHECK(suite->auth == SSLC_AUTH_RSA);
	}

	/* Within each group the AEADs come in the order AES-128-GCM, ChaCha20-Poly1305, AES-256-GCM. */
	for (index = 0; index < count; index++) {
		suite = sslc_tls_suite(out[index]);
		if (suite == NULL)
			continue;
		if (index % 3U == 0U)
			CHECK(suite->aead == SSLC_AEAD_AES_128_GCM);
		if (index % 3U == 1U)
			CHECK(suite->aead == SSLC_AEAD_CHACHA20_POLY1305);
		if (index % 3U == 2U)
			CHECK(suite->aead == SSLC_AEAD_AES_256_GCM);
	}

	/* The list of TLS 1.3 suites is the one the ClientHello of RFC 8448 carries (13 01 13 03 13 02). */
	count = sslc_tls_offered_suites(0x0304U, 0x0304U, out, 16);
	CHECK(count == 3U);
	for (index = 0; index < count && index < 3U; index++)
		CHECK(out[index] == pair(rfc8448_suites[2U * index], rfc8448_suites[2U * index + 1U]));
}

int
main(void)
{
	/* Each group of checks. */
	test_numbers();
	test_registries();
	test_special_bytes();
	test_suite_table();
	test_offered_suites();

	/* The last line run-unit.sh reads. */
	return check_finish("test-tls");
}
