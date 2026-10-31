/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The handshake messages a TLS client writes and reads: their encoding
 * and their syntax checks (RFC 8446 4 and Appendix B.3 for TLS 1.3; RFC
 * 5246 7.4 with RFC 8422 5 for TLS 1.2).  The meaning of the values (was
 * this version offered, does this suite fit) is checked by the handshake
 * (sslc-hs.h); this module checks that the bytes are well formed and that
 * no extension appears that the client did not ask for.
 *
 * Every parse function takes the body of one whole handshake message
 * (after its 4-byte header) and fills a structure of views into that
 * body; the views live as long as the body.  On an error it returns the
 * errno value and writes the alert the client must send: decode_error for
 * bad syntax (EINVAL), unsupported_extension for an extension the client
 * did not offer (EPROTO), illegal_parameter for a value out of its range
 * (EPROTO), as each comment says.  The input is untrusted.
 *
 * Every write function writes a whole message, its header included, and
 * returns 0, or ENOBUFS when room is too small (*written is then 0).
 */

#ifndef LIBSSL_COMPAT_SSLC_MSG_H
#define LIBSSL_COMPAT_SSLC_MSG_H

#include "sslc-buf.h"
#include "../libcrypto-compat/cryc-chain.h"
#include "sslc-tls.h"

/* The most key shares a ClientHello carries (x25519 alone in practice; room for secp256r1 too). */
#define SSLC_MSG_MAX_SHARES	2U

/* The longest HelloRetryRequest cookie accepted (4.2.2: cookie<1..2^16-1>; a server's is far shorter). */
#define SSLC_MSG_COOKIE_MAX	1024U

/* The bits of struct sslc_server_hello.seen: the extensions found. */
#define SSLC_SEEN_SUPPORTED_VERSIONS	0x0001U
#define SSLC_SEEN_KEY_SHARE		0x0002U
#define SSLC_SEEN_COOKIE		0x0004U
#define SSLC_SEEN_EXTENDED_MASTER	0x0008U
#define SSLC_SEEN_RENEGOTIATION_INFO	0x0010U
#define SSLC_SEEN_EC_POINT_FORMATS	0x0020U
#define SSLC_SEEN_SERVER_NAME		0x0040U

/* One key share the client sends: its group and its public key (a view). */
struct sslc_key_share {
	uint16_t group;
	const uint8_t *key;
	size_t key_size;
};

/*
 * What a ClientHello carries (4.1.2; RFC 5246 7.4.1.2).
 *
 *   min_version, max_version  SSLC_TLS12 or SSLC_TLS13 (min <= max).
 *   random                    32 bytes.
 *   session_id                The legacy_session_id: 32 random bytes when
 *                             TLS 1.3 is offered (the middlebox
 *                             compatibility mode of D.4), else empty.
 *   suites                    The offered suites (sslc_tls_offered_suites).
 *   groups                    supported_groups, in preference order.
 *   schemes                   signature_algorithms, in preference order.
 *   shares                    key_share entries (TLS 1.3 only).
 *   server_name               A normalized host name for server_name
 *                             (RFC 6066 3), or NULL (an address: no SNI).
 *   cookie                    The cookie of a HelloRetryRequest, echoed
 *                             in the second ClientHello (4.2.2), or none.
 *
 * Written in this order: legacy_version 0x0303, random, session_id,
 * cipher_suites (plus EMPTY_RENEGOTIATION_INFO_SCSV when TLS 1.2 is
 * offered), compression_methods {null}, then the extensions: server_name,
 * supported_groups, ec_point_formats {uncompressed} and
 * extended_master_secret (when TLS 1.2 is offered), signature_algorithms,
 * supported_versions (when TLS 1.3 is offered, max first), cookie,
 * key_share (when TLS 1.3 is offered).  No padding extension, no GREASE.
 */
struct sslc_client_hello {
	uint16_t min_version;
	uint16_t max_version;
	uint8_t random[32];
	uint8_t session_id[32];
	size_t session_id_size;
	const uint16_t *suites;
	size_t suite_count;
	const uint16_t *groups;
	size_t group_count;
	const uint16_t *schemes;
	size_t scheme_count;
	struct sslc_key_share shares[SSLC_MSG_MAX_SHARES];
	size_t share_count;
	const char *server_name;
	const uint8_t *cookie;
	size_t cookie_size;
};

