/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Checks the TLS 1.2 states of the handshake state machine (sslc-hs.h):
 * what a TLS 1.2 ServerHello may say, the server's flight, the client's
 * flight, the keys and the Finished messages, and the messages after the
 * handshake.
 *
 * The server is tlspeer.h in its message mode (see test-hs13.c for how its
 * messages and the client's actions are carried): full handshakes with
 * each suite and group, both P-256 and P-384 leaves, with and without
 * extended master secret, server_name, a CertificateRequest; every break
 * of the peer; hand-made ServerHellos and ServerKeyExchanges for each
 * check of the header's file comment; the order of the messages in every
 * state; the certificate verdicts; the ClientHello's random draws;
 * HelloRequest during and after the handshake; and truncated and altered
 * server messages.
 *
 * The expected values come from RFC 5246 (the message syntax and order,
 * the verify_data sizes, the empty Certificate), RFC 8422 (the point
 * forms, ServerKeyExchange), RFC 5746 and RFC 7627 (the extensions), and
 * the peer's own computation of the master secret and keys (which a
 * client that mixed up the session hash, the randoms or the labels could
 * not match).  RSA servers are not here: tlspeer has no RSA key.
 */

#include "check.h"
#include "tlspeer.h"

#include "userland/base/libcrypto-compat/cryc-chain.h"
#include "userland/base/libssl-compat/sslc-hs.h"

/* The most server messages one handshake records for replay. */
#define REPLAY_MAX	16U

/* The largest server message recorded for replay. */
#define REPLAY_SIZE	8192U

/*
 * The HelloRetryRequest random, written here again from the RFC.
 * Values referenced: RFC 8446 section 4.1.3.
 */
static const uint8_t retry_random[32] = {
	0xcf, 0x21, 0xad, 0x74, 0xe5, 0x9a, 0x61, 0x11, 0xbe, 0x1d, 0x8c, 0x02, 0x1e, 0x65, 0xb8, 0x91,
	0xc2, 0xa2, 0x11, 0x16, 0x7a, 0xbb, 0x8c, 0x5e, 0x07, 0x9e, 0x09, 0xe2, 0xc8, 0xa8, 0x33, 0x9c
};

/*
 * The x25519 public key of RFC 7748's Bob, a well-formed point for the
 * hand-made ServerKeyExchange messages.
 * Values referenced: RFC 7748 section 6.1.
 */
static const char x25519_bob_public[] = "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f";

/*
 * An ECDSA-Sig-Value that is well formed and verifies nothing: SEQUENCE
 * { INTEGER 1, INTEGER 1 }.
 * Values referenced: RFC 3279 section 2.2.3 (Ecdsa-Sig-Value).
 */
static const uint8_t bogus_signature[8] = { 0x30, 0x06, 0x02, 0x01, 0x01, 0x02, 0x01, 0x01 };

/*
 * The test's random source: byte i of the stream is byte i mod 32 of
 * SHA-256("hs12 stream" || seed || be32(i / 32)); fail_at names the call
 * (1 first) that fails; sizes records each call's size.
 */
struct stream {
	uint8_t seed;
	unsigned long position;
	unsigned calls;
	unsigned fail_at;
	size_t sizes[64];
};

/*
 * One client handshake and its peer: the handshake, its configuration,
 * store and random source; the last error; what the client's actions did
 * (a letter per action in log: S send, C change_cipher_spec, R read keys,
 * W write keys, A warning, D done); the keys it installed, compared with
 * the peer's; the verification callback's record.
 */
struct run {
	struct sslc_hs hs;
	struct sslc_hs_config config;
	struct sslc_hs_actions actions;
	struct cryc_store store;
	struct stream stream;
	struct tlspeer *peer;
	int error;
	int peer_error;
	int done;
	char log[64];
	size_t log_size;
	unsigned key_mismatch;
	int read_keyed;
	struct sslc_traffic_keys read_keys;
	unsigned write_changes;
	unsigned messages_sent;
	unsigned callback_calls;
	int callback_preverified;
	enum cryc_verify_code callback_code;
	int callback_answer;
};

/* What a server sent in one handshake, kept to be fed again (altered) to a client of the same random source. */
struct replay {
	const struct tlspeer_pki *pki;
	unsigned kind[REPLAY_MAX];
	uint8_t data[REPLAY_MAX][REPLAY_SIZE];
	size_t size[REPLAY_MAX];
	size_t count;
};

/* A TLS 1.2 ServerHello made by hand: each field, and which extensions go in. */
struct sh_spec {
	uint16_t legacy_version;
	uint8_t random[32];
	uint8_t session_id[32];
	size_t session_id_size;
	uint16_t suite;
	unsigned compression;
	int renegotiation;
	int extended_master;
	int point_formats;
	int server_name;
	int share;
	uint16_t version;
	int cookie;
	int has_extra;
	uint16_t extra_type;
};

/* The root and leaf of P-256 and of P-384, for host "server.test". */
static struct tlspeer_pki pki256;
static struct tlspeer_pki pki384;

/* The replay of a plain handshake, and of one with a CertificateRequest over secp256r1 and the P-384 leaf. */
static struct replay replay_plain;
static struct replay replay_full;

/* Draws size bytes from the stream; fails at the call fail_at names. */
static int stream_random(void *context, uint8_t *out, size_t size);

/* The verification callback: counts, records, answers as told. */
static int verify_callback(void *context, int preverified, enum cryc_verify_code code);

/* Returns 1 when two sets of record keys are equal. */
static int keys_equal(const struct sslc_traffic_keys *left, const struct sslc_traffic_keys *right);

/* Makes a run: a client of min to max trusting root (may be NULL), verifying host at the PKI time. */
static struct run * run_new(uint16_t min_version, uint16_t max_version, const struct tlspeer_pki *root, const char *host);

/* Frees a run and its peer. */
static void run_free(struct run *run);

/* Appends a letter to the run's log of actions. */
static void log_action(struct run *run, char letter);

/* Carries out the client's actions of the last call against the peer (when there is one). */
static void apply(struct run *run);

/* Feeds the peer's items to the client (checking the client reads each under the peer's keys) until none wait or the client fails. */
static void pump(struct run *run);

/* Makes the run's peer from config. */
static int run_peer(struct run *run, const struct tlspeer_config *config);

/* Starts the client and runs the exchange until it ends; returns the client's error. */
static int go(struct run *run);

/* Feeds the client one message (no peer) and carries out the actions. */
static void feed(struct run *run, const uint8_t *message, size_t size);

/* Returns 1 when size bytes at bytes are all zero. */
static int all_zero(const uint8_t *bytes, size_t size);

/* Checks that a run ended in a failure with error and alert, and that the failed handshake refuses more and holds no secret. */
static void expect_failed(struct run *run, int error, unsigned alert);

/* Checks that a run completed a TLS 1.2 handshake with the peer; log is the client's actions, ems what both sides used. */
static void check_connected(struct run *run, const char *log, int ems);

/* Makes a TLS 1.2 peer config over pki. */
static void peer12(struct tlspeer_config *config, const struct tlspeer_pki *pki);

/* Runs a full handshake against config with a client of min to max for host trusting pki; returns the run (to be freed). */
static struct run * handshake(const struct tlspeer_config *config, const struct tlspeer_pki *pki, uint16_t max_version, const char *host);

/* Runs a handshake that the client must refuse, and checks the failure. */
static void check_peer_fails(const struct tlspeer_config *config, const struct tlspeer_pki *pki, uint16_t max_version, const char *host, int error, unsigned alert);

/* Records the server's items of a handshake with config from a client of host (a 1.2-only client of seed 0). */
static void record_replay(const struct tlspeer_config *config, const struct tlspeer_pki *pki, struct replay *replay);

/* Feeds item number item of a replay to a run. */
static void feed_item(struct run *run, const struct replay *replay, size_t item);

/* Starts a fresh 1.2-only client (no peer) and feeds it the first count items of a replay; returns the run. */
static struct run * run_prefix(const struct replay *replay, size_t count);

static struct run * replay_run(const struct replay *replay, size_t index, const uint8_t *message, size_t size);

/* Writes a handshake message of type with a body of size bytes into out; returns its size. */
static size_t message_make(unsigned type, const uint8_t *body, size_t size, uint8_t *out);

/* Fills a ServerHello of the plain kind: no session id, ECDHE_ECDSA_AES_128_GCM, an empty renegotiation_info. */
static void sh_default(struct sh_spec *spec);

/* Writes a hand-made ServerHello message into out; returns its size. */
static size_t sh_write(const struct sh_spec *spec, uint8_t *out, size_t room);

/* Starts a client without a peer and feeds it a hand-made ServerHello; returns the run. */
static struct run * feed_sh(uint16_t max_version, const char *host, const struct sh_spec *spec);

/* Feeds a hand-made ServerHello and checks the failure. */
static void check_sh(uint16_t max_version, const char *host, const struct sh_spec *spec, int error, unsigned alert);

/* Feeds a hand-made ServerHello and checks that the client takes it and waits for the Certificate. */
static void check_sh_taken(uint16_t max_version, const char *host, const struct sh_spec *spec, int ems);

/* Replaces every occurrence of the size bytes from by to in data; returns how many. */
static unsigned patch(uint8_t *data, size_t data_size, const uint8_t *from, const uint8_t *to, size_t size);

/* Writes a ServerKeyExchange of the given fields into out; returns its size. */
static size_t ske_write(uint8_t *out, size_t room, unsigned curve_type, uint16_t group, const uint8_t *point, size_t point_size, uint16_t scheme, const uint8_t *signature, size_t signature_size);

/* Feeds the replayed flight with the ServerKeyExchange (item 2) replaced; checks the failure. */
static void check_ske(const struct replay *replay, const uint8_t *message, size_t size, int error, unsigned alert);

/* Checks that truncated and altered server messages never complete a handshake and never fault. */
static void test_altered(const struct replay *replay, size_t step);

/* Draws size bytes from the stream; fails at the call fail_at names. */
static int
stream_random(
	void *context,
	uint8_t *out,
	size_t size)
{
	struct stream *stream;
	uint8_t input[16];
	uint8_t block[CRYC_SHA256_SIZE];
	size_t index;
	unsigned long position;

	/* The call is counted, and fails when asked to. */
	stream = context;
	stream->calls++;
	if (stream->calls <= 64U)
		stream->sizes[stream->calls - 1U] = size;
	if (stream->fail_at != 0U && stream->calls == stream->fail_at)
		return EIO;

	/* Each byte from its block. */
	memcpy(input, "hs12 stream", 11);
	input[11] = stream->seed;
	for (index = 0; index < size; index++) {
		position = stream->position + index;
		cryc_store_be32(input + 12, (uint32_t)(position / 32U));
		cryc_hash_digest(CRYC_HASH_SHA256, input, 16, block);
		out[index] = block[position % 32U];
	}

	/* Counts the bytes drawn so far. */
	stream->position += size;
	return 0;
}

/* The verification callback: counts, records, answers as told. */
static int
verify_callback(
	void *context,
	int preverified,
	enum cryc_verify_code code)
{
	struct run *run;

	/* Recorded for the test. */
	run = context;
	run->callback_calls++;
	run->callback_preverified = preverified;
	run->callback_code = code;
	return run->callback_answer;
}

/* Returns 1 when two sets of record keys are equal. */
static int
keys_equal(
	const struct sslc_traffic_keys *left,
	const struct sslc_traffic_keys *right)
{
	int different;

	/* Every field that the record layer uses. */
	if (left->suite != right->suite)
		return 0;
	if (left->key_size != right->key_size || left->iv_size != right->iv_size)
		return 0;

	/* Compares the keys. */
	different = memcmp(left->key, right->key, left->key_size);
	if (different != 0)
		return 0;

	/* Compares the IVs. */
	different = memcmp(left->iv, right->iv, left->iv_size);
	if (different != 0)
		return 0;

	/* Succeeded: the keys are equal. */
	return 1;
}

