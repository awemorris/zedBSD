/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The TLS 1.2 states of the client handshake.
 */

#include <stdlib.h>
#include <string.h>

#include "sslc-hs.h"

/* The size of an uncompressed secp256r1 and secp384r1 public key (SEC 1 2.3.3: 0x04, x and y). */
#define SSLC_HS12_P256_POINT		65U
#define SSLC_HS12_P384_POINT		97U

/* The first byte of an uncompressed point (SEC 1 2.3.3). */
#define SSLC_HS12_POINT_UNCOMPRESSED	4U

/* The most bytes a ServerKeyExchange's ServerECDHParams take: curve_type, the group and a point of up to 255 bytes (RFC 8422 5.4). */
#define SSLC_HS12_PARAMS_MAX		(1U + 2U + 1U + 255U)

/* The most bytes of one client message of the flight: its 4-byte header, the length byte and a secp384r1 point (RFC 8422 5.7). */
#define SSLC_HS12_MESSAGE_MAX		(4U + 1U + SSLC_HS12_P384_POINT)

/* The extensions a TLS 1.2 ServerHello must not carry: the TLS 1.3 ones (RFC 8446 4.2's table). */
#define SSLC_HS12_SEEN_FORBIDDEN	(SSLC_SEEN_SUPPORTED_VERSIONS | SSLC_SEEN_KEY_SHARE | SSLC_SEEN_COOKIE)

/* Records the alert of a failure; returns error. */
static int sslc_hs12_fail(struct sslc_hs *hs, unsigned alert, int error);

/* Takes a HelloRequest in any state (RFC 5246 7.4.1.1). */
static int sslc_hs12_hello_request(struct sslc_hs *hs, size_t size, struct sslc_hs_actions *actions);

/* Takes the server's Certificate (RFC 5246 7.4.2). */
static int sslc_hs12_certificate(struct sslc_hs *hs, const uint8_t *message, size_t size);

/* Takes the server's ServerKeyExchange (RFC 8422 5.4). */
static int sslc_hs12_server_key_exchange(struct sslc_hs *hs, const uint8_t *message, size_t size);

/* Checks that a ServerKeyExchange scheme was offered, suits the suite and the leaf's key; fills params. */
static int sslc_hs12_scheme(struct sslc_hs *hs, uint16_t scheme, struct cryc_sig_params *params);

/* Takes a CertificateRequest (RFC 5246 7.4.4). */
static int sslc_hs12_certificate_request(struct sslc_hs *hs, const uint8_t *message, size_t size);

/* Takes the ServerHelloDone and sends the client's flight (RFC 5246 7.4.5). */
static int sslc_hs12_server_hello_done(struct sslc_hs *hs, const uint8_t *message, size_t size, struct sslc_hs_actions *actions);

/* Queues the client's flight: Certificate when asked, ClientKeyExchange, change_cipher_spec, keys, Finished. */
static int sslc_hs12_client_flight(struct sslc_hs *hs, const uint8_t *shared, size_t shared_size, struct sslc_hs_actions *actions);

/* Takes the server's Finished (RFC 5246 7.4.9). */
static int sslc_hs12_finished(struct sslc_hs *hs, const uint8_t *message, size_t size, struct sslc_hs_actions *actions);

/* Makes the key pair of group (secp256r1 or secp384r1, once); returns 0, ENOMEM or EIO. */
static int sslc_hs12_make_ec(struct sslc_hs *hs, uint16_t group);

/* Queues a key action with the record keys given. */
static int sslc_hs12_queue_keys(struct sslc_hs *hs, struct sslc_hs_actions *actions, enum sslc_hs_action_kind kind, const struct sslc_traffic_keys *keys);

/*
 * TLS 1.2 after its ServerHello: the checks of what a TLS 1.2 server may
 * say.
 *
 * The transcript already holds the ServerHello and hs->version, suite and
 * server_random are set.  The client always offers renegotiation (the
 * signalling suite), so the server must answer with an empty
 * renegotiation_info (RFC 5746 3.4); the client always offers extended
 * master secret, which the server may echo (RFC 7627 5.2).
 */
