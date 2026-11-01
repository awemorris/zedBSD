/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The transcript hash and the TLS 1.3 and TLS 1.2 key schedules.
 */

#include <string.h>

#include "sslc-keysched.h"

/* The longest label sslc_tls13_expand_label takes: the HkdfLabel's label field holds 255 bytes, 6 of them "tls13 ". */
#define SSLC_KEYSCHED_LABEL_MAX		249U

/* The longest context of an HkdfLabel (an opaque<0..255>). */
#define SSLC_KEYSCHED_CONTEXT_MAX	255U

/* The size of an HkdfLabel at its largest: length (2), label length (1) and label (255), context length (1) and context (255). */
#define SSLC_KEYSCHED_INFO_MAX		514U

/* The longest HKDF-Expand-Label output the 16-bit length field of an HkdfLabel can ask for. */
#define SSLC_KEYSCHED_LENGTH_MAX	65535U

/*
 * The longest (EC)DHE shared secret sslc_tls13_handshake accepts.  The
 * groups in use give 32 bytes (x25519, secp256r1); 128 leaves room for
 * every larger group and hybrid without letting a caller hash an
 * unbounded buffer.
 */
#define SSLC_KEYSCHED_SHARED_MAX	128U

/* The longest output of sslc_tls12_prf (the key block of the largest suite is 88 bytes). */
#define SSLC_TLS12_PRF_MAX		1024U

/* The key block's largest size: two 32-byte keys and two 12-byte IVs. */
#define SSLC_TLS12_KEY_BLOCK_MAX	88U

/* The size of the client and server randoms together (RFC 5246 6.3). */
#define SSLC_TLS12_RANDOMS		64U

/* The size of one hello random (RFC 5246 7.4.1.2). */
#define SSLC_TLS12_RANDOM		32U

/* The size of the TLS 1.3 traffic keys' IV (RFC 8446 7.3). */
#define SSLC_TLS13_IV_SIZE		12U

/* Returns the digest size of hash when it is SHA-256 or SHA-384 (the suites' hashes), else 0. */
static size_t sslc_keysched_hash_size(enum cryc_hash_id hash);

/* Computes Derive-Secret(secret, label, transcript_hash) (RFC 8446 7.1) into out. */
static int sslc_tls13_derive_secret(enum cryc_hash_id hash, const uint8_t *secret, const char *label, const uint8_t *transcript_hash, uint8_t *out);

/* Uses Derive-Secret(secret, "derived", "") as the salt to extract ikm into out (RFC 8446 7.1). */
static int sslc_tls13_next_secret(enum cryc_hash_id hash, const uint8_t *secret, const uint8_t *ikm, size_t ikm_size, uint8_t *out);

/* Writes out_size bytes of P_hash (RFC 5246 5) over label || seed into out. */
static int sslc_tls12_p_hash(enum cryc_hash_id hash, const uint8_t *secret, size_t secret_size, const char *label, const uint8_t *seed, size_t seed_size, uint8_t *out, size_t out_size);

/*
 * Starts a transcript hashing with both candidate hashes.
 */
void
sslc_transcript_init(
	struct sslc_transcript *transcript)
{
	/* Starts from nothing, with no suite chosen (zero is no hash). */
	memset(transcript, 0, sizeof(*transcript));

	/* Starts the two candidate hashes; both ids are known, so neither start fails. */
	(void)cryc_hash_init(&transcript->sha256, CRYC_HASH_SHA256);
	(void)cryc_hash_init(&transcript->sha384, CRYC_HASH_SHA384);
	transcript->selected = CRYC_HASH_NONE;
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
	/* A message of no bytes changes nothing (and may come without a pointer). */
	if (size == 0)
		return;

	/* Before a suite is chosen both hashes absorb the message. */
	if (transcript->selected == CRYC_HASH_NONE) {
		cryc_hash_update(&transcript->sha256, message, size);
		cryc_hash_update(&transcript->sha384, message, size);
		return;
	}

	/* After the choice only the suite's hash goes on. */
	if (transcript->selected == CRYC_HASH_SHA256) {
		cryc_hash_update(&transcript->sha256, message, size);
		return;
	}
	if (transcript->selected == CRYC_HASH_SHA384)
		cryc_hash_update(&transcript->sha384, message, size);
}

