/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The transcript hash and the key schedules: TLS 1.3's (RFC 8446 4.4.1,
 * 7.1 to 7.3) and TLS 1.2's PRF, master secret, key block and Finished
 * (RFC 5246 5, 6.3, 7.4.9; the extended master secret of RFC 7627 4).
 *
 * Every secret here is wiped by the structure's _clear function; the
 * functions are constant time in the secrets (HMAC and HKDF are).
 *
 * The unit tests check TLS 1.3 against the traces of RFC 8448 (the
 * labelled values of section 3 "Simple 1-RTT Handshake" and section 5
 * "HelloRetryRequest"), which give every secret, key and IV in hex.
 */

#ifndef LIBSSL_COMPAT_SSLC_KEYSCHED_H
#define LIBSSL_COMPAT_SSLC_KEYSCHED_H

#include "sslc-record.h"

/* The size of a TLS 1.2 Finished's verify_data (RFC 5246 7.4.9; the suites here set no other). */
#define SSLC_TLS12_VERIFY_DATA	12U

/* The size of the TLS 1.2 master secret (RFC 5246 8.1). */
#define SSLC_TLS12_MASTER	48U

/*
 * The running hash of the handshake messages (4.4.1).
 *
 * Until the cipher suite is known, the messages are hashed with both
 * SHA-256 and SHA-384 (sha256 and sha384, active both); after
 * sslc_transcript_select only the suite's hash goes on.  Copyable; holds
 * no secret but the hash of messages that may carry private data (not
 * wiped by need, wiped by sslc_transcript_clear all the same).
 */
struct sslc_transcript {
	struct cryc_hash sha256;
	struct cryc_hash sha384;
	enum cryc_hash_id selected;
};

/*
 * The TLS 1.3 key schedule (7.1): the hash, the current secret in the
 * chain Early Secret -> Handshake Secret -> Master Secret, and the traffic
 * secrets derived so far.  Sizes are the hash's.  Secret.
 */
struct sslc_tls13_schedule {
	enum cryc_hash_id hash;
	size_t size;
	uint8_t secret[CRYC_HASH_MAX_SIZE];
	uint8_t client_handshake[CRYC_HASH_MAX_SIZE];
	uint8_t server_handshake[CRYC_HASH_MAX_SIZE];
	uint8_t client_application[CRYC_HASH_MAX_SIZE];
	uint8_t server_application[CRYC_HASH_MAX_SIZE];
};

/* Starts a transcript hashing with both candidate hashes. */
void sslc_transcript_init(struct sslc_transcript *transcript);

/* Absorbs one whole handshake message (its 4-byte header and body, 4.4.1). */
void sslc_transcript_update(struct sslc_transcript *transcript, const uint8_t *message, size_t size);

/*
 * Keeps only hash (SHA-256 or SHA-384, the suite's), dropping the other.
 *
 * Returns 0, or EINVAL for another hash.  Calling it twice with the same
 * hash is allowed (no change).
 */
int sslc_transcript_select(struct sslc_transcript *transcript, enum cryc_hash_id hash);

/*
 * Writes the hash of the messages so far (Transcript-Hash, 4.4.1) with
 * the selected hash, without ending the transcript.
 *
 * Returns the size written, or 0 before a hash is selected.
 */
size_t sslc_transcript_hash(const struct sslc_transcript *transcript, uint8_t *out);

/*
 * Replaces the transcript after a HelloRetryRequest (4.4.1): with the
 * selected hash H, the transcript becomes message_hash (254) || 00 00 ||
 * Hash.length || H(ClientHello1), where the current state is the hash of
 * exactly ClientHello1.  The caller then absorbs the HelloRetryRequest.
 *
 * Returns 0, or EINVAL before a hash is selected.
 */
int sslc_transcript_restart_hrr(struct sslc_transcript *transcript);

/* Wipes the transcript. */
void sslc_transcript_clear(struct sslc_transcript *transcript);

/*
 * HKDF-Expand-Label (7.1): HKDF-Expand(secret, HkdfLabel, length) with
 * HkdfLabel = uint16 length || opaque label<7..255> = "tls13 " + label ||
 * opaque context<0..255>.
 *
 * secret is hash-size bytes; label is a C string of 1 to 249 characters
 * (EINVAL otherwise); context may be NULL only when context_size is 0
 * and is at most 255 bytes; length at most 255 * hash size and at most
 * 65535.  Returns 0 or EINVAL (out not written).
 */
int sslc_tls13_expand_label(enum cryc_hash_id hash, const uint8_t *secret, const char *label, const uint8_t *context, size_t context_size, uint8_t *out, size_t length);

/*
 * Starts the schedule (7.1): Early Secret = HKDF-Extract(0, 0) (no PSK:
 * both are hash-size strings of zeros).  Returns 0 or EINVAL for a hash
 * other than SHA-256 and SHA-384.
 */
int sslc_tls13_start(struct sslc_tls13_schedule *schedule, enum cryc_hash_id hash);

