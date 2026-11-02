/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The TLS 1.3 states of the client handshake.
 */

#include <string.h>

#include "sslc-hs.h"

/* The bytes of spaces that start the content a CertificateVerify signs (RFC 8446 4.4.3). */
#define SSLC_HS13_CV_PAD		64U

/* The largest KeyUpdate message: its 4-byte header and one byte (4.6.3). */
#define SSLC_HS13_KEY_UPDATE_SIZE	5U

/* The largest client flight after the server Finished: an empty Certificate and a Finished. */
#define SSLC_HS13_FLIGHT_MAX		(4U + 1U + 255U + 3U + 4U + CRYC_HASH_MAX_SIZE)

/*
 * The context string of the server's CertificateVerify (4.4.3), signed
 * with its terminating zero byte, which is the separator the content
 * needs.
 * Values referenced: RFC 8446 section 4.4.3.
 */
static const char sslc_hs13_cv_context[] = "TLS 1.3, server CertificateVerify";

/* Records the alert of a failure; returns error. */
static int sslc_hs13_fail(struct sslc_hs *hs, unsigned alert, int error);

/* Takes EncryptedExtensions (4.3.1). */
static int sslc_hs13_encrypted_extensions(struct sslc_hs *hs, const uint8_t *message, size_t size);

/* Takes a CertificateRequest (4.3.2). */
static int sslc_hs13_certificate_request(struct sslc_hs *hs, const uint8_t *message, size_t size);

/* Takes the server's Certificate (4.4.2). */
static int sslc_hs13_certificate(struct sslc_hs *hs, const uint8_t *message, size_t size);

/* Takes the server's CertificateVerify (4.4.3). */
static int sslc_hs13_certificate_verify(struct sslc_hs *hs, const uint8_t *message, size_t size);

/* Checks that a CertificateVerify scheme was offered and suits the leaf's key; fills params. */
static int sslc_hs13_scheme(struct sslc_hs *hs, uint16_t scheme, struct cryc_sig_params *params);

/* Takes the server's Finished and sends the client's last flight (4.4.4). */
static int sslc_hs13_finished(struct sslc_hs *hs, const uint8_t *message, size_t size, struct sslc_hs_actions *actions);

/* Queues the client's flight after the server Finished. */
static int sslc_hs13_client_flight(struct sslc_hs *hs, struct sslc_hs_actions *actions);

/* Takes a NewSessionTicket after the handshake (4.6.1). */
static int sslc_hs13_new_session_ticket(struct sslc_hs *hs, const uint8_t *message, size_t size);

/* Takes a KeyUpdate after the handshake (4.6.3). */
static int sslc_hs13_key_update(struct sslc_hs *hs, const uint8_t *message, size_t size, struct sslc_hs_actions *actions);

/* Queues a key action with the record keys of a traffic secret. */
static int sslc_hs13_queue_keys(struct sslc_hs *hs, struct sslc_hs_actions *actions, enum sslc_hs_action_kind kind, const uint8_t *secret);

/*
 * Takes a TLS 1.3 ServerHello: makes the handshake secrets and keys.
 *
 * The transcript already holds the ServerHello.  The shared secret makes
 * the Handshake Secret (7.1); the server's handshake traffic keys are for
 * every record after the ServerHello, so they are given at once.  The
 * client's own handshake keys wait for its flight after the server
 * Finished, so that the compatibility change_cipher_spec goes out before
 * the first protected record (D.4).
 */
int
sslc_hs13_server_hello(
	struct sslc_hs *hs,
	const struct sslc_server_hello *hello,
	struct sslc_hs_actions *actions)
{
	uint8_t shared[SSLC_HS_SHARED_MAX];
	uint8_t digest[CRYC_HASH_MAX_SIZE];
	size_t digest_size;
	size_t shared_size;
	int error;