/* Makes a run: a client of min to max trusting root (may be NULL), verifying host at the PKI time. */
static struct run *
run_new(
	uint16_t min_version,
	uint16_t max_version,
	const struct tlspeer_pki *root,
	const char *host)
{
	struct run *run;

	/* Zeroed, with an empty store or one with the root. */
	run = calloc(1, sizeof(*run));
	if (run == NULL) {
		CHECK(0);
		return NULL;
	}

	/* Makes the certificate store. */
	CHECK(cryc_store_init(&run->store) == 0);
	if (root != NULL)
		CHECK(cryc_store_add_der(&run->store, root->root, root->root_size) == 0);

	/* The configuration. */
	run->config.min_version = min_version;
	run->config.max_version = max_version;
	run->config.store = &run->store;
	run->config.params.has_time = 1;
	run->config.params.time = TLSPEER_PKI_TIME;
	if (host != NULL) {
		run->config.params.has_host = 1;
		CHECK(cryc_name_host_normalize(host, run->config.params.host) == 0);
		run->config.server_name = run->config.params.host;
	}

	/* Asks for certificate verification and gives the client the stream of random bytes. */
	run->config.verify_peer = 1;
	run->config.random = stream_random;
	run->config.random_context = &run->stream;
	run->callback_answer = 1;
	return run;
}

/* Frees a run and its peer. */
static void
run_free(
	struct run *run)
{
	/* Everything it holds. */
	if (run == NULL)
		return;
	sslc_hs_free(&run->hs);
	tlspeer_free(run->peer);
	cryc_store_free(&run->store);
	free(run);
}

/* Appends a letter to the run's log of actions. */
static void
log_action(
	struct run *run,
	char letter)
{
	/* Bounded; the last byte stays the terminator. */
	if (run->log_size + 1U < sizeof(run->log)) {
		run->log[run->log_size] = letter;
		run->log_size++;
		run->log[run->log_size] = '\0';
	}
}

/* Carries out the client's actions of the last call against the peer (when there is one). */
static void
apply(
	struct run *run)
{
	const struct sslc_hs_action *action;
	const uint8_t *bytes;
	size_t index;
	size_t offset;
	size_t length;
	int equal;
	int error;

	/* In order. */
	for (index = 0; index < run->actions.count; index++) {
		action = &run->actions.list[index];
		switch (action->kind) {
		case SSLC_HS_SEND:
			log_action(run, 'S');

			/* Whole messages, one after the other, exactly filling the action. */
			offset = 0;
			while (offset < action->size) {
				bytes = run->hs.out + action->offset + offset;
				if (action->size - offset < 4U) {
					CHECK(0);
					break;
				}

				/* Takes the length from the message's header. */
				length = 4U + cryc_load_be24(bytes + 1);
				if (length > action->size - offset) {
					CHECK(0);
					break;
				}

				/* Counts the message. */
				run->messages_sent++;
				if (run->peer != NULL) {
					error = tlspeer_handshake(run->peer, bytes, length);
					if (error != 0 && run->peer_error == 0)
						run->peer_error = error;
				}

				/* Moves on to the next message of the action. */
				offset += length;
			}

			break;
		case SSLC_HS_SEND_CCS:
			log_action(run, 'C');
			if (run->peer != NULL) {
				error = tlspeer_change_cipher_spec(run->peer);
				if (error != 0 && run->peer_error == 0)
					run->peer_error = error;
			}

			break;
		case SSLC_HS_SEND_WARNING:
			log_action(run, 'A');
			break;
		case SSLC_HS_SET_READ_KEYS:
			log_action(run, 'R');
			CHECK(action->version13 == 0);
			run->read_keys = action->keys;
			run->read_keyed = 1;
			break;
		case SSLC_HS_SET_WRITE_KEYS:
			log_action(run, 'W');
			CHECK(action->version13 == 0);

			/* TLS 1.2 changes the write keys once, to the keys of the key block. */
			if (run->peer != NULL) {
				if (run->write_changes == 0U) {
					equal = keys_equal(&action->keys, &run->peer->client_application_keys);
					if (!equal)
						run->key_mismatch++;
				}

				/* Every later change is a violation. */
				if (run->write_changes > 0U)
					run->key_mismatch++;
			}

			/* Counts the key change. */
			run->write_changes++;
			break;
		case SSLC_HS_DONE:
			log_action(run, 'D');
			run->done = 1;
			break;
		default:
			CHECK(0);
			break;
		}
	}
}

/* Feeds the peer's items to the client (checking the client reads each under the peer's keys) until none wait or the client fails. */
static void
pump(
	struct run *run)
{
	static uint8_t copy[TLSPEER_MESSAGE_MAX];
	struct tlspeer_item item;
	int equal;
	int more;

	/* One item at a time; the client's answers may make more. */
	while (run->error == 0) {
		more = tlspeer_next(run->peer, &item);
		if (!more)
			break;
		memcpy(copy, item.data, item.size);
		if (item.kind == TLSPEER_ITEM_HANDSHAKE) {
			if (item.epoch == 0U && run->read_keyed)
				run->key_mismatch++;
			if (item.epoch != 0U) {
				equal = 0;
				if (run->read_keyed)
					equal = keys_equal(&run->peer->write_keys[item.epoch], &run->read_keys);
				if (!equal)
					run->key_mismatch++;
			}

			/* Feeds the message to the client. */
			run->error = sslc_hs_message(&run->hs, copy, item.size, &run->actions);
		} else if (item.kind == TLSPEER_ITEM_CCS) {
			run->error = sslc_hs_change_cipher_spec(&run->hs, &run->actions);
		} else {
			CHECK(0);
			break;
		}

		/* Applies the next action unless the handshake already failed. */
		if (run->error == 0)
			apply(run);
	}
}

/* Makes the run's peer from config. */
static int
run_peer(
	struct run *run,
	const struct tlspeer_config *config)
{
	int error;

	/* The peer, checked. */
	error = tlspeer_new(&run->peer, config);
	CHECK(error == 0);
	return error;
}

/* Starts the client and runs the exchange until it ends; returns the client's error. */
static int
go(
	struct run *run)
{
	/* The ClientHello, then the peer's answers. */
	run->error = sslc_hs_start(&run->hs, &run->config, &run->actions);
	if (run->error != 0)
		return run->error;
	apply(run);
	if (run->peer != NULL)
		pump(run);
	return run->error;
}

/* Feeds the client one message (no peer) and carries out the actions. */
static void
feed(
	struct run *run,
	const uint8_t *message,
	size_t size)
{
	/* The message, then what it asked for. */
	run->error = sslc_hs_message(&run->hs, message, size, &run->actions);
	if (run->error == 0)
		apply(run);
}

/* Returns 1 when size bytes at bytes are all zero. */
static int
all_zero(
	const uint8_t *bytes,
	size_t size)
{
	size_t index;

	/* Every byte. */
	for (index = 0; index < size; index++) {
		if (bytes[index] != 0U)
			return 0;
	}

	/* Succeeded: every byte is zero. */
	return 1;
}

/* Checks that a run ended in a failure with error and alert, and that the failed handshake refuses more and holds no secret. */
static void
expect_failed(
	struct run *run,
	int error,
	unsigned alert)
{
	struct sslc_hs_actions actions;
	static const uint8_t finished[4 + 12] = { 0x14, 0x00, 0x00, 0x0c };

	/* The error and the alert, a failed state, no secret left. */
	CHECK(run->error == error);
	CHECK(run->hs.alert == alert);
	if (run->error != error || run->hs.alert != alert)
		printf("  got error %d alert %u, expected %d %u\n", run->error, run->hs.alert, error, alert);
	CHECK(run->hs.state == SSLC_HS_FAILED);
	CHECK(run->done == 0);
	CHECK(sslc_hs_connected(&run->hs) == 0);
	CHECK(all_zero(run->hs.x25519_private, sizeof(run->hs.x25519_private)));
	CHECK(all_zero(run->hs.ec_private, sizeof(run->hs.ec_private)));
	CHECK(all_zero(run->hs.master, sizeof(run->hs.master)));

	/* Later calls are refused, and the alert stays. */
	actions.count = 3;
	CHECK(sslc_hs_message(&run->hs, finished, sizeof(finished), &actions) == EPROTO);
	CHECK(actions.count == 0U);
	CHECK(sslc_hs_change_cipher_spec(&run->hs, &actions) == EPROTO);
	CHECK(run->hs.alert == alert);
}

/* Checks that a run completed a TLS 1.2 handshake with the peer; log is the client's actions, ems what both sides used. */
static void
check_connected(
	struct run *run,
	const char *log,
	int ems)
{
	const char *why;
	int different;

	/* Both sides finished, with the same keys, one change_cipher_spec from the client. */
	CHECK(run->error == 0);
	CHECK(run->peer_error == 0);
	if (run->error != 0 || run->peer_error != 0) {
		why = "";
		if (run->peer->why != NULL)
			why = run->peer->why;
		printf("  error %d alert %u peer %d (%s)\n", run->error, run->hs.alert, run->peer_error, why);
	}

	/* Checks that the run finished. */
	CHECK(run->done == 1);
	CHECK(sslc_hs_connected(&run->hs) == 1);
	CHECK(run->hs.state == SSLC_HS12_CONNECTED);
	CHECK(run->hs.version == SSLC_TLS12);
	CHECK(run->peer->state == TLSPEER_CONNECTED);
	CHECK(run->peer->error == 0);
	CHECK(run->peer->ccs_count == 1U);
	CHECK(run->key_mismatch == 0U);
	CHECK(run->write_changes == 1U);
	CHECK(strcmp(run->log, log) == 0);
	different = strcmp(run->log, log);
	if (different != 0)
		printf("  log %s, expected %s\n", run->log, log);
	CHECK(run->hs.extended_master_secret == ems);
	CHECK(run->peer->ems == ems);

	/* No secret is kept: the key pairs and the master secret are gone. */
	CHECK(all_zero(run->hs.x25519_private, sizeof(run->hs.x25519_private)));
	CHECK(all_zero(run->hs.ec_private, sizeof(run->hs.ec_private)));
	CHECK(all_zero(run->hs.master, sizeof(run->hs.master)));
}

/* Makes a TLS 1.2 peer config over pki. */
static void
peer12(
	struct tlspeer_config *config,
	const struct tlspeer_pki *pki)
{
	/* The tool's default script. */
	CHECK(tlspeer_config_init(config, SSLC_TLS12, pki) == 0);
}

/* Runs a full handshake against config with a client of min to max for host trusting pki; returns the run (to be freed). */
static struct run *
handshake(
	const struct tlspeer_config *config,
	const struct tlspeer_pki *pki,
	uint16_t max_version,
	const char *host)
{
	struct run *run;
	int error;

	/* A client and its peer, run to the end. */
	run = run_new(SSLC_TLS12, max_version, pki, host);
	error = run_peer(run, config);
	if (error != 0)
		return run;
	go(run);
	return run;
}

/* Runs a handshake that the client must refuse, and checks the failure. */
static void
check_peer_fails(
	const struct tlspeer_config *config,
	const struct tlspeer_pki *pki,
	uint16_t max_version,
	const char *host,
	int error,
	unsigned alert)
{
	struct run *run;

	/* The client fails as expected, and refuses anything after. */
	run = handshake(config, pki, max_version, host);
	expect_failed(run, error, alert);
	run_free(run);
}

/* Records the server's items of a handshake with config from a client of host (a 1.2-only client of seed 0). */
static void
record_replay(
	const struct tlspeer_config *config,
	const struct tlspeer_pki *pki,
	struct replay *replay)
{
	struct tlspeer_item item;
	struct run *run;
	int more;
	int error;

