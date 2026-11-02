/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The client handshake: what TLS 1.3 and TLS 1.2 share.
 */

#include <stdlib.h>
#include <string.h>

#include "sslc-hs.h"

/* The size of a hello random and of the session id sent when TLS 1.3 is offered (RFC 8446 4.1.2, D.4). */
#define SSLC_HS_RANDOM_SIZE		32U
#define SSLC_HS_SESSION_ID_SIZE		32U

/* The size of an uncompressed secp256r1 public key (SEC 1 2.3.3: 0x04, x and y). */
#define SSLC_HS_P256_POINT		65U

/* The size of the (EC)DHE shared secret of both groups (RFC 8446 7.4.1 and 7.4.2). */
#define SSLC_HS_SHARED_SIZE		32U

/* The size of the downgrade marks at the end of a ServerHello random (RFC 8446 4.1.3). */
#define SSLC_HS_DOWNGRADE_SIZE		8U

/* The extensions a TLS 1.3 ServerHello may carry (RFC 8446 4.2's table: "SH"). */
#define SSLC_HS_SEEN_HELLO13		(SSLC_SEEN_SUPPORTED_VERSIONS | SSLC_SEEN_KEY_SHARE)

/* The extensions a HelloRetryRequest may carry (RFC 8446 4.2's table: "HRR"). */
#define SSLC_HS_SEEN_RETRY		(SSLC_SEEN_SUPPORTED_VERSIONS | SSLC_SEEN_KEY_SHARE | SSLC_SEEN_COOKIE)

/*
 * The groups of the ClientHello's supported_groups, in preference order:
 * x25519, whose share is sent first, then secp256r1, which a server may
 * ask for with a HelloRetryRequest.  Constant.
 */
static const uint16_t sslc_hs_offered_groups[2] = {
	SSLC_GROUP_X25519,
	SSLC_GROUP_SECP256R1
};

/*
 * The schemes of the ClientHello's signature_algorithms, in preference
 * order: ECDSA first, then RSA-PSS with an rsaEncryption key, then RSA
 * PKCS #1 v1.5 (which TLS 1.3 allows for certificates only, and TLS 1.2
 * for its ServerKeyExchange).  Constant.
 */
static const uint16_t sslc_hs_offered_schemes[8] = {
	SSLC_SCHEME_ECDSA_SECP256R1_SHA256,
	SSLC_SCHEME_ECDSA_SECP384R1_SHA384,
	SSLC_SCHEME_RSA_PSS_RSAE_SHA256,
	SSLC_SCHEME_RSA_PSS_RSAE_SHA384,
	SSLC_SCHEME_RSA_PSS_RSAE_SHA512,
	SSLC_SCHEME_RSA_PKCS1_SHA256,
	SSLC_SCHEME_RSA_PKCS1_SHA384,
	SSLC_SCHEME_RSA_PKCS1_SHA512
};

/* Records why the handshake failed and wipes its secrets; returns error. */
static int sslc_hs_fail(struct sslc_hs *hs, unsigned alert, int error);

/* Wipes the secrets a failed handshake no longer needs. */
static void sslc_hs_wipe_secrets(struct sslc_hs *hs);

/* Checks a configuration's ranges; returns 0 or EINVAL. */
static int sslc_hs_check_config(const struct sslc_hs_config *config);

/* Draws count bytes from the configuration's random source; returns 0 or EIO. */
static int sslc_hs_draw(const struct sslc_hs_config *config, uint8_t *out, size_t count);

/* Writes the ClientHello as hs->hello now stands and queues it. */
static int sslc_hs_send_hello(struct sslc_hs *hs, struct sslc_hs_actions *actions);

/* Takes a ServerHello or a HelloRetryRequest (the whole message). */
static int sslc_hs_server_hello(struct sslc_hs *hs, const uint8_t *message, size_t size, struct sslc_hs_actions *actions);

/* Decides the version a ServerHello selects; returns 0 or an error with hs->alert. */
static int sslc_hs_choose_version(struct sslc_hs *hs, const struct sslc_server_hello *hello, uint16_t *version);

/* Checks the suite a ServerHello names for the version; returns 0 or an error with hs->alert. */
static int sslc_hs_choose_suite(struct sslc_hs *hs, const struct sslc_server_hello *hello, uint16_t version, const struct sslc_suite **suite);

/* Checks that a TLS 1.3 ServerHello echoes the session id sent; returns 0 or an error with hs->alert. */
static int sslc_hs_check_echo(struct sslc_hs *hs, const struct sslc_server_hello *hello);

/* Checks a TLS 1.3 ServerHello's extensions and key share; returns 0 or an error with hs->alert. */
static int sslc_hs_check_hello13(struct sslc_hs *hs, const struct sslc_server_hello *hello);

/* Takes a HelloRetryRequest and sends the second ClientHello. */
static int sslc_hs_retry(struct sslc_hs *hs, const struct sslc_server_hello *hello, const struct sslc_suite *suite, const uint8_t *message, size_t size, struct sslc_hs_actions *actions);

/* Makes the secp256r1 key pair (once); returns 0, ENOMEM or EIO. */
static int sslc_hs_make_p256(struct sslc_hs *hs);

/* Returns 1 when the ClientHello offered suite id, else 0. */
static int sslc_hs_offered_suite(const struct sslc_hs *hs, uint16_t id);

/* Returns 1 when supported_groups names group, else 0. */
static int sslc_hs_offered_group(uint16_t group);

/* Returns 1 when the ClientHello now carries a key share of group, else 0. */
static int sslc_hs_has_share(const struct sslc_hs *hs, uint16_t group);

/* Returns the bytes of hs->out the actions of this call already use. */
static size_t sslc_hs_out_used(const struct sslc_hs_actions *actions);

/* Returns the alert a certificate verdict is sent with. */
static unsigned sslc_hs_verdict_alert(enum cryc_verify_code code);

