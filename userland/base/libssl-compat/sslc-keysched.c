/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The transcript hash and the TLS 1.3 and TLS 1.2 key schedules.
 */

#include "sslc-keysched.h"

/*
 * Starts a transcript hashing with both candidate hashes.
 */
void
sslc_transcript_init(
	struct sslc_transcript *transcript)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(transcript);
}

/*
 * Absorbs one whole handshake message (its 4-byte header and body, 4.4.1).
 */
void
sslc_transcript_update(
	struct sslc_transcript *transcript,
	const uint8_t *message,
	size_t size)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(transcript);
	UNUSED_PARAMETER(message);
	UNUSED_PARAMETER(size);
}

/*
 * Keeps only hash (SHA-256 or SHA-384, the suite's), dropping the other.
 */
int
sslc_transcript_select(
	struct sslc_transcript *transcript,
	enum cryc_hash_id hash)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(transcript);
	UNUSED_PARAMETER(hash);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}

/*
 * Writes the hash of the messages so far (Transcript-Hash, 4.4.1) with the
 * selected hash, without ending the transcript.
 */
size_t
sslc_transcript_hash(
	const struct sslc_transcript *transcript,
	uint8_t *out)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(transcript);
	UNUSED_PARAMETER(out);

	/* Not implemented: the failure value. */
	return 0;
}

/*
 * Replaces the transcript after a HelloRetryRequest (4.4.1): with the
 * selected hash H, the transcript becomes message_hash (254) || 00 00 ||
 * Hash.length || H(ClientHello1), where the current state is the hash of
 * exactly ClientHello1.
 */
int
sslc_transcript_restart_hrr(
	struct sslc_transcript *transcript)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(transcript);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}

/*
 * Wipes the transcript.
 */
void
sslc_transcript_clear(
	struct sslc_transcript *transcript)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(transcript);
}

/*
 * HKDF-Expand-Label (7.1): HKDF-Expand(secret, HkdfLabel, length) with
 * HkdfLabel = uint16 length || opaque label<7..255> = "tls13 " + label ||
 * opaque context<0..255>.
 */
int
sslc_tls13_expand_label(
	enum cryc_hash_id hash,
	const uint8_t *secret,
	const char *label,
	const uint8_t *context,
	size_t context_size,
	uint8_t *out,
	size_t length)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(hash);
	UNUSED_PARAMETER(secret);
	UNUSED_PARAMETER(label);
	UNUSED_PARAMETER(context);
	UNUSED_PARAMETER(context_size);
	UNUSED_PARAMETER(out);
	UNUSED_PARAMETER(length);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}

/*
 * Starts the schedule (7.1): Early Secret = HKDF-Extract(0, 0) (no PSK:
 * both are hash-size strings of zeros).
 */
int
sslc_tls13_start(
	struct sslc_tls13_schedule *schedule,
	enum cryc_hash_id hash)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(schedule);
	UNUSED_PARAMETER(hash);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}

/*
 * Moves to the Handshake Secret (7.1): Derive-Secret(Early, "derived", "")
 * as the salt, the (EC)DHE shared secret as the IKM; then derives
 * client_handshake and server_handshake ("c hs traffic", "s hs traffic")
 * over transcript_hash (ClientHello...ServerHello).
 */
int
sslc_tls13_handshake(
	struct sslc_tls13_schedule *schedule,
	const uint8_t *shared,
	size_t shared_size,
	const uint8_t *transcript_hash)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(schedule);
	UNUSED_PARAMETER(shared);
	UNUSED_PARAMETER(shared_size);
	UNUSED_PARAMETER(transcript_hash);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}

/*
 * Moves to the Master Secret (7.1): Derive-Secret(Handshake, "derived",
 * "") as the salt, zeros as the IKM; then derives client_application and
 * server_application ("c ap traffic", "s ap traffic") over transcript_hash
 * (ClientHello...server Finished).
 */
int
sslc_tls13_master(
	struct sslc_tls13_schedule *schedule,
	const uint8_t *transcript_hash)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(schedule);
	UNUSED_PARAMETER(transcript_hash);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}

/*
 * Derives the record keys of a traffic secret (7.3): key = HKDF-Expand-
 * Label(secret, "key", "", key_length), iv = HKDF-Expand-Label(secret,
 * "iv", "", 12), for suite.
 */