/*
 * Keeps only hash (SHA-256 or SHA-384, the suite's), dropping the other.
 */
int
sslc_transcript_select(
	struct sslc_transcript *transcript,
	enum cryc_hash_id hash)
{
	/* Only the two hashes a suite can name are candidates. */
	if (hash != CRYC_HASH_SHA256 && hash != CRYC_HASH_SHA384)
		return EINVAL;

	/* Choosing the same hash again changes nothing. */
	if (transcript->selected == hash)
		return 0;

	/* The other hash was already dropped and cannot be brought back. */
	if (transcript->selected != CRYC_HASH_NONE)
		return EINVAL;

	/* Wipes the hash that is not the suite's. */
	if (hash == CRYC_HASH_SHA256)
		cryc_wipe(&transcript->sha384, sizeof(transcript->sha384));
	else
		cryc_wipe(&transcript->sha256, sizeof(transcript->sha256));

	/* Succeeded: from now on one hash runs. */
	transcript->selected = hash;
	return 0;
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
	struct cryc_hash copy;
	size_t size;

	/* Before a hash is selected there is no one hash to report. */
	if (transcript->selected == CRYC_HASH_NONE)
		return 0;

	/* Finishes a copy, so the transcript goes on. */
	if (transcript->selected == CRYC_HASH_SHA256)
		copy = transcript->sha256;
	else
		copy = transcript->sha384;
	size = cryc_hash_size(copy.id);
	cryc_hash_final(&copy, out);

	/* The size of the digest written. */
	return size;
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
	struct cryc_hash *running;
	uint8_t digest[CRYC_HASH_MAX_SIZE];
	uint8_t header[4];
	struct cryc_hash copy;
	enum cryc_hash_id hash;
	size_t size;
	int error;

	/* The synthetic message needs the suite's hash. */
	hash = transcript->selected;
	if (hash == CRYC_HASH_NONE)
		return EINVAL;

	/* Takes the hash of ClientHello1 from a copy of the running hash. */
	if (hash == CRYC_HASH_SHA256)
		running = &transcript->sha256;
	else
		running = &transcript->sha384;
	copy = *running;
	size = cryc_hash_size(hash);
	cryc_hash_final(&copy, digest);

	/*
	 * Builds the message_hash handshake message's header: the type
	 * message_hash (254) and a 24-bit length of the hash's size.
	 */
	header[0] = 254;
	header[1] = 0;
	header[2] = 0;
	header[3] = (uint8_t)size;

	/* Starts the hash again, over the synthetic message. */
	error = cryc_hash_init(running, hash);
	if (error != 0) {
		cryc_wipe(digest, sizeof(digest));
		return EINVAL;
	}
	cryc_hash_update(running, header, sizeof(header));
	cryc_hash_update(running, digest, size);

	/* Wipes the digest of ClientHello1; the new state holds it. */
	cryc_wipe(digest, sizeof(digest));

	/* Succeeded: the caller now absorbs the HelloRetryRequest. */
	return 0;
}

/*
 * Wipes the transcript.
 */
void
sslc_transcript_clear(
	struct sslc_transcript *transcript)
{
	/* Wipes both hashes and forgets the choice (zero is no hash). */
	cryc_wipe(transcript, sizeof(*transcript));
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
	uint8_t info[SSLC_KEYSCHED_INFO_MAX];
	size_t hash_size;
	size_t label_size;
	size_t used;
	int error;

	/* The hash must be one a suite names. */
	hash_size = sslc_keysched_hash_size(hash);
	if (hash_size == 0)
		return EINVAL;

	/* The secret, the label and the output must exist. */
	if (secret == NULL || label == NULL)
		return EINVAL;
	if (out == NULL && length != 0)
		return EINVAL;

	/* The label is 1 to 249 characters, so "tls13 " plus it fits 255. */
	label_size = strlen(label);
	if (label_size == 0 || label_size > SSLC_KEYSCHED_LABEL_MAX)
		return EINVAL;

	/* The context is at most 255 bytes and exists when it has a size. */
	if (context_size > SSLC_KEYSCHED_CONTEXT_MAX)
		return EINVAL;
	if (context == NULL && context_size != 0)
		return EINVAL;

	/* HKDF-Expand makes at most 255 blocks, and the length field is 16 bits. */
	if (length > SSLC_KEYSCHED_LENGTH_MAX)
		return EINVAL;
	if (length > 255U * hash_size)
		return EINVAL;