/*
 * Moves to the Handshake Secret (7.1): Derive-Secret(Early, "derived",
 * "") as the salt, the (EC)DHE shared secret as the IKM; then derives
 * client_handshake and server_handshake ("c hs traffic", "s hs traffic")
 * over transcript_hash (ClientHello...ServerHello).
 *
 * shared is shared_size bytes (32 for x25519, 32 for secp256r1).
 * transcript_hash is the schedule's hash size.  Returns 0 or EINVAL.
 */
int sslc_tls13_handshake(struct sslc_tls13_schedule *schedule, const uint8_t *shared, size_t shared_size, const uint8_t *transcript_hash);

/*
 * Moves to the Master Secret (7.1): Derive-Secret(Handshake, "derived",
 * "") as the salt, zeros as the IKM; then derives client_application and
 * server_application ("c ap traffic", "s ap traffic") over
 * transcript_hash (ClientHello...server Finished).  Returns 0 or EINVAL.
 */
int sslc_tls13_master(struct sslc_tls13_schedule *schedule, const uint8_t *transcript_hash);

/*
 * Derives the record keys of a traffic secret (7.3): key =
 * HKDF-Expand-Label(secret, "key", "", key_length), iv =
 * HKDF-Expand-Label(secret, "iv", "", 12), for suite.  Returns 0 or EINVAL
 * (a suite that is not TLS 1.3's, or whose hash is not the schedule's).
 */
int sslc_tls13_traffic_keys(enum cryc_hash_id hash, const struct sslc_suite *suite, const uint8_t *secret, struct sslc_traffic_keys *keys);

/*
 * Computes a Finished's verify_data (4.4.4): finished_key =
 * HKDF-Expand-Label(base_key, "finished", "", Hash.length); verify_data =
 * HMAC(finished_key, transcript_hash).  base_key is the sender's handshake
 * traffic secret.  out receives the hash size.  Returns 0 or EINVAL.
 */
int sslc_tls13_finished(enum cryc_hash_id hash, const uint8_t *base_key, const uint8_t *transcript_hash, uint8_t *out);

/*
 * Replaces a traffic secret with the next generation (7.2):
 * application_traffic_secret_N+1 = HKDF-Expand-Label(secret_N,
 * "traffic upd", "", Hash.length), in place.  Returns 0 or EINVAL.
 */
int sslc_tls13_update_secret(enum cryc_hash_id hash, uint8_t *secret);

/* Wipes the schedule. */
void sslc_tls13_clear(struct sslc_tls13_schedule *schedule);

/*
 * The TLS 1.2 PRF (RFC 5246 5): PRF(secret, label, seed) = P_hash(secret,
 * label + seed) with the suite's hash (SHA-256 or SHA-384; RFC 5246 5 and
 * RFC 5289 3).
 *
 * label is a C string; seed is seed_size bytes; out receives out_size
 * bytes (at most 1024: the largest use is the key block).  Returns 0 or
 * EINVAL.
 */
int sslc_tls12_prf(enum cryc_hash_id hash, const uint8_t *secret, size_t secret_size, const char *label, const uint8_t *seed, size_t seed_size, uint8_t *out, size_t out_size);

/*
 * Computes the master secret.
 *
 * With extended_master_secret 1 (the server echoed the extension, RFC
 * 7627 5.2): PRF(pre_master, "extended master secret", session_hash)
 * with session_hash the transcript hash through ClientKeyExchange (RFC
 * 7627 4).  With 0: PRF(pre_master, "master secret", client_random ||
 * server_random) (RFC 5246 8.1).  context is session_hash (hash size) or
 * the 64 bytes of the two randoms accordingly.  master receives 48 bytes.
 * Returns 0 or EINVAL.
 */
int sslc_tls12_master(enum cryc_hash_id hash, int extended_master_secret, const uint8_t *pre_master, size_t pre_master_size, const uint8_t *context, size_t context_size, uint8_t *master);

/*
 * Derives the record keys of both directions from the master secret
 * (RFC 5246 6.3): key_block = PRF(master, "key expansion", server_random
 * || client_random), split as client_write_key, server_write_key,
 * client_write_IV, server_write_IV (no MAC keys for AEAD suites), with
 * the suite's key size and fixed IV size (4 for AES-GCM, RFC 5288 3; 12
 * for ChaCha20-Poly1305, RFC 7905 2).  Returns 0 or EINVAL.
 */
int sslc_tls12_key_block(const struct sslc_suite *suite, const uint8_t *master, const uint8_t *client_random, const uint8_t *server_random, struct sslc_traffic_keys *client_keys, struct sslc_traffic_keys *server_keys);

/*
 * Computes a TLS 1.2 Finished's verify_data (RFC 5246 7.4.9):
 * PRF(master, "client finished" or "server finished",
 * transcript_hash)[0..11].  from_client is 1 for the client's label.
 * out receives 12 bytes.  Returns 0 or EINVAL.
 */
int sslc_tls12_finished(enum cryc_hash_id hash, const uint8_t *master, int from_client, const uint8_t *transcript_hash, uint8_t *out);

#endif