	/* A client whose actions go to the peer, and every item of the peer kept. */
	memset(replay, 0, sizeof(*replay));
	replay->pki = pki;
	run = run_new(SSLC_TLS12, SSLC_TLS12, pki, "server.test");
	error = run_peer(run, config);
	if (error != 0) {
		run_free(run);
		return;
	}

	/* Starts the handshake and applies its first actions. */
	run->error = sslc_hs_start(&run->hs, &run->config, &run->actions);
	apply(run);
	while (run->error == 0) {
		more = tlspeer_next(run->peer, &item);
		if (!more)
			break;
		if (replay->count >= REPLAY_MAX || item.size > REPLAY_SIZE) {
			CHECK(0);
			break;
		}

		/* Keeps the item for the later replay. */
		replay->kind[replay->count] = item.kind;
		memcpy(replay->data[replay->count], item.data, item.size);
		replay->size[replay->count] = item.size;
		replay->count++;
		if (item.kind == TLSPEER_ITEM_HANDSHAKE)
			error = sslc_hs_message(&run->hs, replay->data[replay->count - 1U], item.size, &run->actions);
		else
			error = sslc_hs_change_cipher_spec(&run->hs, &run->actions);
		run->error = error;
		if (error == 0)
			apply(run);
	}

	/* Checks that the handshake completed. */
	CHECK(run->error == 0);
	CHECK(run->done == 1);
	CHECK(run->peer->state == TLSPEER_CONNECTED);
	run_free(run);
}

/* Feeds item number item of a replay to a run. */
static void
feed_item(
	struct run *run,
	const struct replay *replay,
	size_t item)
{
	/* A handshake message, or a change_cipher_spec. */
	if (replay->kind[item] == TLSPEER_ITEM_HANDSHAKE) {
		run->error = sslc_hs_message(&run->hs, replay->data[item], replay->size[item], &run->actions);
	} else {
		run->error = sslc_hs_change_cipher_spec(&run->hs, &run->actions);
	}

	/* Applies the actions unless the handshake already failed. */
	if (run->error == 0)
		apply(run);
}

/* Starts a fresh 1.2-only client (no peer) and feeds it the first count items of a replay; returns the run. */
static struct run *
run_prefix(
	const struct replay *replay,
	size_t count)
{
	struct run *run;
	size_t item;

	/* The client, then the items while it takes them. */
	run = run_new(SSLC_TLS12, SSLC_TLS12, replay->pki, "server.test");
	go(run);
	for (item = 0; item < count && item < replay->count && run->error == 0; item++)
		feed_item(run, replay, item);
	return run;
}

/*
 * Replays a recorded server flight to a fresh client (no peer): items
 * before index as they are, item index replaced by message (size bytes),
 * then the rest; returns the run.
 */
static struct run *
replay_run(
	const struct replay *replay,
	size_t index,
	const uint8_t *message,
	size_t size)
{
	struct run *run;
	size_t item;

	/* The client, then the items while it takes them. */
	run = run_new(SSLC_TLS12, SSLC_TLS12, replay->pki, "server.test");
	go(run);
	for (item = 0; item < replay->count && run->error == 0; item++) {
		if (item == index) {
			run->error = sslc_hs_message(&run->hs, message, size, &run->actions);
			if (run->error == 0)
				apply(run);
		} else {
			feed_item(run, replay, item);
		}
	}

	/* Reports the finished run. */
	return run;
}

/* Writes a handshake message of type with a body of size bytes into out; returns its size. */
static size_t
message_make(
	unsigned type,
	const uint8_t *body,
	size_t size,
	uint8_t *out)
{
	/* The header: type and the 24-bit length. */
	out[0] = (uint8_t)type;
	cryc_store_be24(out + 1, (uint32_t)size);
	if (size > 0U)
		memcpy(out + 4, body, size);
	return 4U + size;
}

/* Fills a ServerHello of the plain kind: no session id, ECDHE_ECDSA_AES_128_GCM, an empty renegotiation_info. */
static void
sh_default(
	struct sh_spec *spec)
{
	size_t index;

	/* Every field that makes a well-formed TLS 1.2 ServerHello. */
	memset(spec, 0, sizeof(*spec));
	spec->legacy_version = 0x0303;
	for (index = 0; index < 32U; index++)
		spec->random[index] = (uint8_t)(0x40U + index);
	spec->suite = 0xc02b;
	spec->renegotiation = 1;
}

/* Writes a hand-made ServerHello message into out; returns its size. */
static size_t
sh_write(
	const struct sh_spec *spec,
	uint8_t *out,
	size_t room)
{
	static const uint8_t renegotiated[1] = { 0x01 };
	static uint8_t share_key[32];
	struct tlspeer_writer writer;
	size_t extensions_start;

	/* The fixed fields. */
	tlspeer_writer_init(&writer, out, room);
	tlspeer_put_int(&writer, 2, 1);
	tlspeer_begin(&writer, 3);
	tlspeer_put_int(&writer, spec->legacy_version, 2);
	tlspeer_put(&writer, spec->random, 32);
	tlspeer_begin(&writer, 1);
	tlspeer_put(&writer, spec->session_id, spec->session_id_size);
	tlspeer_end(&writer);
	tlspeer_put_int(&writer, spec->suite, 2);
	tlspeer_put_int(&writer, spec->compression, 1);

	/* The extensions asked for; the block is left out when it would be empty. */
	tlspeer_begin(&writer, 2);
	extensions_start = writer.used;
	if (spec->renegotiation == 1) {
		tlspeer_put_int(&writer, SSLC_EXT_RENEGOTIATION_INFO, 2);
		tlspeer_put_int(&writer, 1, 2);
		tlspeer_put_int(&writer, 0, 1);
	} else if (spec->renegotiation == 2) {
		tlspeer_put_int(&writer, SSLC_EXT_RENEGOTIATION_INFO, 2);
		tlspeer_put_int(&writer, 2, 2);
		tlspeer_put_int(&writer, 1, 1);
		tlspeer_put(&writer, renegotiated, 1);
	}

	/* Adds the extended master secret extension when the spec has it. */
	if (spec->extended_master)
		tlspeer_put_extension(&writer, SSLC_EXT_EXTENDED_MASTER_SECRET, NULL, 0);
	if (spec->point_formats) {
		tlspeer_put_int(&writer, SSLC_EXT_EC_POINT_FORMATS, 2);
		tlspeer_put_int(&writer, 2, 2);
		tlspeer_put_int(&writer, 1, 1);
		tlspeer_put_int(&writer, 0, 1);
	}

	/* Adds the server name extension when the spec has it. */
	if (spec->server_name)
		tlspeer_put_extension(&writer, SSLC_EXT_SERVER_NAME, NULL, 0);
	if (spec->share) {
		tlspeer_put_int(&writer, SSLC_EXT_KEY_SHARE, 2);
		tlspeer_begin(&writer, 2);
		tlspeer_put_int(&writer, SSLC_GROUP_X25519, 2);
		tlspeer_begin(&writer, 2);
		tlspeer_put(&writer, share_key, sizeof(share_key));
		tlspeer_end(&writer);
		tlspeer_end(&writer);
	}

	/* Adds the supported versions extension when the spec has a version. */
	if (spec->version != 0U) {
		tlspeer_put_int(&writer, SSLC_EXT_SUPPORTED_VERSIONS, 2);
		tlspeer_put_int(&writer, 2, 2);
		tlspeer_put_int(&writer, spec->version, 2);
	}

	/* Adds the cookie extension when the spec has one. */
	if (spec->cookie) {
		tlspeer_put_int(&writer, SSLC_EXT_COOKIE, 2);
		tlspeer_begin(&writer, 2);
		tlspeer_begin(&writer, 2);
		tlspeer_put_int(&writer, 0x55, 1);
		tlspeer_end(&writer);
		tlspeer_end(&writer);
	}

	/* Adds the extra extension when the spec has one. */
	if (spec->has_extra)
		tlspeer_put_extension(&writer, spec->extra_type, NULL, 0);
	tlspeer_end(&writer);
	if (writer.error == 0 && writer.used == extensions_start)
		writer.used -= 2U;
	tlspeer_end(&writer);
	CHECK(writer.error == 0);
	return writer.used;
}

/* Starts a client without a peer and feeds it a hand-made ServerHello; returns the run. */
static struct run *
feed_sh(
	uint16_t max_version,
	const char *host,
	const struct sh_spec *spec)
{
	static uint8_t message[4096];
	struct run *run;
	size_t size;
	int error;

	/* A client, its ClientHello sent nowhere, the message. */
	run = run_new(SSLC_TLS12, max_version, &pki256, host);
	error = go(run);
	if (error != 0)
		CHECK(0);
	size = sh_write(spec, message, sizeof(message));
	feed(run, message, size);
	return run;
}

/* Feeds a hand-made ServerHello and checks the failure. */
static void
check_sh(
	uint16_t max_version,
	const char *host,
	const struct sh_spec *spec,
	int error,
	unsigned alert)
{
	struct run *run;

	/* The failure and its refusal of more. */
	run = feed_sh(max_version, host, spec);
	expect_failed(run, error, alert);
	run_free(run);
}

/* Feeds a hand-made ServerHello and checks that the client takes it and waits for the Certificate. */
static void
check_sh_taken(
	uint16_t max_version,
	const char *host,
	const struct sh_spec *spec,
	int ems)
{
	struct run *run;

	/* Taken: no action, the version and suite named, the next state. */
	run = feed_sh(max_version, host, spec);
	CHECK(run->error == 0);
	CHECK(run->hs.state == SSLC_HS12_WAIT_CERTIFICATE);
	CHECK(run->actions.count == 0U);
	CHECK(run->hs.version == SSLC_TLS12);
	CHECK(run->hs.suite != NULL && run->hs.suite->id == spec->suite);
	CHECK(run->hs.extended_master_secret == ems);
	CHECK(memcmp(run->hs.server_random, spec->random, 32) == 0);
	run_free(run);
}

/* Replaces every occurrence of the size bytes from by to in data; returns how many. */
static unsigned
patch(
	uint8_t *data,
	size_t data_size,
	const uint8_t *from,
	const uint8_t *to,
	size_t size)
{
	size_t index;
	unsigned count;
	int different;

	/* A scan. */
	count = 0;
	for (index = 0; index + size <= data_size; index++) {
		different = memcmp(data + index, from, size);
		if (different == 0) {
			memcpy(data + index, to, size);
			count++;
		}
	}

	/* Reports how many were found. */
	return count;
}

/*
 * Checks every combination of the main parameters in a full handshake:
 * three suites, the three groups, both leaves, a client of TLS 1.2 alone and
 * of 1.2 and 1.3, with the optional parts of the server varied.
 */
static void test_handshakes(void);

/* Checks the random draws: the client random, the x25519 key, and the secp256r1 key when the server asks for it. */
static void test_draws(void);

/* Checks each break of the peer. */
static void test_breaks(void);

/* Checks hand-made ServerHellos against each check of the header. */
static void test_server_hello(void);

static void test_session_id(void);

/* Checks hand-made ServerKeyExchange messages against each check of the header. */
static void test_server_key_exchange(void);

/* Checks the suite, scheme and key combinations a ServerKeyExchange may use (the key is EC; RSA keys are for the interoperability tests). */
static void test_scheme_suite(void);

/* Checks the order of the messages: each type in each state, and the states' own messages. */
static void test_order(void);

/* Checks the CertificateRequest and the ServerHelloDone. */
static void test_request_and_done(void);

/* Checks the client's flight: its actions, its messages' bytes, and that no secret but the master secret waits. */
static void test_flight(void);

/* Checks the Finished messages: wrong lengths, and the length of the server's. */
static void test_finished(void);

/* Checks HelloRequest: ignored during the handshake (not in the transcript), answered after it. */
static void test_hello_request(void);