/*
 * Starts a handshake: makes the x25519 key pair and the random values with
 * config->random and builds the ClientHello (the offered suites of the
 * versions; groups x25519 then secp256r1; schemes ecdsa_secp256r1_sha256,
 * ecdsa_secp384r1_sha384, rsa_pss_rsae_sha256, rsa_pss_rsae_sha384,
 * rsa_pss_rsae_sha512, rsa_pkcs1_sha256, rsa_pkcs1_sha384,
 * rsa_pkcs1_sha512; one x25519 share).
 *
 * The random draws come in the order the header gives: the client
 * random, the session id when TLS 1.3 is offered, the x25519 private key.
 */
int
sslc_hs_start(
	struct sslc_hs *hs,
	const struct sslc_hs_config *config,
	struct sslc_hs_actions *actions)
{
	int error;

	/* Refuses a call without its structures. */
	if (hs == NULL)
		return EINVAL;
	if (actions == NULL) {
		hs->state = SSLC_HS_FAILED;
		return EINVAL;
	}

	/* Starts from nothing: no action, no state, no key. */
	memset(hs, 0, sizeof(*hs));
	memset(actions, 0, sizeof(*actions));
	hs->state = SSLC_HS_START;

	/* Refuses a configuration out of range. */
	error = sslc_hs_check_config(config);
	if (error != 0) {
		hs->state = SSLC_HS_FAILED;
		return error;
	}
	hs->config = config;

	/* The suites of the versions allowed, then the groups and the schemes, all in preference order. */
	hs->hello.min_version = config->min_version;
	hs->hello.max_version = config->max_version;
	hs->hello.suite_count = sslc_tls_offered_suites(config->min_version, config->max_version, hs->suites, sizeof(hs->suites) / sizeof(hs->suites[0]));
	hs->hello.suites = hs->suites;
	memcpy(hs->groups, sslc_hs_offered_groups, sizeof(sslc_hs_offered_groups));
	hs->hello.groups = hs->groups;
	hs->hello.group_count = sizeof(sslc_hs_offered_groups) / sizeof(sslc_hs_offered_groups[0]);
	memcpy(hs->schemes, sslc_hs_offered_schemes, sizeof(sslc_hs_offered_schemes));
	hs->hello.schemes = hs->schemes;
	hs->hello.scheme_count = sizeof(sslc_hs_offered_schemes) / sizeof(sslc_hs_offered_schemes[0]);
	hs->hello.server_name = config->server_name;

	/* The client random, the first draw. */
	error = sslc_hs_draw(config, hs->hello.random, SSLC_HS_RANDOM_SIZE);
	if (error != 0)
		return sslc_hs_fail(hs, 0, error);

	/*
	 * The middlebox compatibility mode (D.4): a client that offers TLS
	 * 1.3 sends 32 random bytes as its legacy session id, the second
	 * draw; a TLS 1.2-only client sends none.
	 */
	if (config->max_version == SSLC_TLS13) {
		error = sslc_hs_draw(config, hs->hello.session_id, SSLC_HS_SESSION_ID_SIZE);
		if (error != 0)
			return sslc_hs_fail(hs, 0, error);
		hs->hello.session_id_size = SSLC_HS_SESSION_ID_SIZE;
	}

	/* The x25519 key pair, the third draw: always made, as TLS 1.2 may use it too. */
	error = cryc_x25519_keygen(hs->x25519_private, hs->x25519_public, config->random, config->random_context);
	if (error != 0)
		return sslc_hs_fail(hs, 0, EIO);

	/* TLS 1.3 sends its x25519 share at once (TLS 1.2 has no key_share). */
	if (config->max_version == SSLC_TLS13) {
		hs->hello.shares[0].group = SSLC_GROUP_X25519;
		hs->hello.shares[0].key = hs->x25519_public;
		hs->hello.shares[0].key_size = CRYC_X25519_SIZE;
		hs->hello.share_count = 1;
	}

	/* The transcript starts with the ClientHello, which goes out first. */
	sslc_transcript_init(&hs->transcript);
	error = sslc_hs_send_hello(hs, actions);
	if (error != 0) {
		cryc_wipe(actions, sizeof(*actions));
		return sslc_hs_fail(hs, 0, error);
	}

	/* Succeeded: the ClientHello is queued and the server's answer is awaited. */
	hs->state = SSLC_HS_WAIT_SERVER_HELLO;
	return 0;
}

/*
 * Takes one whole handshake message (header and body, at most
 * SSLC_HANDSHAKE_MAX bytes) received in the state hs->state, checks it,
 * moves the state and gives the resulting actions.
 */
int
sslc_hs_message(
	struct sslc_hs *hs,
	const uint8_t *message,
	size_t size,
	struct sslc_hs_actions *actions)
{
	unsigned type;
	size_t length;
	int error;

	/* Refuses a call without its structures. */
	if (hs == NULL)
		return EINVAL;
	if (actions == NULL)
		return sslc_hs_fail(hs, 0, EINVAL);

	/* Each call answers with its own actions; a failed handshake takes nothing more. */
	actions->count = 0;
	if (hs->state == SSLC_HS_FAILED)
		return EPROTO;

	/* A handshake not started expects nothing yet. */
	if (hs->state == SSLC_HS_START)
		return sslc_hs_fail(hs, SSLC_ALERT_UNEXPECTED_MESSAGE, EPROTO);

	/* One whole message: at least its header, within the limit. */
	if (message == NULL ||
	    size < 4U ||
	    size > SSLC_HANDSHAKE_MAX)
		return sslc_hs_fail(hs, SSLC_ALERT_DECODE_ERROR, EINVAL);

	/* The header's type and length; the length must be the rest. */
	sslc_msg_header(message, &type, &length);
	if (length != size - 4U)
		return sslc_hs_fail(hs, SSLC_ALERT_DECODE_ERROR, EINVAL);