int
sslc_hs12_server_hello(
	struct sslc_hs *hs,
	const struct sslc_server_hello *hello,
	struct sslc_hs_actions *actions)
{
	int differs;

	UNUSED_PARAMETER(actions);

	/* A HelloRetryRequest is TLS 1.3's: a TLS 1.2 ServerHello does not carry its random. */
	if (hello->is_retry) {
		hs->alert = SSLC_ALERT_ILLEGAL_PARAMETER;
		return EPROTO;
	}

	/* The TLS 1.3 extensions have no place in a TLS 1.2 ServerHello (RFC 8446 4.2's table). */
	if ((hello->seen & SSLC_HS12_SEEN_FORBIDDEN) != 0U) {
		hs->alert = SSLC_ALERT_ILLEGAL_PARAMETER;
		return EPROTO;
	}

	/* A server that does not answer renegotiation_info is not one the client talks to (RFC 5746 3.4). */
	if ((hello->seen & SSLC_SEEN_RENEGOTIATION_INFO) == 0U) {
		hs->alert = SSLC_ALERT_HANDSHAKE_FAILURE;
		return EPROTO;
	}

	/* The first handshake has nothing renegotiated: its renegotiated_connection is empty (RFC 5746 3.4). */
	if (hello->renegotiation_info_size != 0U) {
		hs->alert = SSLC_ALERT_HANDSHAKE_FAILURE;
		return EPROTO;
	}

	/* A server_name acknowledgement answers a server_name the client sent, and nothing else (RFC 5246 7.4.1.4). */
	if ((hello->seen & SSLC_SEEN_SERVER_NAME) != 0U && hs->config->server_name == NULL) {
		hs->alert = SSLC_ALERT_UNSUPPORTED_EXTENSION;
		return EPROTO;
	}

	/*
	 * A session id equal to the one sent while it is not empty would
	 * resume a session the client never had (the 32 random bytes of the
	 * middlebox compatibility mode are not a session).
	 */
	if (hello->session_id_size != 0U && hello->session_id_size == hs->hello.session_id_size) {
		differs = memcmp(hello->session_id, hs->hello.session_id, hello->session_id_size);
		if (differs == 0) {
			hs->alert = SSLC_ALERT_ILLEGAL_PARAMETER;
			return EPROTO;
		}
	}

	/* The master secret is the extended one only when the server echoed the extension (RFC 7627 5.2). */
	hs->extended_master_secret = 0;
	if ((hello->seen & SSLC_SEEN_EXTENDED_MASTER) != 0U)
		hs->extended_master_secret = 1;

	/* Succeeded: the server's Certificate comes next. */
	hs->state = SSLC_HS12_WAIT_CERTIFICATE;
	return 0;
}

/*
 * TLS 1.2 messages after the ServerHello, and HelloRequest after the
 * handshake.
 *
 * The order is RFC 5246 7.3's with the ECDHE suites: Certificate,
 * ServerKeyExchange, an optional CertificateRequest, ServerHelloDone;
 * then the client's flight, the server's change_cipher_spec (its own
 * function) and Finished.  A HelloRequest is answered or ignored by the
 * state; any other message out of place is unexpected_message.
 */
int
sslc_hs12_message(
	struct sslc_hs *hs,
	unsigned type,
	const uint8_t *message,
	size_t size,
	struct sslc_hs_actions *actions)
{
	int error;

	/* A HelloRequest is never part of the handshake proper, whatever the state. */
	if (type == SSLC_HS_HELLO_REQUEST) {
		error = sslc_hs12_hello_request(hs, size, actions);
		if (error != 0)
			return error;
		return 0;
	}