/* Checks the messages after the handshake: everything but HelloRequest is unexpected_message. */
static void test_post_handshake(void);

/* Checks the certificate verdicts, the callback and the leaf checks, as TLS 1.2 reaches them. */
static void test_certificates(void);

static void
test_handshakes(void)
{
	static const uint16_t suite_ids[3] = { 0xc02b, 0xc02c, 0xcca9 };
	static const uint16_t groups[3] = { SSLC_GROUP_X25519, SSLC_GROUP_SECP256R1, SSLC_GROUP_SECP384R1 };
	struct tlspeer_config config;
	const struct tlspeer_pki *pki;
	struct run *run;
	unsigned suite_index;
	unsigned group_index;
	unsigned curve_index;
	unsigned client_index;
	unsigned count;
	uint32_t options;
	uint16_t max_version;
	const char *log;
	unsigned expected_messages;
	unsigned expected_ec_public;
	unsigned expected_shared;
	int ems;

	/* Every combination, the options of the server chosen by the combination's number. */
	count = 0;
	for (suite_index = 0; suite_index < 3U; suite_index++) {
		for (group_index = 0; group_index < 3U; group_index++) {
			for (curve_index = 0; curve_index < 2U; curve_index++) {
				for (client_index = 0; client_index < 2U; client_index++) {
					pki = &pki384;
					if (curve_index == 0U)
						pki = &pki256;
					max_version = SSLC_TLS13;
					if (client_index == 0U)
						max_version = SSLC_TLS12;
					options = 0;
					if (count % 2U == 1U)
						options |= TLSPEER_OPT_SNI_ACK | TLSPEER_OPT_POINT_FORMATS;
					if (count % 3U == 0U)
						options |= TLSPEER_OPT_CERT_REQUEST;
					if (count % 5U == 0U)
						options |= TLSPEER_OPT_NO_EMS;
					ems = (options & TLSPEER_OPT_NO_EMS) == 0U;
					peer12(&config, pki);
					config.suite = suite_ids[suite_index];
					config.group = groups[group_index];
					config.options = options;
					run = run_new(SSLC_TLS12, max_version, pki, "server.test");
					run_peer(run, &config);
					go(run);
					log = "SSCWSRD";
					check_connected(run, log, ems);
					CHECK(run->hs.suite != NULL && run->hs.suite->id == suite_ids[suite_index]);
					CHECK(run->hs.verify_code == CRYC_VERIFY_OK);
					CHECK(run->hs.peer_count == 1U);
					expected_messages = 3U;
					if ((options & TLSPEER_OPT_CERT_REQUEST) != 0U)
						expected_messages = 4U;
					CHECK(run->messages_sent == expected_messages);
					CHECK(run->hs.has_request == ((options & TLSPEER_OPT_CERT_REQUEST) != 0U));
					CHECK(run->hs.server_group == groups[group_index]);
					CHECK(run->hs.has_ec == (groups[group_index] != SSLC_GROUP_X25519));
					if (groups[group_index] != SSLC_GROUP_X25519) {
						CHECK(run->hs.ec_group == groups[group_index]);
						expected_ec_public = 65U;
						expected_shared = 32U;
						if (groups[group_index] == SSLC_GROUP_SECP384R1) {
							expected_ec_public = 97U;
							expected_shared = 48U;
						}

						/* Checks the sizes the chosen group gives. */
						CHECK(run->hs.ec_public_size == expected_ec_public);
						CHECK(run->peer->shared_size == expected_shared);
					}

					/* Frees the run and counts the combination. */
					run_free(run);
					count++;
				}
			}
		}
	}

	/* Checks that every suite and group combination ran. */
	CHECK(count == 36U);

	/* The hash of an ECDSA scheme and the curve of the key are independent in TLS 1.2: SHA-256 with P-384, SHA-384 with P-256. */
	peer12(&config, &pki384);
	config.scheme = SSLC_SCHEME_ECDSA_SECP256R1_SHA256;
	run = handshake(&config, &pki384, SSLC_TLS12, "server.test");
	check_connected(run, "SSCWSRD", 1);
	run_free(run);
	peer12(&config, &pki256);
	config.scheme = SSLC_SCHEME_ECDSA_SECP384R1_SHA384;
	run = handshake(&config, &pki256, SSLC_TLS12, "server.test");
	check_connected(run, "SSCWSRD", 1);
	run_free(run);

	/* Without a server name (an address): no SNI sent, none acknowledged. */
	peer12(&config, &pki256);
	run = run_new(SSLC_TLS12, SSLC_TLS12, &pki256, NULL);
	run->config.verify_peer = 0;
	run_peer(run, &config);
	go(run);
	check_connected(run, "SSCWSRD", 1);
	run_free(run);

	/* A server that does not use extended master secret: the master secret is made over the randoms. */
	peer12(&config, &pki256);
	config.options = TLSPEER_OPT_NO_EMS;
	run = handshake(&config, &pki256, SSLC_TLS12, "server.test");
	check_connected(run, "SSCWSRD", 0);
	run_free(run);
}

/* Checks the random draws: the client random, the x25519 key, and the secp256r1 key when the server asks for it. */
static void
test_draws(void)
{
	struct tlspeer_config config;
	struct run *run;

	/* TLS 1.2 alone over x25519: two draws of 32 bytes (no session id). */
	peer12(&config, &pki256);
	run = handshake(&config, &pki256, SSLC_TLS12, "server.test");
	check_connected(run, "SSCWSRD", 1);
	CHECK(run->stream.calls == 2U);
	CHECK(run->stream.sizes[0] == 32U);
	CHECK(run->stream.sizes[1] == 32U);
	CHECK(run->hs.hello.session_id_size == 0U);
	run_free(run);

	/* With TLS 1.3 offered too: the session id is the second draw. */
	run = handshake(&config, &pki256, SSLC_TLS13, "server.test");
	check_connected(run, "SSCWSRD", 1);
	CHECK(run->stream.calls == 3U);
	CHECK(run->stream.sizes[2] == 32U);
	CHECK(run->hs.hello.session_id_size == 32U);
	run_free(run);

	/* A secp256r1 server: the key pair is made after the ServerKeyExchange, with draws of its own. */
	config.group = SSLC_GROUP_SECP256R1;
	run = handshake(&config, &pki256, SSLC_TLS12, "server.test");
	check_connected(run, "SSCWSRD", 1);
	CHECK(run->stream.calls > 2U);
	CHECK(run->hs.ec != NULL);
	CHECK(run->hs.has_ec == 1);
	CHECK(run->hs.ec_public[0] == 4U);
	run_free(run);

	/* The same random source gives the same client random, whoever the server is. */
	run = handshake(&config, &pki256, SSLC_TLS12, "server.test");
	CHECK(run->peer->client_random[0] == run->hs.hello.random[0]);
	CHECK(memcmp(run->peer->client_random, run->hs.hello.random, 32) == 0);
	run_free(run);

	/* A failing source while the secp256r1 key is made (the third draw) is an internal error. */
	run = run_new(SSLC_TLS12, SSLC_TLS12, &pki256, "server.test");
	run->stream.fail_at = 3;
	run_peer(run, &config);
	go(run);
	expect_failed(run, EIO, 0);
	CHECK(run->hs.state == SSLC_HS_FAILED);
	run_free(run);

	/* A server that asks for x25519 never makes the secp256r1 pair. */
	peer12(&config, &pki256);
	run = run_new(SSLC_TLS12, SSLC_TLS12, &pki256, "server.test");
	run->stream.fail_at = 3;
	run_peer(run, &config);
	go(run);
	check_connected(run, "SSCWSRD", 1);
	CHECK(run->hs.ec == NULL);
	CHECK(run->hs.has_ec == 0);
	run_free(run);
}

/* Checks each break of the peer. */
static void
test_breaks(void)
{
	struct tlspeer_config config;
	struct run *run;

	/* The server's Finished does not verify: decrypt_error, after the client's flight went out. */
	peer12(&config, &pki256);
	config.breaks = TLSPEER_BREAK_FINISHED;
	run = handshake(&config, &pki256, SSLC_TLS12, "server.test");
	expect_failed(run, EBADMSG, SSLC_ALERT_DECRYPT_ERROR);
	CHECK(strcmp(run->log, "SSCWSR") == 0);
	run_free(run);

	/* The ServerKeyExchange signature does not verify: decrypt_error, before anything is sent. */
	peer12(&config, &pki256);
	config.breaks = TLSPEER_BREAK_SIGNATURE;
	run = handshake(&config, &pki256, SSLC_TLS12, "server.test");
	expect_failed(run, EBADMSG, SSLC_ALERT_DECRYPT_ERROR);
	CHECK(strcmp(run->log, "S") == 0);
	run_free(run);
	peer12(&config, &pki384);
	config.breaks = TLSPEER_BREAK_SIGNATURE;
	check_peer_fails(&config, &pki384, SSLC_TLS12, "server.test", EBADMSG, SSLC_ALERT_DECRYPT_ERROR);

	/* A downgrade mark: refused by a client that offered TLS 1.3, accepted by one that did not. */
	peer12(&config, &pki256);
	config.breaks = TLSPEER_BREAK_DOWNGRADE12;
	check_peer_fails(&config, &pki256, SSLC_TLS13, "server.test", EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);
	run = handshake(&config, &pki256, SSLC_TLS12, "server.test");
	check_connected(run, "SSCWSRD", 1);
	run_free(run);
	config.breaks = TLSPEER_BREAK_DOWNGRADE11;
	check_peer_fails(&config, &pki256, SSLC_TLS13, "server.test", EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);
	run = handshake(&config, &pki256, SSLC_TLS12, "server.test");
	check_connected(run, "SSCWSRD", 1);
	run_free(run);

	/* The client's own session id echoed: resumption nobody asked for (with TLS 1.3 offered the id is 32 bytes; else it is empty and an echo is empty). */
	peer12(&config, &pki256);
	config.breaks = TLSPEER_BREAK_SESSION_ID;
	check_peer_fails(&config, &pki256, SSLC_TLS13, "server.test", EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);
	run = handshake(&config, &pki256, SSLC_TLS12, "server.test");
	check_connected(run, "SSCWSRD", 1);
	run_free(run);

	/* A zero share: x25519's u = 0 gives an all-zero secret, secp256r1's is not on the curve. */
	peer12(&config, &pki256);
	config.breaks = TLSPEER_BREAK_ZERO_SHARE;
	run = handshake(&config, &pki256, SSLC_TLS12, "server.test");
	expect_failed(run, EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);
	CHECK(strcmp(run->log, "S") == 0);
	run_free(run);
	config.group = SSLC_GROUP_SECP256R1;
	run = handshake(&config, &pki256, SSLC_TLS12, "server.test");
	expect_failed(run, EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);
	CHECK(strcmp(run->log, "S") == 0);
	run_free(run);

	/* No renegotiation_info: handshake_failure. */
	peer12(&config, &pki256);
	config.options = TLSPEER_OPT_NO_RI;
	check_peer_fails(&config, &pki256, SSLC_TLS12, "server.test", EPROTO, SSLC_ALERT_HANDSHAKE_FAILURE);

	/* An unoffered extension in the ServerHello: unsupported_extension (the parser's). */
	peer12(&config, &pki256);
	config.extra_type = SSLC_EXT_ALPN;
	config.extra_at = TLSPEER_AT_SERVER_HELLO;
	check_peer_fails(&config, &pki256, SSLC_TLS12, "server.test", EPROTO, SSLC_ALERT_UNSUPPORTED_EXTENSION);

	/* An ECDHE_RSA suite whose server signs with ECDSA: the scheme does not suit the suite. */
	peer12(&config, &pki256);
	config.suite = SSLC_SUITE_ECDHE_RSA_WITH_AES_128_GCM_SHA256;
	check_peer_fails(&config, &pki256, SSLC_TLS12, "server.test", EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);

	/* A TLS 1.3 suite in a TLS 1.2 ServerHello. */
	peer12(&config, &pki256);
	config.suite = SSLC_SUITE_AES_128_GCM_SHA256;
	check_peer_fails(&config, &pki256, SSLC_TLS13, "server.test", EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);
}