	/* The (EC)DHE shared secret with the server's share (an invalid share is illegal_parameter). */
	error = sslc_hs_shared_secret(hs, hello->share_group, hello->share_key, hello->share_size, shared, &shared_size);
	if (error != 0)
		return error;

	/* The private keys have done their work. */
	cryc_wipe(hs->x25519_private, sizeof(hs->x25519_private));
	cryc_wipe(hs->ec_private, sizeof(hs->ec_private));

	/* The schedule from the Early Secret (no PSK) to the Handshake Secret over ClientHello..ServerHello. */
	error = sslc_tls13_start(&hs->schedule, hs->suite->hash);
	if (error != 0) {
		cryc_wipe(shared, sizeof(shared));
		return sslc_hs13_fail(hs, 0, error);
	}

	/* The transcript hash through the ServerHello, of the suite's hash. */
	digest_size = sslc_transcript_hash(&hs->transcript, digest);
	if (digest_size != hs->schedule.size) {
		cryc_wipe(shared, sizeof(shared));
		return sslc_hs13_fail(hs, 0, EINVAL);
	}

	/* The Handshake Secret and the two handshake traffic secrets; the shared secret is spent. */
	error = sslc_tls13_handshake(&hs->schedule, shared, shared_size, digest);
	cryc_wipe(shared, sizeof(shared));
	if (error != 0)
		return sslc_hs13_fail(hs, 0, error);

	/* The server's handshake keys protect everything it sends next. */
	error = sslc_hs13_queue_keys(hs, actions, SSLC_HS_SET_READ_KEYS, hs->schedule.server_handshake);
	if (error != 0)
		return error;

	/* Succeeded: EncryptedExtensions comes next. */
	hs->state = SSLC_HS13_WAIT_ENCRYPTED_EXTENSIONS;
	return 0;
}

/*
 * Takes the TLS 1.3 messages after the ServerHello, and after the
 * handshake; message is the whole message.
 *
 * The order is Appendix A.1's: EncryptedExtensions, an optional
 * CertificateRequest, Certificate, CertificateVerify, Finished; after the
 * handshake NewSessionTicket and KeyUpdate (4.6).  Any other message in a
 * state is unexpected_message.
 */
int
sslc_hs13_message(
	struct sslc_hs *hs,
	unsigned type,
	const uint8_t *message,
	size_t size,
	struct sslc_hs_actions *actions)
{
	int error;

	/* By the state, the one message (or two) it takes. */
	error = 0;
	switch (hs->state) {
	case SSLC_HS13_WAIT_ENCRYPTED_EXTENSIONS:
		if (type != SSLC_HS_ENCRYPTED_EXTENSIONS)
			return sslc_hs13_fail(hs, SSLC_ALERT_UNEXPECTED_MESSAGE, EPROTO);
		error = sslc_hs13_encrypted_extensions(hs, message, size);
		break;
	case SSLC_HS13_WAIT_CERTIFICATE_OR_REQUEST:
		if (type == SSLC_HS_CERTIFICATE_REQUEST)
			error = sslc_hs13_certificate_request(hs, message, size);
		else if (type == SSLC_HS_CERTIFICATE)
			error = sslc_hs13_certificate(hs, message, size);
		else
			return sslc_hs13_fail(hs, SSLC_ALERT_UNEXPECTED_MESSAGE, EPROTO);
		break;
	case SSLC_HS13_WAIT_CERTIFICATE:
		if (type != SSLC_HS_CERTIFICATE)
			return sslc_hs13_fail(hs, SSLC_ALERT_UNEXPECTED_MESSAGE, EPROTO);
		error = sslc_hs13_certificate(hs, message, size);
		break;
	case SSLC_HS13_WAIT_CERTIFICATE_VERIFY:
		if (type != SSLC_HS_CERTIFICATE_VERIFY)
			return sslc_hs13_fail(hs, SSLC_ALERT_UNEXPECTED_MESSAGE, EPROTO);
		error = sslc_hs13_certificate_verify(hs, message, size);
		break;
	case SSLC_HS13_WAIT_FINISHED:
		if (type != SSLC_HS_FINISHED)
			return sslc_hs13_fail(hs, SSLC_ALERT_UNEXPECTED_MESSAGE, EPROTO);
		error = sslc_hs13_finished(hs, message, size, actions);
		break;
	case SSLC_HS13_CONNECTED:
		if (type == SSLC_HS_NEW_SESSION_TICKET)
			error = sslc_hs13_new_session_ticket(hs, message, size);
		else if (type == SSLC_HS_KEY_UPDATE)
			error = sslc_hs13_key_update(hs, message, size, actions);
		else
			return sslc_hs13_fail(hs, SSLC_ALERT_UNEXPECTED_MESSAGE, EPROTO);
		break;
	default:
		return sslc_hs13_fail(hs, SSLC_ALERT_UNEXPECTED_MESSAGE, EPROTO);
	}
	if (error != 0)
		return error;