	/* Hands the message to the part that owns the state. */
	error = 0;
	switch (hs->state) {
	case SSLC_HS_WAIT_SERVER_HELLO:
		if (type == SSLC_HS_SERVER_HELLO) {
			/* The server's answer to the ClientHello. */
			error = sslc_hs_server_hello(hs, message, size, actions);
		} else if (type == SSLC_HS_HELLO_REQUEST &&
			   hs->version == 0U &&
			   hs->config->min_version == SSLC_TLS12) {
			/*
			 * A TLS 1.2 server may ask for a new handshake while one is
			 * being negotiated: ignored, and not part of the transcript
			 * (RFC 5246 7.4.1.1).  Its body is empty.
			 */
			if (length != 0U)
				return sslc_hs_fail(hs, SSLC_ALERT_DECODE_ERROR, EINVAL);
		} else {
			/* Nothing else may come before the ServerHello. */
			return sslc_hs_fail(hs, SSLC_ALERT_UNEXPECTED_MESSAGE, EPROTO);
		}
		break;
	case SSLC_HS13_WAIT_ENCRYPTED_EXTENSIONS:
	case SSLC_HS13_WAIT_CERTIFICATE_OR_REQUEST:
	case SSLC_HS13_WAIT_CERTIFICATE:
	case SSLC_HS13_WAIT_CERTIFICATE_VERIFY:
	case SSLC_HS13_WAIT_FINISHED:
	case SSLC_HS13_CONNECTED:
		/* The TLS 1.3 states after the ServerHello, and after the handshake. */
		error = sslc_hs13_message(hs, type, message, size, actions);
		break;
	case SSLC_HS12_WAIT_CERTIFICATE:
	case SSLC_HS12_WAIT_SERVER_KEY_EXCHANGE:
	case SSLC_HS12_WAIT_SERVER_HELLO_DONE:
	case SSLC_HS12_WAIT_CHANGE_CIPHER_SPEC:
	case SSLC_HS12_WAIT_FINISHED:
	case SSLC_HS12_CONNECTED:
		/* The TLS 1.2 states after the ServerHello, and after the handshake. */
		error = sslc_hs12_message(hs, type, message, size, actions);
		break;
	default:
		/* A state that takes no message (a ClientHello is not sent yet, or the state is unknown). */
		return sslc_hs_fail(hs, SSLC_ALERT_UNEXPECTED_MESSAGE, EPROTO);
	}

	/* A failure leaves the handshake failed and its actions void. */
	if (error != 0) {
		cryc_wipe(actions, sizeof(*actions));
		return sslc_hs_fail(hs, hs->alert, error);
	}

	/* Succeeded: the message is taken and the actions say what follows. */
	return 0;
}

/*
 * Takes a change_cipher_spec record whose content is the byte 1 (the
 * connection checks the content: another value is unexpected_message).
 */
int
sslc_hs_change_cipher_spec(
	struct sslc_hs *hs,
	struct sslc_hs_actions *actions)
{
	int error;

	/* Refuses a call without its structures. */
	if (hs == NULL)
		return EINVAL;
	if (actions == NULL)
		return sslc_hs_fail(hs, 0, EINVAL);

	/* Each call answers with its own actions; a failed handshake takes nothing more. */
	actions->count = 0;
	if (hs->state == SSLC_HS_FAILED)
		return EPROTO;

	/* Where the record may come. */
	switch (hs->state) {
	case SSLC_HS12_WAIT_CHANGE_CIPHER_SPEC:
		/* TLS 1.2: the server's switch to its keys. */
		error = sslc_hs12_change_cipher_spec(hs, actions);
		if (error != 0) {
			cryc_wipe(actions, sizeof(*actions));
			return sslc_hs_fail(hs, hs->alert, error);
		}
		break;
	case SSLC_HS_WAIT_SERVER_HELLO:
	case SSLC_HS13_WAIT_ENCRYPTED_EXTENSIONS:
	case SSLC_HS13_WAIT_CERTIFICATE_OR_REQUEST:
	case SSLC_HS13_WAIT_CERTIFICATE:
	case SSLC_HS13_WAIT_CERTIFICATE_VERIFY:
	case SSLC_HS13_WAIT_FINISHED:
		/*
		 * After the first ClientHello and before the server's Finished a
		 * compatibility record is dropped without further processing
		 * (RFC 8446 5 and D.4); the connection bounds how many.
		 */
		break;
	default:
		/* Before the first ClientHello, after the server's Finished, or in another TLS 1.2 state. */
		return sslc_hs_fail(hs, SSLC_ALERT_UNEXPECTED_MESSAGE, EPROTO);
	}

	/* Succeeded: the record is taken. */
	return 0;
}

/*
 * Returns 1 when the handshake is complete (a CONNECTED state), else 0.
 */
int
sslc_hs_connected(
	const struct sslc_hs *hs)
{
	/* No handshake is not a connection. */
	if (hs == NULL)
		return 0;

	/* Either version's last state. */
	if (hs->state == SSLC_HS13_CONNECTED)
		return 1;
	if (hs->state == SSLC_HS12_CONNECTED)
		return 1;

	/* Not connected (yet, or ever). */
	return 0;
}

/*
 * Wipes every secret, frees the peer's certificate copies and ec; the
 * structure may then be discarded.
 *
 * The whole structure is wiped and its state left failed, so that a later
 * call on it refuses to work instead of reading freed copies.
 */
void
sslc_hs_free(
	struct sslc_hs *hs)
{
	size_t index;

	/* Nothing to free. */
	if (hs == NULL)
		return;

	/* The peer's certificate copies, every one that was made. */
	for (index = 0; index < hs->peer_count && index < CRYC_CHAIN_MAX_PEER; index++)
		free(hs->peer_der[index]);

	/* The secp256r1 curve, when one was made. */
	free(hs->ec);

	/* Every secret and every pointer goes; the state refuses further use. */
	cryc_wipe(hs, sizeof(*hs));
	hs->state = SSLC_HS_FAILED;
}