/* Checks hand-made ServerHellos against each check of the header. */
static void
test_server_hello(void)
{
	struct sh_spec spec;
	struct run *run;
	size_t index;

	/* The plain one is taken; with the usual extensions, each way. */
	sh_default(&spec);
	check_sh_taken(SSLC_TLS12, "server.test", &spec, 0);
	check_sh_taken(SSLC_TLS13, "server.test", &spec, 0);
	spec.extended_master = 1;
	spec.point_formats = 1;
	spec.server_name = 1;
	check_sh_taken(SSLC_TLS12, "server.test", &spec, 1);
	spec.suite = 0xcca8;
	check_sh_taken(SSLC_TLS12, "server.test", &spec, 1);
	spec.suite = 0xc030;
	check_sh_taken(SSLC_TLS13, "server.test", &spec, 1);

	/* renegotiation_info absent: handshake_failure; with a body: handshake_failure. */
	sh_default(&spec);
	spec.renegotiation = 0;
	check_sh(SSLC_TLS12, "server.test", &spec, EPROTO, SSLC_ALERT_HANDSHAKE_FAILURE);
	check_sh(SSLC_TLS13, "server.test", &spec, EPROTO, SSLC_ALERT_HANDSHAKE_FAILURE);
	spec.extended_master = 1;
	spec.point_formats = 1;
	check_sh(SSLC_TLS12, "server.test", &spec, EPROTO, SSLC_ALERT_HANDSHAKE_FAILURE);
	sh_default(&spec);
	spec.renegotiation = 2;
	check_sh(SSLC_TLS12, "server.test", &spec, EPROTO, SSLC_ALERT_HANDSHAKE_FAILURE);

	/* The TLS 1.3 extensions: key_share is illegal_parameter. */
	sh_default(&spec);
	spec.share = 1;
	check_sh(SSLC_TLS12, "server.test", &spec, EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);
	check_sh(SSLC_TLS13, "server.test", &spec, EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);

	/* supported_versions naming TLS 1.2 is no TLS 1.3 selection: illegal_parameter. */
	sh_default(&spec);
	spec.version = 0x0303;
	check_sh(SSLC_TLS13, "server.test", &spec, EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);

	/* The HelloRetryRequest random in a ServerHello that selects TLS 1.2, with and without a cookie. */
	sh_default(&spec);
	memcpy(spec.random, retry_random, 32);
	check_sh(SSLC_TLS12, "server.test", &spec, EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);
	check_sh(SSLC_TLS13, "server.test", &spec, EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);
	spec.cookie = 1;
	check_sh(SSLC_TLS12, "server.test", &spec, EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);

	/* A server_name acknowledgement when no name was sent: unsupported_extension. */
	sh_default(&spec);
	spec.server_name = 1;
	check_sh(SSLC_TLS12, NULL, &spec, EPROTO, SSLC_ALERT_UNSUPPORTED_EXTENSION);
	check_sh_taken(SSLC_TLS12, "server.test", &spec, 0);

	/* A cookie outside a HelloRetryRequest is recognized and not specified for a TLS 1.2 ServerHello: illegal_parameter. */
	sh_default(&spec);
	spec.cookie = 1;
	run = feed_sh(SSLC_TLS12, "server.test", &spec);
	CHECK(run->error == EPROTO);
	CHECK(run->hs.state == SSLC_HS_FAILED);
	CHECK(run->hs.alert == SSLC_ALERT_ILLEGAL_PARAMETER);
	run_free(run);

	/* signature_algorithms, which the client offers, has no place in a ServerHello: illegal_parameter (RFC 8446 4.2); ALPN, never offered: unsupported_extension. */
	sh_default(&spec);
	spec.has_extra = 1;
	spec.extra_type = SSLC_EXT_SIGNATURE_ALGORITHMS;
	check_sh(SSLC_TLS12, "server.test", &spec, EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);
	spec.extra_type = SSLC_EXT_ALPN;
	check_sh(SSLC_TLS12, "server.test", &spec, EPROTO, SSLC_ALERT_UNSUPPORTED_EXTENSION);

	/* The session id: a TLS 1.2 client has sent none, so a server's own id (any length to 32) is no echo. */
	sh_default(&spec);
	spec.session_id_size = 32;
	for (index = 0; index < 32U; index++)
		spec.session_id[index] = (uint8_t)(0x90U + index);
	check_sh_taken(SSLC_TLS12, "server.test", &spec, 0);
	spec.session_id_size = 1;
	check_sh_taken(SSLC_TLS12, "server.test", &spec, 0);
	spec.session_id_size = 0;
	check_sh_taken(SSLC_TLS12, "server.test", &spec, 0);
}

/*
 * The session id of a client that offered TLS 1.3 is 32 random bytes; a
 * TLS 1.2 ServerHello that echoes it (whole) is illegal_parameter, one
 * that differs anywhere or in length is a plain id of the server's.
 */
static void
test_session_id(void)
{
	static uint8_t message[4096];
	struct sh_spec spec;
	struct run *run;
	size_t size;
	size_t index;

	/* The echo of the whole id. */
	sh_default(&spec);
	run = run_new(SSLC_TLS12, SSLC_TLS13, &pki256, "server.test");
	go(run);
	memcpy(spec.session_id, run->hs.hello.session_id, 32);
	spec.session_id_size = 32;
	size = sh_write(&spec, message, sizeof(message));
	feed(run, message, size);
	expect_failed(run, EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);
	run_free(run);

	/* An id that differs in one byte, whichever, is the server's own. */
	for (index = 0; index < 32U; index += 31U) {
		run = run_new(SSLC_TLS12, SSLC_TLS13, &pki256, "server.test");
		go(run);
		memcpy(spec.session_id, run->hs.hello.session_id, 32);
		spec.session_id[index] ^= 0x10U;
		size = sh_write(&spec, message, sizeof(message));
		feed(run, message, size);
		CHECK(run->error == 0);
		CHECK(run->hs.state == SSLC_HS12_WAIT_CERTIFICATE);
		run_free(run);
	}

	/* A shorter id that is a prefix of ours, and an empty one, are the server's own too. */
	run = run_new(SSLC_TLS12, SSLC_TLS13, &pki256, "server.test");
	go(run);
	memcpy(spec.session_id, run->hs.hello.session_id, 32);
	spec.session_id_size = 16;
	size = sh_write(&spec, message, sizeof(message));
	feed(run, message, size);
	CHECK(run->error == 0);
	run_free(run);
	run = run_new(SSLC_TLS12, SSLC_TLS13, &pki256, "server.test");
	go(run);
	spec.session_id_size = 0;
	size = sh_write(&spec, message, sizeof(message));
	feed(run, message, size);
	CHECK(run->error == 0);
	CHECK(run->hs.state == SSLC_HS12_WAIT_CERTIFICATE);
	run_free(run);
}

/* Writes a ServerKeyExchange of the given fields into out; returns its size. */
static size_t
ske_write(
	uint8_t *out,
	size_t room,
	unsigned curve_type,
	uint16_t group,
	const uint8_t *point,
	size_t point_size,
	uint16_t scheme,
	const uint8_t *signature,
	size_t signature_size)
{
	struct tlspeer_writer writer;

	/* The ServerECDHParams, the scheme and the signature (RFC 8422 5.4). */
	tlspeer_writer_init(&writer, out, room);
	tlspeer_put_int(&writer, SSLC_HS_SERVER_KEY_EXCHANGE, 1);
	tlspeer_begin(&writer, 3);
	tlspeer_put_int(&writer, curve_type, 1);
	tlspeer_put_int(&writer, group, 2);
	tlspeer_begin(&writer, 1);
	tlspeer_put(&writer, point, point_size);
	tlspeer_end(&writer);
	tlspeer_put_int(&writer, scheme, 2);
	tlspeer_begin(&writer, 2);
	tlspeer_put(&writer, signature, signature_size);
	tlspeer_end(&writer);
	tlspeer_end(&writer);
	CHECK(writer.error == 0);
	return writer.used;
}

/* Feeds the replayed flight with the ServerKeyExchange (item 2) replaced; checks the failure. */
static void
check_ske(
	const struct replay *replay,
	const uint8_t *message,
	size_t size,
	int error,
	unsigned alert)
{
	struct run *run;

	/* The client fails at that message, with the alert, not having taken the rest. */
	run = replay_run(replay, 2, message, size);
	expect_failed(run, error, alert);
	run_free(run);
}

