/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * tlspeer: the other end of a TLS connection for the unit tests of the
 * libssl-compat handshake, connection and public interface.  It is a
 * scripted server of TLS 1.3 (RFC 8446) and of TLS 1.2 with ECDHE (RFC
 * 5246 7.4 with RFC 8422 5), the test side's ECDSA signer (FIPS 186-5
 * 6.4.1), and the maker of the test certificates (with derwriter.h).
 *
 * Independence.  The messages are written here from the syntax of RFC
 * 8446 4 and Appendix B.3 and RFC 5246 7.4 with this file's own byte
 * writer, and the ClientHello is read here with this file's own reader;
 * neither sslc-buf nor sslc-msg is used, so a mistake there is not
 * mirrored here.  The transcript is this file's own (cryc_hash).  The key
 * schedules (sslc-keysched.h) and the record protection (sslc-record.h)
 * are libssl-compat's, which their own tests check against RFC 8448;
 * test-tlspeer.c replays the server side of RFC 8448 sections 3 and 5
 * through this file byte for byte.  The signer is built from the verified
 * parts of cryc-ecp.h and cryc-bn.h.  There is no RSA private key here.
 *
 * The peer does not negotiate: its configuration names the version, the
 * suite, the group, the signature scheme, the certificates and every
 * deviation, and the peer sends exactly that whatever the client offered.
 * It does check what the client sends it (the ClientHello's syntax, that
 * a key share for the group is there, the client's Finished, the record
 * tags) and records the first failure in peer->error and peer->why.
 *
 * Two ways to drive it; a test uses one of them for a peer.
 *
 *   Messages, for the handshake state machine (no records):
 *       error = tlspeer_handshake(peer, message, size);   one whole client
 *                                                        handshake message
 *       error = tlspeer_change_cipher_spec(peer);          the client's CCS
 *       while (tlspeer_next(peer, &item)) { ... }          what the server
 *                                                        sends, in order
 *     item.kind TLSPEER_ITEM_HANDSHAKE is one whole handshake message
 *     (header and body); TLSPEER_ITEM_CCS a change_cipher_spec.
 *     item.epoch names the server's write keys it goes under:
 *     peer->write_keys[item.epoch] (epoch 0 is plaintext).
 *
 *   Records, for the connection (a byte transport between the two):
 *       error = tlspeer_input(peer, bytes, size);    bytes the client wrote,
 *                                                    split anywhere
 *       error = tlspeer_take(peer, out, room, &taken);   bytes for the
 *                                                    client, any amount
 *     The peer reassembles and opens the client's records and seals its
 *     own with the right keys at the right places.
 *
 * Typical use:
 *
 *     struct tlspeer_pki pki;
 *     struct tlspeer_config config;
 *     struct tlspeer *peer;
 *
 *     tlspeer_pki_init(&pki, CRYC_CURVE_P256, "server.test");
 *     tlspeer_config_init(&config, SSLC_TLS13, &pki);
 *     config.group = SSLC_GROUP_SECP256R1;
 *     config.hello_retry = 1;
 *     config.breaks = TLSPEER_BREAK_FINISHED;
 *     tlspeer_new(&peer, &config);
 *     ... the client trusts pki.root (pki.root_size bytes of DER) and
 *         verifies at TLSPEER_PKI_TIME ...
 *     tlspeer_free(peer);
 *
 * Every function returns 0 or an errno value: EINVAL for malformed
 * client bytes or a bad argument, EBADMSG for a client Finished or record
 * that does not verify, EPROTO for a client message out of order, ENOENT
 * for a missing key share, ENOBUFS for a limit of this tool, ENOMEM.
 * After a failure the peer refuses further input with the same error.
 *
 * C89 with <stdint.h>, for the POSIX and the Windows (llvm-mingw) builds
 * of the tests.  Every function is static and marked unused, so a test
 * that uses only part of the tool builds with -Werror.
 */

#ifndef WS335_TESTS_TLSPEER_H
#define WS335_TESTS_TLSPEER_H

#include <stdlib.h>
#include <string.h>

#include "derwriter.h"

#include "userland/base/libcrypto-compat/cryc-ecc.h"
#include "userland/base/libcrypto-compat/cryc-x25519.h"
#include "userland/base/libssl-compat/sslc-keysched.h"

/*
 * The limits of the tool (larger inputs are ENOBUFS, not EINVAL: they
 * are the tool's limits, not the protocol's).
 */
#define TLSPEER_CHAIN_MAX	4U
#define TLSPEER_LIST_MAX	64U
#define TLSPEER_EXT_MAX		40U
#define TLSPEER_SHARES_MAX	4U
#define TLSPEER_SHARE_KEY_MAX	133U
#define TLSPEER_COOKIE_MAX	1024U
#define TLSPEER_ITEMS_MAX	64U
#define TLSPEER_EPOCHS		16U
#define TLSPEER_FLIGHT_MAX	65536U
#define TLSPEER_OUT_MAX		262144U
#define TLSPEER_MESSAGE_MAX	65536U
#define TLSPEER_DATA_MAX	65536U
#define TLSPEER_CERT_MAX	1024U
#define TLSPEER_WRITER_DEPTH	8U

/*
 * The validity of the certificates tlspeer_pki_init makes, as UTCTime
 * text and as seconds since 1970 (2026-01-01 and 2036-01-01, 00:00:00
 * UTC), and a time inside it to verify at (2026-07-01 00:00:00 UTC).
 */
#define TLSPEER_NOT_BEFORE		"260101000000Z"
#define TLSPEER_NOT_AFTER		"360101000000Z"
#define TLSPEER_NOT_BEFORE_TIME		1767225600L
#define TLSPEER_NOT_AFTER_TIME		2082758400L
#define TLSPEER_PKI_TIME		1782864000L

/*
 * Options (struct tlspeer_config.options): what a well-behaved server
 * may or may not send.
 *
 *   CCS             TLS 1.3: one change_cipher_spec after the ServerHello
 *                   or the first HelloRetryRequest (middlebox
 *                   compatibility, RFC 8446 D.4).
 *   SNI_ACK         An empty server_name in EncryptedExtensions (1.3) or
 *                   the ServerHello (1.2) when the client sent one.
 *   CERT_REQUEST    A CertificateRequest; the client must answer with an
 *                   empty Certificate (the only answer the peer takes).
 *   POINT_FORMATS   TLS 1.2: ec_point_formats {uncompressed} in the
 *                   ServerHello.
 *   NO_EMS          TLS 1.2: no extended_master_secret even when offered.
 *   NO_RI           TLS 1.2: no renegotiation_info.
 *   RETRY_NO_GROUP  The HelloRetryRequest has no key_share (a cookie
 *                   must then be configured).
 */
#define TLSPEER_OPT_CCS			0x0001U
#define TLSPEER_OPT_SNI_ACK		0x0002U
#define TLSPEER_OPT_CERT_REQUEST	0x0004U
#define TLSPEER_OPT_POINT_FORMATS	0x0008U
#define TLSPEER_OPT_NO_EMS		0x0010U
#define TLSPEER_OPT_NO_RI		0x0020U
#define TLSPEER_OPT_RETRY_NO_GROUP	0x0040U

/*
 * Breaks (struct tlspeer_config.breaks, copied to peer->breaks, which a
 * test may change at any time): deliberate faults.
 *
 *   FINISHED       The server Finished's first byte is flipped (bit 0).
 *   SIGNATURE      The CertificateVerify (1.3) or ServerKeyExchange
 *                  (1.2) signature is made over the signed content with
 *                  its last byte flipped: well-formed, does not verify.
 *   RECORD_TAG     The next protected record sealed gets the last byte of
 *                  its tag flipped (bit 0); the break then clears itself.
 *   SECOND_HRR     The second ClientHello is answered with another
 *                  HelloRetryRequest (and nothing else).
 *   SAME_RECORD    The first handshake message after a write key change
 *                  (EncryptedExtensions after the ServerHello, the message
 *                  after a KeyUpdate) goes into the record before it,
 *                  under the old keys; the break then clears itself
 *                  (with OPT_CCS the CCS stands between the ServerHello
 *                  and EncryptedExtensions, so use it without OPT_CCS).
 *   DOWNGRADE12    TLS 1.2: the ServerHello random ends with "DOWNGRD"
 *                  and 0x01 (RFC 8446 4.1.3).
 *   DOWNGRADE11    The same with 0x00.
 *   SESSION_ID     TLS 1.3: the echoed session id has its first byte
 *                  flipped (a 1-byte id 0x01 when the client's is empty);
 *                  TLS 1.2: the client's own session id is echoed.
 *   CERT_CONTEXT   TLS 1.3: the Certificate's request context is one byte
 *                  (0x01) instead of empty.
 *   UNSHARED       TLS 1.3: a ServerHello for the group even when the
 *                  client sent no share for it; the script ends there.
 *   ZERO_SHARE     The server's share is x25519's u = 0 (an all-zero
 *                  shared secret) or, for secp256r1 and secp384r1,
 *                  0x04 and 64 or 96 zero bytes (not on the curve).
 *                  TLS 1.3: the script ends
 *                  after the ServerHello; TLS 1.2: in the
 *                  ServerKeyExchange, signed as usual.
 */
#define TLSPEER_BREAK_FINISHED		0x0001U
#define TLSPEER_BREAK_SIGNATURE		0x0002U
#define TLSPEER_BREAK_RECORD_TAG	0x0004U
#define TLSPEER_BREAK_SECOND_HRR	0x0008U
#define TLSPEER_BREAK_SAME_RECORD	0x0010U
#define TLSPEER_BREAK_DOWNGRADE12	0x0020U
#define TLSPEER_BREAK_DOWNGRADE11	0x0040U
#define TLSPEER_BREAK_SESSION_ID	0x0080U
#define TLSPEER_BREAK_CERT_CONTEXT	0x0100U
#define TLSPEER_BREAK_UNSHARED		0x0200U
#define TLSPEER_BREAK_ZERO_SHARE	0x0400U

/*
 * Where the extra extension of the configuration goes (extra_at bits):
 * an extension the client did not offer, or one that is not allowed in
 * that message (RFC 8446 4.2's table).
 */
#define TLSPEER_AT_SERVER_HELLO		0x0001U
#define TLSPEER_AT_RETRY		0x0002U
#define TLSPEER_AT_ENCRYPTED_EXTENSIONS	0x0004U
#define TLSPEER_AT_CERT_REQUEST		0x0008U
#define TLSPEER_AT_CERT_ENTRY		0x0010U

/* The kinds of what the server sends (struct tlspeer_item.kind). */
#define TLSPEER_ITEM_HANDSHAKE	1U
#define TLSPEER_ITEM_CCS	2U
#define TLSPEER_ITEM_ALERT	3U
#define TLSPEER_ITEM_DATA	4U
#define TLSPEER_ITEM_RECORD	5U
#define TLSPEER_ITEM_BYTES	6U

/* The peer's states (peer->state). */
#define TLSPEER_WAIT_HELLO		1
#define TLSPEER_WAIT_CERTIFICATE	2
#define TLSPEER_WAIT_KEY_EXCHANGE	3
#define TLSPEER_WAIT_CCS		4
#define TLSPEER_WAIT_FINISHED		5
#define TLSPEER_CONNECTED		6
#define TLSPEER_STOPPED			7
#define TLSPEER_FAILED			8

/*
 * An ECDSA key pair of the test side: the curve, the private key d
 * (ec.size big-endian bytes) and the public key Q = d G (uncompressed,
 * 1 + 2 * ec.size bytes).
 */
struct tlspeer_key {
	struct cryc_ecp ec;
	uint8_t private_key[CRYC_ECP_MAX_BYTES];
	uint8_t public_key[CRYC_ECP_MAX_POINT];
	size_t public_size;
};

/*
 * What tlspeer_make_cert writes: a version 3 certificate signed with
 * ECDSA (with SHA-256 by a P-256 issuer key, SHA-384 by a P-384 one).
 *
 *   subject, issuer    The commonName of each Name (the Name is that one
 *                      attribute); issuer equals subject for a root.
 *   subject_key        Its public key goes into the certificate.
 *   issuer_key         Signs it (the subject key for a self-signed root).
 *   serial             A positive serial number.
 *   is_ca              1: basicConstraints cA TRUE and keyUsage
 *                      keyCertSign and cRLSign; 0: basicConstraints
 *                      empty, keyUsage digitalSignature, extKeyUsage
 *                      serverAuth.
 *   dns_name           A subjectAltName dNSName, or NULL.
 *   address            A subjectAltName iPAddress of address_size bytes
 *                      (4 or 16), or NULL.
 *   not_before, not_after  UTCTime text, YYMMDDHHMMSSZ.
 *
 * Every certificate has a subjectKeyIdentifier (the first 20 bytes of
 * SHA-256 of the public key) and, when it is not self-signed, an
 * authorityKeyIdentifier with the issuer's.
 */
struct tlspeer_cert {
	const char *subject;
	const char *issuer;
	const struct tlspeer_key *subject_key;
	const struct tlspeer_key *issuer_key;
	uint32_t serial;
	int is_ca;
	const char *dns_name;
	const uint8_t *address;
	size_t address_size;
	const char *not_before;
	const char *not_after;
};

/*
 * A root and a leaf: the root CA "tlspeer root CA" (self-signed) and the
 * leaf for one host (commonName and subjectAltName dNSName the host),
 * both valid from TLSPEER_NOT_BEFORE to TLSPEER_NOT_AFTER, and their
 * keys (fixed: made from the labels "tlspeer root" and "tlspeer leaf").
 */
struct tlspeer_pki {
	struct tlspeer_key root_key;
	struct tlspeer_key leaf_key;
	uint8_t root[TLSPEER_CERT_MAX];
	size_t root_size;
	uint8_t leaf[TLSPEER_CERT_MAX];
	size_t leaf_size;
};

/* One key share of a ClientHello: the group and the key_exchange bytes. */
struct tlspeer_share {
	uint16_t group;
	uint8_t key[TLSPEER_SHARE_KEY_MAX];
	size_t key_size;
};

/*
 * A ClientHello as tlspeer_parse_client_hello reads it (RFC 8446 4.1.2;
 * RFC 5246 7.4.1.2).  Values are copied; extensions lists the extension
 * types in the order they came.  The lists are filled only for the
 * extensions present (has_* or a count tells).
 */
struct tlspeer_hello {
	uint16_t legacy_version;
	uint8_t random[32];
	uint8_t session_id[32];
	size_t session_id_size;
	uint16_t suites[TLSPEER_LIST_MAX];
	size_t suite_count;
	int has_scsv;
	uint8_t compressions[TLSPEER_LIST_MAX];
	size_t compression_count;
	int has_extensions;
	uint16_t extensions[TLSPEER_EXT_MAX];
	size_t extension_count;
	int has_server_name;
	char server_name[256];
	uint16_t groups[TLSPEER_LIST_MAX];
	size_t group_count;
	uint8_t point_formats[TLSPEER_LIST_MAX];
	size_t point_format_count;
	uint16_t schemes[TLSPEER_LIST_MAX];
	size_t scheme_count;
	uint16_t versions[TLSPEER_LIST_MAX];
	size_t version_count;
	struct tlspeer_share shares[TLSPEER_SHARES_MAX];
	size_t share_count;
	int has_key_share;
	int has_cookie;
	uint8_t cookie[TLSPEER_COOKIE_MAX];
	size_t cookie_size;
	int has_extended_master_secret;
	int has_renegotiation_info;
	uint8_t renegotiation_info[255];
	size_t renegotiation_info_size;
};

/*
 * The script of one server.  tlspeer_config_init fills a working
 * default; a test changes what it needs.  Pointers are borrowed and must
 * live as long as the peer.
 *
 *   version          SSLC_TLS13 or SSLC_TLS12.
 *   suite            The suite sent (sslc_tls_suite must know it; its
 *                    hash is the transcript's; a suite of the other
 *                    version is sent as it is and the script ends where
 *                    keys cannot be made).
 *   group            The key exchange: SSLC_GROUP_X25519,
 *                    SSLC_GROUP_SECP256R1 or SSLC_GROUP_SECP384R1.
 *   scheme           The CertificateVerify / ServerKeyExchange scheme:
 *                    SSLC_SCHEME_ECDSA_SECP256R1_SHA256 or _SECP384R1_
 *                    SHA384 (the hash; the key is key's curve).
 *   key              The signing key (the leaf's).
 *   chain, chain_size, chain_count  The certificates sent, leaf first.
 *   random           The ServerHello random.
 *   x25519_private   The server's x25519 scalar.
 *   p256_private     The server's secp256r1 private key (1 <= d < n).
 *   p384_private     The server's secp384r1 private key (1 <= d < n).
 *   hello_retry      1: the first ClientHello gets a HelloRetryRequest
 *                    naming group (TLS 1.3).
 *   cookie, cookie_size  A cookie for the HelloRetryRequest (NULL: none).
 *   options, breaks  TLSPEER_OPT_ and TLSPEER_BREAK_ bits.
 *   extra_type, extra_data, extra_size, extra_at  One more extension
 *                    put at the end of the messages extra_at names.
 *   ee_raw, ee_raw_size  When ee_raw is not NULL, EncryptedExtensions
 *                    carries exactly these bytes as its extension list
 *                    (inside the 2-byte length).
 *   cv_raw, cv_raw_size  When cv_raw is not NULL, these bytes are sent
 *                    as the whole CertificateVerify message instead of
 *                    one signed here.
 *   request_context, request_context_size  The TLS 1.3
 *                    CertificateRequest's context (empty by default).
 *   record_size      The most plaintext bytes per record the peer
 *                    writes (0: 2^14).  Consecutive handshake messages
 *                    under the same keys share records.
 */
struct tlspeer_config {
	uint16_t version;
	uint16_t suite;
	uint16_t group;
	uint16_t scheme;
	const struct tlspeer_key *key;
	const uint8_t *chain[TLSPEER_CHAIN_MAX];
	size_t chain_size[TLSPEER_CHAIN_MAX];
	size_t chain_count;
	uint8_t random[32];
	uint8_t x25519_private[CRYC_X25519_SIZE];
	uint8_t p256_private[32];
	uint8_t p384_private[48];
	int hello_retry;
	const uint8_t *cookie;
	size_t cookie_size;
	uint32_t options;
	uint32_t breaks;
	uint16_t extra_type;
	const uint8_t *extra_data;
	size_t extra_size;
	uint32_t extra_at;
	const uint8_t *ee_raw;
	size_t ee_raw_size;
	const uint8_t *cv_raw;
	size_t cv_raw_size;
	const uint8_t *request_context;
	size_t request_context_size;
	size_t record_size;
};

/*
 * One thing the server sends: kind (TLSPEER_ITEM_), the record content
 * type for TLSPEER_ITEM_RECORD, the epoch of the write keys it goes under
 * (peer->write_keys[epoch]; 0 is plaintext), and its bytes (a view valid
 * until the next call that makes the peer send).
 */
struct tlspeer_item {
	unsigned kind;
	unsigned type;
	unsigned epoch;
	const uint8_t *data;
	size_t size;
};

/*
 * A byte writer of the TLS presentation language (RFC 8446 3), the
 * tool's own: integers, bytes and vectors with a length prefix of 1, 2 or
 * 3 bytes.  error is sticky.
 */