	/* Succeeded: the message is taken. */
	return 0;
}

/* Records the alert of a failure (sslc_hs_message then marks the handshake failed). */
static int
sslc_hs13_fail(
	struct sslc_hs *hs,
	unsigned alert,
	int error)
{
	/* The alert the connection sends (0: an internal error). */
	hs->alert = alert;

	/* Reports the failure. */
	return error;
}

/* Takes EncryptedExtensions. */
static int
sslc_hs13_encrypted_extensions(
	struct sslc_hs *hs,
	const uint8_t *message,
	size_t size)
{
	unsigned alert;
	int server_name_ack;
	int error;

	/* Its syntax, and only the extensions the client may be answered with. */
	error = sslc_msg_parse_encrypted_extensions(message + 4, size - 4U, &server_name_ack, &alert);
	if (error != 0)
		return sslc_hs13_fail(hs, alert, error);

	/* A server_name acknowledgement answers a server_name the client sent, and nothing else (4.2). */
	if (server_name_ack && hs->config->server_name == NULL)
		return sslc_hs13_fail(hs, SSLC_ALERT_UNSUPPORTED_EXTENSION, EPROTO);

	/* Succeeded: part of the transcript; a CertificateRequest or the Certificate comes next. */
	sslc_transcript_update(&hs->transcript, message, size);
	hs->state = SSLC_HS13_WAIT_CERTIFICATE_OR_REQUEST;
	return 0;
}

/* Takes a CertificateRequest. */
static int
sslc_hs13_certificate_request(
	struct sslc_hs *hs,
	const uint8_t *message,
	size_t size)
{
	const uint8_t *context;
	size_t context_size;
	unsigned alert;
	int error;

	/* Its syntax, and the context to echo. */
	error = sslc_msg_parse_certificate_request(message + 4, size - 4U, 1, &context, &context_size, &alert);
	if (error != 0)
		return sslc_hs13_fail(hs, alert, error);

	/* The context's one-byte length bounds it by the room kept for it. */
	if (context_size > sizeof(hs->request_context))
		return sslc_hs13_fail(hs, SSLC_ALERT_DECODE_ERROR, EINVAL);

	/*
	 * has_request makes the client answer with an empty Certificate
	 * carrying this context before its Finished (4.4.2.4).
	 */
	memcpy(hs->request_context, context, context_size);
	hs->request_context_size = context_size;
	hs->has_request = 1;

	/* Succeeded: part of the transcript; the Certificate comes next. */
	sslc_transcript_update(&hs->transcript, message, size);
	hs->state = SSLC_HS13_WAIT_CERTIFICATE;
	return 0;
}

/* Takes the server's Certificate. */
static int
sslc_hs13_certificate(
	struct sslc_hs *hs,
	const uint8_t *message,
	size_t size)
{
	int error;

	/* The certificates, the chain's validation and the verdict (with hs->alert on a failure). */
	error = sslc_hs_certificate(hs, 1, message + 4, size - 4U);
	if (error != 0)
		return error;

	/* Succeeded: part of the transcript; the CertificateVerify comes next. */
	sslc_transcript_update(&hs->transcript, message, size);
	hs->state = SSLC_HS13_WAIT_CERTIFICATE_VERIFY;
	return 0;
}