/* Checks hand-made ServerKeyExchange messages against each check of the header. */
static void
test_server_key_exchange(void)
{
	static uint8_t message[512];
	static uint8_t point32[32];
	static uint8_t point65[100];
	static uint8_t signature[16];
	size_t size;
	size_t index;
	size_t point_size;

	/* A point of 32 bytes (x25519) and room for one of up to 100 (secp256r1 is 65, secp384r1 97, uncompressed), neither a signed one. */
	CHECK(hex_decode(x25519_bob_public, point32, sizeof(point32)) == 32U);
	memset(point65, 0x11, sizeof(point65));
	point65[0] = 4;
	memset(signature, 0x30, sizeof(signature));

	/* A curve_type that is not named_curve: illegal_parameter (the parser's). */
	size = ske_write(message, sizeof(message), 1, 0x001d, point32, 32, 0x0403, bogus_signature, sizeof(bogus_signature));
	check_ske(&replay_plain, message, size, EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);

	/* A group that was not offered (secp521r1, or unknown): illegal_parameter. */
	size = ske_write(message, sizeof(message), 3, 0x0019, point65, 65, 0x0403, bogus_signature, sizeof(bogus_signature));
	check_ske(&replay_plain, message, size, EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);
	size = ske_write(message, sizeof(message), 3, 0x0000, point32, 32, 0x0403, bogus_signature, sizeof(bogus_signature));
	check_ske(&replay_plain, message, size, EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);

	/* A point of the wrong form for its group, with a signature that would not verify either: the form is refused first. */
	for (index = 0; index < 3U; index++) {
		point_size = 1U;
		if (index == 0U)
			point_size = 31U;
		else if (index == 1U)
			point_size = 33U;
		size = ske_write(message, sizeof(message), 3, 0x001d, point65, point_size, 0x0403, bogus_signature, sizeof(bogus_signature));
		check_ske(&replay_plain, message, size, EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);
	}

	/* Tries a group the suite does not offer, and a point of the wrong size. */
	size = ske_write(message, sizeof(message), 3, 0x0017, point32, 32, 0x0403, bogus_signature, sizeof(bogus_signature));
	check_ske(&replay_plain, message, size, EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);
	size = ske_write(message, sizeof(message), 3, 0x0017, point65, 64, 0x0403, bogus_signature, sizeof(bogus_signature));
	check_ske(&replay_plain, message, size, EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);
	size = ske_write(message, sizeof(message), 3, 0x0017, point65, 66, 0x0403, bogus_signature, sizeof(bogus_signature));
	check_ske(&replay_plain, message, size, EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);
	size = ske_write(message, sizeof(message), 3, 0x0017, point65, 97, 0x0403, bogus_signature, sizeof(bogus_signature));
	check_ske(&replay_plain, message, size, EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);
	size = ske_write(message, sizeof(message), 3, 0x0018, point65, 65, 0x0403, bogus_signature, sizeof(bogus_signature));
	check_ske(&replay_plain, message, size, EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);
	size = ske_write(message, sizeof(message), 3, 0x0018, point65, 96, 0x0403, bogus_signature, sizeof(bogus_signature));
	check_ske(&replay_plain, message, size, EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);
	size = ske_write(message, sizeof(message), 3, 0x0018, point65, 98, 0x0403, bogus_signature, sizeof(bogus_signature));
	check_ske(&replay_plain, message, size, EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);
	point65[0] = 2;
	size = ske_write(message, sizeof(message), 3, 0x0017, point65, 65, 0x0403, bogus_signature, sizeof(bogus_signature));
	check_ske(&replay_plain, message, size, EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);
	point65[0] = 0;
	size = ske_write(message, sizeof(message), 3, 0x0017, point65, 65, 0x0403, bogus_signature, sizeof(bogus_signature));
	check_ske(&replay_plain, message, size, EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);
	point65[0] = 4;

	/* A scheme that was not offered (ECDSA SHA-512, Ed25519, SHA-1 RSA, none): illegal_parameter. */
	size = ske_write(message, sizeof(message), 3, 0x001d, point32, 32, 0x0603, bogus_signature, sizeof(bogus_signature));
	check_ske(&replay_plain, message, size, EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);
	size = ske_write(message, sizeof(message), 3, 0x001d, point32, 32, 0x0807, bogus_signature, sizeof(bogus_signature));
	check_ske(&replay_plain, message, size, EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);
	size = ske_write(message, sizeof(message), 3, 0x001d, point32, 32, 0x0201, bogus_signature, sizeof(bogus_signature));
	check_ske(&replay_plain, message, size, EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);
	size = ske_write(message, sizeof(message), 3, 0x001d, point32, 32, 0x0000, bogus_signature, sizeof(bogus_signature));
	check_ske(&replay_plain, message, size, EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);

	/* An RSA scheme (each of the six) for an ECDSA suite and an EC leaf: illegal_parameter. */
	size = ske_write(message, sizeof(message), 3, 0x001d, point32, 32, 0x0804, bogus_signature, sizeof(bogus_signature));
	check_ske(&replay_plain, message, size, EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);
	size = ske_write(message, sizeof(message), 3, 0x001d, point32, 32, 0x0805, bogus_signature, sizeof(bogus_signature));
	check_ske(&replay_plain, message, size, EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);
	size = ske_write(message, sizeof(message), 3, 0x001d, point32, 32, 0x0806, bogus_signature, sizeof(bogus_signature));
	check_ske(&replay_plain, message, size, EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);
	size = ske_write(message, sizeof(message), 3, 0x001d, point32, 32, 0x0401, bogus_signature, sizeof(bogus_signature));
	check_ske(&replay_plain, message, size, EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);
	size = ske_write(message, sizeof(message), 3, 0x001d, point32, 32, 0x0501, bogus_signature, sizeof(bogus_signature));
	check_ske(&replay_plain, message, size, EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);
	size = ske_write(message, sizeof(message), 3, 0x001d, point32, 32, 0x0601, bogus_signature, sizeof(bogus_signature));
	check_ske(&replay_plain, message, size, EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);

	/* A well-formed signature that does not verify, and one that is no DER: decrypt_error. */
	size = ske_write(message, sizeof(message), 3, 0x001d, point32, 32, 0x0403, bogus_signature, sizeof(bogus_signature));
	check_ske(&replay_plain, message, size, EBADMSG, SSLC_ALERT_DECRYPT_ERROR);
	size = ske_write(message, sizeof(message), 3, 0x001d, point32, 32, 0x0503, bogus_signature, sizeof(bogus_signature));
	check_ske(&replay_plain, message, size, EBADMSG, SSLC_ALERT_DECRYPT_ERROR);
	size = ske_write(message, sizeof(message), 3, 0x001d, point32, 32, 0x0403, signature, sizeof(signature));
	check_ske(&replay_plain, message, size, EBADMSG, SSLC_ALERT_DECRYPT_ERROR);
	size = ske_write(message, sizeof(message), 3, 0x001d, point32, 32, 0x0403, signature, 0);
	check_ske(&replay_plain, message, size, EBADMSG, SSLC_ALERT_DECRYPT_ERROR);
	size = ske_write(message, sizeof(message), 3, 0x0017, point65, 65, 0x0403, bogus_signature, sizeof(bogus_signature));
	check_ske(&replay_plain, message, size, EBADMSG, SSLC_ALERT_DECRYPT_ERROR);

	/* A ServerKeyExchange that is cut or has bytes after its signature: decode_error. */
	size = ske_write(message, sizeof(message), 3, 0x001d, point32, 32, 0x0403, bogus_signature, sizeof(bogus_signature));
	message[size] = 0;
	cryc_store_be24(message + 1, (uint32_t)(size - 4U + 1U));
	check_ske(&replay_plain, message, size + 1U, EINVAL, SSLC_ALERT_DECODE_ERROR);
	cryc_store_be24(message + 1, (uint32_t)(size - 4U - 1U));
	check_ske(&replay_plain, message, size - 1U, EINVAL, SSLC_ALERT_DECODE_ERROR);
}

/* Checks the suite, scheme and key combinations a ServerKeyExchange may use (the key is EC; RSA keys are for the interoperability tests). */
static void
test_scheme_suite(void)
{
	static uint8_t message[512];
	static uint8_t sh_message[4096];
	static uint8_t point32[32];
	struct sh_spec spec;
	struct run *run;
	size_t sh_size;
	size_t size;

	/* An RSA suite's ServerHello, then the recorded Certificate (a P-256 leaf: not an RSA key). */
	CHECK(hex_decode(x25519_bob_public, point32, sizeof(point32)) == 32U);
	sh_default(&spec);
	spec.suite = 0xc02f;
	sh_size = sh_write(&spec, sh_message, sizeof(sh_message));

	/* An RSA scheme with an EC leaf for an RSA suite: the key is of the wrong type. */
	run = run_new(SSLC_TLS12, SSLC_TLS12, &pki256, "server.test");
	go(run);
	feed(run, sh_message, sh_size);
	CHECK(run->error == 0);
	feed_item(run, &replay_plain, 1);
	CHECK(run->error == 0);
	size = ske_write(message, sizeof(message), 3, 0x001d, point32, 32, 0x0804, bogus_signature, sizeof(bogus_signature));
	feed(run, message, size);
	expect_failed(run, EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);
	run_free(run);

	/* An ECDSA scheme with an EC leaf for an RSA suite: the scheme does not suit the suite. */
	run = run_new(SSLC_TLS12, SSLC_TLS12, &pki256, "server.test");
	go(run);
	feed(run, sh_message, sh_size);
	feed_item(run, &replay_plain, 1);
	CHECK(run->error == 0);
	size = ske_write(message, sizeof(message), 3, 0x001d, point32, 32, 0x0403, bogus_signature, sizeof(bogus_signature));
	feed(run, message, size);
	expect_failed(run, EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);
	run_free(run);

	/* The ECDSA suite with an ECDSA scheme gets as far as the signature. */
	sh_default(&spec);
	sh_size = sh_write(&spec, sh_message, sizeof(sh_message));
	run = run_new(SSLC_TLS12, SSLC_TLS12, &pki256, "server.test");
	go(run);
	feed(run, sh_message, sh_size);
	feed_item(run, &replay_plain, 1);
	CHECK(run->error == 0);
	size = ske_write(message, sizeof(message), 3, 0x001d, point32, 32, 0x0403, bogus_signature, sizeof(bogus_signature));
	feed(run, message, size);
	expect_failed(run, EBADMSG, SSLC_ALERT_DECRYPT_ERROR);
	run_free(run);
}

/* Checks the order of the messages: each type in each state, and the states' own messages. */
static void
test_order(void)
{
	static const unsigned types[16] = { 1, 2, 4, 5, 8, 11, 12, 13, 14, 15, 16, 20, 24, 25, 100, 254 };
	static const unsigned allowed_in[7][2] = {
		{ 0, 0 }, { 11, 0 }, { 12, 0 }, { 13, 14 }, { 0, 0 }, { 20, 0 }, { 0, 0 }
	};
	static const size_t states[6] = { 1, 2, 3, 4, 5, 6 };
	uint8_t message[4];
	struct run *run;
	size_t state_index;
	size_t type_index;
	size_t count;

	/* In every state after the ServerHello, every type that state does not take is unexpected_message. */
	for (state_index = 0; state_index < 6U; state_index++) {
		count = states[state_index];
		for (type_index = 0; type_index < 16U; type_index++) {
			if (types[type_index] == allowed_in[count][0] || types[type_index] == allowed_in[count][1])
				continue;
			run = run_prefix(&replay_plain, count);
			CHECK(run->error == 0);
			run->done = 0;
			message_make(types[type_index], NULL, 0, message);
			feed(run, message, 4);
			expect_failed(run, EPROTO, SSLC_ALERT_UNEXPECTED_MESSAGE);
			run_free(run);
		}
	}

	/* The states themselves. */
	run = run_prefix(&replay_plain, 1);
	CHECK(run->hs.state == SSLC_HS12_WAIT_CERTIFICATE);
	run_free(run);
	run = run_prefix(&replay_plain, 2);
	CHECK(run->hs.state == SSLC_HS12_WAIT_SERVER_KEY_EXCHANGE);
	run_free(run);
	run = run_prefix(&replay_plain, 3);
	CHECK(run->hs.state == SSLC_HS12_WAIT_SERVER_HELLO_DONE);
	run_free(run);
	run = run_prefix(&replay_plain, 4);
	CHECK(run->hs.state == SSLC_HS12_WAIT_CHANGE_CIPHER_SPEC);
	run_free(run);
	run = run_prefix(&replay_plain, 5);
	CHECK(run->hs.state == SSLC_HS12_WAIT_FINISHED);
	run_free(run);
	run = run_prefix(&replay_plain, 6);
	CHECK(run->hs.state == SSLC_HS12_CONNECTED);
	CHECK(run->done == 1);
	run_free(run);

	/* change_cipher_spec: out of place in every TLS 1.2 state but the one that waits for it; after the handshake too. */
	for (count = 1; count <= 6U; count++) {
		if (count == 4U)
			continue;
		run = run_prefix(&replay_plain, count);
		CHECK(run->error == 0);
		run->done = 0;
		run->error = sslc_hs_change_cipher_spec(&run->hs, &run->actions);
		expect_failed(run, EPROTO, SSLC_ALERT_UNEXPECTED_MESSAGE);
		run_free(run);
	}
}

/* Checks the CertificateRequest and the ServerHelloDone. */
static void
test_request_and_done(void)
{
	static const uint8_t request_body[] = { 0x01, 0x40, 0x00, 0x02, 0x04, 0x03, 0x00, 0x00 };
	static uint8_t message[64];
	static uint8_t done_body[1];
	struct run *run;
	size_t size;

	/* A CertificateRequest between the ServerKeyExchange and the ServerHelloDone is taken (no action, same state); a second one is not. */
	run = run_prefix(&replay_plain, 3);
	size = message_make(SSLC_HS_CERTIFICATE_REQUEST, request_body, sizeof(request_body), message);
	feed(run, message, size);
	CHECK(run->error == 0);
	CHECK(run->hs.has_request == 1);
	CHECK(run->hs.state == SSLC_HS12_WAIT_SERVER_HELLO_DONE);
	CHECK(run->actions.count == 0U);
	feed(run, message, size);
	expect_failed(run, EPROTO, SSLC_ALERT_UNEXPECTED_MESSAGE);
	run_free(run);

	/* A CertificateRequest that is not well formed (empty body, cut, nothing for the types): decode_error. */
	run = run_prefix(&replay_plain, 3);
	size = message_make(SSLC_HS_CERTIFICATE_REQUEST, NULL, 0, message);
	feed(run, message, size);
	expect_failed(run, EINVAL, SSLC_ALERT_DECODE_ERROR);
	run_free(run);
	run = run_prefix(&replay_plain, 3);
	size = message_make(SSLC_HS_CERTIFICATE_REQUEST, request_body, sizeof(request_body) - 1U, message);
	feed(run, message, size);
	expect_failed(run, EINVAL, SSLC_ALERT_DECODE_ERROR);
	run_free(run);

	/* The CertificateRequest takes part in the transcript: the handshake that answers it completes (the replay of the full flight). */
	run = run_prefix(&replay_full, replay_full.count);
	CHECK(run->error == 0);
	CHECK(run->done == 1);
	CHECK(run->hs.has_request == 1);
	run_free(run);

	/* A ServerHelloDone with a body: decode_error. */
	run = run_prefix(&replay_plain, 3);
	done_body[0] = 0;
	size = message_make(SSLC_HS_SERVER_HELLO_DONE, done_body, 1, message);
	feed(run, message, size);
	expect_failed(run, EINVAL, SSLC_ALERT_DECODE_ERROR);
	run_free(run);
}