struct tlspeer_writer {
	uint8_t *data;
	size_t room;
	size_t used;
	size_t open[TLSPEER_WRITER_DEPTH];
	unsigned prefix[TLSPEER_WRITER_DEPTH];
	unsigned depth;
	int error;
};

/* A byte reader over untrusted bytes, the tool's own; error is sticky. */
struct tlspeer_reader {
	const uint8_t *data;
	size_t size;
	size_t position;
	int error;
};

/* An item's place in the flight buffer. */
struct tlspeer_slot {
	unsigned kind;
	unsigned type;
	unsigned epoch;
	size_t offset;
	size_t size;
};

/*
 * A server.  Made by tlspeer_new (heap: about 600 KiB), freed by
 * tlspeer_free.  A test reads these fields:
 *
 *   state, error, why    The state (TLSPEER_ constants), the first
 *                        failure's errno value and a sentence saying what
 *                        it was (NULL when none).
 *   breaks, record_size  Live copies of the configuration's; may be
 *                        changed between calls.
 *   hello                The last ClientHello read.
 *   retries              HelloRetryRequests sent.
 *   ccs_count            Change_cipher_spec records from the client.
 *   write_keys, write_v13, write_epochs  The server's write keys of each
 *                        epoch (1 .. write_epochs - 1) and their record
 *                        format; the client's read keys.
 *   client_handshake_keys, client_application_keys  The client's write
 *                        keys as the peer expects them (TLS 1.3; for TLS
 *                        1.2 client_application_keys only), once known.
 *   ems                  TLS 1.2: 1 when extended_master_secret is used.
 *   data, data_size      Application data received (a test may reset
 *                        data_size to 0).
 *   alert_count, alert_level, alert_description, alert_protected,
 *   close_notify         The alerts received: their count, the last one,
 *                        whether it came protected; close_notify is 1
 *                        once one came.
 *   records_in, records_out  Records read and written.
 */
struct tlspeer {
	struct tlspeer_config config;
	uint32_t breaks;
	size_t record_size;
	int state;
	int error;
	const char *why;
	struct tlspeer_hello hello;
	unsigned retries;
	unsigned ccs_count;
	const struct sslc_suite *suite;
	enum cryc_hash_id hash;
	size_t hash_size;
	struct cryc_hash transcript;
	uint8_t client_random[32];
	uint8_t server_random[32];
	struct tlspeer_key p256;
	struct tlspeer_key p384;
	uint8_t server_share[CRYC_ECP_MAX_POINT];
	size_t server_share_size;
	uint8_t shared[CRYC_ECP_MAX_BYTES];
	size_t shared_size;
	struct sslc_tls13_schedule schedule;
	uint8_t client_secret[CRYC_HASH_MAX_SIZE];
	uint8_t server_secret[CRYC_HASH_MAX_SIZE];
	uint8_t master[SSLC_TLS12_MASTER];
	int ems;
	struct sslc_traffic_keys server_keys12;
	struct sslc_traffic_keys write_keys[TLSPEER_EPOCHS];
	int write_v13[TLSPEER_EPOCHS];
	unsigned write_epochs;
	unsigned epoch;
	struct sslc_traffic_keys client_handshake_keys;
	struct sslc_traffic_keys client_application_keys;
	struct sslc_record_dir write_dir;
	unsigned write_dir_epoch;
	struct sslc_record_dir read_dir;
	int read_keyed;
	unsigned read_changes;
	int request_sent;
	struct tlspeer_slot items[TLSPEER_ITEMS_MAX];
	size_t item_count;
	size_t item_taken;
	uint8_t flight[TLSPEER_FLIGHT_MAX];
	size_t flight_used;
	uint8_t out[TLSPEER_OUT_MAX];
	size_t out_used;
	size_t out_taken;
	uint8_t in[SSLC_RECORD_BUFFER];
	size_t in_used;
	uint8_t message[TLSPEER_MESSAGE_MAX];
	size_t message_used;
	uint8_t data[TLSPEER_DATA_MAX];
	size_t data_size;
	unsigned alert_count;
	unsigned alert_level;
	unsigned alert_description;
	int alert_protected;
	int close_notify;
	unsigned long records_in;
	unsigned long records_out;
};

/*
 * The API.
 */

/*
 * Prepares key on curve with the private key d given as ec.size
 * big-endian bytes and computes Q = d G.  Returns 0, EINVAL when d is 0 or
 * not below n, ENOTSUP for an unknown curve.
 */
static int __attribute__((unused)) tlspeer_key_init(struct tlspeer_key *key, enum cryc_curve_id curve, const uint8_t *private_key);

/*
 * Prepares a fixed key made from a label: d = SHA-256(label) for P-256,
 * SHA-384(label) for P-384.  Returns as tlspeer_key_init (EINVAL in the
 * unlikely case the digest is not below n: choose another label).
 */
static int __attribute__((unused)) tlspeer_key_from_label(struct tlspeer_key *key, enum cryc_curve_id curve, const char *label);

/*
 * Signs a hash with ECDSA (FIPS 186-5 6.4.1 steps 3 to 11; the hash is
 * the caller's step 2): E is the leftmost bits(n) bits of hash, R = k G,
 * r = x(R) mod n, s = k^-1 (E + r d) mod n.  k is ec.size big-endian
 * bytes with 1 <= k < n, or NULL for this tool's fixed per-message k:
 * HMAC (SHA-256 for P-256, SHA-384 for P-384) keyed with d over the hash
 * and a counter byte, reduced mod n (not RFC 6979; deterministic, and
 * different for different hashes).  r and s receive ec.size bytes each.
 * Returns 0, or EINVAL for a k out of range or a k that gives r = 0 or
 * s = 0.
 */
static int __attribute__((unused)) tlspeer_ecdsa_sign(const struct tlspeer_key *key, const uint8_t *hash, size_t hash_size, const uint8_t *k, uint8_t *r, uint8_t *s);

/*
 * Signs a hash as tlspeer_ecdsa_sign with the fixed k and writes the DER
 * Ecdsa-Sig-Value (RFC 5480 2.2: SEQUENCE { r INTEGER, s INTEGER }), the
 * form TLS and X.509 carry.  Returns 0, EINVAL, or ENOBUFS.
 */
static int __attribute__((unused)) tlspeer_ecdsa_sign_der(const struct tlspeer_key *key, const uint8_t *hash, size_t hash_size, uint8_t *out, size_t room, size_t *written);

/* Writes the certificate spec describes as DER.  Returns 0, EINVAL or ENOBUFS. */
static int __attribute__((unused)) tlspeer_make_cert(const struct tlspeer_cert *spec, uint8_t *out, size_t room, size_t *written);

/* Makes the root and the leaf for host on curve (P-256 or P-384).  Returns 0 or an error. */
static int __attribute__((unused)) tlspeer_pki_init(struct tlspeer_pki *pki, enum cryc_curve_id curve, const char *host);

/*
 * Fills a working script: version; TLS 1.3 suite TLS_AES_128_GCM_SHA256,
 * TLS 1.2 suite ECDHE_ECDSA_WITH_AES_128_GCM_SHA256; group x25519; the
 * scheme of pki's leaf curve; the leaf as the only certificate; fixed
 * random values made from labels.  Returns 0 or an error.
 */
static int __attribute__((unused)) tlspeer_config_init(struct tlspeer_config *config, uint16_t version, const struct tlspeer_pki *pki);

/* Makes a peer for config (copied).  Returns 0, EINVAL for a config out of range, or ENOMEM. */
static int __attribute__((unused)) tlspeer_new(struct tlspeer **peer, const struct tlspeer_config *config);

/* Wipes and frees a peer (NULL is allowed). */
static void __attribute__((unused)) tlspeer_free(struct tlspeer *peer);

/*
 * Reads a whole ClientHello message (its 4-byte header and body) into
 * hello: every vector's length and range, the extensions block with
 * nothing after it, no extension twice, and the syntax of server_name,
 * supported_groups, ec_point_formats, signature_algorithms,
 * extended_master_secret, supported_versions, cookie, key_share and
 * renegotiation_info.  Returns 0, EINVAL (malformed), ENOBUFS (over a
 * limit of the tool).
 */
static int __attribute__((unused)) tlspeer_parse_client_hello(const uint8_t *message, size_t size, struct tlspeer_hello *hello);

/* Returns 1 when hello carries an extension of type, else 0. */
static int __attribute__((unused)) tlspeer_hello_has(const struct tlspeer_hello *hello, uint16_t type);

/*
 * Takes one whole client handshake message (header and body) and moves
 * the script: a ClientHello gets the server's flight (or a
 * HelloRetryRequest), a TLS 1.2 ClientKeyExchange makes the keys, a
 * client Finished is checked (and in TLS 1.2 answered with CCS and
 * Finished), a TLS 1.3 KeyUpdate moves the read keys (and when it
 * requests it, the peer answers with its own).  Returns 0 or an error.
 */
static int __attribute__((unused)) tlspeer_handshake(struct tlspeer *peer, const uint8_t *message, size_t size);

/*
 * Takes the client's change_cipher_spec: TLS 1.2 after the
 * ClientKeyExchange it switches the read keys; TLS 1.3 before the
 * client's Finished it is counted and dropped.  Elsewhere EPROTO.
 */
static int __attribute__((unused)) tlspeer_change_cipher_spec(struct tlspeer *peer);

/* Gives the next thing the server sends; returns 1 with *item filled, or 0 when there is none. */
static int __attribute__((unused)) tlspeer_next(struct tlspeer *peer, struct tlspeer_item *item);

/*
 * Takes size bytes the client wrote (any split of its records), opens
 * and reassembles them and feeds the handshake, CCS, alerts and
 * application data on.  A TLS 1.3 alert in plaintext is accepted at any
 * time (a client that fails before it has keys sends one).  Returns 0 or
 * an error (a record that does not open is EBADMSG).
 */
static int __attribute__((unused)) tlspeer_input(struct tlspeer *peer, const uint8_t *data, size_t size);

/* Seals everything sent so far into records; returns 0 or an error. */
static int __attribute__((unused)) tlspeer_flush(struct tlspeer *peer);

/*
 * Flushes, then gives up to room of the bytes for the client; *taken
 * receives the count (0 when none wait).  Returns 0 or an error.
 */
static int __attribute__((unused)) tlspeer_take(struct tlspeer *peer, uint8_t *out, size_t room, size_t *taken);

/*
 * Sends a TLS 1.3 NewSessionTicket (lifetime 7200, age_add 0x01020304,
 * nonce 00, a 16-byte ticket, no extensions) under the current keys.
 */
static int __attribute__((unused)) tlspeer_send_ticket(struct tlspeer *peer);

/* Sends a TLS 1.3 KeyUpdate (request_update 0 or 1) and moves the write keys to the next generation. */
static int __attribute__((unused)) tlspeer_send_key_update(struct tlspeer *peer, int request_update);

/* Sends application data (split into records of record_size; an alert or a raw record is never split). */
static int __attribute__((unused)) tlspeer_send_data(struct tlspeer *peer, const uint8_t *data, size_t size);

/* Sends an alert of level and description under the current keys. */
static int __attribute__((unused)) tlspeer_send_alert(struct tlspeer *peer, unsigned level, unsigned description);

/*
 * Sends a handshake message the test made (whole, header and body) under
 * the current keys; it is not added to the transcript (a HelloRequest, an
 * unexpected or malformed message).
 */
static int __attribute__((unused)) tlspeer_send_handshake(struct tlspeer *peer, const uint8_t *message, size_t size);

/* Sends one record of content type type (20 to 23) with these bytes (at most 2^14) under the current keys. */
static int __attribute__((unused)) tlspeer_send_record(struct tlspeer *peer, unsigned type, const uint8_t *content, size_t size);

/* Sends bytes as they are, after everything before them (a hand-made record). */
static int __attribute__((unused)) tlspeer_send_bytes(struct tlspeer *peer, const uint8_t *bytes, size_t size);

/*
 * The tool's own parts.
 */

/* Starts a writer over room bytes. */
static void __attribute__((unused)) tlspeer_writer_init(struct tlspeer_writer *writer, uint8_t *data, size_t room);

/* Writes size bytes. */
static void __attribute__((unused)) tlspeer_put(struct tlspeer_writer *writer, const uint8_t *data, size_t size);

/* Writes the low bytes of value big-endian in count bytes (1 to 4). */
static void __attribute__((unused)) tlspeer_put_int(struct tlspeer_writer *writer, uint32_t value, unsigned count);

/* Begins a vector with a length prefix of prefix bytes. */
static void __attribute__((unused)) tlspeer_begin(struct tlspeer_writer *writer, unsigned prefix);

/* Ends the innermost vector and fills its length. */
static void __attribute__((unused)) tlspeer_end(struct tlspeer_writer *writer);

/* Writes one extension: type and data<0..2^16-1>. */
static void __attribute__((unused)) tlspeer_put_extension(struct tlspeer_writer *writer, uint32_t type, const uint8_t *data, size_t size);

/* Starts a reader over size bytes. */
static void __attribute__((unused)) tlspeer_reader_init(struct tlspeer_reader *reader, const uint8_t *data, size_t size);

/* Reads a big-endian integer of count bytes (1 to 3). */
static uint32_t __attribute__((unused)) tlspeer_get_int(struct tlspeer_reader *reader, unsigned count);

/* Gives a view of the next size bytes, or NULL (error set). */
static const uint8_t * __attribute__((unused)) tlspeer_get(struct tlspeer_reader *reader, size_t size);

/* Reads a vector with a prefix of prefix bytes and a length in [min, max] into inner. */
static void __attribute__((unused)) tlspeer_get_vector(struct tlspeer_reader *reader, unsigned prefix, size_t min, size_t max, struct tlspeer_reader *inner);

/* Returns 1 when the reader has no error and nothing left. */
static int __attribute__((unused)) tlspeer_reader_done(const struct tlspeer_reader *reader);

/* Reads a list of 2-byte values of a vector into out; returns 0, EINVAL or ENOBUFS. */
static int __attribute__((unused)) tlspeer_get_list16(struct tlspeer_reader *reader, unsigned prefix, size_t min, size_t max, uint16_t *out, size_t *count);

/* Reads one ClientHello extension's data into hello; returns 0, EINVAL or ENOBUFS. */
static int __attribute__((unused)) tlspeer_parse_extension(struct tlspeer_hello *hello, uint32_t type, struct tlspeer_reader *data);

/* Writes the n - 2 of key's curve as ec.size big-endian bytes. */
static void __attribute__((unused)) tlspeer_order_minus_two(const struct tlspeer_key *key, uint8_t *out);

/* Makes the fixed per-message k for a hash. */
static int __attribute__((unused)) tlspeer_fixed_k(const struct tlspeer_key *key, const uint8_t *hash, size_t hash_size, unsigned counter, uint8_t *k);

/* Writes a Name of one commonName. */
static void __attribute__((unused)) tlspeer_der_name(struct der_writer *writer, const char *common_name);

/* Writes one Extension: extnID, critical when 1, extnValue. */
static void __attribute__((unused)) tlspeer_der_extension(struct der_writer *writer, const char *oid, int critical, const struct der_writer *value);

/* Writes a key's SubjectPublicKeyInfo. */
static void __attribute__((unused)) tlspeer_der_spki(struct der_writer *writer, const struct tlspeer_key *key);

/* Writes the AlgorithmIdentifier of ECDSA with the hash of key's curve. */
static void __attribute__((unused)) tlspeer_der_sigalg(struct der_writer *writer, const struct tlspeer_key *key);

/* Writes the key identifier of a key (20 bytes) to out. */
static int __attribute__((unused)) tlspeer_key_id(const struct tlspeer_key *key, uint8_t *out);

/* Writes the extensions block [3] of a certificate. */
static int __attribute__((unused)) tlspeer_der_extensions(struct der_writer *writer, const struct tlspeer_cert *spec);

/* Records the first failure and moves to TLSPEER_FAILED; returns error. */
static int __attribute__((unused)) tlspeer_fail(struct tlspeer *peer, int error, const char *why);

/* Ends the script (the client is expected to abort); returns 0. */
static int __attribute__((unused)) tlspeer_stop(struct tlspeer *peer, const char *why);

/* Starts writing an item into the flight buffer (resetting it when everything was taken). */
static void __attribute__((unused)) tlspeer_item_begin(struct tlspeer *peer, struct tlspeer_writer *writer);

/* Registers the bytes writer holds as an item; absorbs a handshake message into the transcript when absorb is 1. */
static int __attribute__((unused)) tlspeer_item_commit(struct tlspeer *peer, struct tlspeer_writer *writer, unsigned kind, unsigned type, int absorb);

/* Adds an item of kind with a copy of size bytes. */
static int __attribute__((unused)) tlspeer_item_add(struct tlspeer *peer, unsigned kind, unsigned type, const uint8_t *data, size_t size, int absorb);

/* Adds new write keys and makes them the epoch of the next items. */
static int __attribute__((unused)) tlspeer_add_epoch(struct tlspeer *peer, const struct sslc_traffic_keys *keys, int version13);

/* Writes the transcript hash so far into out (hash_size bytes). */
static void __attribute__((unused)) tlspeer_transcript_hash(const struct tlspeer *peer, uint8_t *out);

/* Replaces the transcript of ClientHello1 by message_hash (RFC 8446 4.4.1). */
static void __attribute__((unused)) tlspeer_transcript_restart(struct tlspeer *peer);

/* Finds the client's key share of group; returns it or NULL. */
static const struct tlspeer_share * __attribute__((unused)) tlspeer_find_share(const struct tlspeer *peer, uint16_t group);

/* Computes the (EC)DH shared secret with the client's public key. */
static int __attribute__((unused)) tlspeer_compute_shared(struct tlspeer *peer, const uint8_t *client_key, size_t client_key_size);

/* Fills peer->server_share with the server's public key (or the broken one). */
static int __attribute__((unused)) tlspeer_make_share(struct tlspeer *peer);

/* Signs content with the configured key under the configured scheme's hash, into out as DER. */
static int __attribute__((unused)) tlspeer_sign_content(struct tlspeer *peer, uint8_t *content, size_t content_size, uint8_t *out, size_t room, size_t *written);

/* Writes the extra extension when extra_at has where. */
static void __attribute__((unused)) tlspeer_put_extra(const struct tlspeer *peer, struct tlspeer_writer *writer, uint32_t where);

/* Sends a HelloRetryRequest. */
static int __attribute__((unused)) tlspeer_send_retry(struct tlspeer *peer);

/* Answers a TLS 1.3 ClientHello with the whole server flight. */
static int __attribute__((unused)) tlspeer_flight13(struct tlspeer *peer);

/* Answers a TLS 1.2 ClientHello with ServerHello to ServerHelloDone. */
static int __attribute__((unused)) tlspeer_flight12(struct tlspeer *peer);

/* Writes a Certificate message of the chain (TLS 1.3 when tls13 is 1). */
static int __attribute__((unused)) tlspeer_send_certificate(struct tlspeer *peer, int tls13);

/* Takes a ClientHello. */
static int __attribute__((unused)) tlspeer_on_hello(struct tlspeer *peer, const uint8_t *message, size_t size);

/* Takes the client's (empty) Certificate. */
static int __attribute__((unused)) tlspeer_on_certificate(struct tlspeer *peer, const uint8_t *message, size_t size);

