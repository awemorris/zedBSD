/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The client handshake of TLS 1.3 (RFC 8446 2, 4, Appendix A.1) and TLS
 * 1.2 (RFC 5246 7.3 and 7.4 with the ECDHE suites of RFC 8422), as a
 * state machine that does no I/O: the connection (sslc-conn.h) feeds it
 * whole handshake messages and ChangeCipherSpec records, and it answers
 * with actions in order (messages to send, keys to install, the end of the
 * handshake).  This makes it testable against fixed transcripts (RFC 8448)
 * without a socket.
 *
 * sslc-hs.c holds what both versions share (the ClientHello, the
 * ServerHello and the choice of version, certificate validation, the
 * HelloRetryRequest); sslc-hs13.c the TLS 1.3 states; sslc-hs12.c the TLS
 * 1.2 states.
 *
 * Checks, besides the syntax sslc-msg.h checks:
 *   - ServerHello: legacy_version 0x0303; the version is
 *     supported_versions' when present (it must be SSLC_TLS13 and allowed;
 *     else illegal_parameter), else legacy_version's (SSLC_TLS12, allowed;
 *     else protocol_version); a TLS 1.2 ServerHello whose random ends with
 *     sslc_tls_downgrade12 or sslc_tls_downgrade11 while TLS 1.3 was
 *     offered is illegal_parameter (4.1.3); the suite was offered and
 *     belongs to the version (illegal_parameter); compression 0
 *     (illegal_parameter); TLS 1.3: legacy_session_id_echo equals the
 *     session id sent (illegal_parameter), key_share present with a group
 *     a share was sent for (illegal_parameter; missing: missing_extension);
 *     TLS 1.2: renegotiation_info present and empty (RFC 5746 3.4; absent
 *     is handshake_failure, as OpenSSL 3 refuses servers without it),
 *     extended master secret used when echoed.
 *   - HelloRetryRequest (4.1.4): at most one (a second is
 *     unexpected_message); its suite is TLS 1.3's and offered; its group,
 *     when present, is in supported_groups and has no share sent yet
 *     (illegal_parameter); it must change something (a cookie or a group;
 *     else illegal_parameter).  The second ClientHello has the new share
 *     and the cookie; the ServerHello after it must name the same suite.
 *   - Certificate: each DER parses (cryc_x509_parse; else bad_certificate),
 *     the chain is validated (cryc_chain_verify) with the configuration's
 *     parameters; the leaf's key is supported (else
 *     unsupported_certificate).
 *   - CertificateVerify (TLS 1.3, 4.4.3): the scheme was offered, is not
 *     RSA PKCS #1 (illegal_parameter), suits the leaf's key (an ECDSA
 *     scheme names the key's curve; RSA-PSS rsae needs an rsaEncryption
 *     key; else illegal_parameter); the signature over 64 spaces, "TLS
 *     1.3, server CertificateVerify", a zero byte and the transcript hash
 *     verifies (else decrypt_error).
 *   - ServerKeyExchange (TLS 1.2, RFC 8422 5.4): the group was offered
 *     (illegal_parameter); the point decodes (illegal_parameter); the
 *     scheme was offered and suits the suite's authentication and the key
 *     (illegal_parameter); the signature over client_random,
 *     server_random and the params verifies (decrypt_error).
 *   - Finished: verify_data equals the expected value, compared in
 *     constant time (decrypt_error).
 *   - (EC)DHE: an x25519 result of all zeros, or a P-256 point that does
 *     not validate, is illegal_parameter (7.4.2; RFC 8422 5.11).
 *
 * Certificate verdicts.  When the chain does not validate and
 * verify_peer is 1, the configuration's callback (if any) decides: it
 * returns 1 to go on (the verdict stays in verify_code) or 0 to fail.
 * Without a callback the handshake fails.  The alert follows the verdict:
 * EXPIRED and NOT_YET_VALID give certificate_expired; UNABLE_TO_GET_ISSUER,
 * DEPTH_ZERO_SELF_SIGNED and SELF_SIGNED_IN_CHAIN give unknown_ca;
 * UNSUPPORTED_ALGORITHM and KEY_TOO_SMALL give unsupported_certificate;
 * the others give bad_certificate.  When verify_peer is 0 the verdict is
 * recorded and the handshake goes on (OpenSSL's SSL_VERIFY_NONE).  A valid
 * chain with a callback calls it with preverified 1; its 0 fails with
 * certificate_unknown.
 *
 * Messages a TLS 1.3 client accepts after the handshake (4.6):
 * NewSessionTicket (checked and dropped), KeyUpdate (the read keys move
 * to the next generation; when update_requested, a KeyUpdate with
 * update_not_requested is sent and the write keys move too).  Anything
 * else is unexpected_message.  A TLS 1.2 HelloRequest after the handshake
 * is answered with a warning no_renegotiation alert (RFC 5246 7.4.1.1).
 *
 * A CertificateRequest during the handshake is answered with an empty
 * Certificate (no client certificate; TLS 1.3 sends no CertificateVerify
 * then, 4.4.2.4).
 *
 * Secrets (private keys, shared secrets, schedules) are wiped as soon as
 * they are no longer needed and by sslc_hs_free.
 */

#ifndef LIBSSL_COMPAT_SSLC_HS_H
#define LIBSSL_COMPAT_SSLC_HS_H

#include "sslc-keysched.h"
#include "sslc-msg.h"
#include "../libcrypto-compat/cryc-x25519.h"

/* The most actions one call can produce (KeyUpdate: set read keys, send, set write keys; a flight: send, CCS, set keys, send, done). */
#define SSLC_HS_MAX_ACTIONS	8U

/* The room for the messages of one flight (a ClientHello with its shares is under 600 bytes). */
#define SSLC_HS_OUT_MAX		2048U

/* What a connection does for an action, in the order given. */
enum sslc_hs_action_kind {
	SSLC_HS_SEND = 1,
	SSLC_HS_SEND_CCS = 2,
	SSLC_HS_SEND_WARNING = 3,
	SSLC_HS_SET_READ_KEYS = 4,
	SSLC_HS_SET_WRITE_KEYS = 5,
	SSLC_HS_DONE = 6
};

/*
 * One action.  SSLC_HS_SEND: the handshake bytes at hs->out + offset,
 * size bytes (one or more whole messages), sent as handshake records with
 * the write keys in force; first_hello is 1 for the first ClientHello.
 * SSLC_HS_SEND_CCS: one change_cipher_spec record (the byte 1).
 * SSLC_HS_SEND_WARNING: a warning alert with description alert.
 * SSLC_HS_SET_READ_KEYS / _WRITE_KEYS: install keys (version13 tells the
 * record format).  SSLC_HS_DONE: the handshake is complete.  keys is
 * secret; the connection wipes the action list after use.
 */
struct sslc_hs_action {
	enum sslc_hs_action_kind kind;
	size_t offset;
	size_t size;
	int first_hello;
	unsigned alert;
	int version13;
	struct sslc_traffic_keys keys;
};

/* The actions of one call, in order. */
struct sslc_hs_actions {
	struct sslc_hs_action list[SSLC_HS_MAX_ACTIONS];
	size_t count;
};

/*
 * How the handshake is to be done (filled by the public layer from the
 * SSL_CTX and the SSL; not changed during the handshake).
 *
 *   min_version, max_version  SSLC_TLS12 or SSLC_TLS13.
 *   store                     The trust anchors (read under its lock).
 *   params                    The time, host and address to verify.
 *   server_name               The SNI name (normalized), or NULL.
 *   verify_peer               1 to fail on a chain that does not
 *                             validate (subject to callback).
 *   callback, callback_context  Called once with preverified and the
 *                             verdict when verify_peer is 1 (may be NULL).
 *   random, random_context    The random source (cryc_os_random in use).
 */
struct sslc_hs_config {
	uint16_t min_version;
	uint16_t max_version;
	struct cryc_store *store;
	struct cryc_verify_params params;
	const char *server_name;
	int verify_peer;
	int (*callback)(void *context, int preverified, enum cryc_verify_code code);
	void *callback_context;
	cryc_random_fn random;
	void *random_context;
};

/* The states (Appendix A.1 for TLS 1.3; RFC 5246 7.3's flow for TLS 1.2). */
enum sslc_hs_state {
	SSLC_HS_START = 0,
	SSLC_HS_WAIT_SERVER_HELLO,
	SSLC_HS13_WAIT_ENCRYPTED_EXTENSIONS,
	SSLC_HS13_WAIT_CERTIFICATE_OR_REQUEST,
	SSLC_HS13_WAIT_CERTIFICATE,
	SSLC_HS13_WAIT_CERTIFICATE_VERIFY,
	SSLC_HS13_WAIT_FINISHED,
	SSLC_HS13_CONNECTED,
	SSLC_HS12_WAIT_CERTIFICATE,
	SSLC_HS12_WAIT_SERVER_KEY_EXCHANGE,
	SSLC_HS12_WAIT_SERVER_HELLO_DONE,
	SSLC_HS12_WAIT_CHANGE_CIPHER_SPEC,
	SSLC_HS12_WAIT_FINISHED,
	SSLC_HS12_CONNECTED,
	SSLC_HS_FAILED
};

/*
 * One client handshake and, after it, the state the post-handshake
 * messages need.
 *
 * config is borrowed.  state the state.  version and suite once the
 * ServerHello is read.  hello is the ClientHello as sent (its arrays point
 * into suites, groups, schemes and the share keys below).  The key pairs:
 * x25519 always (its share is sent first); secp256r1 made only when a
 * HelloRetryRequest or a TLS 1.2 ServerKeyExchange asks for it (ec points
 * to a malloc'd struct cryc_ecp then).  transcript and schedule (TLS 1.3)
 * or master (TLS 1.2).  The peer's certificates are copied (peer_der,
 * malloc'd each) and parsed (peer); verify is the validation's result and
 * verify_code its verdict (CRYC_VERIFY_OK until a chain is seen without
 * fault).  request_context and has_request record a CertificateRequest.
 * out holds the bytes of SSLC_HS_SEND actions until the next call.  alert
 * is the alert to send when a call fails (0 when none: an internal error
 * such as ENOMEM, sent as internal_error by the connection).
 */
struct sslc_hs {
	const struct sslc_hs_config *config;
	enum sslc_hs_state state;
	uint16_t version;
	const struct sslc_suite *suite;
	struct sslc_client_hello hello;
	uint16_t suites[16];
	uint16_t groups[4];
	uint16_t schemes[12];
	uint8_t cookie[SSLC_MSG_COOKIE_MAX];
	int retried;
	uint8_t x25519_private[CRYC_X25519_SIZE];
	uint8_t x25519_public[CRYC_X25519_SIZE];
	struct cryc_ecp *ec;
	uint8_t p256_private[32];
	uint8_t p256_public[65];
	int has_p256;
	struct sslc_transcript transcript;
	struct sslc_tls13_schedule schedule;
	uint8_t server_random[32];
	uint8_t master[SSLC_TLS12_MASTER];
	int extended_master_secret;
	uint16_t server_group;
	uint8_t server_point[CRYC_ECP_MAX_POINT];
	size_t server_point_size;
	uint8_t *peer_der[CRYC_CHAIN_MAX_PEER];
	size_t peer_der_size[CRYC_CHAIN_MAX_PEER];
	struct cryc_x509 peer[CRYC_CHAIN_MAX_PEER];
	size_t peer_count;
	struct cryc_verify_result verify;
	enum cryc_verify_code verify_code;
	uint8_t request_context[255];
	size_t request_context_size;
	int has_request;
	uint8_t out[SSLC_HS_OUT_MAX];
	unsigned alert;
};

/*
 * Starts a handshake: makes the x25519 key pair and the random values
 * with config->random and builds the ClientHello (the offered suites of
 * the versions; groups x25519 then secp256r1; schemes
 * ecdsa_secp256r1_sha256, ecdsa_secp384r1_sha384, rsa_pss_rsae_sha256,
 * rsa_pss_rsae_sha384, rsa_pss_rsae_sha512, rsa_pkcs1_sha256,
 * rsa_pkcs1_sha384, rsa_pkcs1_sha512; one x25519 share).
 *
 * actions receives SSLC_HS_SEND of the ClientHello (first_hello 1).
 * Returns 0, EIO (random failed), ENOMEM or EINVAL (a configuration out of
 * range).  On an error the state is SSLC_HS_FAILED.
 */
int sslc_hs_start(struct sslc_hs *hs, const struct sslc_hs_config *config, struct sslc_hs_actions *actions);

/*
 * Takes one whole handshake message (header and body, at most
 * SSLC_HANDSHAKE_MAX bytes) received in the state hs->state, checks it,
 * moves the state and gives the resulting actions.
 *
 * A message of a type the state does not expect is unexpected_message
 * (EPROTO).  Returns 0, or an error (EINVAL, EPROTO, EBADMSG, ENOTSUP,
 * ENOMEM, EIO) with hs->alert set and the state SSLC_HS_FAILED; later
 * calls on a failed handshake return EPROTO.
 */
int sslc_hs_message(struct sslc_hs *hs, const uint8_t *message, size_t size, struct sslc_hs_actions *actions);

/*
 * Takes a change_cipher_spec record whose content is the byte 1 (the
 * connection checks the content: another value is unexpected_message).
 *
 * TLS 1.2 in SSLC_HS12_WAIT_CHANGE_CIPHER_SPEC: gives
 * SSLC_HS_SET_READ_KEYS with the server's keys.  TLS 1.3, or before the
 * version is known, after the first ClientHello and before the server's
 * Finished: dropped (5, D.4; libssl-compat tolerates any number there,
 * bounded by the connection's SSLC_CONN_MAX_EMPTY).  Otherwise
 * unexpected_message (EPROTO, state failed).
 */
int sslc_hs_change_cipher_spec(struct sslc_hs *hs, struct sslc_hs_actions *actions);

/* Returns 1 when the handshake is complete (a CONNECTED state), else 0. */
int sslc_hs_connected(const struct sslc_hs *hs);

/* Wipes every secret, frees the peer's certificate copies and ec; the structure may then be discarded. */
void sslc_hs_free(struct sslc_hs *hs);

/*
 * The functions between the three handshake files (sslc-hs.c,
 * sslc-hs13.c, sslc-hs12.c); not used outside them.
 */

/*
 * Appends one whole handshake message (header and body) to hs->out and
 * to the transcript, and an SSLC_HS_SEND action for it (merged with the
 * previous action when that is a SEND of the bytes just before).
 * Returns 0, or ENOBUFS (hs->out or the action list is full: an internal
 * error).
 */
int sslc_hs_queue_message(struct sslc_hs *hs, struct sslc_hs_actions *actions, const uint8_t *message, size_t size);

/*
 * Appends an action without bytes: SSLC_HS_SET_READ_KEYS or
 * _WRITE_KEYS with keys (copied) and version13, SSLC_HS_SEND_CCS,
 * SSLC_HS_SEND_WARNING with alert, SSLC_HS_DONE (keys NULL for those).
 * Returns 0 or ENOBUFS.
 */
int sslc_hs_queue_action(struct sslc_hs_actions *actions, enum sslc_hs_action_kind kind, int version13, const struct sslc_traffic_keys *keys, unsigned alert);

/*
 * Takes a Certificate body (TLS 1.3 when tls13 is 1): copies and parses
 * every certificate, validates the chain and applies the verdict rules
 * of the file comment (the callback, the alert).  Returns 0 to go on, or
 * an error with hs->alert set.
 */
int sslc_hs_certificate(struct sslc_hs *hs, int tls13, const uint8_t *body, size_t size);

/*
 * Computes the (EC)DHE shared secret with the server's share of group
 * (x25519 or secp256r1) into shared (32 bytes).  Returns 0, or EPROTO
 * with hs->alert illegal_parameter for a bad share.
 */
int sslc_hs_shared_secret(struct sslc_hs *hs, uint16_t group, const uint8_t *share, size_t share_size, uint8_t *shared);

/* TLS 1.3 after its ServerHello: the handshake secrets and keys (sslc-hs13.c).  Returns 0 or an error with hs->alert. */
int sslc_hs13_server_hello(struct sslc_hs *hs, const struct sslc_server_hello *hello, struct sslc_hs_actions *actions);

/* TLS 1.3 messages after the ServerHello, and after the handshake (sslc-hs13.c); message is the whole message. */
int sslc_hs13_message(struct sslc_hs *hs, unsigned type, const uint8_t *message, size_t size, struct sslc_hs_actions *actions);

/* TLS 1.2 after its ServerHello (sslc-hs12.c). */
int sslc_hs12_server_hello(struct sslc_hs *hs, const struct sslc_server_hello *hello, struct sslc_hs_actions *actions);

/* TLS 1.2 messages after the ServerHello, and HelloRequest after the handshake (sslc-hs12.c). */
int sslc_hs12_message(struct sslc_hs *hs, unsigned type, const uint8_t *message, size_t size, struct sslc_hs_actions *actions);

/* TLS 1.2 ChangeCipherSpec in its state (sslc-hs12.c). */
int sslc_hs12_change_cipher_spec(struct sslc_hs *hs, struct sslc_hs_actions *actions);

#endif