	/* By the state, the message (or two) it takes. */
	error = 0;
	switch (hs->state) {
	case SSLC_HS12_WAIT_CERTIFICATE:
		if (type != SSLC_HS_CERTIFICATE)
			return sslc_hs12_fail(hs, SSLC_ALERT_UNEXPECTED_MESSAGE, EPROTO);
		error = sslc_hs12_certificate(hs, message, size);
		break;
	case SSLC_HS12_WAIT_SERVER_KEY_EXCHANGE:
		if (type != SSLC_HS_SERVER_KEY_EXCHANGE)
			return sslc_hs12_fail(hs, SSLC_ALERT_UNEXPECTED_MESSAGE, EPROTO);
		error = sslc_hs12_server_key_exchange(hs, message, size);
		break;
	case SSLC_HS12_WAIT_SERVER_HELLO_DONE:
		if (type == SSLC_HS_CERTIFICATE_REQUEST)
			error = sslc_hs12_certificate_request(hs, message, size);
		else if (type == SSLC_HS_SERVER_HELLO_DONE)
			error = sslc_hs12_server_hello_done(hs, message, size, actions);
		else
			return sslc_hs12_fail(hs, SSLC_ALERT_UNEXPECTED_MESSAGE, EPROTO);
		break;
	case SSLC_HS12_WAIT_FINISHED:
		if (type != SSLC_HS_FINISHED)
			return sslc_hs12_fail(hs, SSLC_ALERT_UNEXPECTED_MESSAGE, EPROTO);
		error = sslc_hs12_finished(hs, message, size, actions);
		break;
	default:
		/* Before the server's change_cipher_spec, after the handshake, or in a state TLS 1.2 does not have. */
		return sslc_hs12_fail(hs, SSLC_ALERT_UNEXPECTED_MESSAGE, EPROTO);
	}
	if (error != 0)
		return error;

	/* Succeeded: the message is taken. */
	return 0;
}

/*
 * TLS 1.2 ChangeCipherSpec in its state: the server's write keys become
 * the client's read keys.
 *
 * The key block is derived again from the master secret instead of being
 * kept from the client's flight, so no second set of keys waits in the
 * handshake structure.
 */
int
sslc_hs12_change_cipher_spec(
	struct sslc_hs *hs,
	struct sslc_hs_actions *actions)
{
	struct sslc_traffic_keys client_keys;
	struct sslc_traffic_keys server_keys;
	int error;

	/* The key block of the master secret and the two randoms (RFC 5246 6.3). */
	error = sslc_tls12_key_block(hs->suite, hs->master, hs->hello.random, hs->server_random, &client_keys, &server_keys);
	cryc_wipe(&client_keys, sizeof(client_keys));
	if (error != 0) {
		cryc_wipe(&server_keys, sizeof(server_keys));
		return sslc_hs12_fail(hs, 0, error);
	}

	/* The server's keys protect every record it sends from here on. */
	error = sslc_hs12_queue_keys(hs, actions, SSLC_HS_SET_READ_KEYS, &server_keys);
	cryc_wipe(&server_keys, sizeof(server_keys));
	if (error != 0)
		return error;

	/* Succeeded: the server's Finished comes next, under those keys. */
	hs->state = SSLC_HS12_WAIT_FINISHED;
	return 0;
}

/* Records the alert of a failure (sslc_hs_message then marks the handshake failed). */
static int
sslc_hs12_fail(
	struct sslc_hs *hs,
	unsigned alert,
	int error)
{
	/* The alert the connection sends (0: an internal error). */
	hs->alert = alert;

	/* Reports the failure. */
	return error;
}

/*
 * Takes a HelloRequest: ignored while a handshake is under way and left
 * out of the transcript, answered with a warning no_renegotiation after it
 * (RFC 5246 7.4.1.1).  Its body is empty, so the whole message is its
 * 4-byte header.
 */
static int
sslc_hs12_hello_request(
	struct sslc_hs *hs,
	size_t size,
	struct sslc_hs_actions *actions)
{
	int error;

	/* The body of a HelloRequest is empty (RFC 5246 7.4.1.1). */
	if (size != 4U)
		return sslc_hs12_fail(hs, SSLC_ALERT_DECODE_ERROR, EINVAL);

	/* While the handshake goes on the client is negotiating a session: the request is ignored. */
	if (hs->state != SSLC_HS12_CONNECTED)
		return 0;

	/* After it the client does not renegotiate (no renegotiation is offered). */
	error = sslc_hs_queue_action(actions, SSLC_HS_SEND_WARNING, 0, NULL, SSLC_ALERT_NO_RENEGOTIATION);
	if (error != 0)
		return sslc_hs12_fail(hs, 0, error);

	/* Succeeded: the warning is queued. */
	return 0;
}