/*
 * Appends one whole handshake message (header and body) to hs->out and to
 * the transcript, and an SSLC_HS_SEND action for it (merged with the
 * previous action when that is a SEND of the bytes just before).
 *
 * The bytes of hs->out in use are those of this call's SEND actions; a
 * ClientHello sent before any HelloRetryRequest is marked first_hello and
 * is never merged.
 */
int
sslc_hs_queue_message(
	struct sslc_hs *hs,
	struct sslc_hs_actions *actions,
	const uint8_t *message,
	size_t size)
{
	struct sslc_hs_action *last;
	size_t used;
	int first_hello;
	int merge;

	/* A whole message has at least its header. */
	if (message == NULL || size < 4U)
		return EINVAL;

	/* The room left in hs->out after what this call already queued. */
	used = sslc_hs_out_used(actions);
	if (size > SSLC_HS_OUT_MAX - used)
		return ENOBUFS;

	/* The first ClientHello is the one record 5.1 lets carry the version 0x0301. */
	first_hello = 0;
	if (message[0] == SSLC_HS_CLIENT_HELLO && hs->retried == 0)
		first_hello = 1;

	/* The message joins the previous SEND when it continues that SEND's bytes. */
	last = NULL;
	if (actions->count > 0U)
		last = &actions->list[actions->count - 1U];
	merge = 0;
	if (last != NULL &&
	    last->kind == SSLC_HS_SEND &&
	    last->offset + last->size == used &&
	    last->first_hello == 0 &&
	    first_hello == 0)
		merge = 1;

	/* Either the previous SEND grows, or a new SEND is taken. */
	if (merge) {
		last->size += size;
	} else {
		/* A new action needs a free place in the list. */
		if (actions->count >= SSLC_HS_MAX_ACTIONS)
			return ENOBUFS;

		/* The new SEND starts where the bytes of this call end. */
		last = &actions->list[actions->count];
		memset(last, 0, sizeof(*last));
		last->kind = SSLC_HS_SEND;
		last->offset = used;
		last->size = size;
		last->first_hello = first_hello;
		actions->count++;
	}

	/* The bytes go out as they are and are part of the transcript (4.4.1). */
	memcpy(hs->out + used, message, size);
	sslc_transcript_update(&hs->transcript, message, size);

	/* Succeeded: the message is queued. */
	return 0;
}

/*
 * Appends an action without bytes: SSLC_HS_SET_READ_KEYS or _WRITE_KEYS
 * with keys (copied) and version13, SSLC_HS_SEND_CCS, SSLC_HS_SEND_WARNING
 * with alert, SSLC_HS_DONE (keys NULL for those).
 */
int
sslc_hs_queue_action(
	struct sslc_hs_actions *actions,
	enum sslc_hs_action_kind kind,
	int version13,
	const struct sslc_traffic_keys *keys,
	unsigned alert)
{
	struct sslc_hs_action *action;
	int with_keys;

	/* A key action carries keys; the others carry none. */
	with_keys = 0;
	if (kind == SSLC_HS_SET_READ_KEYS || kind == SSLC_HS_SET_WRITE_KEYS)
		with_keys = 1;
	if (with_keys && keys == NULL)
		return EINVAL;

	/* The list holds a bounded number of actions. */
	if (actions->count >= SSLC_HS_MAX_ACTIONS)
		return ENOBUFS;

	/* The action, with the keys copied when it has some. */
	action = &actions->list[actions->count];
	memset(action, 0, sizeof(*action));
	action->kind = kind;
	action->version13 = version13;
	action->alert = alert;
	if (with_keys)
		action->keys = *keys;
	actions->count++;

	/* Succeeded: the action is queued. */
	return 0;
}

/*
 * Takes a Certificate body (TLS 1.3 when tls13 is 1): copies and parses
 * every certificate, validates the chain and applies the verdict rules of
 * the file comment (the callback, the alert).
 *
 * A verdict that ends the handshake is EPROTO with the verdict's alert; a
 * leaf whose key libssl-compat cannot use is ENOTSUP with
 * unsupported_certificate.
 */
int
sslc_hs_certificate(
	struct sslc_hs *hs,
	int tls13,
	const uint8_t *body,
	size_t size)
{
	struct sslc_certificate_msg certificates;
	const struct sslc_hs_config *config;
	unsigned alert;
	size_t index;
	int go_on;
	int error;

	/* The message's syntax. */
	config = hs->config;
	error = sslc_msg_parse_certificate(body, size, tls13, &certificates, &alert);
	if (error != 0) {
		hs->alert = alert;
		return error;
	}

	/* The server answers no CertificateRequest: its context is empty (4.4.2). */
	if (tls13 && certificates.context_size != 0U) {
		hs->alert = SSLC_ALERT_ILLEGAL_PARAMETER;
		return EPROTO;
	}

	/* A server sends one Certificate per handshake. */
	if (hs->peer_count != 0U) {
		hs->alert = SSLC_ALERT_UNEXPECTED_MESSAGE;
		return EPROTO;
	}

	/* Each certificate, copied so that its parsed views outlive the message, then parsed. */
	for (index = 0; index < certificates.count; index++) {
		hs->peer_der[index] = malloc(certificates.der_size[index]);
		if (hs->peer_der[index] == NULL) {
			hs->alert = 0;
			return ENOMEM;
		}

		/* peer_count counts the copies made, which sslc_hs_free releases. */
		hs->peer_count = index + 1U;
		memcpy(hs->peer_der[index], certificates.der[index], certificates.der_size[index]);
		hs->peer_der_size[index] = certificates.der_size[index];

		/* A certificate that does not parse is bad_certificate. */
		error = cryc_x509_parse(&hs->peer[index], hs->peer_der[index], hs->peer_der_size[index]);
		if (error != 0) {
			hs->alert = SSLC_ALERT_BAD_CERTIFICATE;
			return EINVAL;
		}
	}

	/* The chain against the trust anchors, with the configured time and name. */
	error = cryc_chain_verify(config->store, hs->peer, hs->peer_count, &config->params, &hs->verify);
	if (error != 0) {
		hs->alert = 0;
		return error;
	}
	hs->verify_code = hs->verify.code;

	/*
	 * A chain that does not validate ends the handshake when the peer
	 * must be verified, unless the callback lets it go on (the verdict
	 * stays recorded either way).  Without verification it is only
	 * recorded.
	 */
	if (hs->verify_code != CRYC_VERIFY_OK && config->verify_peer) {
		go_on = 0;
		if (config->callback != NULL)
			go_on = config->callback(config->callback_context, 0, hs->verify_code);
		if (go_on == 0) {
			hs->alert = sslc_hs_verdict_alert(hs->verify_code);
			return EPROTO;
		}
	}

	/* A chain that validates is still offered to the callback, which may refuse it. */
	if (hs->verify_code == CRYC_VERIFY_OK &&
	    config->verify_peer &&
	    config->callback != NULL) {
		go_on = config->callback(config->callback_context, 1, CRYC_VERIFY_OK);
		if (go_on == 0) {
			hs->alert = SSLC_ALERT_CERTIFICATE_UNKNOWN;
			return EPROTO;
		}
	}

	/* The leaf's key must be one the handshake can verify a signature with. */
	if (hs->peer[0].key.type != CRYC_KEY_RSA && hs->peer[0].key.type != CRYC_KEY_EC) {
		hs->alert = SSLC_ALERT_UNSUPPORTED_CERTIFICATE;
		return ENOTSUP;
	}

	/* Succeeded: the certificates are kept and the verdict recorded. */
	return 0;
}