/*
 * Takes the server's CertificateVerify: the scheme must suit the leaf's
 * key, and the signature over 64 spaces, the context string, a zero byte
 * and the transcript hash through the Certificate must verify (4.4.3).
 */
static int
sslc_hs13_certificate_verify(
	struct sslc_hs *hs,
	const uint8_t *message,
	size_t size)
{
	uint8_t content[SSLC_HS13_CV_PAD + sizeof(sslc_hs13_cv_context) + CRYC_HASH_MAX_SIZE];
	struct cryc_sig_params params;
	const uint8_t *signature;
	size_t signature_size;
	size_t content_size;
	size_t digest_size;
	uint16_t scheme;
	unsigned alert;
	int error;

	/* Its syntax: the scheme and the signature. */
	error = sslc_msg_parse_certificate_verify(message + 4, size - 4U, &scheme, &signature, &signature_size, &alert);
	if (error != 0)
		return sslc_hs13_fail(hs, alert, error);

	/* The scheme was offered, may sign a TLS 1.3 handshake, and suits the leaf's key. */
	error = sslc_hs13_scheme(hs, scheme, &params);
	if (error != 0)
		return error;

	/* The signed content: the padding, the context with its zero byte, the transcript hash. */
	memset(content, 0x20, SSLC_HS13_CV_PAD);
	memcpy(content + SSLC_HS13_CV_PAD, sslc_hs13_cv_context, sizeof(sslc_hs13_cv_context));
	content_size = SSLC_HS13_CV_PAD + sizeof(sslc_hs13_cv_context);
	digest_size = sslc_transcript_hash(&hs->transcript, content + content_size);
	if (digest_size == 0U)
		return sslc_hs13_fail(hs, 0, EINVAL);
	content_size += digest_size;

	/* The signature by the leaf's key. */
	error = cryc_signature_verify(&hs->peer[0].key, &params, content, content_size, signature, signature_size);
	if (error == EBADMSG)
		return sslc_hs13_fail(hs, SSLC_ALERT_DECRYPT_ERROR, EBADMSG);
	if (error == ENOTSUP)
		return sslc_hs13_fail(hs, SSLC_ALERT_UNSUPPORTED_CERTIFICATE, ENOTSUP);
	if (error != 0)
		return sslc_hs13_fail(hs, SSLC_ALERT_BAD_CERTIFICATE, EINVAL);

	/* Succeeded: part of the transcript; the Finished comes next. */
	sslc_transcript_update(&hs->transcript, message, size);
	hs->state = SSLC_HS13_WAIT_FINISHED;
	return 0;
}

/*
 * Checks that a CertificateVerify scheme was offered and suits the leaf's
 * key, and fills params.  RSA PKCS #1 v1.5 may not sign a TLS 1.3
 * handshake (4.2.3); an ECDSA scheme names the key's curve; RSA-PSS rsae
 * needs an rsaEncryption key and uses a salt of the hash's length.
 */
static int
sslc_hs13_scheme(
	struct sslc_hs *hs,
	uint16_t scheme,
	struct cryc_sig_params *params)
{
	const struct cryc_pubkey *key;
	enum cryc_key_type key_type;
	enum cryc_curve_id curve;
	size_t index;
	int offered;

	/* The scheme must be one the ClientHello offered. */
	offered = 0;
	for (index = 0; index < hs->hello.scheme_count; index++) {
		if (hs->schemes[index] == scheme)
			offered = 1;
	}
	if (!offered)
		return sslc_hs13_fail(hs, SSLC_ALERT_ILLEGAL_PARAMETER, EPROTO);