/* Takes the server's Certificate. */
static int
sslc_hs12_certificate(
	struct sslc_hs *hs,
	const uint8_t *message,
	size_t size)
{
	int error;

	/* The certificates, the chain's validation and the verdict (with hs->alert on a failure). */
	error = sslc_hs_certificate(hs, 0, message + 4, size - 4U);
	if (error != 0)
		return error;

	/* Succeeded: part of the transcript; the ServerKeyExchange comes next (the suites here are all ECDHE). */
	sslc_transcript_update(&hs->transcript, message, size);
	hs->state = SSLC_HS12_WAIT_SERVER_KEY_EXCHANGE;
	return 0;
}

/*
 * Takes the server's ServerKeyExchange: the group and the point it
 * names, and the signature over the two randoms and the parameters by the
 * leaf's key (RFC 8422 5.4).  The secp256r1 or secp384r1 key pair is made
 * now when the server chose that group; the shared secret waits for the
 * ServerHelloDone.
 */
static int
sslc_hs12_server_key_exchange(
	struct sslc_hs *hs,
	const uint8_t *message,
	size_t size)
{
	uint8_t content[64U + SSLC_HS12_PARAMS_MAX];
	struct sslc_server_key_exchange exchange;
	struct cryc_sig_params params;
	size_t index;
	size_t content_size;
	unsigned alert;
	int offered;
	int error;

	/* Its syntax: the group, the point, the scheme and the signature. */
	error = sslc_msg_parse_server_key_exchange(message + 4, size - 4U, &exchange, &alert);
	if (error != 0)
		return sslc_hs12_fail(hs, alert, error);

	/* The group must be one the ClientHello offered (RFC 8422 5.4). */
	offered = 0;
	for (index = 0; index < hs->hello.group_count; index++) {
		if (hs->groups[index] == exchange.group)
			offered = 1;
	}
	if (!offered)
		return sslc_hs12_fail(hs, SSLC_ALERT_ILLEGAL_PARAMETER, EPROTO);

	/* The point has the form of its group: a u-coordinate of 32 bytes, or an uncompressed point (RFC 8422 5.4.1; its place on the curve is checked with the shared secret). */
	if (exchange.group == SSLC_GROUP_X25519) {
		if (exchange.point_size != CRYC_X25519_SIZE)
			return sslc_hs12_fail(hs, SSLC_ALERT_ILLEGAL_PARAMETER, EPROTO);
	} else {
		if (exchange.point_size != (exchange.group == SSLC_GROUP_SECP384R1 ? SSLC_HS12_P384_POINT : SSLC_HS12_P256_POINT))
			return sslc_hs12_fail(hs, SSLC_ALERT_ILLEGAL_PARAMETER, EPROTO);
		if (exchange.point[0] != SSLC_HS12_POINT_UNCOMPRESSED)
			return sslc_hs12_fail(hs, SSLC_ALERT_ILLEGAL_PARAMETER, EPROTO);
	}

	/* The scheme was offered, suits the suite's authentication and the leaf's key. */
	error = sslc_hs12_scheme(hs, exchange.scheme, &params);
	if (error != 0)
		return error;

	/* The signed content: client_random, server_random, then the ServerECDHParams as sent (RFC 8422 5.4). */
	if (exchange.params_size > SSLC_HS12_PARAMS_MAX)
		return sslc_hs12_fail(hs, SSLC_ALERT_DECODE_ERROR, EINVAL);
	memcpy(content, hs->hello.random, 32);
	memcpy(content + 32, hs->server_random, 32);
	memcpy(content + 64, exchange.params, exchange.params_size);
	content_size = 64U + exchange.params_size;

	/* The signature by the leaf's key. */
	error = cryc_signature_verify(&hs->peer[0].key, &params, content, content_size, exchange.signature, exchange.signature_size);
	if (error == EBADMSG)
		return sslc_hs12_fail(hs, SSLC_ALERT_DECRYPT_ERROR, EBADMSG);
	if (error == ENOTSUP)
		return sslc_hs12_fail(hs, SSLC_ALERT_UNSUPPORTED_CERTIFICATE, ENOTSUP);
	if (error != 0)
		return sslc_hs12_fail(hs, SSLC_ALERT_BAD_CERTIFICATE, EINVAL);

	/* A server that chose secp256r1 or secp384r1 needs a key pair of ours to answer with. */
	if (exchange.group == SSLC_GROUP_SECP256R1 || exchange.group == SSLC_GROUP_SECP384R1) {
		error = sslc_hs12_make_ec(hs, exchange.group);
		if (error != 0)
			return sslc_hs12_fail(hs, 0, error);
	}

	/* The server's share, kept (the message does not outlive this call). */
	hs->server_group = exchange.group;
	memcpy(hs->server_point, exchange.point, exchange.point_size);
	hs->server_point_size = exchange.point_size;

	/* Succeeded: part of the transcript; a CertificateRequest or the ServerHelloDone comes next. */
	sslc_transcript_update(&hs->transcript, message, size);
	hs->state = SSLC_HS12_WAIT_SERVER_HELLO_DONE;
	return 0;
}

