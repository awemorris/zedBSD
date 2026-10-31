/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The client handshake: what TLS 1.3 and TLS 1.2 share.
 */

#include "sslc-hs.h"

/*
 * Starts a handshake: makes the x25519 key pair and the random values with
 * config->random and builds the ClientHello (the offered suites of the
 * versions; groups x25519 then secp256r1; schemes ecdsa_secp256r1_sha256,
 * ecdsa_secp384r1_sha384, rsa_pss_rsae_sha256, rsa_pss_rsae_sha384,
 * rsa_pss_rsae_sha512, rsa_pkcs1_sha256, rsa_pkcs1_sha384,
 * rsa_pkcs1_sha512; one x25519 share).
 */
int
sslc_hs_start(
	struct sslc_hs *hs,
	const struct sslc_hs_config *config,
	struct sslc_hs_actions *actions)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(hs);
	UNUSED_PARAMETER(config);
	UNUSED_PARAMETER(actions);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
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
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(hs);
	UNUSED_PARAMETER(message);
	UNUSED_PARAMETER(size);
	UNUSED_PARAMETER(actions);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
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
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(hs);
	UNUSED_PARAMETER(actions);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}

/*
 * Returns 1 when the handshake is complete (a CONNECTED state), else 0.
 */
int
sslc_hs_connected(
	const struct sslc_hs *hs)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(hs);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}

/*
 * Wipes every secret, frees the peer's certificate copies and ec; the
 * structure may then be discarded.
 */
void
sslc_hs_free(
	struct sslc_hs *hs)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(hs);
}

/*
 * Appends one whole handshake message (header and body) to hs->out and to
 * the transcript, and an SSLC_HS_SEND action for it (merged with the
 * previous action when that is a SEND of the bytes just before).
 */
int
sslc_hs_queue_message(
	struct sslc_hs *hs,
	struct sslc_hs_actions *actions,
	const uint8_t *message,
	size_t size)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(hs);
	UNUSED_PARAMETER(actions);
	UNUSED_PARAMETER(message);
	UNUSED_PARAMETER(size);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
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
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(actions);
	UNUSED_PARAMETER(kind);
	UNUSED_PARAMETER(version13);
	UNUSED_PARAMETER(keys);
	UNUSED_PARAMETER(alert);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}

/*
 * Takes a Certificate body (TLS 1.3 when tls13 is 1): copies and parses
 * every certificate, validates the chain and applies the verdict rules of
 * the file comment (the callback, the alert).
 */
int
sslc_hs_certificate(
	struct sslc_hs *hs,
	int tls13,
	const uint8_t *body,
	size_t size)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(hs);
	UNUSED_PARAMETER(tls13);
	UNUSED_PARAMETER(body);
	UNUSED_PARAMETER(size);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}

/*
 * Computes the (EC)DHE shared secret with the server's share of group
 * (x25519 or secp256r1) into shared (32 bytes).
 */
int
sslc_hs_shared_secret(
	struct sslc_hs *hs,
	uint16_t group,
	const uint8_t *share,
	size_t share_size,
	uint8_t *shared)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(hs);
	UNUSED_PARAMETER(group);
	UNUSED_PARAMETER(share);
	UNUSED_PARAMETER(share_size);
	UNUSED_PARAMETER(shared);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}