	/* The algorithm and the key it needs. */
	memset(params, 0, sizeof(*params));
	curve = CRYC_CURVE_NONE;
	switch (scheme) {
	case SSLC_SCHEME_ECDSA_SECP256R1_SHA256:
		params->alg = CRYC_SIG_ECDSA_SHA256;
		key_type = CRYC_KEY_EC;
		curve = CRYC_CURVE_P256;
		break;
	case SSLC_SCHEME_ECDSA_SECP384R1_SHA384:
		params->alg = CRYC_SIG_ECDSA_SHA384;
		key_type = CRYC_KEY_EC;
		curve = CRYC_CURVE_P384;
		break;
	case SSLC_SCHEME_RSA_PSS_RSAE_SHA256:
		params->alg = CRYC_SIG_RSA_PSS_SHA256;
		params->salt_size = CRYC_SHA256_SIZE;
		key_type = CRYC_KEY_RSA;
		break;
	case SSLC_SCHEME_RSA_PSS_RSAE_SHA384:
		params->alg = CRYC_SIG_RSA_PSS_SHA384;
		params->salt_size = CRYC_SHA384_SIZE;
		key_type = CRYC_KEY_RSA;
		break;
	case SSLC_SCHEME_RSA_PSS_RSAE_SHA512:
		params->alg = CRYC_SIG_RSA_PSS_SHA512;
		params->salt_size = CRYC_SHA512_SIZE;
		key_type = CRYC_KEY_RSA;
		break;
	default:
		/* RSA PKCS #1 v1.5, offered for certificates only. */
		return sslc_hs13_fail(hs, SSLC_ALERT_ILLEGAL_PARAMETER, EPROTO);
	}

	/* The leaf's key must be of that type, and for ECDSA on that curve. */
	key = &hs->peer[0].key;
	if (key->type != key_type)
		return sslc_hs13_fail(hs, SSLC_ALERT_ILLEGAL_PARAMETER, EPROTO);
	if (key_type == CRYC_KEY_EC && key->curve != curve)
		return sslc_hs13_fail(hs, SSLC_ALERT_ILLEGAL_PARAMETER, EPROTO);

	/* Succeeded: params name the algorithm. */
	return 0;
}

/*
 * Takes the server's Finished: its verify_data over the transcript through
 * the CertificateVerify, compared in constant time (4.4.4).  Then the
 * Master Secret and the application secrets over the transcript through
 * this Finished (7.1), and the client's flight.
 */
static int
sslc_hs13_finished(
	struct sslc_hs *hs,
	const uint8_t *message,
	size_t size,
	struct sslc_hs_actions *actions)
{
	uint8_t digest[CRYC_HASH_MAX_SIZE];
	uint8_t expected[CRYC_HASH_MAX_SIZE];
	size_t digest_size;
	int equal;
	int error;

	/* verify_data is as long as the hash. */
	if (size - 4U != hs->schedule.size)
		return sslc_hs13_fail(hs, SSLC_ALERT_DECODE_ERROR, EINVAL);

	/* The value the server's handshake secret gives over the transcript so far. */
	digest_size = sslc_transcript_hash(&hs->transcript, digest);
	if (digest_size != hs->schedule.size)
		return sslc_hs13_fail(hs, 0, EINVAL);
	error = sslc_tls13_finished(hs->schedule.hash, hs->schedule.server_handshake, digest, expected);
	if (error != 0)
		return sslc_hs13_fail(hs, 0, error);

	/* The comparison, in constant time. */
	equal = cryc_ct_equal(message + 4, expected, hs->schedule.size);
	cryc_wipe(expected, sizeof(expected));
	if (!equal)
		return sslc_hs13_fail(hs, SSLC_ALERT_DECRYPT_ERROR, EBADMSG);

	/* Part of the transcript; the application secrets are over ClientHello..server Finished. */
	sslc_transcript_update(&hs->transcript, message, size);
	digest_size = sslc_transcript_hash(&hs->transcript, digest);
	if (digest_size != hs->schedule.size)
		return sslc_hs13_fail(hs, 0, EINVAL);
	error = sslc_tls13_master(&hs->schedule, digest);
	if (error != 0)
		return sslc_hs13_fail(hs, 0, error);