/*
 * Checks that a ServerKeyExchange scheme was offered, suits the suite's
 * authentication and the leaf's key, and fills params.
 *
 * In TLS 1.2 an ECDSA scheme names only the hash (RFC 8422 5.1.3), so
 * either curve's key goes with either ECDSA scheme.  RSA-PSS rsae and RSA
 * PKCS #1 v1.5 need an rsaEncryption key; RSA-PSS uses a salt of the
 * hash's length.
 */
static int
sslc_hs12_scheme(
	struct sslc_hs *hs,
	uint16_t scheme,
	struct cryc_sig_params *params)
{
	enum cryc_key_type key_type;
	enum sslc_auth_id auth;
	size_t index;
	int offered;

	/* The scheme must be one the ClientHello offered. */
	offered = 0;
	for (index = 0; index < hs->hello.scheme_count; index++) {
		if (hs->schemes[index] == scheme)
			offered = 1;
	}
	if (!offered)
		return sslc_hs12_fail(hs, SSLC_ALERT_ILLEGAL_PARAMETER, EPROTO);

	/* The algorithm, the key it needs and the authentication it belongs to. */
	memset(params, 0, sizeof(*params));
	switch (scheme) {
	case SSLC_SCHEME_ECDSA_SECP256R1_SHA256:
		params->alg = CRYC_SIG_ECDSA_SHA256;
		key_type = CRYC_KEY_EC;
		auth = SSLC_AUTH_ECDSA;
		break;
	case SSLC_SCHEME_ECDSA_SECP384R1_SHA384:
		params->alg = CRYC_SIG_ECDSA_SHA384;
		key_type = CRYC_KEY_EC;
		auth = SSLC_AUTH_ECDSA;
		break;
	case SSLC_SCHEME_RSA_PSS_RSAE_SHA256:
		params->alg = CRYC_SIG_RSA_PSS_SHA256;
		params->salt_size = CRYC_SHA256_SIZE;
		key_type = CRYC_KEY_RSA;
		auth = SSLC_AUTH_RSA;
		break;
	case SSLC_SCHEME_RSA_PSS_RSAE_SHA384:
		params->alg = CRYC_SIG_RSA_PSS_SHA384;
		params->salt_size = CRYC_SHA384_SIZE;
		key_type = CRYC_KEY_RSA;
		auth = SSLC_AUTH_RSA;
		break;
	case SSLC_SCHEME_RSA_PSS_RSAE_SHA512:
		params->alg = CRYC_SIG_RSA_PSS_SHA512;
		params->salt_size = CRYC_SHA512_SIZE;
		key_type = CRYC_KEY_RSA;
		auth = SSLC_AUTH_RSA;
		break;
	case SSLC_SCHEME_RSA_PKCS1_SHA256:
		params->alg = CRYC_SIG_RSA_PKCS1_SHA256;
		key_type = CRYC_KEY_RSA;
		auth = SSLC_AUTH_RSA;
		break;
	case SSLC_SCHEME_RSA_PKCS1_SHA384:
		params->alg = CRYC_SIG_RSA_PKCS1_SHA384;
		key_type = CRYC_KEY_RSA;
		auth = SSLC_AUTH_RSA;
		break;
	case SSLC_SCHEME_RSA_PKCS1_SHA512:
		params->alg = CRYC_SIG_RSA_PKCS1_SHA512;
		key_type = CRYC_KEY_RSA;
		auth = SSLC_AUTH_RSA;
		break;
	default:
		/* A scheme that was offered always has a case above; any other value is not one. */
		return sslc_hs12_fail(hs, SSLC_ALERT_ILLEGAL_PARAMETER, EPROTO);
	}