int
sslc_tls13_traffic_keys(
	enum cryc_hash_id hash,
	const struct sslc_suite *suite,
	const uint8_t *secret,
	struct sslc_traffic_keys *keys)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(hash);
	UNUSED_PARAMETER(suite);
	UNUSED_PARAMETER(secret);
	UNUSED_PARAMETER(keys);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}

/*
 * Computes a Finished's verify_data (4.4.4): finished_key = HKDF-Expand-
 * Label(base_key, "finished", "", Hash.length); verify_data =
 * HMAC(finished_key, transcript_hash).
 */
int
sslc_tls13_finished(
	enum cryc_hash_id hash,
	const uint8_t *base_key,
	const uint8_t *transcript_hash,
	uint8_t *out)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(hash);
	UNUSED_PARAMETER(base_key);
	UNUSED_PARAMETER(transcript_hash);
	UNUSED_PARAMETER(out);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}

/*
 * Replaces a traffic secret with the next generation (7.2):
 * application_traffic_secret_N+1 = HKDF-Expand-Label(secret_N, "traffic
 * upd", "", Hash.length), in place.
 */
int
sslc_tls13_update_secret(
	enum cryc_hash_id hash,
	uint8_t *secret)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(hash);
	UNUSED_PARAMETER(secret);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}

/*
 * Wipes the schedule.
 */
void
sslc_tls13_clear(
	struct sslc_tls13_schedule *schedule)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(schedule);
}

/*
 * The TLS 1.2 PRF (RFC 5246 5): PRF(secret, label, seed) = P_hash(secret,
 * label + seed) with the suite's hash (SHA-256 or SHA-384; RFC 5246 5 and
 * RFC 5289 3).
 */
int
sslc_tls12_prf(
	enum cryc_hash_id hash,
	const uint8_t *secret,
	size_t secret_size,
	const char *label,
	const uint8_t *seed,
	size_t seed_size,
	uint8_t *out,
	size_t out_size)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(hash);
	UNUSED_PARAMETER(secret);
	UNUSED_PARAMETER(secret_size);
	UNUSED_PARAMETER(label);
	UNUSED_PARAMETER(seed);
	UNUSED_PARAMETER(seed_size);
	UNUSED_PARAMETER(out);
	UNUSED_PARAMETER(out_size);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}

/*
 * Computes the master secret.
 */
int
sslc_tls12_master(
	enum cryc_hash_id hash,
	int extended_master_secret,
	const uint8_t *pre_master,
	size_t pre_master_size,
	const uint8_t *context,
	size_t context_size,
	uint8_t *master)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(hash);
	UNUSED_PARAMETER(extended_master_secret);
	UNUSED_PARAMETER(pre_master);
	UNUSED_PARAMETER(pre_master_size);
	UNUSED_PARAMETER(context);
	UNUSED_PARAMETER(context_size);
	UNUSED_PARAMETER(master);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}

/*
 * Derives the record keys of both directions from the master secret (RFC
 * 5246 6.3): key_block = PRF(master, "key expansion", server_random ||
 * client_random), split as client_write_key, server_write_key,
 * client_write_IV, server_write_IV (no MAC keys for AEAD suites), with the
 * suite's key size and fixed IV size (4 for AES-GCM, RFC 5288 3; 12 for
 * ChaCha20-Poly1305, RFC 7905 2).
 */
int
sslc_tls12_key_block(
	const struct sslc_suite *suite,
	const uint8_t *master,
	const uint8_t *client_random,
	const uint8_t *server_random,
	struct sslc_traffic_keys *client_keys,
	struct sslc_traffic_keys *server_keys)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(suite);
	UNUSED_PARAMETER(master);
	UNUSED_PARAMETER(client_random);
	UNUSED_PARAMETER(server_random);
	UNUSED_PARAMETER(client_keys);
	UNUSED_PARAMETER(server_keys);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}

/*
 * Computes a TLS 1.2 Finished's verify_data (RFC 5246 7.4.9): PRF(master,
 * "client finished" or "server finished", transcript_hash)[0..11].
 */
int
sslc_tls12_finished(
	enum cryc_hash_id hash,
	const uint8_t *master,
	int from_client,
	const uint8_t *transcript_hash,
	uint8_t *out)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(hash);
	UNUSED_PARAMETER(master);
	UNUSED_PARAMETER(from_client);
	UNUSED_PARAMETER(transcript_hash);
	UNUSED_PARAMETER(out);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}