	/*
	 * Writes the HkdfLabel: the length, the prefixed label, the context.
	 *
	 * Values referenced: RFC 8446 section 7.1 (the label prefix is the
	 * six characters "tls13 ").
	 */
	used = 0;
	info[used++] = (uint8_t)(length >> 8);
	info[used++] = (uint8_t)(length & 0xffU);
	info[used++] = (uint8_t)(6U + label_size);
	memcpy(info + used, "tls13 ", 6U);
	used += 6U;
	memcpy(info + used, label, label_size);
	used += label_size;
	info[used++] = (uint8_t)context_size;
	if (context_size != 0) {
		memcpy(info + used, context, context_size);
		used += context_size;
	}

	/* Expands the secret over the HkdfLabel. */
	error = cryc_hkdf_expand(hash, secret, hash_size, info, used, out, length);
	if (error != 0)
		return EINVAL;

	/* Succeeded: out holds length bytes. */
	return 0;
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
	uint8_t zeros[CRYC_HASH_MAX_SIZE];
	size_t size;
	int error;

	/* Only the suites' two hashes start a schedule. */
	size = sslc_keysched_hash_size(hash);
	if (size == 0)
		return EINVAL;

	/* Begins from an empty schedule of this hash. */
	cryc_wipe(schedule, sizeof(*schedule));
	schedule->hash = hash;
	schedule->size = size;

	/* Extracts the Early Secret from a zero salt and a zero input (no PSK). */
	memset(zeros, 0, sizeof(zeros));
	error = cryc_hkdf_extract(hash, zeros, size, zeros, size, schedule->secret);
	if (error != 0) {
		cryc_wipe(schedule, sizeof(*schedule));
		return EINVAL;
	}

	/* Succeeded: schedule->secret is the Early Secret. */
	return 0;
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
	uint8_t secret[CRYC_HASH_MAX_SIZE];
	uint8_t client[CRYC_HASH_MAX_SIZE];
	uint8_t server[CRYC_HASH_MAX_SIZE];
	size_t size;
	int error;

	/* The schedule must have been started. */
	size = sslc_keysched_hash_size(schedule->hash);
	if (size == 0 || size != schedule->size)
		return EINVAL;

	/* The shared secret and the transcript hash must exist, and the secret has a sensible size. */
	if (shared == NULL || transcript_hash == NULL)
		return EINVAL;
	if (shared_size == 0 || shared_size > SSLC_KEYSCHED_SHARED_MAX)
		return EINVAL;

	/* Extracts the Handshake Secret: the derived Early Secret as the salt. */
	error = sslc_tls13_next_secret(schedule->hash, schedule->secret, shared, shared_size, secret);
	if (error != 0)
		goto fail;

	/*
	 * Derives the client's handshake traffic secret.
	 *
	 * Values referenced: RFC 8446 section 7.1 (the labels "c hs traffic"
	 * and "s hs traffic").
	 */
	error = sslc_tls13_derive_secret(schedule->hash, secret, "c hs traffic", transcript_hash, client);
	if (error != 0)
		goto fail;

	/* Derives the server's handshake traffic secret. */
	error = sslc_tls13_derive_secret(schedule->hash, secret, "s hs traffic", transcript_hash, server);
	if (error != 0)
		goto fail;

	/* Publishes the three secrets only now that all are made. */
	memcpy(schedule->secret, secret, size);
	memcpy(schedule->client_handshake, client, size);
	memcpy(schedule->server_handshake, server, size);
	cryc_wipe(secret, sizeof(secret));
	cryc_wipe(client, sizeof(client));
	cryc_wipe(server, sizeof(server));

	/* Succeeded: the schedule is at the Handshake Secret. */
	return 0;

fail:
	/* Leaves the schedule as it was and wipes the temporaries. */
	cryc_wipe(secret, sizeof(secret));
	cryc_wipe(client, sizeof(client));
	cryc_wipe(server, sizeof(server));
	return EINVAL;
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
	uint8_t secret[CRYC_HASH_MAX_SIZE];
	uint8_t zeros[CRYC_HASH_MAX_SIZE];
	uint8_t client[CRYC_HASH_MAX_SIZE];
	uint8_t server[CRYC_HASH_MAX_SIZE];
	size_t size;
	int error;