/* Takes a TLS 1.2 ClientKeyExchange. */
static int __attribute__((unused)) tlspeer_on_key_exchange(struct tlspeer *peer, const uint8_t *message, size_t size);

/* Takes the client's Finished. */
static int __attribute__((unused)) tlspeer_on_finished(struct tlspeer *peer, const uint8_t *message, size_t size);

/* Takes a TLS 1.3 KeyUpdate after the handshake. */
static int __attribute__((unused)) tlspeer_on_key_update(struct tlspeer *peer, const uint8_t *message, size_t size);

/* Sets the read keys the client's next records use. */
static int __attribute__((unused)) tlspeer_set_read(struct tlspeer *peer, const struct sslc_traffic_keys *keys, int version13);

/* Seals one record of type with size bytes under the write dir into out. */
static int __attribute__((unused)) tlspeer_seal(struct tlspeer *peer, unsigned type, const uint8_t *data, size_t size);

/* Seals size bytes of type as records of at most record_size bytes. */
static int __attribute__((unused)) tlspeer_seal_split(struct tlspeer *peer, unsigned type, const uint8_t *data, size_t size);

/* Moves the write dir to epoch. */
static int __attribute__((unused)) tlspeer_write_epoch(struct tlspeer *peer, unsigned epoch);

/* Takes one record's content of type (plaintext or opened). */
static int __attribute__((unused)) tlspeer_on_content(struct tlspeer *peer, unsigned type, const uint8_t *content, size_t size, int protected_record);

/* Takes one whole record from peer->in. */
static int __attribute__((unused)) tlspeer_on_record(struct tlspeer *peer, uint8_t *record, size_t size);

/*
 * The special Random of a HelloRetryRequest and the downgrade marks
 * (the last eight bytes of a ServerHello random).
 * Values referenced: RFC 8446 section 4.1.3.
 */
static const uint8_t tlspeer_retry_random[32] __attribute__((unused)) = {
	0xcf, 0x21, 0xad, 0x74, 0xe5, 0x9a, 0x61, 0x11,
	0xbe, 0x1d, 0x8c, 0x02, 0x1e, 0x65, 0xb8, 0x91,
	0xc2, 0xa2, 0x11, 0x16, 0x7a, 0xbb, 0x8c, 0x5e,
	0x07, 0x9e, 0x09, 0xe2, 0xc8, 0xa8, 0x33, 0x9c
};
static const uint8_t tlspeer_downgrade[8] __attribute__((unused)) = {
	0x44, 0x4f, 0x57, 0x4e, 0x47, 0x52, 0x44, 0x01
};

/*
 * The context string of a server CertificateVerify (64 spaces, this
 * string and a zero byte precede the transcript hash).
 * Value referenced: RFC 8446 section 4.4.3.
 */
static const char tlspeer_cv_context[] __attribute__((unused)) = "TLS 1.3, server CertificateVerify";

/*
 * The object identifiers of the certificates: ecdsa-with-SHA256 and
 * -SHA384 (RFC 5758 3.2), id-ecPublicKey and the named curves
 * secp256r1 and secp384r1 (RFC 5480 2.1.1), commonName (X.520), and the
 * extensions basicConstraints, keyUsage, extKeyUsage with
 * id-kp-serverAuth, subjectAltName, subjectKeyIdentifier and
 * authorityKeyIdentifier (RFC 5280 4.2.1).
 * Values referenced: RFC 5758 section 3.2, RFC 5480 section 2.1.1, RFC
 * 5280 sections 4.1.2.4 and 4.2.1.
 */
#define TLSPEER_OID_ECDSA_SHA256	"1.2.840.10045.4.3.2"
#define TLSPEER_OID_ECDSA_SHA384	"1.2.840.10045.4.3.3"
#define TLSPEER_OID_EC_PUBLIC_KEY	"1.2.840.10045.2.1"
#define TLSPEER_OID_SECP256R1		"1.2.840.10045.3.1.7"
#define TLSPEER_OID_SECP384R1		"1.3.132.0.34"
#define TLSPEER_OID_COMMON_NAME		"2.5.4.3"
#define TLSPEER_OID_BASIC_CONSTRAINTS	"2.5.29.19"
#define TLSPEER_OID_KEY_USAGE		"2.5.29.15"
#define TLSPEER_OID_EXT_KEY_USAGE	"2.5.29.37"
#define TLSPEER_OID_SERVER_AUTH		"1.3.6.1.5.5.7.3.1"
#define TLSPEER_OID_SUBJECT_ALT_NAME	"2.5.29.17"
#define TLSPEER_OID_SUBJECT_KEY_ID	"2.5.29.14"
#define TLSPEER_OID_AUTHORITY_KEY_ID	"2.5.29.35"

/*
 * The writer and the reader.
 */

/* Starts a writer over room bytes. */
static void __attribute__((unused))
tlspeer_writer_init(
	struct tlspeer_writer *writer,
	uint8_t *data,
	size_t room)
{
	/* Empty, nothing open. */
	writer->data = data;
	writer->room = room;
	writer->used = 0;
	writer->depth = 0;
	writer->error = 0;
}

/* Writes size bytes. */
static void __attribute__((unused))
tlspeer_put(
	struct tlspeer_writer *writer,
	const uint8_t *data,
	size_t size)
{
	/* All or nothing. */
	if (writer->error != 0 || size > writer->room - writer->used) {
		writer->error = ENOBUFS;
		return;
	}
	if (size > 0U)
		memcpy(writer->data + writer->used, data, size);
	writer->used += size;
}

/* Writes the low bytes of value big-endian in count bytes (1 to 4). */
static void __attribute__((unused))
tlspeer_put_int(
	struct tlspeer_writer *writer,
	uint32_t value,
	unsigned count)
{
	uint8_t bytes[4];
	unsigned index;

	/* Most significant byte first. */
	for (index = 0; index < count && index < 4U; index++)
		bytes[index] = (uint8_t)(value >> (8U * (count - 1U - index)));
	tlspeer_put(writer, bytes, count);
}

/* Begins a vector with a length prefix of prefix bytes. */
static void __attribute__((unused))
tlspeer_begin(
	struct tlspeer_writer *writer,
	unsigned prefix)
{
	/* Too deep is the tool's mistake. */
	if (writer->depth >= TLSPEER_WRITER_DEPTH) {
		writer->error = ENOBUFS;
		return;
	}

	/* The prefix's room, filled by tlspeer_end. */
	writer->open[writer->depth] = writer->used;
	writer->prefix[writer->depth] = prefix;
	writer->depth++;
	tlspeer_put_int(writer, 0, prefix);
}

/* Ends the innermost vector and fills its length. */
static void __attribute__((unused))
tlspeer_end(
	struct tlspeer_writer *writer)
{
	size_t start;
	size_t length;
	unsigned prefix;
	unsigned index;

	/* Nothing open, or an earlier error. */
	if (writer->error != 0 || writer->depth == 0U) {
		writer->error = EINVAL;
		return;
	}

	/* The length of what follows the prefix, which must fit it. */
	writer->depth--;
	start = writer->open[writer->depth];
	prefix = writer->prefix[writer->depth];
	length = writer->used - start - prefix;
	if (prefix < 4U && length >= ((size_t)1 << (8U * prefix))) {
		writer->error = EINVAL;
		return;
	}

	/* Most significant byte first. */
	for (index = 0; index < prefix; index++)
		writer->data[start + index] = (uint8_t)(length >> (8U * (prefix - 1U - index)));
}

/* Writes one extension: type and data<0..2^16-1>. */
static void __attribute__((unused))
tlspeer_put_extension(
	struct tlspeer_writer *writer,
	uint32_t type,
	const uint8_t *data,
	size_t size)
{
	/* Extension { type, extension_data<0..2^16-1> } (RFC 8446 4.2). */
	tlspeer_put_int(writer, type, 2);
	tlspeer_begin(writer, 2);
	tlspeer_put(writer, data, size);
	tlspeer_end(writer);
}

/* Starts a reader over size bytes. */
static void __attribute__((unused))
tlspeer_reader_init(
	struct tlspeer_reader *reader,
	const uint8_t *data,
	size_t size)
{
	/* At the start, no error. */
	reader->data = data;
	reader->size = size;
	reader->position = 0;
	reader->error = 0;
}

/* Reads a big-endian integer of count bytes (1 to 3). */
static uint32_t __attribute__((unused))
tlspeer_get_int(
	struct tlspeer_reader *reader,
	unsigned count)
{
	const uint8_t *bytes;
	uint32_t value;
	unsigned index;

	/* The bytes, or zero after an error. */
	bytes = tlspeer_get(reader, count);
	if (bytes == NULL)
		return 0;

	/* Most significant byte first. */
	value = 0;
	for (index = 0; index < count; index++)
		value = (value << 8) | bytes[index];

	/* The value read. */
	return value;
}

/* Gives a view of the next size bytes, or NULL (error set). */
static const uint8_t * __attribute__((unused))
tlspeer_get(
	struct tlspeer_reader *reader,
	size_t size)
{
	const uint8_t *view;

	/* Past the end, or an earlier error. */
	if (reader->error != 0 || size > reader->size - reader->position) {
		reader->error = EINVAL;
		return NULL;
	}

	/* An empty reader has no bytes to point at (no error: size is 0). */
	if (reader->data == NULL)
		return NULL;

	/* The view, then past it. */
	view = reader->data + reader->position;
	reader->position += size;
	return view;
}

/* Reads a vector with a prefix of prefix bytes and a length in [min, max] into inner. */
static void __attribute__((unused))
tlspeer_get_vector(
	struct tlspeer_reader *reader,
	unsigned prefix,
	size_t min,
	size_t max,
	struct tlspeer_reader *inner)
{
	size_t length;
	const uint8_t *view;

	/* The length, within its range. */
	tlspeer_reader_init(inner, NULL, 0);
	length = tlspeer_get_int(reader, prefix);
	if (reader->error == 0 && (length < min || length > max))
		reader->error = EINVAL;
	if (reader->error != 0) {
		inner->error = EINVAL;
		return;
	}

	/* The bytes (an empty vector of an empty reader has no view). */
	view = tlspeer_get(reader, length);
	if (reader->error != 0) {
		inner->error = EINVAL;
		return;
	}
	tlspeer_reader_init(inner, view, length);
}

/* Returns 1 when the reader has no error and nothing left. */
static int __attribute__((unused))
tlspeer_reader_done(
	const struct tlspeer_reader *reader)
{
	/* An error, or bytes left over. */
	if (reader->error != 0)
		return 0;
	if (reader->position != reader->size)
		return 0;

	/* Read exactly. */
	return 1;
}

/* Reads a list of 2-byte values of a vector into out; returns 0, EINVAL or ENOBUFS. */
static int __attribute__((unused))
tlspeer_get_list16(
	struct tlspeer_reader *reader,
	unsigned prefix,
	size_t min,
	size_t max,
	uint16_t *out,
	size_t *count)
{
	struct tlspeer_reader list;

	/* The vector, of whole 2-byte values. */
	tlspeer_get_vector(reader, prefix, min, max, &list);
	if (list.error != 0)
		return EINVAL;
	if (list.size % 2U != 0U)
		return EINVAL;
	if (list.size / 2U > TLSPEER_LIST_MAX)
		return ENOBUFS;

	/* Each value. */
	*count = 0;
	while (list.position < list.size) {
		out[*count] = (uint16_t)tlspeer_get_int(&list, 2);
		(*count)++;
	}

	/* Read. */
	return 0;
}

/*
 * The ClientHello.
 */

/* Reads one ClientHello extension's data into hello; returns 0, EINVAL or ENOBUFS. */
static int __attribute__((unused))
tlspeer_parse_extension(
	struct tlspeer_hello *hello,
	uint32_t type,
	struct tlspeer_reader *data)
{
	struct tlspeer_reader list;
	struct tlspeer_reader entry;
	const uint8_t *bytes;
	uint32_t name_type;
	struct tlspeer_share *share;
	int error;

	/* Each known extension by its syntax; others are only recorded. */
	error = 0;
	switch (type) {
	case SSLC_EXT_SERVER_NAME:
		/* ServerNameList server_name_list<1..2^16-1> of one host_name (RFC 6066 3). */
		tlspeer_get_vector(data, 2, 1, 65535, &list);
		while (list.error == 0 && list.position < list.size) {
			name_type = tlspeer_get_int(&list, 1);
			tlspeer_get_vector(&list, 2, 1, 65535, &entry);
			if (entry.error != 0 || name_type != 0U || hello->has_server_name != 0) {
				list.error = EINVAL;
				break;
			}
			if (entry.size > 255U) {
				error = ENOBUFS;
				break;
			}
			if (memchr(entry.data, 0, entry.size) != NULL) {
				list.error = EINVAL;
				break;
			}
			memcpy(hello->server_name, entry.data, entry.size);
			hello->server_name[entry.size] = '\0';
			hello->has_server_name = 1;
		}
		if (list.error != 0)
			error = EINVAL;
		break;
	case SSLC_EXT_SUPPORTED_GROUPS:
		/* NamedGroup named_group_list<2..2^16-1> (RFC 8446 4.2.7). */
		error = tlspeer_get_list16(data, 2, 2, 65535, hello->groups, &hello->group_count);
		break;
	case SSLC_EXT_EC_POINT_FORMATS:
		/* ECPointFormat ec_point_format_list<1..2^8-1> (RFC 8422 5.1.2). */
		tlspeer_get_vector(data, 1, 1, 255, &list);
		if (list.error != 0) {
			error = EINVAL;
			break;
		}
		if (list.size > TLSPEER_LIST_MAX) {
			error = ENOBUFS;
			break;
		}
		memcpy(hello->point_formats, list.data, list.size);
		hello->point_format_count = list.size;
		break;
	case SSLC_EXT_SIGNATURE_ALGORITHMS:
		/* SignatureScheme supported_signature_algorithms<2..2^16-2> (RFC 8446 4.2.3). */
		error = tlspeer_get_list16(data, 2, 2, 65534, hello->schemes, &hello->scheme_count);
		break;
	case SSLC_EXT_EXTENDED_MASTER_SECRET:
		/* Empty (RFC 7627 5.1). */
		hello->has_extended_master_secret = 1;
		break;
	case SSLC_EXT_SUPPORTED_VERSIONS:
		/* ProtocolVersion versions<2..254> (RFC 8446 4.2.1). */
		error = tlspeer_get_list16(data, 1, 2, 254, hello->versions, &hello->version_count);
		break;
	case SSLC_EXT_COOKIE:
		/* opaque cookie<1..2^16-1> (RFC 8446 4.2.2). */
		tlspeer_get_vector(data, 2, 1, 65535, &list);
		if (list.error != 0) {
			error = EINVAL;
			break;
		}
		if (list.size > TLSPEER_COOKIE_MAX) {
			error = ENOBUFS;
			break;
		}
		memcpy(hello->cookie, list.data, list.size);
		hello->cookie_size = list.size;
		hello->has_cookie = 1;
		break;
	case SSLC_EXT_KEY_SHARE:
		/* KeyShareEntry client_shares<0..2^16-1>, each group and key_exchange<1..2^16-1> (RFC 8446 4.2.8). */
		hello->has_key_share = 1;
		tlspeer_get_vector(data, 2, 0, 65535, &list);
		while (list.error == 0 && list.position < list.size) {
			if (hello->share_count >= TLSPEER_SHARES_MAX) {
				error = ENOBUFS;
				break;
			}
			share = &hello->shares[hello->share_count];
			share->group = (uint16_t)tlspeer_get_int(&list, 2);
			tlspeer_get_vector(&list, 2, 1, 65535, &entry);
			if (entry.error != 0)
				break;
			if (entry.size > TLSPEER_SHARE_KEY_MAX) {
				error = ENOBUFS;
				break;
			}
			memcpy(share->key, entry.data, entry.size);
			share->key_size = entry.size;
			hello->share_count++;
		}
		if (error == 0 && list.error != 0)
			error = EINVAL;
		break;
	case SSLC_EXT_RENEGOTIATION_INFO:
		/* opaque renegotiated_connection<0..255> (RFC 5746 3.2). */
		tlspeer_get_vector(data, 1, 0, 255, &list);
		if (list.error != 0) {
			error = EINVAL;
			break;
		}
		if (list.size > 0U)
			memcpy(hello->renegotiation_info, list.data, list.size);
		hello->renegotiation_info_size = list.size;
		hello->has_renegotiation_info = 1;
		break;
	default:
		/* Recorded by type only; its data is not read. */
		bytes = tlspeer_get(data, data->size - data->position);
		if (bytes == NULL && data->error != 0)
			error = EINVAL;
		break;
	}

	/* An extension's data must be read exactly. */
	if (error != 0)
		return error;
	if (!tlspeer_reader_done(data))
		return EINVAL;

	/* Read. */
	return 0;
}

/*
 * Reads a whole ClientHello message into hello (RFC 8446 4.1.2: legacy_
 * version, random, legacy_session_id<0..32>, cipher_suites<2..2^16-2>,
 * legacy_compression_methods<1..2^8-1>, extensions<0..2^16-1>).
 */
static int __attribute__((unused))
tlspeer_parse_client_hello(
	const uint8_t *message,
	size_t size,
	struct tlspeer_hello *hello)
{
	struct tlspeer_reader reader;
	struct tlspeer_reader body;
	struct tlspeer_reader vector;
	struct tlspeer_reader extensions;
	struct tlspeer_reader data;
	const uint8_t *bytes;
	uint32_t type;
	size_t index;
	int error;

	/* Nothing is kept from an earlier hello. */
	memset(hello, 0, sizeof(*hello));

	/* The handshake header: type 1 and a length that is the rest. */
	tlspeer_reader_init(&reader, message, size);
	type = tlspeer_get_int(&reader, 1);
	tlspeer_get_vector(&reader, 3, 0, 0xffffffU, &body);
	if (body.error != 0 || type != SSLC_HS_CLIENT_HELLO)
		return EINVAL;
	if (!tlspeer_reader_done(&reader))
		return EINVAL;

	/* legacy_version and random. */
	hello->legacy_version = (uint16_t)tlspeer_get_int(&body, 2);
	bytes = tlspeer_get(&body, 32);
	if (bytes == NULL)
		return EINVAL;
	memcpy(hello->random, bytes, 32);

	/* legacy_session_id<0..32>. */
	tlspeer_get_vector(&body, 1, 0, 32, &vector);
	if (vector.error != 0)
		return EINVAL;
	if (vector.size > 0U)
		memcpy(hello->session_id, vector.data, vector.size);
	hello->session_id_size = vector.size;

	/* cipher_suites<2..2^16-2>, and the renegotiation SCSV among them. */
	error = tlspeer_get_list16(&body, 2, 2, 65534, hello->suites, &hello->suite_count);
	if (error != 0)
		return error;
	for (index = 0; index < hello->suite_count; index++) {
		if (hello->suites[index] == SSLC_SUITE_EMPTY_RENEGOTIATION_INFO_SCSV)
			hello->has_scsv = 1;
	}

	/* legacy_compression_methods<1..2^8-1>. */
	tlspeer_get_vector(&body, 1, 1, 255, &vector);
	if (vector.error != 0)
		return EINVAL;
	if (vector.size > TLSPEER_LIST_MAX)
		return ENOBUFS;
	memcpy(hello->compressions, vector.data, vector.size);
	hello->compression_count = vector.size;

