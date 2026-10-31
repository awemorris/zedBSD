/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The constants of the TLS protocol libssl-compat speaks, and its table of
 * cipher suites.
 *
 * Range.  A client of TLS 1.3 and TLS 1.2 (RFC 8996 removed 1.0 and
 * 1.1).  TLS 1.3 suites: TLS_AES_128_GCM_SHA256, TLS_AES_256_GCM_SHA384,
 * TLS_CHACHA20_POLY1305_SHA256.  TLS 1.2 suites: ECDHE with ECDSA or RSA
 * signatures and AES-GCM or ChaCha20-Poly1305 only (no CBC, no RSA key
 * transport, no finite-field DHE, no static ECDH).  Groups: x25519 and
 * secp256r1 for key exchange.  Signature schemes: ECDSA with P-256 and
 * P-384, RSA-PSS with an rsaEncryption key (rsae), RSA PKCS #1 v1.5 (for
 * TLS 1.2 and for certificates only, 4.2.3).  Extensions: server_name,
 * supported_groups, ec_point_formats (1.2), signature_algorithms,
 * extended_master_secret (1.2),
 * supported_versions, cookie, key_share, renegotiation_info (1.2).  No
 * session resumption, no 0-RTT, no PSK, no client certificates (an empty
 * Certificate answers a CertificateRequest), no renegotiation, no
 * post-handshake authentication, no ALPN.
 */

#ifndef LIBSSL_COMPAT_SSLC_TLS_H
#define LIBSSL_COMPAT_SSLC_TLS_H

#include "../libcrypto-compat/cryc-hash.h"

/*
 * The section numbers in the comments of this file are those of RFC 8446
 * (TLS 1.3, August 2018) unless another RFC is named; TLS 1.2 is RFC
 * 5246 (August 2008) with the ECDHE suites of RFC 8422, the AES-GCM
 * suites of RFC 5289, the ChaCha20-Poly1305 suites of RFC 7905, the
 * extended master secret of RFC 7627 and the renegotiation indication of
 * RFC 5746.  The numbers are IANA's registry values that those RFCs
 * assign, used as data.
 */

/*
 * The protocol versions (ProtocolVersion: 4.1.2, 4.2.1; RFC 5246 6.2.1).
 * Values referenced: RFC 8446 Appendix B.3.1.1 and RFC 5246 Appendix A.1.
 */
#define SSLC_TLS12		0x0303U
#define SSLC_TLS13		0x0304U

/*
 * The record content types (5.1, Appendix B.1; RFC 5246 6.2.1).
 * Values referenced: RFC 8446 Appendix B.1.
 */
#define SSLC_CT_CHANGE_CIPHER_SPEC	20U
#define SSLC_CT_ALERT			21U
#define SSLC_CT_HANDSHAKE		22U
#define SSLC_CT_APPLICATION_DATA	23U

/*
 * The handshake message types (4, Appendix B.3; RFC 5246 7.4).
 * Values referenced: RFC 8446 Appendix B.3 and RFC 5246 Appendix A.4.
 */
#define SSLC_HS_HELLO_REQUEST		0U
#define SSLC_HS_CLIENT_HELLO		1U
#define SSLC_HS_SERVER_HELLO		2U
#define SSLC_HS_NEW_SESSION_TICKET	4U
#define SSLC_HS_END_OF_EARLY_DATA	5U
#define SSLC_HS_ENCRYPTED_EXTENSIONS	8U
#define SSLC_HS_CERTIFICATE		11U
#define SSLC_HS_SERVER_KEY_EXCHANGE	12U
#define SSLC_HS_CERTIFICATE_REQUEST	13U
#define SSLC_HS_SERVER_HELLO_DONE	14U
#define SSLC_HS_CERTIFICATE_VERIFY	15U
#define SSLC_HS_CLIENT_KEY_EXCHANGE	16U
#define SSLC_HS_FINISHED		20U
#define SSLC_HS_KEY_UPDATE		24U
#define SSLC_HS_MESSAGE_HASH		254U

/*
 * The alert levels and descriptions (6, Appendix B.2; RFC 5246 7.2).
 * Values referenced: RFC 8446 Appendix B.2 and RFC 5246 Appendix A.3.
 */