	/* The schedule must have been started. */
	size = sslc_keysched_hash_size(schedule->hash);
	if (size == 0 || size != schedule->size)
		return EINVAL;

	/* The transcript hash must exist. */
	if (transcript_hash == NULL)
		return EINVAL;

	/* Extracts the Master Secret: the derived Handshake Secret as the salt, zeros as the input. */
	memset(zeros, 0, sizeof(zeros));
	error = sslc_tls13_next_secret(schedule->hash, schedule->secret, zeros, size, secret);
	if (error != 0)
		goto fail;

	/*
	 * Derives the client's application traffic secret.
	 *
	 * Values referenced: RFC 8446 section 7.1 (the labels "c ap traffic"
	 * and "s ap traffic").
	 */
	error = sslc_tls13_derive_secret(schedule->hash, secret, "c ap traffic", transcript_hash, client);
	if (error != 0)
		goto fail;

	/* Derives the server's application traffic secret. */
	error = sslc_tls13_derive_secret(schedule->hash, secret, "s ap traffic", transcript_hash, server);
	if (error != 0)
		goto fail;

	/* Publishes the three secrets only now that all are made. */
	memcpy(schedule->secret, secret, size);
	memcpy(schedule->client_application, client, size);
	memcpy(schedule->server_application, server, size);
	cryc_wipe(secret, sizeof(secret));
	cryc_wipe(client, sizeof(client));
	cryc_wipe(server, sizeof(server));

	/* Succeeded: the schedule is at the Master Secret. */
	return 0;

fail:
	/* Leaves the schedule as it was and wipes the temporaries. */
	cryc_wipe(secret, sizeof(secret));
	cryc_wipe(client, sizeof(client));
	cryc_wipe(server, sizeof(server));
	return EINVAL;
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
	size_t hash_size;
	int error;

	/* The inputs must exist. */
	if (suite == NULL || secret == NULL || keys == NULL)
		return EINVAL;

	/* The suite must be a TLS 1.3 suite of this hash. */
	hash_size = sslc_keysched_hash_size(hash);
	if (hash_size == 0)
		return EINVAL;
	if (suite->version != SSLC_TLS13)
		return EINVAL;
	if (suite->hash != hash)
		return EINVAL;

	/* The suite's key and IV must fit the structure (the IV is 12 bytes in TLS 1.3). */
	if (suite->key_size == 0 || suite->key_size > sizeof(keys->key))
		return EINVAL;
	if (suite->fixed_iv_size != SSLC_TLS13_IV_SIZE)
		return EINVAL;

	/* Starts from nothing, so an error leaves no half-made key. */
	cryc_wipe(keys, sizeof(*keys));

	/*
	 * Derives the AEAD key.
	 *
	 * Values referenced: RFC 8446 section 7.3 (the labels "key" and "iv",
	 * an empty context).
	 */
	error = sslc_tls13_expand_label(hash, secret, "key", NULL, 0, keys->key, suite->key_size);
	if (error != 0) {
		cryc_wipe(keys, sizeof(*keys));
		return EINVAL;
	}

	/* Derives the 12-byte IV. */
	error = sslc_tls13_expand_label(hash, secret, "iv", NULL, 0, keys->iv, SSLC_TLS13_IV_SIZE);
	if (error != 0) {
		cryc_wipe(keys, sizeof(*keys));
		return EINVAL;
	}

	/* Records the suite and the sizes of what was derived. */
	keys->suite = suite;
	keys->key_size = suite->key_size;
	keys->iv_size = SSLC_TLS13_IV_SIZE;

	/* Succeeded: keys holds the key and the IV. */
	return 0;
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
	uint8_t finished_key[CRYC_HASH_MAX_SIZE];
	size_t size;
	int error;

	/* The hash must be a suite's and the inputs must exist. */
	size = sslc_keysched_hash_size(hash);
	if (size == 0)
		return EINVAL;
	if (base_key == NULL || transcript_hash == NULL || out == NULL)
		return EINVAL;

	/*
	 * Derives the Finished key from the sender's handshake traffic secret.
	 *
	 * Values referenced: RFC 8446 section 4.4.4 (the label "finished").
	 */
	error = sslc_tls13_expand_label(hash, base_key, "finished", NULL, 0, finished_key, size);
	if (error != 0)
		return EINVAL;