	/* No extensions block at all (allowed in TLS 1.2, RFC 5246 7.4.1.2). */
	if (body.position == body.size)
		return 0;

	/* The extensions block, which ends the body. */
	tlspeer_get_vector(&body, 2, 0, 65535, &extensions);
	if (extensions.error != 0)
		return EINVAL;
	if (!tlspeer_reader_done(&body))
		return EINVAL;
	hello->has_extensions = 1;

	/* Each extension, none twice. */
	while (extensions.position < extensions.size) {
		type = tlspeer_get_int(&extensions, 2);
		tlspeer_get_vector(&extensions, 2, 0, 65535, &data);
		if (data.error != 0)
			return EINVAL;
		for (index = 0; index < hello->extension_count; index++) {
			if (hello->extensions[index] == type)
				return EINVAL;
		}
		if (hello->extension_count >= TLSPEER_EXT_MAX)
			return ENOBUFS;
		hello->extensions[hello->extension_count] = (uint16_t)type;
		hello->extension_count++;
		error = tlspeer_parse_extension(hello, type, &data);
		if (error != 0)
			return error;
	}

	/* Read. */
	return 0;
}

/* Returns 1 when hello carries an extension of type, else 0. */
static int __attribute__((unused))
tlspeer_hello_has(
	const struct tlspeer_hello *hello,
	uint16_t type)
{
	size_t index;

	/* A search of the types in order. */
	for (index = 0; index < hello->extension_count; index++) {
		if (hello->extensions[index] == type)
			return 1;
	}

	/* Absent. */
	return 0;
}

/*
 * The signer.
 */