#define SSLC_ALERT_WARNING			1U
#define SSLC_ALERT_FATAL			2U
#define SSLC_ALERT_CLOSE_NOTIFY			0U
#define SSLC_ALERT_UNEXPECTED_MESSAGE		10U
#define SSLC_ALERT_BAD_RECORD_MAC		20U
#define SSLC_ALERT_RECORD_OVERFLOW		22U
#define SSLC_ALERT_HANDSHAKE_FAILURE		40U
#define SSLC_ALERT_BAD_CERTIFICATE		42U
#define SSLC_ALERT_UNSUPPORTED_CERTIFICATE	43U
#define SSLC_ALERT_CERTIFICATE_REVOKED		44U
#define SSLC_ALERT_CERTIFICATE_EXPIRED		45U
#define SSLC_ALERT_CERTIFICATE_UNKNOWN		46U
#define SSLC_ALERT_ILLEGAL_PARAMETER		47U
#define SSLC_ALERT_UNKNOWN_CA			48U
#define SSLC_ALERT_ACCESS_DENIED		49U
#define SSLC_ALERT_DECODE_ERROR			50U
#define SSLC_ALERT_DECRYPT_ERROR		51U
#define SSLC_ALERT_PROTOCOL_VERSION		70U
#define SSLC_ALERT_INSUFFICIENT_SECURITY	71U
#define SSLC_ALERT_INTERNAL_ERROR		80U
#define SSLC_ALERT_INAPPROPRIATE_FALLBACK	86U
#define SSLC_ALERT_USER_CANCELED		90U
#define SSLC_ALERT_NO_RENEGOTIATION		100U
#define SSLC_ALERT_MISSING_EXTENSION		109U
#define SSLC_ALERT_UNSUPPORTED_EXTENSION	110U
#define SSLC_ALERT_UNRECOGNIZED_NAME		112U
#define SSLC_ALERT_BAD_CERTIFICATE_STATUS_RESPONSE	113U
#define SSLC_ALERT_UNKNOWN_PSK_IDENTITY		115U
#define SSLC_ALERT_CERTIFICATE_REQUIRED		116U
#define SSLC_ALERT_NO_APPLICATION_PROTOCOL	120U

/*
 * The cipher suites (B.4; RFC 8422 6, RFC 5289 3, RFC 7905 2).
 * Values referenced: RFC 8446 Appendix B.4, RFC 5289 section 3, RFC 7905
 * section 2.
 */
#define SSLC_SUITE_AES_128_GCM_SHA256				0x1301U
#define SSLC_SUITE_AES_256_GCM_SHA384				0x1302U
#define SSLC_SUITE_CHACHA20_POLY1305_SHA256			0x1303U
#define SSLC_SUITE_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256		0xc02bU
#define SSLC_SUITE_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384		0xc02cU
#define SSLC_SUITE_ECDHE_RSA_WITH_AES_128_GCM_SHA256		0xc02fU
#define SSLC_SUITE_ECDHE_RSA_WITH_AES_256_GCM_SHA384		0xc030U
#define SSLC_SUITE_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256	0xcca8U
#define SSLC_SUITE_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256	0xcca9U

/*
 * The renegotiation signalling cipher suite value (RFC 5746 3.3), sent
 * in a TLS 1.2-capable ClientHello instead of an empty renegotiation_info
 * (either is allowed; the SCSV keeps the ClientHello's extensions the same
 * as when only 1.3 is offered).  Value referenced: RFC 5746 section 3.3.
 */
#define SSLC_SUITE_EMPTY_RENEGOTIATION_INFO_SCSV	0x00ffU

/*
 * The named groups (4.2.7; RFC 8422 5.1.1).
 * Values referenced: RFC 8446 section 4.2.7.
 */
#define SSLC_GROUP_SECP256R1		0x0017U
#define SSLC_GROUP_SECP384R1		0x0018U
#define SSLC_GROUP_X25519		0x001dU

/*
 * The signature schemes (4.2.3; RFC 8422 5.1.3 and RFC 5246 7.4.1.4.1
 * name the TLS 1.2 forms with the same code points).
 * Values referenced: RFC 8446 section 4.2.3.
 */
#define SSLC_SCHEME_RSA_PKCS1_SHA256		0x0401U
#define SSLC_SCHEME_RSA_PKCS1_SHA384		0x0501U
#define SSLC_SCHEME_RSA_PKCS1_SHA512		0x0601U
#define SSLC_SCHEME_ECDSA_SECP256R1_SHA256	0x0403U
#define SSLC_SCHEME_ECDSA_SECP384R1_SHA384	0x0503U
#define SSLC_SCHEME_RSA_PSS_RSAE_SHA256		0x0804U
#define SSLC_SCHEME_RSA_PSS_RSAE_SHA384		0x0805U
#define SSLC_SCHEME_RSA_PSS_RSAE_SHA512		0x0806U

/*
 * The extension types (4.2; RFC 6066 3, RFC 7301 3.1, RFC 8422 5.1.2,
 * RFC 7627 5.1, RFC 5746 3.2).
 * Values referenced: RFC 8446 section 4.2, RFC 7627 section 5.1, RFC
 * 5746 section 3.2, RFC 8422 section 5.1.2.
 */