/*
 * A ServerHello or HelloRetryRequest (4.1.3, 4.1.4; RFC 5246 7.4.1.3).
 *
 * is_retry is 1 when random equals sslc_tls_hrr_random.  seen tells which
 * extensions were present.  selected_version is supported_versions'
 * value; share_group and share_key the key_share (a HelloRetryRequest has
 * only the group); cookie (HelloRetryRequest only); renegotiation_info is
 * the renegotiated_connection bytes (must be empty for the client's first
 * handshake; checked by the handshake).
 */
struct sslc_server_hello {
	uint16_t legacy_version;
	uint8_t random[32];
	const uint8_t *session_id;
	size_t session_id_size;
	uint16_t suite;
	unsigned compression;
	int is_retry;
	uint32_t seen;
	uint16_t selected_version;
	uint16_t share_group;
	const uint8_t *share_key;
	size_t share_size;
	const uint8_t *cookie;
	size_t cookie_size;
	const uint8_t *renegotiation_info;
	size_t renegotiation_info_size;
};

/*
 * A Certificate message: the certificates' DER (views), leaf first, and
 * for TLS 1.3 its request context (4.4.2; RFC 5246 7.4.2).
 */
struct sslc_certificate_msg {
	const uint8_t *context;
	size_t context_size;
	const uint8_t *der[CRYC_CHAIN_MAX_PEER];
	size_t der_size[CRYC_CHAIN_MAX_PEER];
	size_t count;
};

/*
 * A TLS 1.2 ECDHE ServerKeyExchange (RFC 8422 5.4): the group, the
 * server's public point (a view), the signature scheme and the signature
 * (views), and params, the ServerECDHParams bytes the signature covers
 * (with the two randoms before them, RFC 8422 5.4).
 */
struct sslc_server_key_exchange {
	uint16_t group;
	const uint8_t *point;
	size_t point_size;
	uint16_t scheme;
	const uint8_t *signature;
	size_t signature_size;
	const uint8_t *params;
	size_t params_size;
};

/*
 * Reads a handshake message header (4; RFC 5246 7.4): type and body
 * length (24 bits).  in has at least 4 bytes.
 */
void sslc_msg_header(const uint8_t *in, unsigned *type, size_t *length);

/*
 * Writes a ClientHello (above).  Returns 0, ENOBUFS, or EINVAL for
 * inconsistent fields (no suite, min above max, a share without TLS 1.3,
 * a server name over CRYC_NAME_HOST_MAX).
 */
int sslc_msg_write_client_hello(const struct sslc_client_hello *hello, uint8_t *out, size_t room, size_t *written);

/*
 * Parses a ServerHello or HelloRetryRequest body.
 *
 * Syntax (decode_error, EINVAL): the fixed fields, a session id of at
 * most 32 bytes, an extensions block that ends with the body (it may be
 * absent for TLS 1.2, RFC 5246 7.4.1.3), each extension's own syntax, no
 * extension type twice.  Allowed extensions (any other is
 * unsupported_extension, EPROTO): supported_versions, key_share, cookie
 * (cookie only in a HelloRetryRequest; key_share in a HelloRetryRequest
 * is the group alone, 4.2.8), extended_master_secret (empty),
 * renegotiation_info, ec_point_formats (its list must contain
 * uncompressed, RFC 8422 5.2, else illegal_parameter), server_name
 * (empty).  A cookie over SSLC_MSG_COOKIE_MAX is illegal_parameter.
 *
 * Returns 0, EINVAL or EPROTO with *alert set on an error.
 */
int sslc_msg_parse_server_hello(const uint8_t *body, size_t size, struct sslc_server_hello *hello, unsigned *alert);

/*
 * Parses an EncryptedExtensions body (4.3.1): an extensions block, each
 * well formed, no type twice; allowed: server_name (empty) and
 * supported_groups (a list, ignored, 4.2.7); anything else is
 * unsupported_extension (4.2: libssl-compat offers nothing else that a
 * server answers here).  *server_name_ack is 1 when server_name was
 * present.  Returns 0, EINVAL or EPROTO with *alert.
 */
int sslc_msg_parse_encrypted_extensions(const uint8_t *body, size_t size, int *server_name_ack, unsigned *alert);