/*
 * Computes the (EC)DHE shared secret with the server's share of group
 * (x25519 or secp256r1) into shared (32 bytes).
 *
 * An x25519 share must be 32 bytes and give a result that is not all
 * zeros (RFC 8446 7.4.2); a secp256r1 share must be an uncompressed point
 * on the curve (cryc_ecc_ecdh validates it).  shared is wiped on an error.
 */
int
sslc_hs_shared_secret(
	struct sslc_hs *hs,
	uint16_t group,
	const uint8_t *share,
	size_t share_size,
	uint8_t *shared)
{
	int error;

	/* x25519: a u-coordinate of 32 bytes, and a result that is not all zeros. */
	if (group == SSLC_GROUP_X25519) {
		if (share == NULL || share_size != CRYC_X25519_SIZE) {
			cryc_wipe(shared, SSLC_HS_SHARED_SIZE);
			hs->alert = SSLC_ALERT_ILLEGAL_PARAMETER;
			return EPROTO;
		}

		/* The product with our private key (EINVAL: the all-zero result of a small-order point). */
		error = cryc_x25519(shared, hs->x25519_private, share);
		if (error != 0) {
			cryc_wipe(shared, SSLC_HS_SHARED_SIZE);
			hs->alert = SSLC_ALERT_ILLEGAL_PARAMETER;
			return EPROTO;
		}

		/* Succeeded: shared holds the x25519 secret. */
		return 0;
	}

	/* secp256r1: only with a key pair of our own, and a share that validates. */
	if (group == SSLC_GROUP_SECP256R1) {
		if (!hs->has_p256 ||
		    hs->ec == NULL ||
		    share == NULL) {
			cryc_wipe(shared, SSLC_HS_SHARED_SIZE);
			hs->alert = SSLC_ALERT_ILLEGAL_PARAMETER;
			return EPROTO;
		}

		/* The x coordinate of the product (cryc_ecc_ecdh validates the point and wipes on an error). */
		error = cryc_ecc_ecdh(hs->ec, hs->p256_private, share, share_size, shared);
		if (error != 0) {
			cryc_wipe(shared, SSLC_HS_SHARED_SIZE);
			hs->alert = SSLC_ALERT_ILLEGAL_PARAMETER;
			return EPROTO;
		}

		/* Succeeded: shared holds the secp256r1 secret. */
		return 0;
	}

	/* Any other group was never offered. */
	cryc_wipe(shared, SSLC_HS_SHARED_SIZE);
	hs->alert = SSLC_ALERT_ILLEGAL_PARAMETER;
	return EPROTO;
}

/* Records why the handshake failed and wipes its secrets. */
static int
sslc_hs_fail(
	struct sslc_hs *hs,
	unsigned alert,
	int error)
{
	/* The alert the connection sends, and a state that refuses everything after. */
	hs->alert = alert;
	hs->state = SSLC_HS_FAILED;

	/* Nothing secret is needed any more. */
	sslc_hs_wipe_secrets(hs);

	/* Reports the failure. */
	return error;
}

/* Wipes the secrets a failed handshake no longer needs. */
static void
sslc_hs_wipe_secrets(
	struct sslc_hs *hs)
{
	/* The key pairs, the key schedule, the master secret. */
	cryc_wipe(hs->x25519_private, sizeof(hs->x25519_private));
	cryc_wipe(hs->p256_private, sizeof(hs->p256_private));
	sslc_tls13_clear(&hs->schedule);
	cryc_wipe(hs->master, sizeof(hs->master));
}

/* Checks a configuration's ranges. */
static int
sslc_hs_check_config(
	const struct sslc_hs_config *config)
{
	/* A configuration exists. */
	if (config == NULL)
		return EINVAL;

	/* Each version is one libssl-compat speaks, the lower not above the higher. */
	if (config->min_version != SSLC_TLS12 && config->min_version != SSLC_TLS13)
		return EINVAL;
	if (config->max_version != SSLC_TLS12 && config->max_version != SSLC_TLS13)
		return EINVAL;
	if (config->min_version > config->max_version)
		return EINVAL;

	/* A random source and trust anchors to verify against. */
	if (config->random == NULL)
		return EINVAL;
	if (config->store == NULL)
		return EINVAL;

	/* The configuration is usable. */
	return 0;
}