	/* The client's flight and the switch to the application keys. */
	error = sslc_hs13_client_flight(hs, actions);
	if (error != 0)
		return error;

	/*
	 * The Master Secret and the handshake secrets have no further use (no
	 * resumption, no exporter); only the application secrets stay, for
	 * KeyUpdate.
	 */
	cryc_wipe(hs->schedule.secret, sizeof(hs->schedule.secret));
	cryc_wipe(hs->schedule.client_handshake, sizeof(hs->schedule.client_handshake));
	cryc_wipe(hs->schedule.server_handshake, sizeof(hs->schedule.server_handshake));

	/* Succeeded: the handshake is complete. */
	hs->state = SSLC_HS13_CONNECTED;
	return 0;
}

/*
 * Queues the client's flight after the server Finished, in order: the
 * compatibility change_cipher_spec unless one went before the second
 * ClientHello (D.4), the client's handshake keys, an empty Certificate
 * when one was asked for (4.4.2.4, no CertificateVerify then), the
 * Finished (4.4.4), then the application keys of both directions and the
 * end of the handshake.
 */
static int
sslc_hs13_client_flight(
	struct sslc_hs *hs,
	struct sslc_hs_actions *actions)
{
	uint8_t message[SSLC_HS13_FLIGHT_MAX];
	uint8_t digest[CRYC_HASH_MAX_SIZE];
	uint8_t verify_data[CRYC_HASH_MAX_SIZE];
	size_t digest_size;
	size_t written;
	int error;

	/* The compatibility record before the first protected one, once per handshake. */
	if (!hs->retried) {
		error = sslc_hs_queue_action(actions, SSLC_HS_SEND_CCS, 0, NULL, 0);
		if (error != 0)
			return sslc_hs13_fail(hs, 0, error);
	}

	/* The client's handshake keys protect its Certificate and Finished. */
	error = sslc_hs13_queue_keys(hs, actions, SSLC_HS_SET_WRITE_KEYS, hs->schedule.client_handshake);
	if (error != 0)
		return error;

	/* An empty Certificate echoing the request's context, when the server asked for one. */
	if (hs->has_request) {
		error = sslc_msg_write_empty_certificate(1, hs->request_context, hs->request_context_size, message, sizeof(message), &written);
		if (error != 0)
			return sslc_hs13_fail(hs, 0, error);
		error = sslc_hs_queue_message(hs, actions, message, written);
		if (error != 0)
			return sslc_hs13_fail(hs, 0, error);
	}

	/* The Finished over the transcript through the server Finished (and the Certificate). */
	digest_size = sslc_transcript_hash(&hs->transcript, digest);
	if (digest_size != hs->schedule.size)
		return sslc_hs13_fail(hs, 0, EINVAL);
	error = sslc_tls13_finished(hs->schedule.hash, hs->schedule.client_handshake, digest, verify_data);
	if (error != 0)
		return sslc_hs13_fail(hs, 0, error);
	error = sslc_msg_write_finished(verify_data, hs->schedule.size, message, sizeof(message), &written);
	cryc_wipe(verify_data, sizeof(verify_data));
	if (error != 0)
		return sslc_hs13_fail(hs, 0, error);
	error = sslc_hs_queue_message(hs, actions, message, written);
	if (error != 0)
		return sslc_hs13_fail(hs, 0, error);

	/* The application keys: the client's for what it sends next, the server's for what it reads. */
	error = sslc_hs13_queue_keys(hs, actions, SSLC_HS_SET_WRITE_KEYS, hs->schedule.client_application);
	if (error != 0)
		return error;
	error = sslc_hs13_queue_keys(hs, actions, SSLC_HS_SET_READ_KEYS, hs->schedule.server_application);
	if (error != 0)
		return error;