	/* The suite says how the server signs: ECDHE_ECDSA with ECDSA, ECDHE_RSA with RSA (RFC 8422 2). */
	if (hs->suite->auth != auth)
		return sslc_hs12_fail(hs, SSLC_ALERT_ILLEGAL_PARAMETER, EPROTO);

	/* The leaf's key must be of the scheme's type; for ECDSA either curve will do. */
	if (hs->peer[0].key.type != key_type)
		return sslc_hs12_fail(hs, SSLC_ALERT_ILLEGAL_PARAMETER, EPROTO);

	/* Succeeded: params name the algorithm. */
	return 0;
}

/* Takes a CertificateRequest: only its syntax is checked, and an empty Certificate will answer it. */
static int
sslc_hs12_certificate_request(
	struct sslc_hs *hs,
	const uint8_t *message,
	size_t size)
{
	const uint8_t *context;
	size_t context_size;
	unsigned alert;
	int error;

	/* One CertificateRequest per handshake (RFC 5246 7.3). */
	if (hs->has_request)
		return sslc_hs12_fail(hs, SSLC_ALERT_UNEXPECTED_MESSAGE, EPROTO);

	/* Its syntax (TLS 1.2's has no context to echo). */
	error = sslc_msg_parse_certificate_request(message + 4, size - 4U, 0, &context, &context_size, &alert);
	if (error != 0)
		return sslc_hs12_fail(hs, alert, error);

	/* has_request makes the client answer with an empty Certificate before its ClientKeyExchange (RFC 5246 7.4.6). */
	hs->has_request = 1;

	/* Succeeded: part of the transcript; the ServerHelloDone comes next. */
	sslc_transcript_update(&hs->transcript, message, size);
	return 0;
}

/*
 * Takes the ServerHelloDone, which ends the server's flight and has an
 * empty body (RFC 5246 7.4.5), computes the shared secret and sends the
 * client's flight.
 */
static int
sslc_hs12_server_hello_done(
	struct sslc_hs *hs,
	const uint8_t *message,
	size_t size,
	struct sslc_hs_actions *actions)
{
	uint8_t shared[SSLC_HS_SHARED_MAX];
	size_t shared_size;
	int error;

	/* The body is empty. */
	if (size != 4U)
		return sslc_hs12_fail(hs, SSLC_ALERT_DECODE_ERROR, EINVAL);

	/* Part of the transcript the keys and Finished are computed over. */
	sslc_transcript_update(&hs->transcript, message, size);

	/* The (EC)DHE shared secret with the server's point: the premaster secret (an invalid point is illegal_parameter). */
	error = sslc_hs_shared_secret(hs, hs->server_group, hs->server_point, hs->server_point_size, shared, &shared_size);
	if (error != 0)
		return error;

	/* The private keys have done their work. */
	cryc_wipe(hs->x25519_private, sizeof(hs->x25519_private));
	cryc_wipe(hs->ec_private, sizeof(hs->ec_private));

	/* The client's flight; the premaster secret is spent once the master secret is made. */
	error = sslc_hs12_client_flight(hs, shared, shared_size, actions);
	cryc_wipe(shared, sizeof(shared));
	if (error != 0)
		return error;

	/* Succeeded: the server's change_cipher_spec comes next. */
	hs->state = SSLC_HS12_WAIT_CHANGE_CIPHER_SPEC;
	return 0;
}

/*
 * Queues the client's flight in order: an empty Certificate when one was
 * asked for (RFC 5246 7.4.6), the ClientKeyExchange with our public point
 * (RFC 8422 5.7), the change_cipher_spec, the client's write keys, and
 * the Finished under them (RFC 5246 7.4.9).  The master secret is made
 * between the ClientKeyExchange and the Finished: over the session hash
 * through the ClientKeyExchange when extended master secret is used (RFC
 * 7627 4), else over the two randoms (RFC 5246 8.1).
 */