/* Draws count bytes from the configuration's random source. */
static int
sslc_hs_draw(
	const struct sslc_hs_config *config,
	uint8_t *out,
	size_t count)
{
	int error;

	/* A failing source is an error of the system, never replaced by anything else. */
	error = config->random(config->random_context, out, count);
	if (error != 0) {
		cryc_wipe(out, count);
		return EIO;
	}

	/* Succeeded: out holds fresh random bytes. */
	return 0;
}

/* Writes the ClientHello as hs->hello now stands and queues it. */
static int
sslc_hs_send_hello(
	struct sslc_hs *hs,
	struct sslc_hs_actions *actions)
{
	uint8_t message[SSLC_HS_OUT_MAX];
	size_t written;
	int error;

	/* The message, written from the fields (EINVAL for a server name over the limit). */
	error = sslc_msg_write_client_hello(&hs->hello, message, sizeof(message), &written);
	if (error != 0)
		return error;

	/* Queued for sending and absorbed into the transcript. */
	error = sslc_hs_queue_message(hs, actions, message, written);
	if (error != 0)
		return error;

	/* Succeeded: the ClientHello is queued. */
	return 0;
}

/*
 * Takes a ServerHello or a HelloRetryRequest: the checks the two versions
 * share, the HelloRetryRequest, then the version's own continuation.
 */
static int
sslc_hs_server_hello(
	struct sslc_hs *hs,
	const uint8_t *message,
	size_t size,
	struct sslc_hs_actions *actions)
{
	struct sslc_server_hello hello;
	const struct sslc_suite *suite;
	uint16_t version;
	unsigned alert;
	int error;

	/* The message's syntax and the extensions the client asked for. */
	error = sslc_msg_parse_server_hello(message + 4, size - 4U, &hello, &alert);
	if (error != 0) {
		hs->alert = alert;
		return error;
	}

	/* The legacy version is TLS 1.2's whatever is negotiated (4.1.3). */
	if (hello.legacy_version != SSLC_TLS12) {
		hs->alert = SSLC_ALERT_PROTOCOL_VERSION;
		return EPROTO;
	}

	/* The version selected. */
	error = sslc_hs_choose_version(hs, &hello, &version);
	if (error != 0)
		return error;

	/* The suite, offered and of the version. */
	error = sslc_hs_choose_suite(hs, &hello, version, &suite);
	if (error != 0)
		return error;

	/* No compression (4.1.3; RFC 5246 7.4.1.3 with the client's null method alone). */
	if (hello.compression != 0U) {
		hs->alert = SSLC_ALERT_ILLEGAL_PARAMETER;
		return EPROTO;
	}

	/* TLS 1.3 echoes the session id the client sent (4.1.3, also in a HelloRetryRequest, 4.1.4). */
	if (version == SSLC_TLS13) {
		error = sslc_hs_check_echo(hs, &hello);
		if (error != 0)
			return error;
	}

	/* A HelloRetryRequest asks for another ClientHello. */
	if (version == SSLC_TLS13 && hello.is_retry) {
		error = sslc_hs_retry(hs, &hello, suite, message, size, actions);
		if (error != 0)
			return error;
		return 0;
	}

	/* After a HelloRetryRequest the ServerHello keeps its suite (4.1.4). */
	if (hs->retried && suite != hs->suite) {
		hs->alert = SSLC_ALERT_ILLEGAL_PARAMETER;
		return EPROTO;
	}

	/* A TLS 1.3 ServerHello's extensions and key share. */
	if (version == SSLC_TLS13) {
		error = sslc_hs_check_hello13(hs, &hello);
		if (error != 0)
			return error;
	}

	/* The negotiated values. */
	hs->version = version;
	hs->suite = suite;
	memcpy(hs->server_random, hello.random, sizeof(hs->server_random));

	/* The transcript goes on with the suite's hash alone, and takes the ServerHello. */
	error = sslc_transcript_select(&hs->transcript, suite->hash);
	if (error != 0) {
		hs->alert = 0;
		return error;
	}
	sslc_transcript_update(&hs->transcript, message, size);

	/* The version's own continuation: TLS 1.3's keys, or TLS 1.2's checks and states. */
	if (version == SSLC_TLS13)
		error = sslc_hs13_server_hello(hs, &hello, actions);
	else
		error = sslc_hs12_server_hello(hs, &hello, actions);
	if (error != 0)
		return error;

	/* Succeeded: the ServerHello is taken. */
	return 0;
}

/*
 * Decides the version a ServerHello selects: supported_versions' when
 * present, else the legacy version's TLS 1.2; a TLS 1.2 answer to a client
 * that offered TLS 1.3 must not carry a downgrade mark (4.1.3).
 */
static int
sslc_hs_choose_version(
	struct sslc_hs *hs,
	const struct sslc_server_hello *hello,
	uint16_t *version)
{
	const uint8_t *tail;
	int differs12;
	int differs11;

	/* supported_versions names TLS 1.3, which must have been offered (4.2.1). */
	if ((hello->seen & SSLC_SEEN_SUPPORTED_VERSIONS) != 0U) {
		if (hello->selected_version != SSLC_TLS13 || hs->config->max_version != SSLC_TLS13) {
			hs->alert = SSLC_ALERT_ILLEGAL_PARAMETER;
			return EPROTO;
		}
		*version = SSLC_TLS13;
	} else {
		if (hs->config->min_version != SSLC_TLS12) {
			hs->alert = SSLC_ALERT_PROTOCOL_VERSION;
			return EPROTO;
		}
		*version = SSLC_TLS12;
	}

	/* After a HelloRetryRequest the version stays TLS 1.3 (4.1.4). */
	if (hs->retried && *version != SSLC_TLS13) {
		hs->alert = SSLC_ALERT_ILLEGAL_PARAMETER;
		return EPROTO;
	}

	/* A TLS 1.2 server that saw TLS 1.3 offered and marks its random as a downgrade is refused. */
	if (*version == SSLC_TLS12 && hs->config->max_version == SSLC_TLS13) {
		tail = hello->random + sizeof(hello->random) - SSLC_HS_DOWNGRADE_SIZE;
		differs12 = memcmp(tail, sslc_tls_downgrade12, SSLC_HS_DOWNGRADE_SIZE);
		differs11 = memcmp(tail, sslc_tls_downgrade11, SSLC_HS_DOWNGRADE_SIZE);
		if (differs12 == 0 || differs11 == 0) {
			hs->alert = SSLC_ALERT_ILLEGAL_PARAMETER;
			return EPROTO;
		}
	}

	/* Succeeded: the version is known. */
	return 0;
}