#define SSLC_EXT_SERVER_NAME			0U
#define SSLC_EXT_SUPPORTED_GROUPS		10U
#define SSLC_EXT_EC_POINT_FORMATS		11U
#define SSLC_EXT_SIGNATURE_ALGORITHMS		13U
#define SSLC_EXT_ALPN				16U
#define SSLC_EXT_EXTENDED_MASTER_SECRET		23U
#define SSLC_EXT_SESSION_TICKET			35U
#define SSLC_EXT_PRE_SHARED_KEY			41U
#define SSLC_EXT_EARLY_DATA			42U
#define SSLC_EXT_SUPPORTED_VERSIONS		43U
#define SSLC_EXT_COOKIE				44U
#define SSLC_EXT_PSK_KEY_EXCHANGE_MODES		45U
#define SSLC_EXT_KEY_SHARE			51U
#define SSLC_EXT_RENEGOTIATION_INFO		0xff01U

/*
 * The record limits (5.1, 5.2, 5.4; RFC 5246 6.2.1 to 6.2.3): a
 * plaintext fragment of at most 2^14 bytes; a TLS 1.3 protected record of
 * at most 2^14 + 256 bytes; a TLS 1.2 record of at most 2^14 + 2048
 * bytes; the 5-byte record header.
 */
#define SSLC_RECORD_HEADER		5U
#define SSLC_RECORD_PLAINTEXT_MAX	16384U
#define SSLC_RECORD13_CIPHERTEXT_MAX	(16384U + 256U)
#define SSLC_RECORD12_CIPHERTEXT_MAX	(16384U + 2048U)
#define SSLC_RECORD_BUFFER		(SSLC_RECORD_HEADER + SSLC_RECORD12_CIPHERTEXT_MAX)

/*
 * The largest handshake message accepted, body and 4-byte header: a
 * Certificate message with a long chain is the largest a server sends;
 * OpenSSL's default limit (SSL_CTX_set_max_cert_list) is 100 KiB.
 */
#define SSLC_HANDSHAKE_MAX		(102400U + 4U)

/*
 * The special Random of a HelloRetryRequest (4.1.3), and the last eight
 * bytes of ServerHello.random that signal a downgrade (4.1.3: "DOWNGRD"
 * followed by 0x01 for TLS 1.2 and 0x00 for TLS 1.1 or below).  Values
 * referenced: RFC 8446 section 4.1.3; defined in sslc-tls.c.
 */
extern const uint8_t sslc_tls_hrr_random[32];
extern const uint8_t sslc_tls_downgrade12[8];
extern const uint8_t sslc_tls_downgrade11[8];

/* The AEADs of the suites. */
enum sslc_aead_id {
	SSLC_AEAD_NONE = 0,
	SSLC_AEAD_AES_128_GCM = 1,
	SSLC_AEAD_AES_256_GCM = 2,
	SSLC_AEAD_CHACHA20_POLY1305 = 3
};

/* How a TLS 1.2 suite authenticates the server's key exchange (TLS 1.3 suites do not say: SSLC_AUTH_ANY). */
enum sslc_auth_id {
	SSLC_AUTH_ANY = 0,
	SSLC_AUTH_ECDSA = 1,
	SSLC_AUTH_RSA = 2
};

/*
 * One cipher suite: its number, the protocol version it belongs to, its
 * AEAD, the hash of its HKDF or PRF and transcript, how a 1.2 suite
 * signs, the AEAD key length, and the IV: for TLS 1.3 and for TLS 1.2
 * ChaCha20-Poly1305 the 12-byte per-record nonce base (5.3, RFC 7905 2);
 * for TLS 1.2 AES-GCM the 4-byte implicit salt (RFC 5288 3) followed by
 * the 8 explicit bytes sent in each record.
 */
struct sslc_suite {
	uint16_t id;
	uint16_t version;
	enum sslc_aead_id aead;
	enum cryc_hash_id hash;
	enum sslc_auth_id auth;
	uint8_t key_size;
	uint8_t fixed_iv_size;
	const char *name;
};

/*
 * Returns the suite numbered id when libssl-compat supports it, else
 * NULL.  The table (cited as data in sslc-tls.c) is constant.  name is
 * the IANA name (TLS_AES_128_GCM_SHA256, ...).
 */
const struct sslc_suite *sslc_tls_suite(uint16_t id);

/*
 * The suites a ClientHello offers, in preference order, for the versions
 * allowed: TLS 1.3 suites first (AES-128-GCM, ChaCha20-Poly1305,
 * AES-256-GCM), then the TLS 1.2 suites (ECDSA before RSA; AES-128-GCM,
 * ChaCha20-Poly1305, AES-256-GCM).  Writes at most room numbers to out and
 * returns how many it wrote.
 */
size_t sslc_tls_offered_suites(uint16_t min_version, uint16_t max_version, uint16_t *out, size_t room);

#endif