	/* The verify_data is the HMAC of the transcript hash under that key. */
	error = cryc_hmac(hash, finished_key, size, transcript_hash, size, out);
	cryc_wipe(finished_key, sizeof(finished_key));
	if (error != 0)
		return EINVAL;

	/* Succeeded: out holds the verify_data. */
	return 0;
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
	uint8_t next[CRYC_HASH_MAX_SIZE];
	size_t size;
	int error;

	/* The hash must be a suite's and the secret must exist. */
	size = sslc_keysched_hash_size(hash);
	if (size == 0)
		return EINVAL;
	if (secret == NULL)
		return EINVAL;

	/*
	 * Derives the next generation into a temporary (the expansion may not
	 * overlap its input).
	 *
	 * Values referenced: RFC 8446 section 7.2 (the label "traffic upd").
	 */
	error = sslc_tls13_expand_label(hash, secret, "traffic upd", NULL, 0, next, size);
	if (error != 0)
		return EINVAL;

	/* Replaces the old generation, which is no longer wanted. */
	memcpy(secret, next, size);
	cryc_wipe(next, sizeof(next));

	/* Succeeded: secret is the next generation. */
	return 0;
}

/*
 * Wipes the schedule.
 */
void
sslc_tls13_clear(
	struct sslc_tls13_schedule *schedule)
{
	/* Wipes every secret and the hash choice. */
	cryc_wipe(schedule, sizeof(*schedule));
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
	size_t hash_size;
	int error;

	/* The hash must be a suite's. */
	hash_size = sslc_keysched_hash_size(hash);
	if (hash_size == 0)
		return EINVAL;

	/* The label must exist; the byte strings exist when they have a size. */
	if (label == NULL)
		return EINVAL;
	if (secret == NULL && secret_size != 0)
		return EINVAL;
	if (seed == NULL && seed_size != 0)
		return EINVAL;
	if (out == NULL && out_size != 0)
		return EINVAL;

	/* The output is at most 1024 bytes. */
	if (out_size > SSLC_TLS12_PRF_MAX)
		return EINVAL;

	/* Expands the secret into the output. */
	error = sslc_tls12_p_hash(hash, secret, secret_size, label, seed, seed_size, out, out_size);
	if (error != 0)
		return EINVAL;

	/* Succeeded: out holds out_size bytes. */
	return 0;
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
	const char *label;
	size_t hash_size;
	size_t wanted;
	int error;

	/* The hash must be a suite's. */
	hash_size = sslc_keysched_hash_size(hash);
	if (hash_size == 0)
		return EINVAL;

	/* The inputs must exist; a pre-master secret is never empty. */
	if (pre_master == NULL || context == NULL || master == NULL)
		return EINVAL;
	if (pre_master_size == 0)
		return EINVAL;

	/*
	 * The extended master secret hashes the session (the hash's size);
	 * the older form hashes the two randoms (64 bytes).
	 *
	 * Values referenced: RFC 7627 section 4 (the label "extended master
	 * secret") and RFC 5246 section 8.1 (the label "master secret").
	 */
	if (extended_master_secret == 1) {
		label = "extended master secret";
		wanted = hash_size;
	} else if (extended_master_secret == 0) {
		label = "master secret";
		wanted = SSLC_TLS12_RANDOMS;
	} else {
		return EINVAL;
	}
	if (context_size != wanted)
		return EINVAL;

	/* Expands the pre-master secret into the 48-byte master secret. */
	error = sslc_tls12_prf(hash, pre_master, pre_master_size, label, context, context_size, master, SSLC_TLS12_MASTER);
	if (error != 0)
		return EINVAL;

	/* Succeeded: master holds 48 bytes. */
	return 0;
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
	uint8_t seed[SSLC_TLS12_RANDOMS];
	uint8_t block[SSLC_TLS12_KEY_BLOCK_MAX];
	size_t key_size;
	size_t iv_size;
	size_t total;
	size_t position;
	int error;

	/* The inputs must exist, and the two outputs must be two structures. */
	if (suite == NULL || master == NULL || client_random == NULL || server_random == NULL)
		return EINVAL;
	if (client_keys == NULL || server_keys == NULL || client_keys == server_keys)
		return EINVAL;