/* Checks the suite a ServerHello names: offered, known, and of the version. */
static int
sslc_hs_choose_suite(
	struct sslc_hs *hs,
	const struct sslc_server_hello *hello,
	uint16_t version,
	const struct sslc_suite **suite)
{
	int offered;

	/* The suite must be one the ClientHello offered (the signalling value is no suite). */
	offered = sslc_hs_offered_suite(hs, hello->suite);
	if (!offered) {
		hs->alert = SSLC_ALERT_ILLEGAL_PARAMETER;
		return EPROTO;
	}

	/* And it must belong to the version selected. */
	*suite = sslc_tls_suite(hello->suite);
	if (*suite == NULL || (*suite)->version != version) {
		hs->alert = SSLC_ALERT_ILLEGAL_PARAMETER;
		return EPROTO;
	}

	/* Succeeded: the suite fits. */
	return 0;
}

/* Checks that a TLS 1.3 ServerHello echoes the session id sent. */
static int
sslc_hs_check_echo(
	struct sslc_hs *hs,
	const struct sslc_server_hello *hello)
{
	int differs;

	/* The same length. */
	if (hello->session_id_size != hs->hello.session_id_size) {
		hs->alert = SSLC_ALERT_ILLEGAL_PARAMETER;
		return EPROTO;
	}

	/* The same bytes (public: the client sent them in the clear). */
	differs = memcmp(hello->session_id, hs->hello.session_id, hello->session_id_size);
	if (differs != 0) {
		hs->alert = SSLC_ALERT_ILLEGAL_PARAMETER;
		return EPROTO;
	}

	/* Succeeded: the session id is echoed. */
	return 0;
}

/* Checks a TLS 1.3 ServerHello's key share and extensions. */
static int
sslc_hs_check_hello13(
	struct sslc_hs *hs,
	const struct sslc_server_hello *hello)
{
	int shared;

	/* The key share is required (4.2.8). */
	if ((hello->seen & SSLC_SEEN_KEY_SHARE) == 0U) {
		hs->alert = SSLC_ALERT_MISSING_EXTENSION;
		return EPROTO;
	}

	/* Its group is one a share was sent for (after a HelloRetryRequest, the group it named). */
	shared = sslc_hs_has_share(hs, hello->share_group);
	if (!shared) {
		hs->alert = SSLC_ALERT_ILLEGAL_PARAMETER;
		return EPROTO;
	}

	/* Only supported_versions and key_share belong in a TLS 1.3 ServerHello (4.2's table). */
	if ((hello->seen & ~(uint32_t)SSLC_HS_SEEN_HELLO13) != 0U) {
		hs->alert = SSLC_ALERT_ILLEGAL_PARAMETER;
		return EPROTO;
	}

	/* Succeeded: the ServerHello's extensions fit. */
	return 0;
}

/*
 * Takes a HelloRetryRequest (4.1.4): checks what it asks for, restarts the
 * transcript with the hash of the first ClientHello (4.4.1), and sends the
 * compatibility change_cipher_spec (D.4) and the second ClientHello with
 * the share of the group asked for and the cookie (4.1.2).
 */
static int
sslc_hs_retry(
	struct sslc_hs *hs,
	const struct sslc_server_hello *hello,
	const struct sslc_suite *suite,
	const uint8_t *message,
	size_t size,
	struct sslc_hs_actions *actions)
{
	int offered;
	int shared;
	int error;

	/* One HelloRetryRequest per handshake (4.1.4). */
	if (hs->retried) {
		hs->alert = SSLC_ALERT_UNEXPECTED_MESSAGE;
		return EPROTO;
	}

	/* Only supported_versions, key_share and cookie belong in it (4.2's table). */
	if ((hello->seen & ~(uint32_t)SSLC_HS_SEEN_RETRY) != 0U) {
		hs->alert = SSLC_ALERT_ILLEGAL_PARAMETER;
		return EPROTO;
	}

	/* A group asked for must be offered and have no share yet (4.2.8). */
	if ((hello->seen & SSLC_SEEN_KEY_SHARE) != 0U) {
		offered = sslc_hs_offered_group(hello->share_group);
		shared = sslc_hs_has_share(hs, hello->share_group);
		if (!offered || shared) {
			hs->alert = SSLC_ALERT_ILLEGAL_PARAMETER;
			return EPROTO;
		}
	}

	/* A request that would not change the ClientHello is refused (4.1.4). */
	if ((hello->seen & (SSLC_SEEN_KEY_SHARE | SSLC_SEEN_COOKIE)) == 0U) {
		hs->alert = SSLC_ALERT_ILLEGAL_PARAMETER;
		return EPROTO;
	}

	/* The suite and the version are fixed from here on (4.1.4). */
	hs->suite = suite;
	hs->version = SSLC_TLS13;

	/* The transcript: the suite's hash alone, message_hash of the first ClientHello, then this message. */
	error = sslc_transcript_select(&hs->transcript, suite->hash);
	if (error != 0) {
		hs->alert = 0;
		return error;
	}
	error = sslc_transcript_restart_hrr(&hs->transcript);
	if (error != 0) {
		hs->alert = 0;
		return error;
	}
	sslc_transcript_update(&hs->transcript, message, size);