static int
sslc_hs12_client_flight(
	struct sslc_hs *hs,
	const uint8_t *shared,
	size_t shared_size,
	struct sslc_hs_actions *actions)
{
	uint8_t message[SSLC_HS12_MESSAGE_MAX];
	uint8_t context[CRYC_HASH_MAX_SIZE + 64U];
	uint8_t digest[CRYC_HASH_MAX_SIZE];
	uint8_t verify_data[SSLC_TLS12_VERIFY_DATA];
	struct sslc_traffic_keys client_keys;
	struct sslc_traffic_keys server_keys;
	const uint8_t *point;
	size_t point_size;
	size_t context_size;
	size_t digest_size;
	size_t written;
	int error;

	/* An empty Certificate, when the server asked for one. */
	if (hs->has_request) {
		error = sslc_msg_write_empty_certificate(0, NULL, 0, message, sizeof(message), &written);
		if (error != 0)
			return sslc_hs12_fail(hs, 0, error);
		error = sslc_hs_queue_message(hs, actions, message, written);
		if (error != 0)
			return sslc_hs12_fail(hs, 0, error);
	}

	/* The ClientKeyExchange carries our public point of the group the server chose. */
	point = hs->x25519_public;
	point_size = CRYC_X25519_SIZE;
	if (hs->server_group == SSLC_GROUP_SECP256R1 || hs->server_group == SSLC_GROUP_SECP384R1) {
		point = hs->ec_public;
		point_size = hs->ec_public_size;
	}
	error = sslc_msg_write_client_key_exchange(point, point_size, message, sizeof(message), &written);
	if (error != 0)
		return sslc_hs12_fail(hs, 0, error);
	error = sslc_hs_queue_message(hs, actions, message, written);
	if (error != 0)
		return sslc_hs12_fail(hs, 0, error);

	/* What the master secret is made over: the session hash through the ClientKeyExchange, or the two randoms. */
	if (hs->extended_master_secret) {
		context_size = sslc_transcript_hash(&hs->transcript, context);
		if (context_size == 0U)
			return sslc_hs12_fail(hs, 0, EINVAL);
	} else {
		memcpy(context, hs->hello.random, 32);
		memcpy(context + 32, hs->server_random, 32);
		context_size = 64U;
	}

	/* The master secret from the premaster secret. */
	error = sslc_tls12_master(hs->suite->hash, hs->extended_master_secret, shared, shared_size, context, context_size, hs->master);
	if (error != 0)
		return sslc_hs12_fail(hs, 0, error);

	/* The key block; the server's half is derived again when its change_cipher_spec comes. */
	error = sslc_tls12_key_block(hs->suite, hs->master, hs->hello.random, hs->server_random, &client_keys, &server_keys);
	cryc_wipe(&server_keys, sizeof(server_keys));
	if (error != 0) {
		cryc_wipe(&client_keys, sizeof(client_keys));
		return sslc_hs12_fail(hs, 0, error);
	}

	/* The switch to the client's keys: a change_cipher_spec record, then the keys for what follows. */
	error = sslc_hs_queue_action(actions, SSLC_HS_SEND_CCS, 0, NULL, 0);
	if (error != 0) {
		cryc_wipe(&client_keys, sizeof(client_keys));
		return sslc_hs12_fail(hs, 0, error);
	}
	error = sslc_hs12_queue_keys(hs, actions, SSLC_HS_SET_WRITE_KEYS, &client_keys);
	cryc_wipe(&client_keys, sizeof(client_keys));
	if (error != 0)
		return error;

	/* The Finished over the transcript through the ClientKeyExchange (and the Certificate), under the client's keys. */
	digest_size = sslc_transcript_hash(&hs->transcript, digest);
	if (digest_size == 0U)
		return sslc_hs12_fail(hs, 0, EINVAL);
	error = sslc_tls12_finished(hs->suite->hash, hs->master, 1, digest, verify_data);
	if (error != 0)
		return sslc_hs12_fail(hs, 0, error);
	error = sslc_msg_write_finished(verify_data, sizeof(verify_data), message, sizeof(message), &written);
	cryc_wipe(verify_data, sizeof(verify_data));
	if (error != 0)
		return sslc_hs12_fail(hs, 0, error);
	error = sslc_hs_queue_message(hs, actions, message, written);
	if (error != 0)
		return sslc_hs12_fail(hs, 0, error);