/* Prepares key on curve with the private key d and computes Q = d G. */
static int __attribute__((unused))
tlspeer_key_init(
	struct tlspeer_key *key,
	enum cryc_curve_id curve,
	const uint8_t *private_key)
{
	uint32_t d[CRYC_ECP_MAX_LIMBS];
	struct cryc_ecp_point point;
	uint32_t below;
	uint32_t zero;
	int error;

	/* The curve. */
	memset(key, 0, sizeof(*key));
	error = cryc_ecp_init(&key->ec, curve);
	if (error != 0)
		return error;

	/* d, in [1, n - 1]. */
	error = cryc_bn_decode(d, key->ec.limbs, private_key, key->ec.size);
	if (error != 0)
		return EINVAL;
	below = cryc_bn_lt(d, key->ec.order.modulus, key->ec.limbs);
	zero = cryc_bn_is_zero(d, key->ec.limbs);
	if (below == 0U || zero != 0U)
		return EINVAL;
	memcpy(key->private_key, private_key, key->ec.size);

	/* Q = d G, uncompressed. */
	cryc_ecp_mul_base(&key->ec, &point, d);
	key->public_size = 1U + 2U * key->ec.size;
	error = cryc_ecp_encode(&key->ec, &point, key->public_key, sizeof(key->public_key));
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Prepares a fixed key made from a label. */
static int __attribute__((unused))
tlspeer_key_from_label(
	struct tlspeer_key *key,
	enum cryc_curve_id curve,
	const char *label)
{
	uint8_t digest[CRYC_HASH_MAX_SIZE];
	enum cryc_hash_id hash;
	int error;

	/* The hash of the curve's size. */
	hash = CRYC_HASH_SHA256;
	if (curve == CRYC_CURVE_P384)
		hash = CRYC_HASH_SHA384;

	/* d is the label's digest. */
	error = cryc_hash_digest(hash, (const uint8_t *)label, strlen(label), digest);
	if (error != 0)
		return error;
	error = tlspeer_key_init(key, curve, digest);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Writes the n - 2 of key's curve as ec.size big-endian bytes. */
static void __attribute__((unused))
tlspeer_order_minus_two(
	const struct tlspeer_key *key,
	uint8_t *out)
{
	size_t index;
	unsigned borrow;
	unsigned value;

	/* n, then 2 subtracted from the last byte with the borrow carried. */
	cryc_bn_encode(key->ec.order.modulus, key->ec.limbs, out, key->ec.size);
	borrow = 2;
	for (index = key->ec.size; index > 0U && borrow != 0U; index--) {
		value = out[index - 1U];
		out[index - 1U] = (uint8_t)(value - borrow);
		borrow = value < borrow ? 1U : 0U;
	}
}

/* Makes the fixed per-message k for a hash. */
static int __attribute__((unused))
tlspeer_fixed_k(
	const struct tlspeer_key *key,
	const uint8_t *hash,
	size_t hash_size,
	unsigned counter,
	uint8_t *k)
{
	struct cryc_hmac hmac;
	uint8_t mac[CRYC_HASH_MAX_SIZE];
	uint8_t count;
	uint32_t value[CRYC_ECP_MAX_LIMBS];
	enum cryc_hash_id id;
	int error;

	/* HMAC of the curve's size, keyed with d, over the hash and the counter. */
	id = CRYC_HASH_SHA256;
	if (key->ec.size == 48U)
		id = CRYC_HASH_SHA384;
	error = cryc_hmac_init(&hmac, id, key->private_key, key->ec.size);
	if (error != 0)
		return error;
	count = (uint8_t)counter;
	cryc_hmac_update(&hmac, hash, hash_size);
	cryc_hmac_update(&hmac, &count, 1);
	cryc_hmac_final(&hmac, mac);

	/* Reduced mod n once (the MAC is below 2^bits(n) < 2n). */
	error = cryc_bn_decode(value, key->ec.limbs, mac, key->ec.size);
	if (error != 0)
		return error;
	cryc_bn_reduce_once(value, value, &key->ec.order);
	cryc_bn_encode(value, key->ec.limbs, k, key->ec.size);

	/* Made (a zero k is refused by the signer, which then tries the next counter). */
	return 0;
}

/* Signs a hash with ECDSA (FIPS 186-5 6.4.1). */
static int __attribute__((unused))
tlspeer_ecdsa_sign(
	const struct tlspeer_key *key,
	const uint8_t *hash,
	size_t hash_size,
	const uint8_t *k,
	uint8_t *r,
	uint8_t *s)
{
	const struct cryc_ecp *ec;
	const struct cryc_bn_mont *order;
	uint32_t e[CRYC_ECP_MAX_LIMBS];
	uint32_t k_value[CRYC_ECP_MAX_LIMBS];
	uint32_t k_mont[CRYC_ECP_MAX_LIMBS];
	uint32_t k_inverse[CRYC_ECP_MAX_LIMBS];
	uint32_t r_value[CRYC_ECP_MAX_LIMBS];
	uint32_t r_mont[CRYC_ECP_MAX_LIMBS];
	uint32_t d[CRYC_ECP_MAX_LIMBS];
	uint32_t rd[CRYC_ECP_MAX_LIMBS];
	uint32_t sum[CRYC_ECP_MAX_LIMBS];
	uint32_t s_value[CRYC_ECP_MAX_LIMBS];
	uint8_t x[CRYC_ECP_MAX_BYTES];
	uint8_t exponent[CRYC_ECP_MAX_BYTES];
	uint8_t fixed[CRYC_ECP_MAX_BYTES];
	struct cryc_ecp_point point;
	size_t e_size;
	uint32_t below;
	uint32_t zero;
	unsigned counter;
	int error;

	/* The curve and its order. */
	ec = &key->ec;
	order = &ec->order;

	/* Step 3: E is the leftmost bits(n) bits (bits(n) is a whole number of bytes here), reduced mod n. */
	e_size = hash_size;
	if (e_size > ec->size)
		e_size = ec->size;
	error = cryc_bn_decode(e, ec->limbs, hash, e_size);
	if (error != 0)
		return EINVAL;
	cryc_bn_reduce_once(e, e, order);

	/* k: the caller's, or the fixed one (the next counter when it gives zero). */
	counter = 0;
	if (k == NULL) {
		do {
			error = tlspeer_fixed_k(key, hash, hash_size, counter, fixed);
			if (error != 0)
				return error;
			error = cryc_bn_decode(k_value, ec->limbs, fixed, ec->size);
			if (error != 0)
				return error;
			zero = cryc_bn_is_zero(k_value, ec->limbs);
			counter++;
		} while (zero != 0U && counter < 8U);
	} else {
		error = cryc_bn_decode(k_value, ec->limbs, k, ec->size);
		if (error != 0)
			return EINVAL;
	}

	/* k in [1, n - 1]. */
	below = cryc_bn_lt(k_value, order->modulus, ec->limbs);
	zero = cryc_bn_is_zero(k_value, ec->limbs);
	if (below == 0U || zero != 0U)
		return EINVAL;

	/* Steps 5 to 7: R = k G, r = x(R) mod n (x < p < 2n), not zero. */
	cryc_ecp_mul_base(ec, &point, k_value);
	error = cryc_ecp_affine_x(ec, &point, x);
	if (error != 0)
		return EINVAL;
	error = cryc_bn_decode(r_value, ec->limbs, x, ec->size);
	if (error != 0)
		return EINVAL;
	cryc_bn_reduce_once(r_value, r_value, order);
	zero = cryc_bn_is_zero(r_value, ec->limbs);
	if (zero != 0U)
		return EINVAL;

	/* Step 4: k^-1 = k^(n - 2) mod n (n is prime), kept in Montgomery form. */
	tlspeer_order_minus_two(key, exponent);
	cryc_bn_to_mont(k_mont, k_value, order);
	cryc_bn_mont_exp(k_inverse, k_mont, exponent, ec->size, order);

	/* Step 8: s = k^-1 (E + r d) mod n; a Montgomery form times a plain value gives a plain value. */
	error = cryc_bn_decode(d, ec->limbs, key->private_key, ec->size);
	if (error != 0)
		return EINVAL;
	cryc_bn_to_mont(r_mont, r_value, order);
	cryc_bn_mont_mul(rd, r_mont, d, order);
	cryc_bn_mod_add(sum, e, rd, order);
	cryc_bn_mont_mul(s_value, k_inverse, sum, order);
	zero = cryc_bn_is_zero(s_value, ec->limbs);
	if (zero != 0U)
		return EINVAL;

	/* Step 11: (r, s). */
	cryc_bn_encode(r_value, ec->limbs, r, ec->size);
	cryc_bn_encode(s_value, ec->limbs, s, ec->size);

	/* Succeeded. */
	return 0;
}

/* Signs a hash with the fixed k and writes the DER Ecdsa-Sig-Value. */
static int __attribute__((unused))
tlspeer_ecdsa_sign_der(
	const struct tlspeer_key *key,
	const uint8_t *hash,
	size_t hash_size,
	uint8_t *out,
	size_t room,
	size_t *written)
{
	uint8_t r[CRYC_ECP_MAX_BYTES];
	uint8_t s[CRYC_ECP_MAX_BYTES];
	unsigned char inner_bytes[2U * (CRYC_ECP_MAX_BYTES + 3U)];
	struct der_writer inner;
	struct der_writer outer;
	int error;

	/* r and s. */
	*written = 0;
	error = tlspeer_ecdsa_sign(key, hash, hash_size, NULL, r, s);
	if (error != 0)
		return error;

	/* SEQUENCE { r INTEGER, s INTEGER }. */
	der_init(&inner, inner_bytes, sizeof(inner_bytes));
	der_unsigned(&inner, r, key->ec.size);
	der_unsigned(&inner, s, key->ec.size);
	der_init(&outer, out, room);
	der_wrap(&outer, 0x30U, &inner);
	if (outer.error != 0)
		return ENOBUFS;

	/* Succeeded. */
	*written = outer.used;
	return 0;
}

/*
 * The certificates.
 */

/* Writes a Name of one commonName (RFC 5280 4.1.2.4: RDNSequence of one RDN of one AttributeTypeAndValue). */
static void __attribute__((unused))
tlspeer_der_name(
	struct der_writer *writer,
	const char *common_name)
{
	unsigned char atv_bytes[320];
	unsigned char attribute_bytes[330];
	unsigned char rdn_bytes[340];
	struct der_writer atv;
	struct der_writer attribute;
	struct der_writer rdn;

	/* type and a UTF8String value, in a SEQUENCE, in a SET, in a SEQUENCE. */
	der_init(&atv, atv_bytes, sizeof(atv_bytes));
	der_oid(&atv, TLSPEER_OID_COMMON_NAME);
	der_put(&atv, 0x0cU, (const unsigned char *)common_name, strlen(common_name));
	der_init(&attribute, attribute_bytes, sizeof(attribute_bytes));
	der_wrap(&attribute, 0x30U, &atv);
	der_init(&rdn, rdn_bytes, sizeof(rdn_bytes));
	der_wrap(&rdn, 0x31U, &attribute);
	der_wrap(writer, 0x30U, &rdn);
}

/* Writes one Extension: extnID, critical when 1, extnValue (RFC 5280 4.1). */
static void __attribute__((unused))
tlspeer_der_extension(
	struct der_writer *writer,
	const char *oid,
	int critical,
	const struct der_writer *value)
{
	static const unsigned char true_value[1] = { 0xff };
	unsigned char extension_bytes[512];
	struct der_writer extension;

	/* extnID, critical (DEFAULT FALSE: written only when TRUE), extnValue OCTET STRING. */
	der_init(&extension, extension_bytes, sizeof(extension_bytes));
	der_oid(&extension, oid);
	if (critical != 0)
		der_put(&extension, 0x01U, true_value, 1);
	der_wrap(&extension, 0x04U, value);
	der_wrap(writer, 0x30U, &extension);
}

/* Writes a key's SubjectPublicKeyInfo (RFC 5480 2: id-ecPublicKey with the named curve, the point as the BIT STRING). */
static void __attribute__((unused))
tlspeer_der_spki(
	struct der_writer *writer,
	const struct tlspeer_key *key)
{
	unsigned char algorithm_bytes[64];
	unsigned char spki_bytes[192];
	unsigned char bits[1U + CRYC_ECP_MAX_POINT];
	struct der_writer spki;
	struct der_writer inner;

	/* AlgorithmIdentifier { id-ecPublicKey, namedCurve }. */
	der_init(&inner, algorithm_bytes, sizeof(algorithm_bytes));
	der_oid(&inner, TLSPEER_OID_EC_PUBLIC_KEY);
	if (key->ec.size == 48U)
		der_oid(&inner, TLSPEER_OID_SECP384R1);
	else
		der_oid(&inner, TLSPEER_OID_SECP256R1);

	/* The point with no unused bits. */
	bits[0] = 0;
	memcpy(bits + 1, key->public_key, key->public_size);
	der_init(&spki, spki_bytes, sizeof(spki_bytes));
	der_wrap(&spki, 0x30U, &inner);
	der_put(&spki, 0x03U, bits, 1U + key->public_size);
	der_wrap(writer, 0x30U, &spki);
}

/* Writes the AlgorithmIdentifier of ECDSA with the hash of key's curve (RFC 5758 3.2: no parameters). */
static void __attribute__((unused))
tlspeer_der_sigalg(
	struct der_writer *writer,
	const struct tlspeer_key *key)
{
	unsigned char inner_bytes[32];
	struct der_writer inner;

	/* SEQUENCE { the OID }. */
	der_init(&inner, inner_bytes, sizeof(inner_bytes));
	if (key->ec.size == 48U)
		der_oid(&inner, TLSPEER_OID_ECDSA_SHA384);
	else
		der_oid(&inner, TLSPEER_OID_ECDSA_SHA256);
	der_wrap(writer, 0x30U, &inner);
}

/* Writes the key identifier of a key (20 bytes) to out (RFC 5280 4.2.1.2 allows any method; this is SHA-256 of the point, cut). */
static int __attribute__((unused))
tlspeer_key_id(
	const struct tlspeer_key *key,
	uint8_t *out)
{
	uint8_t digest[CRYC_SHA256_SIZE];
	int error;

	/* The first 20 bytes of the digest. */
	error = cryc_hash_digest(CRYC_HASH_SHA256, key->public_key, key->public_size, digest);
	if (error != 0)
		return error;
	memcpy(out, digest, 20);

	/* Made. */
	return 0;
}

/* Writes the extensions block [3] of a certificate (RFC 5280 4.2.1). */
static int __attribute__((unused))
tlspeer_der_extensions(
	struct der_writer *writer,
	const struct tlspeer_cert *spec)
{
	static const unsigned char ca_true[1] = { 0xff };
	static const unsigned char ca_usage[2] = { 0x01, 0x06 };
	static const unsigned char leaf_usage[2] = { 0x07, 0x80 };
	unsigned char list_bytes[900];
	unsigned char value_bytes[320];
	unsigned char inner_bytes[300];
	unsigned char outer_bytes[920];
	uint8_t identifier[20];
	struct der_writer list;
	struct der_writer value;
	struct der_writer inner;
	struct der_writer outer;
	int self_signed;
	int error;

	/* Extensions ::= SEQUENCE SIZE (1..MAX) OF Extension. */
	der_init(&list, list_bytes, sizeof(list_bytes));
	self_signed = 0;
	if (spec->subject_key == spec->issuer_key)
		self_signed = 1;

	/* basicConstraints (critical): cA TRUE for a CA, else the empty SEQUENCE (4.2.1.9). */
	der_init(&inner, inner_bytes, sizeof(inner_bytes));
	if (spec->is_ca != 0)
		der_put(&inner, 0x01U, ca_true, 1);
	der_init(&value, value_bytes, sizeof(value_bytes));
	der_wrap(&value, 0x30U, &inner);
	tlspeer_der_extension(&list, TLSPEER_OID_BASIC_CONSTRAINTS, 1, &value);

	/* keyUsage (critical): keyCertSign and cRLSign (bits 5, 6) or digitalSignature (bit 0) (4.2.1.3). */
	der_init(&value, value_bytes, sizeof(value_bytes));
	if (spec->is_ca != 0)
		der_put(&value, 0x03U, ca_usage, 2);
	else
		der_put(&value, 0x03U, leaf_usage, 2);
	tlspeer_der_extension(&list, TLSPEER_OID_KEY_USAGE, 1, &value);

	/* extKeyUsage serverAuth for a leaf (4.2.1.12). */
	if (spec->is_ca == 0) {
		der_init(&inner, inner_bytes, sizeof(inner_bytes));
		der_oid(&inner, TLSPEER_OID_SERVER_AUTH);
		der_init(&value, value_bytes, sizeof(value_bytes));
		der_wrap(&value, 0x30U, &inner);
		tlspeer_der_extension(&list, TLSPEER_OID_EXT_KEY_USAGE, 0, &value);
	}

	/* subjectAltName: dNSName [2] and iPAddress [7], implicit (4.2.1.6). */
	if (spec->dns_name != NULL || spec->address != NULL) {
		der_init(&inner, inner_bytes, sizeof(inner_bytes));
		if (spec->dns_name != NULL)
			der_put(&inner, 0x82U, (const unsigned char *)spec->dns_name, strlen(spec->dns_name));
		if (spec->address != NULL)
			der_put(&inner, 0x87U, spec->address, spec->address_size);
		der_init(&value, value_bytes, sizeof(value_bytes));
		der_wrap(&value, 0x30U, &inner);
		tlspeer_der_extension(&list, TLSPEER_OID_SUBJECT_ALT_NAME, 0, &value);
	}

	/* subjectKeyIdentifier (4.2.1.2). */
	error = tlspeer_key_id(spec->subject_key, identifier);
	if (error != 0)
		return error;
	der_init(&value, value_bytes, sizeof(value_bytes));
	der_put(&value, 0x04U, identifier, sizeof(identifier));
	tlspeer_der_extension(&list, TLSPEER_OID_SUBJECT_KEY_ID, 0, &value);

	/* authorityKeyIdentifier { keyIdentifier [0] } when another key signs (4.2.1.1). */
	if (!self_signed) {
		error = tlspeer_key_id(spec->issuer_key, identifier);
		if (error != 0)
			return error;
		der_init(&inner, inner_bytes, sizeof(inner_bytes));
		der_put(&inner, 0x80U, identifier, sizeof(identifier));
		der_init(&value, value_bytes, sizeof(value_bytes));
		der_wrap(&value, 0x30U, &inner);
		tlspeer_der_extension(&list, TLSPEER_OID_AUTHORITY_KEY_ID, 0, &value);
	}

	/* [3] EXPLICIT Extensions. */
	der_init(&outer, outer_bytes, sizeof(outer_bytes));
	der_wrap(&outer, 0x30U, &list);
	der_wrap(writer, 0xa3U, &outer);
	if (writer->error != 0)
		return ENOBUFS;

	/* Written. */
	return 0;
}

/* Writes the certificate spec describes as DER (RFC 5280 4.1). */
static int __attribute__((unused))
tlspeer_make_cert(
	const struct tlspeer_cert *spec,
	uint8_t *out,
	size_t room,
	size_t *written)
{
	static const unsigned char version3[1] = { 0x02 };
	unsigned char version_bytes[8];
	unsigned char validity_bytes[40];
	unsigned char tbs_bytes[TLSPEER_CERT_MAX];
	unsigned char body_bytes[TLSPEER_CERT_MAX];
	unsigned char cert_bytes[TLSPEER_CERT_MAX];
	uint8_t serial[4];
	uint8_t digest[CRYC_HASH_MAX_SIZE];
	uint8_t signature[1U + 2U * (CRYC_ECP_MAX_BYTES + 3U) + 2U];
	size_t signature_size;
	struct der_writer version;
	struct der_writer validity;
	struct der_writer tbs;
	struct der_writer body;
	struct der_writer cert;
	struct der_writer final;
	enum cryc_hash_id hash;
	int error;

	/* A spec without its parts. */
	*written = 0;
	if (spec->subject == NULL || spec->issuer == NULL || spec->subject_key == NULL || spec->issuer_key == NULL)
		return EINVAL;
	if (spec->not_before == NULL || spec->not_after == NULL || spec->serial == 0U)
		return EINVAL;

	/* version [0] EXPLICIT v3 and serialNumber. */
	der_init(&tbs, tbs_bytes, sizeof(tbs_bytes));
	der_init(&version, version_bytes, sizeof(version_bytes));
	der_put(&version, 0x02U, version3, 1);
	der_wrap(&tbs, 0xa0U, &version);
	serial[0] = (uint8_t)(spec->serial >> 24);
	serial[1] = (uint8_t)(spec->serial >> 16);
	serial[2] = (uint8_t)(spec->serial >> 8);
	serial[3] = (uint8_t)spec->serial;
	der_unsigned(&tbs, serial, sizeof(serial));

	/* signature, issuer, validity (UTCTime both), subject, subjectPublicKeyInfo. */
	tlspeer_der_sigalg(&tbs, spec->issuer_key);
	tlspeer_der_name(&tbs, spec->issuer);
	der_init(&validity, validity_bytes, sizeof(validity_bytes));
	der_put(&validity, 0x17U, (const unsigned char *)spec->not_before, strlen(spec->not_before));
	der_put(&validity, 0x17U, (const unsigned char *)spec->not_after, strlen(spec->not_after));
	der_wrap(&tbs, 0x30U, &validity);
	tlspeer_der_name(&tbs, spec->subject);
	tlspeer_der_spki(&tbs, spec->subject_key);

	/* extensions. */
	error = tlspeer_der_extensions(&tbs, spec);
	if (error != 0)
		return error;

	/* The tbsCertificate SEQUENCE. */
	der_init(&body, body_bytes, sizeof(body_bytes));
	der_wrap(&body, 0x30U, &tbs);
	if (body.error != 0)
		return ENOBUFS;

	/* The signature over it with the issuer's key (a BIT STRING with no unused bits). */
	hash = CRYC_HASH_SHA256;
	if (spec->issuer_key->ec.size == 48U)
		hash = CRYC_HASH_SHA384;
	error = cryc_hash_digest(hash, body.data, body.used, digest);
	if (error != 0)
		return error;
	signature[0] = 0;
	error = tlspeer_ecdsa_sign_der(spec->issuer_key, digest, cryc_hash_size(hash), signature + 1, sizeof(signature) - 1U, &signature_size);
	if (error != 0)
		return error;

	/* Certificate ::= SEQUENCE { tbsCertificate, signatureAlgorithm, signatureValue }. */
	der_init(&cert, cert_bytes, sizeof(cert_bytes));
	der_raw(&cert, body.data, body.used);
	tlspeer_der_sigalg(&cert, spec->issuer_key);
	der_put(&cert, 0x03U, signature, 1U + signature_size);
	der_init(&final, out, room);
	der_wrap(&final, 0x30U, &cert);
	if (cert.error != 0 || final.error != 0)
		return ENOBUFS;

	/* Succeeded. */
	*written = final.used;
	return 0;
}

/* Makes the root and the leaf for host on curve. */
static int __attribute__((unused))
tlspeer_pki_init(
	struct tlspeer_pki *pki,
	enum cryc_curve_id curve,
	const char *host)
{
	struct tlspeer_cert spec;
	int error;

	/* The two keys. */
	memset(pki, 0, sizeof(*pki));
	error = tlspeer_key_from_label(&pki->root_key, curve, "tlspeer root");
	if (error != 0)
		return error;
	error = tlspeer_key_from_label(&pki->leaf_key, curve, "tlspeer leaf");
	if (error != 0)
		return error;

	/* The self-signed root. */
	memset(&spec, 0, sizeof(spec));
	spec.subject = "tlspeer root CA";
	spec.issuer = "tlspeer root CA";
	spec.subject_key = &pki->root_key;
	spec.issuer_key = &pki->root_key;
	spec.serial = 1;
	spec.is_ca = 1;
	spec.not_before = TLSPEER_NOT_BEFORE;
	spec.not_after = TLSPEER_NOT_AFTER;
	error = tlspeer_make_cert(&spec, pki->root, sizeof(pki->root), &pki->root_size);
	if (error != 0)
		return error;

	/* The leaf for host, signed by the root. */
	spec.subject = host;
	spec.subject_key = &pki->leaf_key;
	spec.serial = 2;
	spec.is_ca = 0;
	spec.dns_name = host;
	error = tlspeer_make_cert(&spec, pki->leaf, sizeof(pki->leaf), &pki->leaf_size);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * The peer: configuration and life.
 */

/* Fills a working script. */
static int __attribute__((unused))
tlspeer_config_init(
	struct tlspeer_config *config,
	uint16_t version,
	const struct tlspeer_pki *pki)
{
	uint8_t digest[CRYC_SHA384_SIZE];
	int error;

	/* The version's default suite, x25519, the leaf's key and scheme. */
	memset(config, 0, sizeof(*config));
	config->version = version;
	config->suite = SSLC_SUITE_AES_128_GCM_SHA256;
	if (version == SSLC_TLS12)
		config->suite = SSLC_SUITE_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256;
	config->group = SSLC_GROUP_X25519;
	config->scheme = SSLC_SCHEME_ECDSA_SECP256R1_SHA256;
	if (pki->leaf_key.ec.size == 48U)
		config->scheme = SSLC_SCHEME_ECDSA_SECP384R1_SHA384;
	config->key = &pki->leaf_key;
	config->chain[0] = pki->leaf;
	config->chain_size[0] = pki->leaf_size;
	config->chain_count = 1;

	/* The fixed values, each a label's digest. */
	error = cryc_hash_digest(CRYC_HASH_SHA256, (const uint8_t *)"tlspeer random", 14, digest);
	if (error != 0)
		return error;
	memcpy(config->random, digest, 32);
	error = cryc_hash_digest(CRYC_HASH_SHA256, (const uint8_t *)"tlspeer x25519", 14, digest);
	if (error != 0)
		return error;
	memcpy(config->x25519_private, digest, 32);
	error = cryc_hash_digest(CRYC_HASH_SHA256, (const uint8_t *)"tlspeer p256", 12, digest);
	if (error != 0)
		return error;
	memcpy(config->p256_private, digest, 32);
	error = cryc_hash_digest(CRYC_HASH_SHA384, (const uint8_t *)"tlspeer p384", 12, digest);
	if (error != 0)
		return error;
	memcpy(config->p384_private, digest, 48);

	/* Succeeded. */
	return 0;
}

/* Makes a peer for config. */
static int __attribute__((unused))
tlspeer_new(
	struct tlspeer **out,
	const struct tlspeer_config *config)
{
	struct tlspeer *peer;
	const struct sslc_suite *suite;
	int error;

	/* The parts of the script the peer cannot do without. */
	*out = NULL;
	if (config->version != SSLC_TLS13 && config->version != SSLC_TLS12)
		return EINVAL;
	if (config->group != SSLC_GROUP_X25519 && config->group != SSLC_GROUP_SECP256R1 &&
	    config->group != SSLC_GROUP_SECP384R1)
		return EINVAL;
	if (config->chain_count > TLSPEER_CHAIN_MAX)
		return EINVAL;
	if (config->key == NULL && config->cv_raw == NULL)
		return EINVAL;
	suite = sslc_tls_suite(config->suite);
	if (suite == NULL)
		return EINVAL;

	/* The peer, zeroed. */
	peer = (struct tlspeer *)calloc(1, sizeof(*peer));
	if (peer == NULL)
		return ENOMEM;

	/* The script and its live copies. */
	peer->config = *config;
	peer->breaks = config->breaks;
	peer->record_size = config->record_size;
	if (peer->record_size == 0U || peer->record_size > SSLC_RECORD_PLAINTEXT_MAX)
		peer->record_size = SSLC_RECORD_PLAINTEXT_MAX;
	peer->state = TLSPEER_WAIT_HELLO;
	peer->suite = suite;
	peer->hash = suite->hash;
	peer->hash_size = cryc_hash_size(suite->hash);
	peer->write_epochs = 1;
	sslc_record_init(&peer->write_dir);
	sslc_record_init(&peer->read_dir);

	/* The transcript in the suite's hash. */
	error = cryc_hash_init(&peer->transcript, peer->hash);
	if (error != 0) {
		free(peer);
		return EINVAL;
	}

	/* The secp256r1 or secp384r1 key pair when that is the group. */
	if (config->group == SSLC_GROUP_SECP256R1) {
		error = tlspeer_key_init(&peer->p256, CRYC_CURVE_P256, config->p256_private);
		if (error != 0) {
			free(peer);
			return EINVAL;
		}
	}
	if (config->group == SSLC_GROUP_SECP384R1) {
		error = tlspeer_key_init(&peer->p384, CRYC_CURVE_P384, config->p384_private);
		if (error != 0) {
			free(peer);
			return EINVAL;
		}
	}

	/* Succeeded. */
	*out = peer;
	return 0;
}

/* Wipes and frees a peer. */
static void __attribute__((unused))
tlspeer_free(
	struct tlspeer *peer)
{
	/* Nothing to free. */
	if (peer == NULL)
		return;

	/* Every secret is in the structure. */
	sslc_record_clear(&peer->write_dir);
	sslc_record_clear(&peer->read_dir);
	sslc_tls13_clear(&peer->schedule);
	cryc_wipe(peer, sizeof(*peer));
	free(peer);
}

/* Records the first failure and moves to TLSPEER_FAILED. */
static int __attribute__((unused))
tlspeer_fail(
	struct tlspeer *peer,
	int error,
	const char *why)
{
	/* Only the first failure is kept. */
	if (peer->error == 0) {
		peer->error = error;
		peer->why = why;
	}
	peer->state = TLSPEER_FAILED;

	/* The failure. */
	return peer->error;
}

/* Ends the script. */
static int __attribute__((unused))
tlspeer_stop(
	struct tlspeer *peer,
	const char *why)
{
	/* No more is sent or taken; why says where the script ended. */
	peer->state = TLSPEER_STOPPED;
	peer->why = why;
	return 0;
}

/*
 * The flight: items, epochs, the transcript.
 */

/* Starts writing an item into the flight buffer. */
static void __attribute__((unused))
tlspeer_item_begin(
	struct tlspeer *peer,
	struct tlspeer_writer *writer)
{
	/* Everything taken: the buffer starts over (views given earlier end here). */
	if (peer->item_taken == peer->item_count) {
		peer->item_count = 0;
		peer->item_taken = 0;
		peer->flight_used = 0;
	}
	tlspeer_writer_init(writer, peer->flight + peer->flight_used, sizeof(peer->flight) - peer->flight_used);
}

/* Registers the bytes writer holds as an item. */
static int __attribute__((unused))
tlspeer_item_commit(
	struct tlspeer *peer,
	struct tlspeer_writer *writer,
	unsigned kind,
	unsigned type,
	int absorb)
{
	struct tlspeer_slot *slot;

	/* A message that did not fit, or too many items. */
	if (writer->error != 0 || writer->depth != 0U)
		return tlspeer_fail(peer, ENOBUFS, "a server message does not fit the flight buffer");
	if (peer->item_count >= TLSPEER_ITEMS_MAX)
		return tlspeer_fail(peer, ENOBUFS, "too many items wait to be sent");

	/* The item, under the current epoch. */
	slot = &peer->items[peer->item_count];
	slot->kind = kind;
	slot->type = type;
	slot->epoch = peer->epoch;
	slot->offset = peer->flight_used;
	slot->size = writer->used;
	peer->item_count++;
	peer->flight_used += writer->used;

	/* A handshake message of the handshake goes into the transcript. */
	if (absorb != 0)
		cryc_hash_update(&peer->transcript, writer->data, writer->used);

	/* Registered. */
	return 0;
}

/* Adds an item of kind with a copy of size bytes. */
static int __attribute__((unused))
tlspeer_item_add(
	struct tlspeer *peer,
	unsigned kind,
	unsigned type,
	const uint8_t *data,
	size_t size,
	int absorb)
{
	struct tlspeer_writer writer;
	int error;

	/* The copy, then the item. */
	tlspeer_item_begin(peer, &writer);
	tlspeer_put(&writer, data, size);
	error = tlspeer_item_commit(peer, &writer, kind, type, absorb);
	if (error != 0)
		return error;

	/* Added. */
	return 0;
}

/* Adds new write keys and makes them the epoch of the next items. */
static int __attribute__((unused))
tlspeer_add_epoch(
	struct tlspeer *peer,
	const struct sslc_traffic_keys *keys,
	int version13)
{
	/* A bounded number of key changes. */
	if (peer->write_epochs >= TLSPEER_EPOCHS)
		return tlspeer_fail(peer, ENOBUFS, "too many write key changes");

	/* The keys, and the next items go under them. */
	peer->write_keys[peer->write_epochs] = *keys;
	peer->write_v13[peer->write_epochs] = version13;
	peer->epoch = peer->write_epochs;
	peer->write_epochs++;
	return 0;
}

/* Writes the transcript hash so far into out. */
static void __attribute__((unused))
tlspeer_transcript_hash(
	const struct tlspeer *peer,
	uint8_t *out)
{
	struct cryc_hash copy;

	/* A copy is finished; the transcript goes on. */
	copy = peer->transcript;
	cryc_hash_final(&copy, out);
}

/* Replaces the transcript of ClientHello1 by message_hash (RFC 8446 4.4.1). */
static void __attribute__((unused))
tlspeer_transcript_restart(
	struct tlspeer *peer)
{
	uint8_t digest[CRYC_HASH_MAX_SIZE];
	uint8_t header[4];

	/* Hash(ClientHello1), then message_hash || 00 00 || Hash.length || it. */
	tlspeer_transcript_hash(peer, digest);
	cryc_hash_init(&peer->transcript, peer->hash);
	header[0] = SSLC_HS_MESSAGE_HASH;
	header[1] = 0;
	header[2] = 0;
	header[3] = (uint8_t)peer->hash_size;
	cryc_hash_update(&peer->transcript, header, sizeof(header));
	cryc_hash_update(&peer->transcript, digest, peer->hash_size);
}

/*
 * The key exchange and the signatures.
 */

/* Finds the client's key share of group. */
static const struct tlspeer_share * __attribute__((unused))
tlspeer_find_share(
	const struct tlspeer *peer,
	uint16_t group)
{
	size_t index;

	/* The first share of the group. */
	for (index = 0; index < peer->hello.share_count; index++) {
		if (peer->hello.shares[index].group == group)
			return &peer->hello.shares[index];
	}

	/* None. */
	return NULL;
}

/* Computes the (EC)DH shared secret with the client's public key. */
static int __attribute__((unused))
tlspeer_compute_shared(
	struct tlspeer *peer,
	const uint8_t *client_key,
	size_t client_key_size)
{
	int error;

	/* x25519 (RFC 7748 6.1; RFC 8446 7.4.2: an all-zero result is refused). */
	if (peer->config.group == SSLC_GROUP_X25519) {
		if (client_key_size != CRYC_X25519_SIZE)
			return tlspeer_fail(peer, EINVAL, "the client's x25519 key is not 32 bytes");
		error = cryc_x25519(peer->shared, peer->config.x25519_private, client_key);
		if (error != 0)
			return tlspeer_fail(peer, EINVAL, "the client's x25519 key gives a zero secret");
		peer->shared_size = CRYC_X25519_SIZE;
		return 0;
	}

	/* secp384r1 ECDH (RFC 8446 7.4.2; RFC 8422 5.10): the 48-byte x coordinate. */
	if (peer->config.group == SSLC_GROUP_SECP384R1) {
		error = cryc_ecc_ecdh(&peer->p384.ec, peer->p384.private_key, client_key, client_key_size, peer->shared);
		if (error != 0)
			return tlspeer_fail(peer, EINVAL, "the client's secp384r1 key does not validate");
		peer->shared_size = 48;
		return 0;
	}

	/* secp256r1 ECDH (RFC 8446 7.4.2; RFC 8422 5.10). */
	error = cryc_ecc_ecdh(&peer->p256.ec, peer->p256.private_key, client_key, client_key_size, peer->shared);
	if (error != 0)
		return tlspeer_fail(peer, EINVAL, "the client's secp256r1 key does not validate");
	peer->shared_size = 32;

	/* Computed. */
	return 0;
}

/* Fills peer->server_share with the server's public key (or the broken one). */
static int __attribute__((unused))
tlspeer_make_share(
	struct tlspeer *peer)
{
	/* The broken shares: x25519 u = 0, or a secp256r1 point off the curve. */
	if ((peer->breaks & TLSPEER_BREAK_ZERO_SHARE) != 0U) {
		memset(peer->server_share, 0, sizeof(peer->server_share));
		peer->server_share_size = CRYC_X25519_SIZE;
		if (peer->config.group == SSLC_GROUP_SECP256R1) {
			peer->server_share[0] = 0x04;
			peer->server_share_size = 65;
		}
		if (peer->config.group == SSLC_GROUP_SECP384R1) {
			peer->server_share[0] = 0x04;
			peer->server_share_size = 97;
		}
		return 0;
	}

	/* x25519: X25519(scalar, 9). */
	if (peer->config.group == SSLC_GROUP_X25519) {
		cryc_x25519_public(peer->server_share, peer->config.x25519_private);
		peer->server_share_size = CRYC_X25519_SIZE;
		return 0;
	}

	/* secp384r1: the uncompressed point of the configured key. */
	if (peer->config.group == SSLC_GROUP_SECP384R1) {
		memcpy(peer->server_share, peer->p384.public_key, peer->p384.public_size);
		peer->server_share_size = peer->p384.public_size;
		return 0;
	}

	/* secp256r1: the uncompressed point of the configured key. */
	memcpy(peer->server_share, peer->p256.public_key, peer->p256.public_size);
	peer->server_share_size = peer->p256.public_size;
	return 0;
}

/* Signs content with the configured key under the configured scheme's hash. */
static int __attribute__((unused))
tlspeer_sign_content(
	struct tlspeer *peer,
	uint8_t *content,
	size_t content_size,
	uint8_t *out,
	size_t room,
	size_t *written)
{
	uint8_t digest[CRYC_HASH_MAX_SIZE];
	enum cryc_hash_id hash;
	int error;

	/* Only the ECDSA schemes can be signed here. */
	if (peer->config.scheme == SSLC_SCHEME_ECDSA_SECP256R1_SHA256) {
		hash = CRYC_HASH_SHA256;
	} else if (peer->config.scheme == SSLC_SCHEME_ECDSA_SECP384R1_SHA384) {
		hash = CRYC_HASH_SHA384;
	} else {
		return tlspeer_fail(peer, EINVAL, "the scheme is not one tlspeer can sign");
	}
	if (peer->config.key == NULL)
		return tlspeer_fail(peer, EINVAL, "no signing key");

	/* The broken signature covers other bytes than those the client checks. */
	if ((peer->breaks & TLSPEER_BREAK_SIGNATURE) != 0U)
		content[content_size - 1U] ^= 1U;
	error = cryc_hash_digest(hash, content, content_size, digest);
	if ((peer->breaks & TLSPEER_BREAK_SIGNATURE) != 0U)
		content[content_size - 1U] ^= 1U;
	if (error != 0)
		return tlspeer_fail(peer, error, "the signed content does not hash");

	/* The DER signature. */
	error = tlspeer_ecdsa_sign_der(peer->config.key, digest, cryc_hash_size(hash), out, room, written);
	if (error != 0)
		return tlspeer_fail(peer, error, "the signature cannot be made");

	/* Signed. */
	return 0;
}

/* Writes the extra extension when extra_at has where. */
static void __attribute__((unused))
tlspeer_put_extra(
	const struct tlspeer *peer,
	struct tlspeer_writer *writer,
	uint32_t where)
{
	/* Only where it was asked for. */
	if ((peer->config.extra_at & where) == 0U)
		return;
	tlspeer_put_extension(writer, peer->config.extra_type, peer->config.extra_data, peer->config.extra_size);
}

/*
 * The server's messages.
 */

/* Sends a HelloRetryRequest (RFC 8446 4.1.4: a ServerHello with the special random). */
static int __attribute__((unused))
tlspeer_send_retry(
	struct tlspeer *peer)
{
	static const uint8_t ccs[1] = { 1 };
	struct tlspeer_writer writer;
	int error;

	/* The transcript of the first ClientHello becomes message_hash (not again for a second retry). */
	if (peer->retries == 0U)
		tlspeer_transcript_restart(peer);

	/* legacy_version, random, the echoed session id, the suite, compression 0. */
	tlspeer_item_begin(peer, &writer);
	tlspeer_put_int(&writer, SSLC_HS_SERVER_HELLO, 1);
	tlspeer_begin(&writer, 3);
	tlspeer_put_int(&writer, SSLC_TLS12, 2);
	tlspeer_put(&writer, tlspeer_retry_random, 32);
	tlspeer_begin(&writer, 1);
	tlspeer_put(&writer, peer->hello.session_id, peer->hello.session_id_size);
	tlspeer_end(&writer);
	tlspeer_put_int(&writer, peer->config.suite, 2);
	tlspeer_put_int(&writer, 0, 1);

	/* key_share (the selected group alone, 4.2.8), cookie, supported_versions, the extra. */
	tlspeer_begin(&writer, 2);
	if ((peer->config.options & TLSPEER_OPT_RETRY_NO_GROUP) == 0U) {
		tlspeer_put_int(&writer, SSLC_EXT_KEY_SHARE, 2);
		tlspeer_put_int(&writer, 2, 2);
		tlspeer_put_int(&writer, peer->config.group, 2);
	}
	if (peer->config.cookie != NULL) {
		tlspeer_put_int(&writer, SSLC_EXT_COOKIE, 2);
		tlspeer_begin(&writer, 2);
		tlspeer_begin(&writer, 2);
		tlspeer_put(&writer, peer->config.cookie, peer->config.cookie_size);
		tlspeer_end(&writer);
		tlspeer_end(&writer);
	}
	tlspeer_put_int(&writer, SSLC_EXT_SUPPORTED_VERSIONS, 2);
	tlspeer_put_int(&writer, 2, 2);
	tlspeer_put_int(&writer, SSLC_TLS13, 2);
	tlspeer_put_extra(peer, &writer, TLSPEER_AT_RETRY);
	tlspeer_end(&writer);
	tlspeer_end(&writer);
	error = tlspeer_item_commit(peer, &writer, TLSPEER_ITEM_HANDSHAKE, SSLC_CT_HANDSHAKE, 1);
	if (error != 0)
		return error;

	/* The compatibility change_cipher_spec after the first retry (D.4). */
	if ((peer->config.options & TLSPEER_OPT_CCS) != 0U && peer->retries == 0U) {
		error = tlspeer_item_add(peer, TLSPEER_ITEM_CCS, SSLC_CT_CHANGE_CIPHER_SPEC, ccs, 1, 0);
		if (error != 0)
			return error;
	}

	/* The client sends another ClientHello. */
	peer->retries++;
	peer->state = TLSPEER_WAIT_HELLO;
	return 0;
}

/* Writes a Certificate message of the chain (RFC 8446 4.4.2; RFC 5246 7.4.2). */
static int __attribute__((unused))
tlspeer_send_certificate(
	struct tlspeer *peer,
	int tls13)
{
	struct tlspeer_writer writer;
	size_t index;
	int error;

	/* The header; for TLS 1.3 the request context (empty, or one byte when broken). */
	tlspeer_item_begin(peer, &writer);
	tlspeer_put_int(&writer, SSLC_HS_CERTIFICATE, 1);
	tlspeer_begin(&writer, 3);
	if (tls13 != 0) {
		tlspeer_begin(&writer, 1);
		if ((peer->breaks & TLSPEER_BREAK_CERT_CONTEXT) != 0U)
			tlspeer_put_int(&writer, 1, 1);
		tlspeer_end(&writer);
	}

	/* The list: each cert_data<1..2^24-1>, and in TLS 1.3 its extensions (the extra on the leaf). */
	tlspeer_begin(&writer, 3);
	for (index = 0; index < peer->config.chain_count; index++) {
		tlspeer_begin(&writer, 3);
		tlspeer_put(&writer, peer->config.chain[index], peer->config.chain_size[index]);
		tlspeer_end(&writer);
		if (tls13 != 0) {
			tlspeer_begin(&writer, 2);
			if (index == 0U)
				tlspeer_put_extra(peer, &writer, TLSPEER_AT_CERT_ENTRY);
			tlspeer_end(&writer);
		}
	}
	tlspeer_end(&writer);
	tlspeer_end(&writer);
	error = tlspeer_item_commit(peer, &writer, TLSPEER_ITEM_HANDSHAKE, SSLC_CT_HANDSHAKE, 1);
	if (error != 0)
		return error;

	/* Sent. */
	return 0;
}

/* Answers a TLS 1.3 ClientHello with the whole server flight (RFC 8446 2 and 4). */
static int __attribute__((unused))
tlspeer_flight13(
	struct tlspeer *peer)
{
	static const uint8_t ccs[1] = { 1 };
	struct tlspeer_writer writer;
	struct sslc_traffic_keys keys;
	const struct tlspeer_share *share;
	uint8_t digest[CRYC_HASH_MAX_SIZE];
	uint8_t content[64U + sizeof(tlspeer_cv_context) + CRYC_HASH_MAX_SIZE];
	uint8_t signature[2U * (CRYC_ECP_MAX_BYTES + 3U) + 4U];
	uint8_t verify[CRYC_HASH_MAX_SIZE];
	size_t signature_size;
	size_t content_size;
	int sharing;
	int error;

	/* The client's share of the group (its absence ends here unless the script sends anyway). */
	share = tlspeer_find_share(peer, peer->config.group);
	if (share == NULL && (peer->breaks & TLSPEER_BREAK_UNSHARED) == 0U)
		return tlspeer_fail(peer, ENOENT, "the ClientHello has no key share of the group");
	sharing = share != NULL && (peer->breaks & TLSPEER_BREAK_ZERO_SHARE) == 0U;

	/* The shared secret and the server's share. */
	if (sharing) {
		error = tlspeer_compute_shared(peer, share->key, share->key_size);
		if (error != 0)
			return error;
	}
	tlspeer_make_share(peer);

	/* ServerHello: legacy_version, random, the session id echoed (or not), suite, compression 0. */
	memcpy(peer->server_random, peer->config.random, 32);
	tlspeer_item_begin(peer, &writer);
	tlspeer_put_int(&writer, SSLC_HS_SERVER_HELLO, 1);
	tlspeer_begin(&writer, 3);
	tlspeer_put_int(&writer, SSLC_TLS12, 2);
	tlspeer_put(&writer, peer->server_random, 32);
	tlspeer_begin(&writer, 1);
	if ((peer->breaks & TLSPEER_BREAK_SESSION_ID) != 0U && peer->hello.session_id_size == 0U) {
		tlspeer_put_int(&writer, 1, 1);
	} else {
		tlspeer_put(&writer, peer->hello.session_id, peer->hello.session_id_size);
		if ((peer->breaks & TLSPEER_BREAK_SESSION_ID) != 0U)
			writer.data[writer.used - peer->hello.session_id_size] ^= 1U;
	}
	tlspeer_end(&writer);
	tlspeer_put_int(&writer, peer->config.suite, 2);
	tlspeer_put_int(&writer, 0, 1);

	/* key_share { group, key_exchange }, supported_versions 1.3, the extra. */
	tlspeer_begin(&writer, 2);
	tlspeer_put_int(&writer, SSLC_EXT_KEY_SHARE, 2);
	tlspeer_begin(&writer, 2);
	tlspeer_put_int(&writer, peer->config.group, 2);
	tlspeer_begin(&writer, 2);
	tlspeer_put(&writer, peer->server_share, peer->server_share_size);
	tlspeer_end(&writer);
	tlspeer_end(&writer);
	tlspeer_put_int(&writer, SSLC_EXT_SUPPORTED_VERSIONS, 2);
	tlspeer_put_int(&writer, 2, 2);
	tlspeer_put_int(&writer, SSLC_TLS13, 2);
	tlspeer_put_extra(peer, &writer, TLSPEER_AT_SERVER_HELLO);
	tlspeer_end(&writer);
	tlspeer_end(&writer);
	error = tlspeer_item_commit(peer, &writer, TLSPEER_ITEM_HANDSHAKE, SSLC_CT_HANDSHAKE, 1);
	if (error != 0)
		return error;

	/* The compatibility change_cipher_spec, when none followed a retry (D.4). */
	if ((peer->config.options & TLSPEER_OPT_CCS) != 0U && peer->retries == 0U) {
		error = tlspeer_item_add(peer, TLSPEER_ITEM_CCS, SSLC_CT_CHANGE_CIPHER_SPEC, ccs, 1, 0);
		if (error != 0)
			return error;
	}

	/* Without a shared secret there are no keys: the script ends at the ServerHello. */
	if (!sharing)
		return tlspeer_stop(peer, "the script ends after a ServerHello without a usable share");

	/* The handshake secrets over ClientHello..ServerHello (7.1). */
	tlspeer_transcript_hash(peer, digest);
	error = sslc_tls13_start(&peer->schedule, peer->hash);
	if (error != 0)
		return tlspeer_stop(peer, "the script ends after a ServerHello whose suite has no TLS 1.3 schedule");
	error = sslc_tls13_handshake(&peer->schedule, peer->shared, peer->shared_size, digest);
	if (error != 0)
		return tlspeer_fail(peer, error, "the handshake secret cannot be made");

	/* The server's handshake keys for what follows, the client's for what it sends (7.3). */
	error = sslc_tls13_traffic_keys(peer->hash, peer->suite, peer->schedule.server_handshake, &keys);
	if (error != 0)
		return tlspeer_stop(peer, "the script ends after a ServerHello whose suite is not TLS 1.3's");
	error = tlspeer_add_epoch(peer, &keys, 1);
	if (error != 0)
		return error;
	error = sslc_tls13_traffic_keys(peer->hash, peer->suite, peer->schedule.client_handshake, &peer->client_handshake_keys);
	if (error != 0)
		return tlspeer_fail(peer, error, "the client's handshake keys cannot be made");
	error = tlspeer_set_read(peer, &peer->client_handshake_keys, 1);
	if (error != 0)
		return error;

	/* EncryptedExtensions: the raw list, or server_name acknowledged and the extra (4.3.1). */
	tlspeer_item_begin(peer, &writer);
	tlspeer_put_int(&writer, SSLC_HS_ENCRYPTED_EXTENSIONS, 1);
	tlspeer_begin(&writer, 3);
	tlspeer_begin(&writer, 2);
	if (peer->config.ee_raw != NULL) {
		tlspeer_put(&writer, peer->config.ee_raw, peer->config.ee_raw_size);
	} else {
		if ((peer->config.options & TLSPEER_OPT_SNI_ACK) != 0U && peer->hello.has_server_name != 0)
			tlspeer_put_extension(&writer, SSLC_EXT_SERVER_NAME, NULL, 0);
		tlspeer_put_extra(peer, &writer, TLSPEER_AT_ENCRYPTED_EXTENSIONS);
	}
	tlspeer_end(&writer);
	tlspeer_end(&writer);
	error = tlspeer_item_commit(peer, &writer, TLSPEER_ITEM_HANDSHAKE, SSLC_CT_HANDSHAKE, 1);
	if (error != 0)
		return error;

	/* CertificateRequest: the context and signature_algorithms (4.3.2). */
	if ((peer->config.options & TLSPEER_OPT_CERT_REQUEST) != 0U) {
		tlspeer_item_begin(peer, &writer);
		tlspeer_put_int(&writer, SSLC_HS_CERTIFICATE_REQUEST, 1);
		tlspeer_begin(&writer, 3);
		tlspeer_begin(&writer, 1);
		tlspeer_put(&writer, peer->config.request_context, peer->config.request_context_size);
		tlspeer_end(&writer);
		tlspeer_begin(&writer, 2);
		tlspeer_put_int(&writer, SSLC_EXT_SIGNATURE_ALGORITHMS, 2);
		tlspeer_put_int(&writer, 6, 2);
		tlspeer_put_int(&writer, 4, 2);
		tlspeer_put_int(&writer, SSLC_SCHEME_ECDSA_SECP256R1_SHA256, 2);
		tlspeer_put_int(&writer, SSLC_SCHEME_RSA_PSS_RSAE_SHA256, 2);
		tlspeer_put_extra(peer, &writer, TLSPEER_AT_CERT_REQUEST);
		tlspeer_end(&writer);
		tlspeer_end(&writer);
		error = tlspeer_item_commit(peer, &writer, TLSPEER_ITEM_HANDSHAKE, SSLC_CT_HANDSHAKE, 1);
		if (error != 0)
			return error;
		peer->request_sent = 1;
	}

	/* Certificate. */
	error = tlspeer_send_certificate(peer, 1);
	if (error != 0)
		return error;

	/* CertificateVerify: the given bytes, or a signature over 64 spaces, the context, 0 and the transcript hash (4.4.3). */
	if (peer->config.cv_raw != NULL) {
		error = tlspeer_item_add(peer, TLSPEER_ITEM_HANDSHAKE, SSLC_CT_HANDSHAKE, peer->config.cv_raw, peer->config.cv_raw_size, 1);
		if (error != 0)
			return error;
	} else {
		memset(content, 0x20, 64);
		memcpy(content + 64, tlspeer_cv_context, sizeof(tlspeer_cv_context));
		content_size = 64U + sizeof(tlspeer_cv_context);
		tlspeer_transcript_hash(peer, content + content_size);
		content_size += peer->hash_size;
		error = tlspeer_sign_content(peer, content, content_size, signature, sizeof(signature), &signature_size);
		if (error != 0)
			return error;
		tlspeer_item_begin(peer, &writer);
		tlspeer_put_int(&writer, SSLC_HS_CERTIFICATE_VERIFY, 1);
		tlspeer_begin(&writer, 3);
		tlspeer_put_int(&writer, peer->config.scheme, 2);
		tlspeer_begin(&writer, 2);
		tlspeer_put(&writer, signature, signature_size);
		tlspeer_end(&writer);
		tlspeer_end(&writer);
		error = tlspeer_item_commit(peer, &writer, TLSPEER_ITEM_HANDSHAKE, SSLC_CT_HANDSHAKE, 1);
		if (error != 0)
			return error;
	}

	/* Finished over the transcript through CertificateVerify (4.4.4). */
	tlspeer_transcript_hash(peer, digest);
	error = sslc_tls13_finished(peer->hash, peer->schedule.server_handshake, digest, verify);
	if (error != 0)
		return tlspeer_fail(peer, error, "the server Finished cannot be made");
	if ((peer->breaks & TLSPEER_BREAK_FINISHED) != 0U)
		verify[0] ^= 1U;
	tlspeer_item_begin(peer, &writer);
	tlspeer_put_int(&writer, SSLC_HS_FINISHED, 1);
	tlspeer_begin(&writer, 3);
	tlspeer_put(&writer, verify, peer->hash_size);
	tlspeer_end(&writer);
	error = tlspeer_item_commit(peer, &writer, TLSPEER_ITEM_HANDSHAKE, SSLC_CT_HANDSHAKE, 1);
	if (error != 0)
		return error;

	/* The application secrets over the transcript through the server Finished (7.1). */
	tlspeer_transcript_hash(peer, digest);
	error = sslc_tls13_master(&peer->schedule, digest);
	if (error != 0)
		return tlspeer_fail(peer, error, "the master secret cannot be made");
	memcpy(peer->server_secret, peer->schedule.server_application, peer->hash_size);
	memcpy(peer->client_secret, peer->schedule.client_application, peer->hash_size);
	error = sslc_tls13_traffic_keys(peer->hash, peer->suite, peer->server_secret, &keys);
	if (error != 0)
		return tlspeer_fail(peer, error, "the server's application keys cannot be made");
	error = tlspeer_add_epoch(peer, &keys, 1);
	if (error != 0)
		return error;
	error = sslc_tls13_traffic_keys(peer->hash, peer->suite, peer->client_secret, &peer->client_application_keys);
	if (error != 0)
		return tlspeer_fail(peer, error, "the client's application keys cannot be made");

	/* The client answers with its (empty) Certificate when asked, then Finished. */
	peer->state = TLSPEER_WAIT_FINISHED;
	if (peer->request_sent != 0)
		peer->state = TLSPEER_WAIT_CERTIFICATE;
	cryc_wipe(&keys, sizeof(keys));
	return 0;
}

/* Answers a TLS 1.2 ClientHello with ServerHello to ServerHelloDone (RFC 5246 7.3; RFC 8422 5.4). */
static int __attribute__((unused))
tlspeer_flight12(
	struct tlspeer *peer)
{
	struct tlspeer_writer writer;
	uint8_t content[64U + 4U + CRYC_ECP_MAX_POINT];
	uint8_t signature[2U * (CRYC_ECP_MAX_BYTES + 3U) + 4U];
	size_t signature_size;
	size_t params_offset;
	size_t params_size;
	size_t extensions_start;
	int error;

	/* The random, with a downgrade mark when broken (RFC 8446 4.1.3). */
	memcpy(peer->server_random, peer->config.random, 32);
	if ((peer->breaks & TLSPEER_BREAK_DOWNGRADE12) != 0U)
		memcpy(peer->server_random + 24, tlspeer_downgrade, 8);
	if ((peer->breaks & TLSPEER_BREAK_DOWNGRADE11) != 0U) {
		memcpy(peer->server_random + 24, tlspeer_downgrade, 8);
		peer->server_random[31] = 0;
	}

	/* extended_master_secret when offered and not refused (RFC 7627 5.2). */
	peer->ems = 0;
	if (peer->hello.has_extended_master_secret != 0 && (peer->config.options & TLSPEER_OPT_NO_EMS) == 0U)
		peer->ems = 1;

	/* ServerHello: version, random, session id (empty, or the client's when broken), suite, compression. */
	tlspeer_item_begin(peer, &writer);
	tlspeer_put_int(&writer, SSLC_HS_SERVER_HELLO, 1);
	tlspeer_begin(&writer, 3);
	tlspeer_put_int(&writer, SSLC_TLS12, 2);
	tlspeer_put(&writer, peer->server_random, 32);
	tlspeer_begin(&writer, 1);
	if ((peer->breaks & TLSPEER_BREAK_SESSION_ID) != 0U)
		tlspeer_put(&writer, peer->hello.session_id, peer->hello.session_id_size);
	tlspeer_end(&writer);
	tlspeer_put_int(&writer, peer->config.suite, 2);
	tlspeer_put_int(&writer, 0, 1);

	/* The extensions; the block is left out when it would be empty. */
	tlspeer_begin(&writer, 2);
	extensions_start = writer.used;
	if ((peer->config.options & TLSPEER_OPT_NO_RI) == 0U) {
		tlspeer_put_int(&writer, SSLC_EXT_RENEGOTIATION_INFO, 2);
		tlspeer_put_int(&writer, 1, 2);
		tlspeer_put_int(&writer, 0, 1);
	}
	if (peer->ems != 0)
		tlspeer_put_extension(&writer, SSLC_EXT_EXTENDED_MASTER_SECRET, NULL, 0);
	if ((peer->config.options & TLSPEER_OPT_POINT_FORMATS) != 0U) {
		tlspeer_put_int(&writer, SSLC_EXT_EC_POINT_FORMATS, 2);
		tlspeer_put_int(&writer, 2, 2);
		tlspeer_put_int(&writer, 1, 1);
		tlspeer_put_int(&writer, 0, 1);
	}
	if ((peer->config.options & TLSPEER_OPT_SNI_ACK) != 0U && peer->hello.has_server_name != 0)
		tlspeer_put_extension(&writer, SSLC_EXT_SERVER_NAME, NULL, 0);
	tlspeer_put_extra(peer, &writer, TLSPEER_AT_SERVER_HELLO);
	tlspeer_end(&writer);
	if (writer.error == 0 && writer.used == extensions_start)
		writer.used -= 2U;
	tlspeer_end(&writer);
	error = tlspeer_item_commit(peer, &writer, TLSPEER_ITEM_HANDSHAKE, SSLC_CT_HANDSHAKE, 1);
	if (error != 0)
		return error;

	/* Certificate. */
	error = tlspeer_send_certificate(peer, 0);
	if (error != 0)
		return error;

	/* ServerKeyExchange: ServerECDHParams (named_curve 3, the group, the point<1..2^8-1>) (RFC 8422 5.4). */
	tlspeer_make_share(peer);
	tlspeer_item_begin(peer, &writer);
	tlspeer_put_int(&writer, SSLC_HS_SERVER_KEY_EXCHANGE, 1);
	tlspeer_begin(&writer, 3);
	params_offset = writer.used;
	tlspeer_put_int(&writer, 3, 1);
	tlspeer_put_int(&writer, peer->config.group, 2);
	tlspeer_begin(&writer, 1);
	tlspeer_put(&writer, peer->server_share, peer->server_share_size);
	tlspeer_end(&writer);
	params_size = writer.used - params_offset;

	/* The signature over client_random, server_random and the params, with its scheme. */
	if (writer.error != 0)
		return tlspeer_fail(peer, ENOBUFS, "the ServerKeyExchange does not fit");
	memcpy(content, peer->client_random, 32);
	memcpy(content + 32, peer->server_random, 32);
	memcpy(content + 64, writer.data + params_offset, params_size);
	error = tlspeer_sign_content(peer, content, 64U + params_size, signature, sizeof(signature), &signature_size);
	if (error != 0)
		return error;
	tlspeer_put_int(&writer, peer->config.scheme, 2);
	tlspeer_begin(&writer, 2);
	tlspeer_put(&writer, signature, signature_size);
	tlspeer_end(&writer);
	tlspeer_end(&writer);
	error = tlspeer_item_commit(peer, &writer, TLSPEER_ITEM_HANDSHAKE, SSLC_CT_HANDSHAKE, 1);
	if (error != 0)
		return error;

	/* CertificateRequest: ecdsa_sign, ecdsa_secp256r1_sha256, no authorities (RFC 5246 7.4.4). */
	if ((peer->config.options & TLSPEER_OPT_CERT_REQUEST) != 0U) {
		tlspeer_item_begin(peer, &writer);
		tlspeer_put_int(&writer, SSLC_HS_CERTIFICATE_REQUEST, 1);
		tlspeer_begin(&writer, 3);
		tlspeer_put_int(&writer, 1, 1);
		tlspeer_put_int(&writer, 64, 1);
		tlspeer_put_int(&writer, 2, 2);
		tlspeer_put_int(&writer, SSLC_SCHEME_ECDSA_SECP256R1_SHA256, 2);
		tlspeer_put_int(&writer, 0, 2);
		tlspeer_end(&writer);
		error = tlspeer_item_commit(peer, &writer, TLSPEER_ITEM_HANDSHAKE, SSLC_CT_HANDSHAKE, 1);
		if (error != 0)
			return error;
		peer->request_sent = 1;
	}

	/* ServerHelloDone. */
	tlspeer_item_begin(peer, &writer);
	tlspeer_put_int(&writer, SSLC_HS_SERVER_HELLO_DONE, 1);
	tlspeer_put_int(&writer, 0, 3);
	error = tlspeer_item_commit(peer, &writer, TLSPEER_ITEM_HANDSHAKE, SSLC_CT_HANDSHAKE, 1);
	if (error != 0)
		return error;

	/* The client answers with its (empty) Certificate when asked, then ClientKeyExchange. */
	peer->state = TLSPEER_WAIT_KEY_EXCHANGE;
	if (peer->request_sent != 0)
		peer->state = TLSPEER_WAIT_CERTIFICATE;
	return 0;
}

/*
 * The client's messages.
 */

/* Takes a ClientHello. */
static int __attribute__((unused))
tlspeer_on_hello(
	struct tlspeer *peer,
	const uint8_t *message,
	size_t size)
{
	int error;

	/* Its syntax, and its place in the transcript. */
	error = tlspeer_parse_client_hello(message, size, &peer->hello);
	if (error != 0)
		return tlspeer_fail(peer, error, "the ClientHello does not parse");
	cryc_hash_update(&peer->transcript, message, size);
	memcpy(peer->client_random, peer->hello.random, 32);

	/* TLS 1.2. */
	if (peer->config.version == SSLC_TLS12) {
		error = tlspeer_flight12(peer);
		if (error != 0)
			return error;
		return 0;
	}

	/* A HelloRetryRequest for the first ClientHello, and when broken for the second. */
	if (peer->config.hello_retry != 0 && peer->retries == 0U) {
		error = tlspeer_send_retry(peer);
		if (error != 0)
			return error;
		return 0;
	}
	if ((peer->breaks & TLSPEER_BREAK_SECOND_HRR) != 0U && peer->retries == 1U) {
		error = tlspeer_send_retry(peer);
		if (error != 0)
			return error;
		return tlspeer_stop(peer, "the script ends after a second HelloRetryRequest");
	}

	/* The cookie must come back after a retry that sent one (RFC 8446 4.2.2). */
	if (peer->retries > 0U && peer->config.cookie != NULL) {
		if (peer->hello.has_cookie == 0 || peer->hello.cookie_size != peer->config.cookie_size)
			return tlspeer_fail(peer, EPROTO, "the second ClientHello does not echo the cookie");
		if (memcmp(peer->hello.cookie, peer->config.cookie, peer->config.cookie_size) != 0)
			return tlspeer_fail(peer, EPROTO, "the second ClientHello does not echo the cookie");
	}

	/* The whole flight. */
	error = tlspeer_flight13(peer);
	if (error != 0)
		return error;

	/* Answered. */
	return 0;
}

/* Takes the client's (empty) Certificate (RFC 8446 4.4.2.4; RFC 5246 7.4.6). */
static int __attribute__((unused))
tlspeer_on_certificate(
	struct tlspeer *peer,
	const uint8_t *message,
	size_t size)
{
	struct tlspeer_reader reader;
	struct tlspeer_reader context;
	struct tlspeer_reader list;

	/* TLS 1.3: the request's context echoed, then the list. */
	tlspeer_reader_init(&reader, message + 4, size - 4U);
	if (peer->config.version == SSLC_TLS13) {
		tlspeer_get_vector(&reader, 1, 0, 255, &context);
		if (context.error != 0)
			return tlspeer_fail(peer, EINVAL, "the client Certificate does not parse");
		if (context.size != peer->config.request_context_size)
			return tlspeer_fail(peer, EPROTO, "the client Certificate does not echo the request context");
		if (context.size > 0U && memcmp(context.data, peer->config.request_context, context.size) != 0)
			return tlspeer_fail(peer, EPROTO, "the client Certificate does not echo the request context");
	}

	/* An empty list, the only answer tlspeer takes. */
	tlspeer_get_vector(&reader, 3, 0, 0xffffffU, &list);
	if (!tlspeer_reader_done(&reader))
		return tlspeer_fail(peer, EINVAL, "the client Certificate does not parse");
	if (list.size != 0U)
		return tlspeer_fail(peer, EPROTO, "the client Certificate is not empty");
	cryc_hash_update(&peer->transcript, message, size);

	/* Next: TLS 1.3 Finished, TLS 1.2 ClientKeyExchange. */
	peer->state = TLSPEER_WAIT_FINISHED;
	if (peer->config.version == SSLC_TLS12)
		peer->state = TLSPEER_WAIT_KEY_EXCHANGE;
	return 0;
}

/* Takes a TLS 1.2 ClientKeyExchange (RFC 8422 5.7) and makes the keys (RFC 5246 8.1, 6.3; RFC 7627 4). */
static int __attribute__((unused))
tlspeer_on_key_exchange(
	struct tlspeer *peer,
	const uint8_t *message,
	size_t size)
{
	struct tlspeer_reader reader;
	struct tlspeer_reader point;
	uint8_t context[CRYC_HASH_MAX_SIZE + 64U];
	size_t context_size;
	int error;

	/* ClientECDiffieHellmanPublic: point<1..2^8-1>, nothing after. */
	tlspeer_reader_init(&reader, message + 4, size - 4U);
	tlspeer_get_vector(&reader, 1, 1, 255, &point);
	if (!tlspeer_reader_done(&reader))
		return tlspeer_fail(peer, EINVAL, "the ClientKeyExchange does not parse");

	/* The premaster secret is the (EC)DH result. */
	error = tlspeer_compute_shared(peer, point.data, point.size);
	if (error != 0)
		return error;
	cryc_hash_update(&peer->transcript, message, size);

	/* The master secret: over the session hash with EMS, else over the two randoms. */
	if (peer->ems != 0) {
		tlspeer_transcript_hash(peer, context);
		context_size = peer->hash_size;
	} else {
		memcpy(context, peer->client_random, 32);
		memcpy(context + 32, peer->server_random, 32);
		context_size = 64;
	}
	error = sslc_tls12_master(peer->hash, peer->ems, peer->shared, peer->shared_size, context, context_size, peer->master);
	if (error != 0)
		return tlspeer_fail(peer, error, "the master secret cannot be made");

	/* The key block: the client's keys for reading after its CCS, the server's for after its own. */
	error = sslc_tls12_key_block(peer->suite, peer->master, peer->client_random, peer->server_random, &peer->client_application_keys, &peer->server_keys12);
	if (error != 0)
		return tlspeer_fail(peer, error, "the key block cannot be made (a suite of TLS 1.3?)");

	/* The client's change_cipher_spec comes next. */
	peer->state = TLSPEER_WAIT_CCS;
	return 0;
}

/* Takes the client's Finished. */
static int __attribute__((unused))
tlspeer_on_finished(
	struct tlspeer *peer,
	const uint8_t *message,
	size_t size)
{
	static const uint8_t ccs[1] = { 1 };
	struct tlspeer_writer writer;
	uint8_t digest[CRYC_HASH_MAX_SIZE];
	uint8_t expected[CRYC_HASH_MAX_SIZE];
	size_t expected_size;
	int error;

	/* The expected verify_data over the transcript before it. */
	tlspeer_transcript_hash(peer, digest);
	if (peer->config.version == SSLC_TLS13) {
		error = sslc_tls13_finished(peer->hash, peer->schedule.client_handshake, digest, expected);
		expected_size = peer->hash_size;
	} else {
		error = sslc_tls12_finished(peer->hash, peer->master, 1, digest, expected);
		expected_size = SSLC_TLS12_VERIFY_DATA;
	}
	if (error != 0)
		return tlspeer_fail(peer, error, "the client Finished cannot be computed");

	/* The message must carry exactly that. */
	if (size != 4U + expected_size)
		return tlspeer_fail(peer, EINVAL, "the client Finished has the wrong length");
	if (memcmp(message + 4, expected, expected_size) != 0)
		return tlspeer_fail(peer, EBADMSG, "the client Finished does not verify");
	cryc_hash_update(&peer->transcript, message, size);

	/* TLS 1.3: the client's application keys from now on. */
	if (peer->config.version == SSLC_TLS13) {
		error = tlspeer_set_read(peer, &peer->client_application_keys, 1);
		if (error != 0)
			return error;
		peer->state = TLSPEER_CONNECTED;
		return 0;
	}

	/* TLS 1.2: the server's change_cipher_spec, its keys, its Finished. */
	error = tlspeer_item_add(peer, TLSPEER_ITEM_CCS, SSLC_CT_CHANGE_CIPHER_SPEC, ccs, 1, 0);
	if (error != 0)
		return error;
	error = tlspeer_add_epoch(peer, &peer->server_keys12, 0);
	if (error != 0)
		return error;
	tlspeer_transcript_hash(peer, digest);
	error = sslc_tls12_finished(peer->hash, peer->master, 0, digest, expected);
	if (error != 0)
		return tlspeer_fail(peer, error, "the server Finished cannot be computed");
	if ((peer->breaks & TLSPEER_BREAK_FINISHED) != 0U)
		expected[0] ^= 1U;
	tlspeer_item_begin(peer, &writer);
	tlspeer_put_int(&writer, SSLC_HS_FINISHED, 1);
	tlspeer_begin(&writer, 3);
	tlspeer_put(&writer, expected, SSLC_TLS12_VERIFY_DATA);
	tlspeer_end(&writer);
	error = tlspeer_item_commit(peer, &writer, TLSPEER_ITEM_HANDSHAKE, SSLC_CT_HANDSHAKE, 1);
	if (error != 0)
		return error;

	/* Connected. */
	peer->state = TLSPEER_CONNECTED;
	return 0;
}

/* Takes a TLS 1.3 KeyUpdate after the handshake (RFC 8446 4.6.3). */
static int __attribute__((unused))
tlspeer_on_key_update(
	struct tlspeer *peer,
	const uint8_t *message,
	size_t size)
{
	int error;

	/* One byte, 0 or 1. */
	if (size != 5U || message[4] > 1U)
		return tlspeer_fail(peer, EINVAL, "the client KeyUpdate does not parse");

	/* The client's next generation of keys (7.2). */
	error = sslc_tls13_update_secret(peer->hash, peer->client_secret);
	if (error != 0)
		return tlspeer_fail(peer, error, "the client secret cannot be updated");
	error = sslc_tls13_traffic_keys(peer->hash, peer->suite, peer->client_secret, &peer->client_application_keys);
	if (error != 0)
		return tlspeer_fail(peer, error, "the client keys cannot be updated");
	error = tlspeer_set_read(peer, &peer->client_application_keys, 1);
	if (error != 0)
		return error;

	/* A request is answered with a KeyUpdate of our own. */
	if (message[4] == 1U) {
		error = tlspeer_send_key_update(peer, 0);
		if (error != 0)
			return error;
	}

	/* Taken. */
	return 0;
}

/* Takes one whole client handshake message and moves the script. */
static int __attribute__((unused))
tlspeer_handshake(
	struct tlspeer *peer,
	const uint8_t *message,
	size_t size)
{
	unsigned type;
	int error;

	/* A failed or ended script takes nothing more. */
	if (peer->state == TLSPEER_FAILED)
		return peer->error;
	if (peer->state == TLSPEER_STOPPED)
		return tlspeer_fail(peer, EPROTO, "a message came after the script ended");

	/* One whole message: a header whose length is the rest. */
	if (size < 4U)
		return tlspeer_fail(peer, EINVAL, "a client handshake message is shorter than its header");
	if (cryc_load_be24(message + 1) != size - 4U)
		return tlspeer_fail(peer, EINVAL, "a client handshake message's length is wrong");
	type = message[0];

	/* By the state. */
	error = 0;
	switch (peer->state) {
	case TLSPEER_WAIT_HELLO:
		if (type != SSLC_HS_CLIENT_HELLO)
			return tlspeer_fail(peer, EPROTO, "a ClientHello was expected");
		error = tlspeer_on_hello(peer, message, size);
		break;
	case TLSPEER_WAIT_CERTIFICATE:
		if (type != SSLC_HS_CERTIFICATE)
			return tlspeer_fail(peer, EPROTO, "the client's Certificate was expected");
		error = tlspeer_on_certificate(peer, message, size);
		break;
	case TLSPEER_WAIT_KEY_EXCHANGE:
		if (type != SSLC_HS_CLIENT_KEY_EXCHANGE)
			return tlspeer_fail(peer, EPROTO, "a ClientKeyExchange was expected");
		error = tlspeer_on_key_exchange(peer, message, size);
		break;
	case TLSPEER_WAIT_CCS:
		return tlspeer_fail(peer, EPROTO, "the client's change_cipher_spec was expected");
	case TLSPEER_WAIT_FINISHED:
		if (type != SSLC_HS_FINISHED)
			return tlspeer_fail(peer, EPROTO, "the client's Finished was expected");
		error = tlspeer_on_finished(peer, message, size);
		break;
	default:
		if (peer->config.version != SSLC_TLS13 || type != SSLC_HS_KEY_UPDATE)
			return tlspeer_fail(peer, EPROTO, "an unexpected client message after the handshake");
		error = tlspeer_on_key_update(peer, message, size);
		break;
	}
	if (error != 0)
		return error;

	/* Taken. */
	return 0;
}

/* Takes the client's change_cipher_spec. */
static int __attribute__((unused))
tlspeer_change_cipher_spec(
	struct tlspeer *peer)
{
	int error;

	/* A failed script takes nothing more. */
	if (peer->state == TLSPEER_FAILED)
		return peer->error;
	peer->ccs_count++;

	/* TLS 1.2: only after the ClientKeyExchange, and the client's keys follow. */
	if (peer->config.version == SSLC_TLS12) {
		if (peer->state != TLSPEER_WAIT_CCS)
			return tlspeer_fail(peer, EPROTO, "a change_cipher_spec out of place");
		error = tlspeer_set_read(peer, &peer->client_application_keys, 0);
		if (error != 0)
			return error;
		peer->state = TLSPEER_WAIT_FINISHED;
		return 0;
	}

	/* TLS 1.3: dropped before the client's Finished (RFC 8446 5). */
	if (peer->state == TLSPEER_CONNECTED)
		return tlspeer_fail(peer, EPROTO, "a change_cipher_spec after the handshake");
	return 0;
}

/* Gives the next thing the server sends. */
static int __attribute__((unused))
tlspeer_next(
	struct tlspeer *peer,
	struct tlspeer_item *item)
{
	const struct tlspeer_slot *slot;

	/* Nothing waits. */
	if (peer->item_taken >= peer->item_count)
		return 0;

	/* The next one, a view into the flight buffer. */
	slot = &peer->items[peer->item_taken];
	item->kind = slot->kind;
	item->type = slot->type;
	item->epoch = slot->epoch;
	item->data = peer->flight + slot->offset;
	item->size = slot->size;
	peer->item_taken++;
	return 1;
}

/*
 * Post-handshake and hand-made sends.
 */

/* Sends a TLS 1.3 NewSessionTicket (RFC 8446 4.6.1). */
static int __attribute__((unused))
tlspeer_send_ticket(
	struct tlspeer *peer)
{
	static const uint8_t ticket[16] = {
		0x74, 0x6c, 0x73, 0x70, 0x65, 0x65, 0x72, 0x20,
		0x74, 0x69, 0x63, 0x6b, 0x65, 0x74, 0x00, 0x01
	};
	struct tlspeer_writer writer;
	int error;

	/* lifetime, age_add, nonce<0..255>, ticket<1..2^16-1>, extensions<0..2^16-2>. */
	tlspeer_item_begin(peer, &writer);
	tlspeer_put_int(&writer, SSLC_HS_NEW_SESSION_TICKET, 1);
	tlspeer_begin(&writer, 3);
	tlspeer_put_int(&writer, 7200, 4);
	tlspeer_put_int(&writer, 0x01020304U, 4);
	tlspeer_put_int(&writer, 1, 1);
	tlspeer_put_int(&writer, 0, 1);
	tlspeer_begin(&writer, 2);
	tlspeer_put(&writer, ticket, sizeof(ticket));
	tlspeer_end(&writer);
	tlspeer_put_int(&writer, 0, 2);
	tlspeer_end(&writer);
	error = tlspeer_item_commit(peer, &writer, TLSPEER_ITEM_HANDSHAKE, SSLC_CT_HANDSHAKE, 0);
	if (error != 0)
		return error;

	/* Queued. */
	return 0;
}

/* Sends a TLS 1.3 KeyUpdate and moves the write keys to the next generation (RFC 8446 4.6.3, 7.2). */
static int __attribute__((unused))
tlspeer_send_key_update(
	struct tlspeer *peer,
	int request_update)
{
	struct tlspeer_writer writer;
	struct sslc_traffic_keys keys;
	int error;

	/* Only after the server's application secret exists. */
	if (peer->config.version != SSLC_TLS13 || peer->write_epochs < 3U)
		return tlspeer_fail(peer, EPROTO, "a KeyUpdate before the application keys");

	/* The message under the current keys. */
	tlspeer_item_begin(peer, &writer);
	tlspeer_put_int(&writer, SSLC_HS_KEY_UPDATE, 1);
	tlspeer_put_int(&writer, 1, 3);
	tlspeer_put_int(&writer, request_update != 0 ? 1U : 0U, 1);
	error = tlspeer_item_commit(peer, &writer, TLSPEER_ITEM_HANDSHAKE, SSLC_CT_HANDSHAKE, 0);
	if (error != 0)
		return error;

	/* The next generation for everything after it. */
	error = sslc_tls13_update_secret(peer->hash, peer->server_secret);
	if (error != 0)
		return tlspeer_fail(peer, error, "the server secret cannot be updated");
	error = sslc_tls13_traffic_keys(peer->hash, peer->suite, peer->server_secret, &keys);
	if (error != 0)
		return tlspeer_fail(peer, error, "the server keys cannot be updated");
	error = tlspeer_add_epoch(peer, &keys, 1);
	cryc_wipe(&keys, sizeof(keys));
	if (error != 0)
		return error;

	/* Queued. */
	return 0;
}

/* Sends application data. */
static int __attribute__((unused))
tlspeer_send_data(
	struct tlspeer *peer,
	const uint8_t *data,
	size_t size)
{
	int error;

	/* One item; the records are cut at flush. */
	error = tlspeer_item_add(peer, TLSPEER_ITEM_DATA, SSLC_CT_APPLICATION_DATA, data, size, 0);
	if (error != 0)
		return error;

	/* Queued. */
	return 0;
}

/* Sends an alert of level and description. */
static int __attribute__((unused))
tlspeer_send_alert(
	struct tlspeer *peer,
	unsigned level,
	unsigned description)
{
	uint8_t alert[2];
	int error;

	/* Alert { level, description } (RFC 8446 6). */
	alert[0] = (uint8_t)level;
	alert[1] = (uint8_t)description;
	error = tlspeer_item_add(peer, TLSPEER_ITEM_ALERT, SSLC_CT_ALERT, alert, 2, 0);
	if (error != 0)
		return error;

	/* Queued. */
	return 0;
}

/* Sends a handshake message the test made. */
static int __attribute__((unused))
tlspeer_send_handshake(
	struct tlspeer *peer,
	const uint8_t *message,
	size_t size)
{
	int error;

	/* Not in the transcript. */
	error = tlspeer_item_add(peer, TLSPEER_ITEM_HANDSHAKE, SSLC_CT_HANDSHAKE, message, size, 0);
	if (error != 0)
		return error;

	/* Queued. */
	return 0;
}

/* Sends one record of content type type. */
static int __attribute__((unused))
tlspeer_send_record(
	struct tlspeer *peer,
	unsigned type,
	const uint8_t *content,
	size_t size)
{
	int error;

	/* One record, as it is. */
	error = tlspeer_item_add(peer, TLSPEER_ITEM_RECORD, type, content, size, 0);
	if (error != 0)
		return error;

	/* Queued. */
	return 0;
}

/* Sends bytes as they are. */
static int __attribute__((unused))
tlspeer_send_bytes(
	struct tlspeer *peer,
	const uint8_t *bytes,
	size_t size)
{
	int error;

	/* Not sealed. */
	error = tlspeer_item_add(peer, TLSPEER_ITEM_BYTES, 0, bytes, size, 0);
	if (error != 0)
		return error;

	/* Queued. */
	return 0;
}

/*
 * The record layer.
 */

/* Sets the read keys the client's next records use. */
static int __attribute__((unused))
tlspeer_set_read(
	struct tlspeer *peer,
	const struct sslc_traffic_keys *keys,
	int version13)
{
	int error;

	/* The direction keyed. */
	error = sslc_record_set_keys(&peer->read_dir, version13, keys);
	if (error != 0)
		return tlspeer_fail(peer, error, "the read keys do not suit the suite");
	peer->read_keyed = 1;
	peer->read_changes++;

	/* Set. */
	return 0;
}

/* Moves the write dir to epoch. */
static int __attribute__((unused))
tlspeer_write_epoch(
	struct tlspeer *peer,
	unsigned epoch)
{
	int error;

	/* Already there. */
	if (epoch == peer->write_dir_epoch)
		return 0;

	/* Plaintext, or the epoch's keys (the sequence starts at 0). */
	if (epoch == 0U) {
		sslc_record_clear(&peer->write_dir);
	} else {
		error = sslc_record_set_keys(&peer->write_dir, peer->write_v13[epoch], &peer->write_keys[epoch]);
		if (error != 0)
			return tlspeer_fail(peer, error, "the write keys do not suit the suite");
	}
	peer->write_dir_epoch = epoch;

	/* Moved. */
	return 0;
}

/* Seals one record of type with size bytes under the write dir into out. */
static int __attribute__((unused))
tlspeer_seal(
	struct tlspeer *peer,
	unsigned type,
	const uint8_t *data,
	size_t size)
{
	size_t written;
	int error;

	/* Sealed at the end of the output. */
	error = sslc_record_seal(&peer->write_dir, type, 0, data, size, peer->out + peer->out_used, sizeof(peer->out) - peer->out_used, &written);
	if (error != 0)
		return tlspeer_fail(peer, error, "a server record cannot be sealed (output full?)");

	/* The broken tag: the record's last byte. */
	if (peer->write_dir.suite != NULL && (peer->breaks & TLSPEER_BREAK_RECORD_TAG) != 0U) {
		peer->out[peer->out_used + written - 1U] ^= 1U;
		peer->breaks &= ~(uint32_t)TLSPEER_BREAK_RECORD_TAG;
	}
	peer->out_used += written;
	peer->records_out++;

	/* Sealed. */
	return 0;
}

/* Seals size bytes of type as records of at most record_size bytes. */
static int __attribute__((unused))
tlspeer_seal_split(
	struct tlspeer *peer,
	unsigned type,
	const uint8_t *data,
	size_t size)
{
	size_t done;
	size_t part;
	int error;

	/* Each piece (an empty item is one empty record). */
	done = 0;
	do {
		part = size - done;
		if (part > peer->record_size)
			part = peer->record_size;
		error = tlspeer_seal(peer, type, data + done, part);
		if (error != 0)
			return error;
		done += part;
	} while (done < size);

	/* Sealed. */
	return 0;
}

/* Seals everything sent so far into records. */
static int __attribute__((unused))
tlspeer_flush(
	struct tlspeer *peer)
{
	const struct tlspeer_slot *slot;
	size_t last;
	size_t end;
	int error;

	/* Each item in order. */
	while (peer->item_taken < peer->item_count) {
		slot = &peer->items[peer->item_taken];

		/* Bytes as they are. */
		if (slot->kind == TLSPEER_ITEM_BYTES) {
			if (slot->size > sizeof(peer->out) - peer->out_used)
				return tlspeer_fail(peer, ENOBUFS, "the output buffer is full");
			memcpy(peer->out + peer->out_used, peer->flight + slot->offset, slot->size);
			peer->out_used += slot->size;
			peer->item_taken++;
			continue;
		}

		/* The keys of the item. */
		error = tlspeer_write_epoch(peer, slot->epoch);
		if (error != 0)
			return error;

		/* Anything but handshake messages is records of its own (an alert is never split, RFC 8446 5.1). */
		if (slot->kind != TLSPEER_ITEM_HANDSHAKE) {
			if (slot->kind == TLSPEER_ITEM_RECORD || slot->kind == TLSPEER_ITEM_ALERT)
				error = tlspeer_seal(peer, slot->type, peer->flight + slot->offset, slot->size);
			else
				error = tlspeer_seal_split(peer, slot->type, peer->flight + slot->offset, slot->size);
			if (error != 0)
				return error;
			peer->item_taken++;
			continue;
		}

		/* Handshake messages under the same keys share records (they lie together in the flight buffer). */
		last = peer->item_taken + 1U;
		while (last < peer->item_count &&
		    peer->items[last].kind == TLSPEER_ITEM_HANDSHAKE &&
		    peer->items[last].epoch == slot->epoch)
			last++;

		/* The broken record carries the first message after the key change too. */
		if ((peer->breaks & TLSPEER_BREAK_SAME_RECORD) != 0U &&
		    last < peer->item_count &&
		    peer->items[last].kind == TLSPEER_ITEM_HANDSHAKE) {
			last++;
			peer->breaks &= ~(uint32_t)TLSPEER_BREAK_SAME_RECORD;
		}
		end = peer->items[last - 1U].offset + peer->items[last - 1U].size;
		error = tlspeer_seal_split(peer, SSLC_CT_HANDSHAKE, peer->flight + slot->offset, end - slot->offset);
		if (error != 0)
			return error;
		peer->item_taken = last;
	}

	/* Everything is in the output. */
	return 0;
}

/* Flushes, then gives up to room of the bytes for the client. */
static int __attribute__((unused))
tlspeer_take(
	struct tlspeer *peer,
	uint8_t *out,
	size_t room,
	size_t *taken)
{
	size_t count;
	int error;

	/* Everything sent so far, sealed. */
	*taken = 0;
	error = tlspeer_flush(peer);
	if (error != 0)
		return error;

	/* As much as fits. */
	count = peer->out_used - peer->out_taken;
	if (count > room)
		count = room;
	if (count > 0U)
		memcpy(out, peer->out + peer->out_taken, count);
	peer->out_taken += count;
	*taken = count;

	/* Everything taken: the output starts over. */
	if (peer->out_taken == peer->out_used) {
		peer->out_taken = 0;
		peer->out_used = 0;
	}
	return 0;
}

/* Takes one record's content of type (plaintext or opened). */
static int __attribute__((unused))
tlspeer_on_content(
	struct tlspeer *peer,
	unsigned type,
	const uint8_t *content,
	size_t size,
	int protected_record)
{
	size_t length;
	unsigned changes;
	int error;

	/* By the content type. */
	switch (type) {
	case SSLC_CT_HANDSHAKE:
		/* Appended to the messages being reassembled. */
		if (size > sizeof(peer->message) - peer->message_used)
			return tlspeer_fail(peer, ENOBUFS, "a client handshake message is too long for the tool");
		memcpy(peer->message + peer->message_used, content, size);
		peer->message_used += size;

		/* Each whole message; a key change must fall on a record's end (RFC 8446 5.1). */
		while (peer->message_used >= 4U) {
			length = 4U + cryc_load_be24(peer->message + 1);
			if (length > sizeof(peer->message))
				return tlspeer_fail(peer, ENOBUFS, "a client handshake message is too long for the tool");
			if (peer->message_used < length)
				break;
			changes = peer->read_changes;
			error = tlspeer_handshake(peer, peer->message, length);
			if (error != 0)
				return error;
			memmove(peer->message, peer->message + length, peer->message_used - length);
			peer->message_used -= length;
			if (changes != peer->read_changes && peer->message_used > 0U)
				return tlspeer_fail(peer, EPROTO, "client handshake bytes span a key change");
		}
		return 0;
	case SSLC_CT_CHANGE_CIPHER_SPEC:
		/* The byte 1, in plaintext, not inside a handshake message. */
		if (size != 1U || content[0] != 1U || protected_record != 0)
			return tlspeer_fail(peer, EPROTO, "a malformed change_cipher_spec");
		if (peer->message_used > 0U)
			return tlspeer_fail(peer, EPROTO, "a change_cipher_spec inside a handshake message");
		error = tlspeer_change_cipher_spec(peer);
		if (error != 0)
			return error;
		return 0;
	case SSLC_CT_ALERT:
		/* Recorded. */
		if (size != 2U)
			return tlspeer_fail(peer, EINVAL, "a malformed alert");
		peer->alert_count++;
		peer->alert_level = content[0];
		peer->alert_description = content[1];
		peer->alert_protected = protected_record;
		if (content[1] == SSLC_ALERT_CLOSE_NOTIFY)
			peer->close_notify = 1;
		return 0;
	case SSLC_CT_APPLICATION_DATA:
		/* Only protected, kept for the test. */
		if (protected_record == 0)
			return tlspeer_fail(peer, EPROTO, "application data in plaintext");
		if (size > sizeof(peer->data) - peer->data_size)
			return tlspeer_fail(peer, ENOBUFS, "too much application data for the tool");
		memcpy(peer->data + peer->data_size, content, size);
		peer->data_size += size;
		return 0;
	default:
		return tlspeer_fail(peer, EPROTO, "a record of an unknown content type");
	}
}

/* Takes one whole record. */
static int __attribute__((unused))
tlspeer_on_record(
	struct tlspeer *peer,
	uint8_t *record,
	size_t size)
{
	struct sslc_record opened;
	unsigned type;
	int error;

	/* Plaintext records: before keys, CCS always, and TLS 1.3 plaintext alerts. */
	type = record[0];
	peer->records_in++;
	if (peer->read_keyed == 0 ||
	    type == SSLC_CT_CHANGE_CIPHER_SPEC ||
	    (peer->config.version == SSLC_TLS13 && type == SSLC_CT_ALERT)) {
		error = tlspeer_on_content(peer, type, record + SSLC_RECORD_HEADER, size - SSLC_RECORD_HEADER, 0);
		if (error != 0)
			return error;
		return 0;
	}

	/* TLS 1.3 protected records show application_data outside. */
	if (peer->config.version == SSLC_TLS13 && type != SSLC_CT_APPLICATION_DATA)
		return tlspeer_fail(peer, EPROTO, "a plaintext record after the client's keys");

	/* Opened with the client's keys. */
	error = sslc_record_open(&peer->read_dir, record, size, &opened);
	if (error != 0)
		return tlspeer_fail(peer, error, "a client record does not open");
	error = tlspeer_on_content(peer, opened.type, opened.data, opened.size, 1);
	if (error != 0)
		return error;

	/* Taken. */
	return 0;
}

/* Takes size bytes the client wrote. */
static int __attribute__((unused))
tlspeer_input(
	struct tlspeer *peer,
	const uint8_t *data,
	size_t size)
{
	size_t length;
	int error;

	/* A failed script takes nothing more. */
	if (peer->state == TLSPEER_FAILED)
		return peer->error;

	/* Fed piece by piece through the record buffer. */
	while (size > 0U) {
		/* As much as the buffer takes. */
		length = sizeof(peer->in) - peer->in_used;
		if (length > size)
			length = size;
		memcpy(peer->in + peer->in_used, data, length);
		peer->in_used += length;
		data += length;
		size -= length;

		/* Each whole record in the buffer. */
		while (peer->in_used >= SSLC_RECORD_HEADER) {
			length = SSLC_RECORD_HEADER + cryc_load_be16(peer->in + 3);
			if (length > sizeof(peer->in))
				return tlspeer_fail(peer, EINVAL, "a client record is longer than TLS allows");
			if (peer->in_used < length)
				break;
			error = tlspeer_on_record(peer, peer->in, length);
			if (error != 0)
				return error;
			memmove(peer->in, peer->in + length, peer->in_used - length);
			peer->in_used -= length;
		}
	}

	/* Taken. */
	return 0;
}

#endif