	/* The suite must be a TLS 1.2 suite with a hash the PRF takes. */
	if (suite->version != SSLC_TLS12)
		return EINVAL;
	if (sslc_keysched_hash_size(suite->hash) == 0)
		return EINVAL;

	/* The key and IV must fit the structures. */
	key_size = suite->key_size;
	iv_size = suite->fixed_iv_size;
	if (key_size == 0 || key_size > sizeof(client_keys->key))
		return EINVAL;
	if (iv_size == 0 || iv_size > sizeof(client_keys->iv))
		return EINVAL;
	total = 2U * key_size + 2U * iv_size;

	/* The seed is the server's random and then the client's (the opposite of the master secret's). */
	memcpy(seed, server_random, SSLC_TLS12_RANDOM);
	memcpy(seed + SSLC_TLS12_RANDOM, client_random, SSLC_TLS12_RANDOM);

	/*
	 * Expands the master secret into the key block.
	 *
	 * Values referenced: RFC 5246 section 6.3 (the label "key expansion").
	 */
	error = sslc_tls12_prf(suite->hash, master, SSLC_TLS12_MASTER, "key expansion", seed, sizeof(seed), block, total);
	if (error != 0) {
		cryc_wipe(block, sizeof(block));
		return EINVAL;
	}

	/* Starts both outputs from nothing. */
	cryc_wipe(client_keys, sizeof(*client_keys));
	cryc_wipe(server_keys, sizeof(*server_keys));

	/* Splits the block: the client's key, the server's key, the client's IV, the server's IV. */
	position = 0;
	memcpy(client_keys->key, block + position, key_size);
	position += key_size;
	memcpy(server_keys->key, block + position, key_size);
	position += key_size;
	memcpy(client_keys->iv, block + position, iv_size);
	position += iv_size;
	memcpy(server_keys->iv, block + position, iv_size);

	/* Records the suite and the sizes. */
	client_keys->suite = suite;
	client_keys->key_size = key_size;
	client_keys->iv_size = iv_size;
	server_keys->suite = suite;
	server_keys->key_size = key_size;
	server_keys->iv_size = iv_size;

	/* Wipes the key block; the outputs hold it now. */
	cryc_wipe(block, sizeof(block));

	/* Succeeded: both directions have their keys. */
	return 0;
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
	const char *label;
	size_t size;
	int error;

	/* The hash must be a suite's and the inputs must exist. */
	size = sslc_keysched_hash_size(hash);
	if (size == 0)
		return EINVAL;
	if (master == NULL || transcript_hash == NULL || out == NULL)
		return EINVAL;

	/*
	 * The label names the sender.
	 *
	 * Values referenced: RFC 5246 section 7.4.9 (the labels "client
	 * finished" and "server finished").
	 */
	if (from_client != 0)
		label = "client finished";
	else
		label = "server finished";

	/* The verify_data is the first 12 bytes of the PRF over the transcript hash. */
	error = sslc_tls12_prf(hash, master, SSLC_TLS12_MASTER, label, transcript_hash, size, out, SSLC_TLS12_VERIFY_DATA);
	if (error != 0)
		return EINVAL;

	/* Succeeded: out holds 12 bytes. */
	return 0;
}

/*
 * Returns the digest size of hash when it is SHA-256 or SHA-384, else 0.
 */
static size_t
sslc_keysched_hash_size(
	enum cryc_hash_id hash)
{
	/* The two hashes the suites name. */
	if (hash == CRYC_HASH_SHA256)
		return cryc_hash_size(hash);
	if (hash == CRYC_HASH_SHA384)
		return cryc_hash_size(hash);

	/* No other hash is used. */
	return 0;
}

/*
 * Computes Derive-Secret(secret, label, transcript_hash) into out: an
 * HKDF-Expand-Label whose context is the transcript hash and whose
 * length is the hash size.
 */
static int
sslc_tls13_derive_secret(
	enum cryc_hash_id hash,
	const uint8_t *secret,
	const char *label,
	const uint8_t *transcript_hash,
	uint8_t *out)
{
	size_t size;
	int error;

	/* Expands over the transcript hash. */
	size = sslc_keysched_hash_size(hash);
	error = sslc_tls13_expand_label(hash, secret, label, transcript_hash, size, out, size);
	if (error != 0)
		return error;

	/* Succeeded: out holds the derived secret. */
	return 0;
}