/* Checks the client's flight: its actions, its messages' bytes, and that no secret but the master secret waits. */
static void
test_flight(void)
{
	static const uint8_t empty_certificate[7] = { 0x0b, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00 };
	struct run *run;
	const uint8_t *bytes;
	size_t index;

	/* After the ServerKeyExchange (before the ServerHelloDone): no secret of the flight yet. */
	run = run_prefix(&replay_plain, 3);
	CHECK(run->hs.state == SSLC_HS12_WAIT_SERVER_HELLO_DONE);
	CHECK(all_zero(run->hs.master, sizeof(run->hs.master)));
	CHECK(!all_zero(run->hs.x25519_private, sizeof(run->hs.x25519_private)));
	CHECK(run->hs.server_group == SSLC_GROUP_X25519);
	CHECK(run->hs.server_point_size == 32U);
	feed_item(run, &replay_plain, 3);
	CHECK(run->error == 0);

	/* The actions: the ClientKeyExchange, change_cipher_spec, the write keys, the Finished. */
	CHECK(run->actions.count == 4U);
	CHECK(run->actions.list[0].kind == SSLC_HS_SEND);
	CHECK(run->actions.list[1].kind == SSLC_HS_SEND_CCS);
	CHECK(run->actions.list[2].kind == SSLC_HS_SET_WRITE_KEYS);
	CHECK(run->actions.list[3].kind == SSLC_HS_SEND);
	CHECK(run->actions.list[0].first_hello == 0);
	CHECK(run->actions.list[0].offset == 0U);
	CHECK(run->actions.list[0].size == 4U + 1U + 32U);
	CHECK(run->actions.list[2].version13 == 0);
	CHECK(run->actions.list[2].keys.suite != NULL && run->actions.list[2].keys.suite->id == 0xc02b);
	CHECK(run->actions.list[3].offset == 4U + 1U + 32U);
	CHECK(run->actions.list[3].size == 4U + 12U);

	/* The ClientKeyExchange: type 16, length 33, a point of 32 bytes, our x25519 public key. */
	bytes = run->hs.out + run->actions.list[0].offset;
	CHECK(bytes[0] == 16U);
	CHECK(bytes[1] == 0U && bytes[2] == 0U && bytes[3] == 33U);
	CHECK(bytes[4] == 32U);
	CHECK_BYTES(bytes + 5, run->hs.x25519_public, 32);

	/* The Finished: type 20, length 12. */
	bytes = run->hs.out + run->actions.list[3].offset;
	CHECK(bytes[0] == 20U);
	CHECK(bytes[1] == 0U && bytes[2] == 0U && bytes[3] == 12U);

	/* The private keys are gone, the master secret kept for the server's Finished. */
	CHECK(all_zero(run->hs.x25519_private, sizeof(run->hs.x25519_private)));
	CHECK(all_zero(run->hs.ec_private, sizeof(run->hs.ec_private)));
	CHECK(!all_zero(run->hs.master, sizeof(run->hs.master)));
	CHECK(run->hs.state == SSLC_HS12_WAIT_CHANGE_CIPHER_SPEC);
	run_free(run);

	/* The server's change_cipher_spec: the read keys, and only then a state that takes the Finished. */
	run = run_prefix(&replay_plain, 4);
	run->error = sslc_hs_change_cipher_spec(&run->hs, &run->actions);
	CHECK(run->error == 0);
	CHECK(run->actions.count == 1U);
	CHECK(run->actions.list[0].kind == SSLC_HS_SET_READ_KEYS);
	CHECK(run->actions.list[0].version13 == 0);
	CHECK(run->actions.list[0].keys.suite != NULL && run->actions.list[0].keys.suite->id == 0xc02b);
	CHECK(run->hs.state == SSLC_HS12_WAIT_FINISHED);
	CHECK(!all_zero(run->hs.master, sizeof(run->hs.master)));
	run_free(run);

	/* With a CertificateRequest: an empty Certificate first, in the same send as the ClientKeyExchange; over secp256r1 the point is 65 bytes. */
	run = run_prefix(&replay_full, 4);
	CHECK(run->hs.state == SSLC_HS12_WAIT_SERVER_HELLO_DONE);
	CHECK(run->hs.server_group == SSLC_GROUP_SECP256R1);
	CHECK(run->hs.server_point_size == 65U);
	feed_item(run, &replay_full, 4);
	CHECK(run->error == 0);
	CHECK(run->actions.count == 4U);
	CHECK(run->actions.list[0].size == 7U + 4U + 1U + 65U);
	bytes = run->hs.out + run->actions.list[0].offset;
	for (index = 0; index < 7U; index++)
		CHECK(bytes[index] == empty_certificate[index]);
	CHECK(bytes[7] == 16U);
	CHECK(bytes[10] == 66U);
	CHECK(bytes[11] == 65U);
	CHECK(bytes[12] == 4U);
	CHECK_BYTES(bytes + 12, run->hs.ec_public, 65);
	CHECK(run->actions.list[3].offset == 7U + 4U + 1U + 65U);
	run_free(run);
}

/* Checks the Finished messages: wrong lengths, and the length of the server's. */
static void
test_finished(void)
{
	static uint8_t message[64];
	static uint8_t body[40];
	struct run *run;
	size_t length;
	size_t size;

	/* A Finished of 0, 11, 13 and 32 bytes (the TLS 1.3 length) is decode_error. */
	for (length = 0; length <= 32U; length++) {
		if (length != 0U && length != 11U && length != 13U && length != 32U)
			continue;
		run = run_prefix(&replay_plain, 5);
		memset(body, 0x5a, sizeof(body));
		size = message_make(SSLC_HS_FINISHED, body, length, message);
		feed(run, message, size);
		expect_failed(run, EINVAL, SSLC_ALERT_DECODE_ERROR);
		run_free(run);
	}

	/* A Finished of 12 bytes that is wrong, in any one byte or bit: decrypt_error. */
	for (length = 0; length < 12U; length++) {
		run = run_prefix(&replay_plain, 5);
		memcpy(message, replay_plain.data[5], replay_plain.size[5]);
		message[4U + length] ^= 0x80U;
		feed(run, message, replay_plain.size[5]);
		expect_failed(run, EBADMSG, SSLC_ALERT_DECRYPT_ERROR);
		run_free(run);
	}

	/* The right one completes the handshake: DONE, the master secret gone. */
	run = run_prefix(&replay_plain, 5);
	feed_item(run, &replay_plain, 5);
	CHECK(run->error == 0);
	CHECK(run->actions.count == 1U);
	CHECK(run->actions.list[0].kind == SSLC_HS_DONE);
	CHECK(run->hs.state == SSLC_HS12_CONNECTED);
	CHECK(sslc_hs_connected(&run->hs) == 1);
	CHECK(all_zero(run->hs.master, sizeof(run->hs.master)));
	run_free(run);
}

/* Checks HelloRequest: ignored during the handshake (not in the transcript), answered after it. */
static void
test_hello_request(void)
{
	static const uint8_t request[4] = { 0, 0, 0, 0 };
	static const uint8_t request_with_body[5] = { 0, 0, 0, 1, 0 };
	struct run *run;
	size_t position;
	size_t item;

	/* In every position of the server's flight, including right after the ClientHello: no action, and the handshake still completes. */
	for (position = 0; position < replay_plain.count; position++) {
		run = run_new(SSLC_TLS12, SSLC_TLS12, &pki256, "server.test");
		go(run);
		for (item = 0; item < replay_plain.count && run->error == 0; item++) {
			if (item == position) {
				run->actions.count = 7;
				feed(run, request, sizeof(request));
				CHECK(run->error == 0);
				CHECK(run->actions.count == 0U);
			}

			/* Feeds the item to the client. */
			feed_item(run, &replay_plain, item);
		}

		/* Checks that the handshake completed with the expected actions. */
		CHECK(run->error == 0);
		CHECK(run->done == 1);
		CHECK(strcmp(run->log, "SSCWSRD") == 0);
		run_free(run);
	}

	/* With a body, in a state after the ServerHello: decode_error. */
	run = run_prefix(&replay_plain, 2);
	feed(run, request_with_body, sizeof(request_with_body));
	expect_failed(run, EINVAL, SSLC_ALERT_DECODE_ERROR);
	run_free(run);
	run = run_prefix(&replay_plain, 4);
	feed(run, request_with_body, sizeof(request_with_body));
	expect_failed(run, EINVAL, SSLC_ALERT_DECODE_ERROR);
	run_free(run);

	/* After the handshake: a warning no_renegotiation (100), each time; the connection stays. */
	run = run_prefix(&replay_plain, 6);
	CHECK(run->done == 1);
	feed(run, request, sizeof(request));
	CHECK(run->error == 0);
	CHECK(run->actions.count == 1U);
	CHECK(run->actions.list[0].kind == SSLC_HS_SEND_WARNING);
	CHECK(run->actions.list[0].alert == SSLC_ALERT_NO_RENEGOTIATION);
	CHECK(run->actions.list[0].alert == 100U);
	CHECK(run->hs.state == SSLC_HS12_CONNECTED);
	feed(run, request, sizeof(request));
	CHECK(run->error == 0);
	CHECK(run->actions.count == 1U);
	CHECK(run->actions.list[0].alert == 100U);
	CHECK(strcmp(run->log, "SSCWSRDAA") == 0);
	CHECK(sslc_hs_connected(&run->hs) == 1);

	/* With a body after the handshake: decode_error. */
	run->done = 0;
	feed(run, request_with_body, sizeof(request_with_body));
	expect_failed(run, EINVAL, SSLC_ALERT_DECODE_ERROR);
	run_free(run);
}

/* Checks the messages after the handshake: everything but HelloRequest is unexpected_message. */
static void
test_post_handshake(void)
{
	static const uint8_t types[10] = { 1, 2, 4, 11, 12, 13, 14, 20, 24, 15 };
	static uint8_t message[4];
	struct run *run;
	size_t index;

	/* NewSessionTicket and KeyUpdate are TLS 1.3's: a TLS 1.2 client has no use for them. */
	for (index = 0; index < 10U; index++) {
		run = run_prefix(&replay_plain, 6);
		run->done = 0;
		message_make(types[index], NULL, 0, message);
		feed(run, message, 4);
		expect_failed(run, EPROTO, SSLC_ALERT_UNEXPECTED_MESSAGE);
		run_free(run);
	}
}