	/* The end of the handshake. */
	error = sslc_hs_queue_action(actions, SSLC_HS_DONE, 0, NULL, 0);
	if (error != 0)
		return sslc_hs13_fail(hs, 0, error);

	/* Succeeded: the flight is queued. */
	return 0;
}

/* Takes a NewSessionTicket after the handshake: checked and dropped (no resumption). */
static int
sslc_hs13_new_session_ticket(
	struct sslc_hs *hs,
	const uint8_t *message,
	size_t size)
{
	unsigned alert;
	int error;

	/* Its syntax only. */
	error = sslc_msg_parse_new_session_ticket(message + 4, size - 4U, &alert);
	if (error != 0)
		return sslc_hs13_fail(hs, alert, error);

	/* Succeeded: the ticket is dropped. */
	return 0;
}

/*
 * Takes a KeyUpdate after the handshake (4.6.3): the server's next
 * generation of keys for every record after it (7.2); when the server
 * asks, a KeyUpdate that does not ask, sent under the current keys, and
 * then the client's next generation.
 */
static int
sslc_hs13_key_update(
	struct sslc_hs *hs,
	const uint8_t *message,
	size_t size,
	struct sslc_hs_actions *actions)
{
	uint8_t answer[SSLC_HS13_KEY_UPDATE_SIZE];
	size_t written;
	unsigned alert;
	int update_requested;
	int error;

	/* Its syntax: one byte, 0 or 1. */
	error = sslc_msg_parse_key_update(message + 4, size - 4U, &update_requested, &alert);
	if (error != 0)
		return sslc_hs13_fail(hs, alert, error);

	/* The server's next generation, for the records after this one. */
	error = sslc_tls13_update_secret(hs->schedule.hash, hs->schedule.server_application);
	if (error != 0)
		return sslc_hs13_fail(hs, 0, error);
	error = sslc_hs13_queue_keys(hs, actions, SSLC_HS_SET_READ_KEYS, hs->schedule.server_application);
	if (error != 0)
		return error;

	/* Without a request the client's keys stay. */
	if (!update_requested)
		return 0;

	/* The answer, which does not ask in turn, under the client's current keys. */
	error = sslc_msg_write_key_update(0, answer, sizeof(answer), &written);
	if (error != 0)
		return sslc_hs13_fail(hs, 0, error);
	error = sslc_hs_queue_message(hs, actions, answer, written);
	if (error != 0)
		return sslc_hs13_fail(hs, 0, error);

	/* Then the client's next generation. */
	error = sslc_tls13_update_secret(hs->schedule.hash, hs->schedule.client_application);
	if (error != 0)
		return sslc_hs13_fail(hs, 0, error);
	error = sslc_hs13_queue_keys(hs, actions, SSLC_HS_SET_WRITE_KEYS, hs->schedule.client_application);
	if (error != 0)
		return error;

	/* Succeeded: both directions moved. */
	return 0;
}

/* Queues a key action with the record keys of a traffic secret. */
static int
sslc_hs13_queue_keys(
	struct sslc_hs *hs,
	struct sslc_hs_actions *actions,
	enum sslc_hs_action_kind kind,
	const uint8_t *secret)
{
	struct sslc_traffic_keys keys;
	int error;

	/* The key and the IV of the secret (7.3). */
	error = sslc_tls13_traffic_keys(hs->schedule.hash, hs->suite, secret, &keys);
	if (error != 0) {
		cryc_wipe(&keys, sizeof(keys));
		return sslc_hs13_fail(hs, 0, error);
	}

	/* The action holds a copy; this one is wiped. */
	error = sslc_hs_queue_action(actions, kind, 1, &keys, 0);
	cryc_wipe(&keys, sizeof(keys));
	if (error != 0)
		return sslc_hs13_fail(hs, 0, error);

	/* Succeeded: the keys are queued. */
	return 0;
}