/*
 * Parses a Certificate body: TLS 1.3 (4.4.2) when tls13 is 1, else TLS
 * 1.2 (RFC 5246 7.4.2).
 *
 * TLS 1.3: certificate_request_context<0..255>, then the
 * CertificateEntry list; each entry's cert_data<1..2^24-1> is a view; each
 * entry's extensions must be empty (the client offered none of
 * status_request or signed_certificate_timestamp; anything there is
 * unsupported_extension).  TLS 1.2: the ASN.1Cert list.  An empty list is
 * decode_error (4.4.2.4); more than CRYC_CHAIN_MAX_PEER certificates is
 * bad_certificate (EPROTO).  The DER is not parsed here.
 *
 * Returns 0, EINVAL or EPROTO with *alert.
 */
int sslc_msg_parse_certificate(const uint8_t *body, size_t size, int tls13, struct sslc_certificate_msg *message, unsigned *alert);

/*
 * Parses a CertificateVerify body (4.4.3): the scheme and a view of the
 * signature<0..2^16-1>.  Returns 0 or EINVAL with *alert decode_error.
 */
int sslc_msg_parse_certificate_verify(const uint8_t *body, size_t size, uint16_t *scheme, const uint8_t **signature, size_t *signature_size, unsigned *alert);

/*
 * Parses a ServerKeyExchange body for ECDHE (RFC 8422 5.4 with RFC 5246
 * 7.4.1.4.1's SignatureAndHashAlgorithm): curve_type must be named_curve
 * (3; else illegal_parameter), then the group, the point<1..2^8-1>, the
 * scheme and the signature<0..2^16-1>, nothing after.
 * Returns 0, EINVAL or EPROTO with *alert.
 */
int sslc_msg_parse_server_key_exchange(const uint8_t *body, size_t size, struct sslc_server_key_exchange *exchange, unsigned *alert);

/*
 * Checks a CertificateRequest body (4.3.2 when tls13 is 1: context<0..
 * 2^8-1>, extensions<2..2^16-1>; RFC 5246 7.4.4 otherwise) for syntax
 * only and gives the TLS 1.3 context (a view) to echo in the empty
 * Certificate (4.4.2).  Returns 0 or EINVAL with *alert decode_error.
 */
int sslc_msg_parse_certificate_request(const uint8_t *body, size_t size, int tls13, const uint8_t **context, size_t *context_size, unsigned *alert);

/*
 * Checks a TLS 1.3 NewSessionTicket body (4.6.1) for syntax: lifetime,
 * age_add, nonce<0..255>, ticket<1..2^16-1>, extensions<0..2^16-2>.  The
 * ticket is not kept (no resumption), so its lifetime is not checked.
 * Returns 0 or EINVAL with *alert.
 */
int sslc_msg_parse_new_session_ticket(const uint8_t *body, size_t size, unsigned *alert);

/*
 * Parses a KeyUpdate body (4.6.3): one byte, 0 (update_not_requested) or
 * 1 (update_requested), else illegal_parameter (EPROTO); another length is
 * decode_error.  Returns 0, EINVAL or EPROTO with *alert.
 */
int sslc_msg_parse_key_update(const uint8_t *body, size_t size, int *update_requested, unsigned *alert);

/*
 * Writes a Finished (4.4.4; RFC 5246 7.4.9) carrying verify_data
 * (verify_size bytes).
 */
int sslc_msg_write_finished(const uint8_t *verify_data, size_t verify_size, uint8_t *out, size_t room, size_t *written);

/*
 * Writes an empty client Certificate: TLS 1.3 with the request's context
 * echoed and no entries (4.4.2.4); TLS 1.2 with an empty list (RFC 5246
 * 7.4.6).
 */
int sslc_msg_write_empty_certificate(int tls13, const uint8_t *context, size_t context_size, uint8_t *out, size_t room, size_t *written);

/* Writes a TLS 1.2 ECDHE ClientKeyExchange (RFC 8422 5.7): the point<1..2^8-1>. */
int sslc_msg_write_client_key_exchange(const uint8_t *point, size_t point_size, uint8_t *out, size_t room, size_t *written);

/* Writes a TLS 1.3 KeyUpdate (4.6.3) with request_update 0 or 1. */
int sslc_msg_write_key_update(int update_requested, uint8_t *out, size_t room, size_t *written);

#endif