/* Checks the certificate verdicts, the callback and the leaf checks, as TLS 1.2 reaches them. */
static void
test_certificates(void)
{
	/*
	 * The DER of the OIDs ecdsa-with-SHA256 and ecdsa-with-SHA224, and of
	 * secp256r1 and prime239v3 (a named curve libcrypto-compat does not
	 * support), each of the same length.
	 * Values referenced: RFC 5758 section 3.2; RFC 5480 section 2.1.1.1;
	 * ANSI X9.62 (1.2.840.10045.3.1.6).
	 */
	static const uint8_t sha256_oid[10] = { 0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x02 };
	static const uint8_t sha224_oid[10] = { 0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x01 };
	static const uint8_t p256_oid[10] = { 0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03, 0x01, 0x07 };
	static const uint8_t other_oid[10] = { 0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03, 0x01, 0x06 };
	static uint8_t leaf[TLSPEER_CERT_MAX];
	uint8_t bad_public[65];
	struct tlspeer_config config;
	struct run *run;

	/* An empty store: unknown_ca when verifying, the verdict recorded either way. */
	peer12(&config, &pki256);
	check_peer_fails(&config, NULL, SSLC_TLS12, "server.test", EPROTO, SSLC_ALERT_UNKNOWN_CA);
	run = run_new(SSLC_TLS12, SSLC_TLS12, NULL, "server.test");
	run->config.verify_peer = 0;
	run->config.callback = verify_callback;
	run->config.callback_context = run;
	run_peer(run, &config);
	go(run);
	check_connected(run, "SSCWSRD", 1);
	CHECK(run->hs.verify_code == CRYC_VERIFY_UNABLE_TO_GET_ISSUER);
	CHECK(run->callback_calls == 0U);
	run_free(run);

	/* The callback lets an invalid chain through, or refuses it. */
	run = run_new(SSLC_TLS12, SSLC_TLS12, NULL, "server.test");
	run->config.callback = verify_callback;
	run->config.callback_context = run;
	run_peer(run, &config);
	go(run);
	check_connected(run, "SSCWSRD", 1);
	CHECK(run->callback_calls == 1U);
	CHECK(run->callback_preverified == 0);
	CHECK(run->callback_code == CRYC_VERIFY_UNABLE_TO_GET_ISSUER);
	run_free(run);
	run = run_new(SSLC_TLS12, SSLC_TLS12, NULL, "server.test");
	run->config.callback = verify_callback;
	run->config.callback_context = run;
	run->callback_answer = 0;
	run_peer(run, &config);
	go(run);
	expect_failed(run, EPROTO, SSLC_ALERT_UNKNOWN_CA);
	CHECK(run->callback_calls == 1U);
	run_free(run);

	/* A valid chain is offered to the callback, which may refuse it. */
	run = run_new(SSLC_TLS12, SSLC_TLS12, &pki256, "server.test");
	run->config.callback = verify_callback;
	run->config.callback_context = run;
	run_peer(run, &config);
	go(run);
	check_connected(run, "SSCWSRD", 1);
	CHECK(run->callback_calls == 1U);
	CHECK(run->callback_preverified == 1);
	CHECK(run->callback_code == CRYC_VERIFY_OK);
	run_free(run);
	run = run_new(SSLC_TLS12, SSLC_TLS12, &pki256, "server.test");
	run->config.callback = verify_callback;
	run->config.callback_context = run;
	run->callback_answer = 0;
	run_peer(run, &config);
	go(run);
	expect_failed(run, EPROTO, SSLC_ALERT_CERTIFICATE_UNKNOWN);
	run_free(run);

	/* Expired, not yet valid, another host. */
	run = run_new(SSLC_TLS12, SSLC_TLS12, &pki256, "server.test");
	run->config.params.time = TLSPEER_NOT_AFTER_TIME + 1;
	run_peer(run, &config);
	go(run);
	expect_failed(run, EPROTO, SSLC_ALERT_CERTIFICATE_EXPIRED);
	CHECK(run->hs.verify_code == CRYC_VERIFY_EXPIRED);
	run_free(run);
	run = run_new(SSLC_TLS12, SSLC_TLS12, &pki256, "server.test");
	CHECK(cryc_name_host_normalize("other.test", run->config.params.host) == 0);
	run_peer(run, &config);
	go(run);
	expect_failed(run, EPROTO, SSLC_ALERT_BAD_CERTIFICATE);
	CHECK(run->hs.verify_code == CRYC_VERIFY_HOSTNAME_MISMATCH);
	run_free(run);

	/* The root alone as the server's certificate. */
	peer12(&config, &pki256);
	config.chain[0] = pki256.root;
	config.chain_size[0] = pki256.root_size;
	run = handshake(&config, NULL, SSLC_TLS12, "server.test");
	expect_failed(run, EPROTO, SSLC_ALERT_UNKNOWN_CA);
	CHECK(run->hs.verify_code == CRYC_VERIFY_DEPTH_ZERO_SELF_SIGNED);
	run_free(run);

	/* The root sent after the leaf to a client that trusts it: tolerated, both certificates kept. */
	peer12(&config, &pki256);
	config.chain[1] = pki256.root;
	config.chain_size[1] = pki256.root_size;
	config.chain_count = 2;
	run = handshake(&config, &pki256, SSLC_TLS12, "server.test");
	check_connected(run, "SSCWSRD", 1);
	CHECK(run->hs.peer_count == 2U);
	run_free(run);

	/* A leaf signed with an algorithm libcrypto-compat does not support. */
	memcpy(leaf, pki256.leaf, pki256.leaf_size);
	CHECK(patch(leaf, pki256.leaf_size, sha256_oid, sha224_oid, sizeof(sha256_oid)) == 2U);
	peer12(&config, &pki256);
	config.chain[0] = leaf;
	config.chain_size[0] = pki256.leaf_size;
	run = handshake(&config, &pki256, SSLC_TLS12, "server.test");
	expect_failed(run, EPROTO, SSLC_ALERT_UNSUPPORTED_CERTIFICATE);
	run_free(run);

	/* A leaf of a curve libcrypto-compat does not support, without verification: unsupported_certificate. */
	memcpy(leaf, pki256.leaf, pki256.leaf_size);
	CHECK(patch(leaf, pki256.leaf_size, p256_oid, other_oid, sizeof(p256_oid)) == 1U);
	config.chain[0] = leaf;
	run = run_new(SSLC_TLS12, SSLC_TLS12, &pki256, "server.test");
	run->config.verify_peer = 0;
	run_peer(run, &config);
	go(run);
	expect_failed(run, ENOTSUP, SSLC_ALERT_UNSUPPORTED_CERTIFICATE);
	run_free(run);

	/* A leaf whose public key is not on its curve, without verification: the signature check refuses the malformed key (bad_certificate). */
	memcpy(leaf, pki256.leaf, pki256.leaf_size);
	memcpy(bad_public, pki256.leaf_key.public_key, sizeof(bad_public));
	bad_public[sizeof(bad_public) - 1U] ^= 1U;
	CHECK(patch(leaf, pki256.leaf_size, pki256.leaf_key.public_key, bad_public, sizeof(bad_public)) == 1U);
	peer12(&config, &pki256);
	config.chain[0] = leaf;
	config.chain_size[0] = pki256.leaf_size;
	run = run_new(SSLC_TLS12, SSLC_TLS12, &pki256, "server.test");
	run->config.verify_peer = 0;
	run_peer(run, &config);
	go(run);
	expect_failed(run, EINVAL, SSLC_ALERT_BAD_CERTIFICATE);
	CHECK(strcmp(run->log, "S") == 0);
	CHECK(run->hs.peer_count == 1U);
	run_free(run);

	/* A certificate that does not parse, and an empty certificate list. */
	peer12(&config, &pki256);
	config.chain_size[0] = pki256.leaf_size - 1U;
	check_peer_fails(&config, &pki256, SSLC_TLS12, "server.test", EINVAL, SSLC_ALERT_BAD_CERTIFICATE);
	peer12(&config, &pki256);
	config.chain_count = 0;
	check_peer_fails(&config, &pki256, SSLC_TLS12, "server.test", EINVAL, SSLC_ALERT_DECODE_ERROR);

	/* A second Certificate: out of place. */
	run = run_prefix(&replay_plain, 2);
	feed_item(run, &replay_plain, 1);
	expect_failed(run, EPROTO, SSLC_ALERT_UNEXPECTED_MESSAGE);
	run_free(run);
}

/* Checks that truncated and altered server messages never complete a handshake and never fault. */
static void
test_altered(
	const struct replay *replay,
	size_t step)
{
	static uint8_t message[REPLAY_SIZE];
	struct stream noise;
	struct run *run;
	size_t index;
	size_t cut;
	size_t position;
	unsigned trial;

	/* Each message, each length shorter than it, with the header's length made to fit. */
	for (index = 0; index < replay->count; index++) {
		if (replay->kind[index] != TLSPEER_ITEM_HANDSHAKE)
			continue;
		for (cut = 4; cut < replay->size[index]; cut += step) {
			memcpy(message, replay->data[index], cut);
			cryc_store_be24(message + 1, (uint32_t)(cut - 4U));
			run = replay_run(replay, index, message, cut);
			CHECK(run->error != 0);
			CHECK(run->hs.state == SSLC_HS_FAILED);
			CHECK(run->done == 0);
			run_free(run);
		}

		/* Cut without fixing the header: always decode_error (a ServerHelloDone has no body to cut). */
		if (replay->size[index] > 4U) {
			run = replay_run(replay, index, replay->data[index], replay->size[index] - 1U);
			CHECK(run->error == EINVAL);
			CHECK(run->hs.alert == SSLC_ALERT_DECODE_ERROR);
			run_free(run);
		}
	}

	/* Each byte after the header with one bit flipped: never a completed handshake. */
	for (index = 0; index < replay->count; index++) {
		if (replay->kind[index] != TLSPEER_ITEM_HANDSHAKE)
			continue;
		for (position = 4; position < replay->size[index]; position += step) {
			memcpy(message, replay->data[index], replay->size[index]);
			message[position] ^= (uint8_t)(1U << (position % 8U));
			run = replay_run(replay, index, message, replay->size[index]);
			CHECK(run->done == 0);
			CHECK(run->error != 0);
			CHECK(run->hs.state == SSLC_HS_FAILED);
			run_free(run);
		}
	}

	/* Random bodies of random types and lengths in every state. */
	memset(&noise, 0, sizeof(noise));
	noise.seed = 0x77;
	for (trial = 0; trial < 200U; trial++) {
		index = trial % replay->count;
		stream_random(&noise, message, 8);
		cut = 4U + (size_t)(message[1] * 4U + (message[2] % 4U));
		stream_random(&noise, message + 4, cut - 4U);
		message[0] = (uint8_t)(message[0] % 26U);
		cryc_store_be24(message + 1, (uint32_t)(cut - 4U));
		run = replay_run(replay, index, message, cut);
		CHECK(run->done == 0);
		CHECK(run->error != 0 || message[0] == 0U);
		run_free(run);
	}
}

/* Runs every check. */
int
main(void)
{
	struct tlspeer_config config;

	/* The certificates of both curves. */
	CHECK(tlspeer_pki_init(&pki256, CRYC_CURVE_P256, "server.test") == 0);
	CHECK(tlspeer_pki_init(&pki384, CRYC_CURVE_P384, "server.test") == 0);

	/* The recorded flights: a plain one, and one with a CertificateRequest over secp256r1 and the P-384 leaf. */
	peer12(&config, &pki256);
	record_replay(&config, &pki256, &replay_plain);
	CHECK(replay_plain.count == 6U);
	peer12(&config, &pki384);
	config.group = SSLC_GROUP_SECP256R1;
	config.options = TLSPEER_OPT_CERT_REQUEST | TLSPEER_OPT_SNI_ACK | TLSPEER_OPT_POINT_FORMATS;
	record_replay(&config, &pki384, &replay_full);
	CHECK(replay_full.count == 7U);

	/* The checks. */
	test_handshakes();
	test_draws();
	test_breaks();
	test_server_hello();
	test_session_id();
	test_server_key_exchange();
	test_scheme_suite();
	test_order();
	test_request_and_done();
	test_flight();
	test_finished();
	test_hello_request();
	test_post_handshake();
	test_certificates();
	test_altered(&replay_plain, 1);
	test_altered(&replay_full, 3);
	return check_finish("test-hs12");
}