	/* The cookie, echoed in the second ClientHello (4.2.2; its size the parser bounded). */
	if ((hello->seen & SSLC_SEEN_COOKIE) != 0U) {
		memcpy(hs->cookie, hello->cookie, hello->cookie_size);
		hs->hello.cookie = hs->cookie;
		hs->hello.cookie_size = hello->cookie_size;
	}

	/*
	 * The share of the group asked for replaces the shares sent (4.1.2).
	 * The only offered group without a share is secp256r1, whose key pair
	 * is made now; the x25519 key will not be used again.
	 */
	if ((hello->seen & SSLC_SEEN_KEY_SHARE) != 0U && hello->share_group == SSLC_GROUP_SECP256R1) {
		error = sslc_hs_make_p256(hs);
		if (error != 0) {
			hs->alert = 0;
			return error;
		}
		cryc_wipe(hs->x25519_private, sizeof(hs->x25519_private));
		hs->hello.shares[0].group = SSLC_GROUP_SECP256R1;
		hs->hello.shares[0].key = hs->p256_public;
		hs->hello.shares[0].key_size = SSLC_HS_P256_POINT;
		hs->hello.share_count = 1;
	}

	/*
	 * retried marks that the HelloRetryRequest was taken: a second one is
	 * refused, the ServerHello must keep its suite and version, and the
	 * compatibility change_cipher_spec, sent now before the second
	 * ClientHello, is not sent again before the Finished.
	 */
	hs->retried = 1;
	error = sslc_hs_queue_action(actions, SSLC_HS_SEND_CCS, 0, NULL, 0);
	if (error != 0) {
		hs->alert = 0;
		return error;
	}

	/* The second ClientHello. */
	error = sslc_hs_send_hello(hs, actions);
	if (error != 0) {
		hs->alert = 0;
		return error;
	}

	/* Succeeded: the ServerHello is awaited again. */
	return 0;
}

/* Makes the secp256r1 key pair (once). */
static int
sslc_hs_make_p256(
	struct sslc_hs *hs)
{
	int error;

	/* Made once per handshake. */
	if (hs->has_p256)
		return 0;

	/* The curve, on the heap (it is about 4 KiB). */
	if (hs->ec == NULL) {
		hs->ec = malloc(sizeof(*hs->ec));
		if (hs->ec == NULL)
			return ENOMEM;

		/* Prepared for arithmetic. */
		error = cryc_ecp_init(hs->ec, CRYC_CURVE_P256);
		if (error != 0)
			return error;
	}

	/* The key pair from the configuration's random source. */
	error = cryc_ecc_keygen(hs->ec, hs->p256_private, hs->p256_public, hs->config->random, hs->config->random_context);
	if (error != 0)
		return EIO;

	/* Succeeded: has_p256 tells the shared secret and the second ClientHello that the pair exists. */
	hs->has_p256 = 1;
	return 0;
}

/* Returns 1 when the ClientHello offered suite id. */
static int
sslc_hs_offered_suite(
	const struct sslc_hs *hs,
	uint16_t id)
{
	size_t index;

	/* A search of the offered list. */
	for (index = 0; index < hs->hello.suite_count; index++) {
		if (hs->suites[index] == id)
			return 1;
	}

	/* Not offered. */
	return 0;
}

/* Returns 1 when supported_groups names group. */
static int
sslc_hs_offered_group(
	uint16_t group)
{
	size_t index;

	/* A search of the offered list. */
	for (index = 0; index < sizeof(sslc_hs_offered_groups) / sizeof(sslc_hs_offered_groups[0]); index++) {
		if (sslc_hs_offered_groups[index] == group)
			return 1;
	}

	/* Not offered. */
	return 0;
}

/* Returns 1 when the ClientHello now carries a key share of group. */
static int
sslc_hs_has_share(
	const struct sslc_hs *hs,
	uint16_t group)
{
	size_t index;

	/* A search of the shares sent. */
	for (index = 0; index < hs->hello.share_count; index++) {
		if (hs->hello.shares[index].group == group)
			return 1;
	}

	/* No share of that group. */
	return 0;
}

/* Returns the bytes of hs->out the actions of this call already use. */
static size_t
sslc_hs_out_used(
	const struct sslc_hs_actions *actions)
{
	size_t used;
	size_t end;
	size_t index;

	/* The end of the furthest SEND. */
	used = 0;
	for (index = 0; index < actions->count && index < SSLC_HS_MAX_ACTIONS; index++) {
		if (actions->list[index].kind != SSLC_HS_SEND)
			continue;
		end = actions->list[index].offset + actions->list[index].size;
		if (end > used)
			used = end;
	}

	/* The bytes in use. */
	return used;
}

/* Returns the alert a certificate verdict is sent with. */
static unsigned
sslc_hs_verdict_alert(
	enum cryc_verify_code code)
{
	/* By the verdict, as the header assigns them. */
	switch (code) {
	case CRYC_VERIFY_EXPIRED:
	case CRYC_VERIFY_NOT_YET_VALID:
		return SSLC_ALERT_CERTIFICATE_EXPIRED;
	case CRYC_VERIFY_UNABLE_TO_GET_ISSUER:
	case CRYC_VERIFY_DEPTH_ZERO_SELF_SIGNED:
	case CRYC_VERIFY_SELF_SIGNED_IN_CHAIN:
		return SSLC_ALERT_UNKNOWN_CA;
	case CRYC_VERIFY_UNSUPPORTED_ALGORITHM:
	case CRYC_VERIFY_KEY_TOO_SMALL:
		return SSLC_ALERT_UNSUPPORTED_CERTIFICATE;
	default:
		break;
	}

	/* Every other verdict. */
	return SSLC_ALERT_BAD_CERTIFICATE;
}