	/* Succeeded: the flight is queued. */
	return 0;
}

/*
 * Takes the server's Finished: its verify_data over the transcript through
 * the client's Finished, compared in constant time (RFC 5246 7.4.9).
 */
static int
sslc_hs12_finished(
	struct sslc_hs *hs,
	const uint8_t *message,
	size_t size,
	struct sslc_hs_actions *actions)
{
	uint8_t digest[CRYC_HASH_MAX_SIZE];
	uint8_t expected[SSLC_TLS12_VERIFY_DATA];
	size_t digest_size;
	int equal;
	int error;

	/* verify_data is 12 bytes for every suite here (RFC 5246 7.4.9). */
	if (size - 4U != SSLC_TLS12_VERIFY_DATA)
		return sslc_hs12_fail(hs, SSLC_ALERT_DECODE_ERROR, EINVAL);

	/* The value the master secret gives over the transcript so far, with the server's label. */
	digest_size = sslc_transcript_hash(&hs->transcript, digest);
	if (digest_size == 0U)
		return sslc_hs12_fail(hs, 0, EINVAL);
	error = sslc_tls12_finished(hs->suite->hash, hs->master, 0, digest, expected);
	if (error != 0)
		return sslc_hs12_fail(hs, 0, error);

	/* The comparison, in constant time. */
	equal = cryc_ct_equal(message + 4, expected, SSLC_TLS12_VERIFY_DATA);
	cryc_wipe(expected, sizeof(expected));
	if (!equal)
		return sslc_hs12_fail(hs, SSLC_ALERT_DECRYPT_ERROR, EBADMSG);

	/* The master secret has no further use (no resumption, no renegotiation, no exporter). */
	cryc_wipe(hs->master, sizeof(hs->master));

	/* The end of the handshake. */
	error = sslc_hs_queue_action(actions, SSLC_HS_DONE, 0, NULL, 0);
	if (error != 0)
		return sslc_hs12_fail(hs, 0, error);

	/* Succeeded: the handshake is complete. */
	hs->state = SSLC_HS12_CONNECTED;
	return 0;
}

/* Makes the key pair of group (once; a TLS 1.2 handshake has one ServerKeyExchange). */
static int
sslc_hs12_make_ec(
	struct sslc_hs *hs,
	uint16_t group)
{
	enum cryc_curve_id curve;
	int error;

	/* Made once per handshake. */
	if (hs->has_ec)
		return 0;

	/* The curve, on the heap (it is about 4 KiB). */
	curve = group == SSLC_GROUP_SECP384R1 ? CRYC_CURVE_P384 : CRYC_CURVE_P256;
	if (hs->ec == NULL) {
		hs->ec = malloc(sizeof(*hs->ec));
		if (hs->ec == NULL)
			return ENOMEM;
	}

	/* Prepared for arithmetic. */
	error = cryc_ecp_init(hs->ec, curve);
	if (error != 0)
		return error;

	/* The key pair from the configuration's random source. */
	error = cryc_ecc_keygen(hs->ec, hs->ec_private, hs->ec_public, hs->config->random, hs->config->random_context);
	if (error != 0)
		return EIO;

	/* Succeeded: has_ec tells the shared secret and the ClientKeyExchange that the pair exists. */
	hs->ec_group = group;
	hs->ec_public_size = 1U + 2U * hs->ec->size;
	hs->has_ec = 1;
	return 0;
}

/* Queues a key action with the record keys given (the TLS 1.2 record format). */
static int
sslc_hs12_queue_keys(
	struct sslc_hs *hs,
	struct sslc_hs_actions *actions,
	enum sslc_hs_action_kind kind,
	const struct sslc_traffic_keys *keys)
{
	int error;

	/* The action holds a copy of the keys; the caller wipes its own. */
	error = sslc_hs_queue_action(actions, kind, 0, keys, 0);
	if (error != 0)
		return sslc_hs12_fail(hs, 0, error);

	/* Succeeded: the keys are queued. */
	return 0;
}