/*
 * Uses Derive-Secret(secret, "derived", "") as the salt to extract ikm
 * into out: the step from one secret of the chain to the next.
 */
static int
sslc_tls13_next_secret(
	enum cryc_hash_id hash,
	const uint8_t *secret,
	const uint8_t *ikm,
	size_t ikm_size,
	uint8_t *out)
{
	uint8_t empty[CRYC_HASH_MAX_SIZE];
	uint8_t derived[CRYC_HASH_MAX_SIZE];
	uint8_t nothing[1];
	size_t size;
	int error;

	/* Hashes the empty string, the context of the "derived" secret. */
	size = sslc_keysched_hash_size(hash);
	nothing[0] = 0;
	error = cryc_hash_digest(hash, nothing, 0, empty);
	if (error != 0)
		return EINVAL;

	/*
	 * Derives the salt from the current secret.
	 *
	 * Values referenced: RFC 8446 section 7.1 (the label "derived").
	 */
	error = sslc_tls13_derive_secret(hash, secret, "derived", empty, derived);
	if (error != 0) {
		cryc_wipe(derived, sizeof(derived));
		return EINVAL;
	}

	/* Extracts the next secret with that salt. */
	error = cryc_hkdf_extract(hash, derived, size, ikm, ikm_size, out);
	cryc_wipe(derived, sizeof(derived));
	if (error != 0)
		return EINVAL;

	/* Succeeded: out holds the next secret. */
	return 0;
}

/*
 * Writes out_size bytes of P_hash (RFC 5246 5) over label || seed into
 * out: HMAC(secret, A(i) || label || seed) for i = 1, 2, ..., with
 * A(0) = label || seed and A(i) = HMAC(secret, A(i - 1)).
 */
static int
sslc_tls12_p_hash(
	enum cryc_hash_id hash,
	const uint8_t *secret,
	size_t secret_size,
	const char *label,
	const uint8_t *seed,
	size_t seed_size,
	uint8_t *out,
	size_t out_size)
{
	struct cryc_hmac hmac;
	uint8_t chain[CRYC_HASH_MAX_SIZE];
	uint8_t next[CRYC_HASH_MAX_SIZE];
	uint8_t block[CRYC_HASH_MAX_SIZE];
	size_t label_size;
	size_t size;
	size_t written;
	size_t take;
	int error;

	/* A(1) = HMAC(secret, label || seed). */
	size = sslc_keysched_hash_size(hash);
	label_size = strlen(label);
	error = cryc_hmac_init(&hmac, hash, secret, secret_size);
	if (error != 0)
		return EINVAL;
	cryc_hmac_update(&hmac, (const uint8_t *)label, label_size);
	cryc_hmac_update(&hmac, seed, seed_size);
	cryc_hmac_final(&hmac, chain);

	/* Appends HMAC(secret, A(i) || label || seed) blocks until out is full. */
	written = 0;
	while (written < out_size) {
		/* One block of output from the current A(i). */
		error = cryc_hmac_init(&hmac, hash, secret, secret_size);
		if (error != 0)
			goto fail;
		cryc_hmac_update(&hmac, chain, size);
		cryc_hmac_update(&hmac, (const uint8_t *)label, label_size);
		cryc_hmac_update(&hmac, seed, seed_size);
		cryc_hmac_final(&hmac, block);

		/* The last block may be used in part. */
		take = out_size - written;
		if (take > size)
			take = size;
		memcpy(out + written, block, take);
		written += take;

		/* The next A(i) is needed only when more output is. */
		if (written < out_size) {
			error = cryc_hmac(hash, secret, secret_size, chain, size, next);
			if (error != 0)
				goto fail;
			memcpy(chain, next, size);
		}
	}

	/* Wipes the working values. */
	cryc_wipe(chain, sizeof(chain));
	cryc_wipe(next, sizeof(next));
	cryc_wipe(block, sizeof(block));

	/* Succeeded: out holds out_size bytes. */
	return 0;

fail:
	/* Wipes what was made, for a caller that must not see a part. */
	cryc_wipe(chain, sizeof(chain));
	cryc_wipe(next, sizeof(next));
	cryc_wipe(block, sizeof(block));
	cryc_wipe(out, out_size);
	return EINVAL;
}
