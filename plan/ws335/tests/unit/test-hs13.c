/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Checks the handshake state machine (sslc-hs.h): what both versions
 * share and the TLS 1.3 states.
 *
 * The server is tlspeer.h in its message mode: the client's actions are
 * carried out against it (its messages fed in, its change_cipher_spec
 * passed on, its key changes compared with the keys the peer computed on
 * its own), and the peer's messages are fed back.  Full handshakes over
 * x25519 and over secp256r1 after a HelloRetryRequest, with each suite,
 * P-256 and P-384 keys, cookies, a CertificateRequest, server_name and
 * change_cipher_spec; then every break of the peer, every check of the
 * header's file comment with hand-made messages, the certificate
 * verdicts, the messages after the handshake, the shared functions, the
 * ClientHello bytes from a fixed random source, and truncated and altered
 * server messages.
 *
 * The expected values come from RFC 8446 (the message syntax, the order
 * of the ClientHello's fields as sslc-msg.h promises them, the special
 * values), RFC 7748 6.1 and the NIST CAVP ECC CDH file (the shared
 * secrets), and the peer's own key schedule.
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
 * The HelloRetryRequest random and the TLS 1.2 downgrade marks, written
 * here again from the RFC so that the library's copies are checked.
 * Values referenced: RFC 8446 section 4.1.3.
 */
static const uint8_t retry_random[32] = {
	0xcf, 0x21, 0xad, 0x74, 0xe5, 0x9a, 0x61, 0x11, 0xbe, 0x1d, 0x8c, 0x02, 0x1e, 0x65, 0xb8, 0x91,
	0xc2, 0xa2, 0x11, 0x16, 0x7a, 0xbb, 0x8c, 0x5e, 0x07, 0x9e, 0x09, 0xe2, 0xc8, 0xa8, 0x33, 0x9c
};
static const uint8_t mark12[8] = { 0x44, 0x4f, 0x57, 0x4e, 0x47, 0x52, 0x44, 0x01 };
static const uint8_t mark11[8] = { 0x44, 0x4f, 0x57, 0x4e, 0x47, 0x52, 0x44, 0x00 };

/*
 * The x25519 test vector: Alice's private key a, Bob's public key, and
 * their shared secret K.
 * Values referenced: RFC 7748 section 6.1.
 */
static const char x25519_alice[] = "77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a";
static const char x25519_bob_public[] = "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f";
static const char x25519_shared[] = "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742";

/*
 * One secp256r1 case: QCAVSx, QCAVSy (the peer's point), dIUT (the
 * private key), QIUTx, QIUTy (its public key), ZIUT (the shared x).
 * Values referenced: NIST CAVP KAS_ECC_CDH_PrimitiveTest.txt (CAVS 14.1,
 * ecccdhtestvectors.zip), section [P-256], COUNT = 0.
 */
static const char p256_peer_x[] = "700c48f77f56584c5cc632ca65640db91b6bacce3a4df6b42ce7cc838833d287";
static const char p256_peer_y[] = "db71e509e3fd9b060ddb20ba5c51dcc5948d46fbf640dfe0441782cab85fa4ac";
static const char p256_private[] = "7d7dc5f71eb29ddaf80d6214632eeae03d9058af1fb6d22ed80badb62bc1a534";
static const char p256_public_x[] = "ead218590119e8876b29146ff89ca61770c4edbbf97d38ce385ed281d8a6b230";
static const char p256_public_y[] = "28af61281fd35e2fa7002523acc85a429cb06ee6648325389f59edfce1405141";
static const char p256_shared[] = "46fc62106420ff012e54a434fbdd2d25ccc5852060561e68040dd7778997bd7b";

/*
 * One secp384r1 case, the same fields.
 * Values referenced: NIST CAVP KAS_ECC_CDH_PrimitiveTest.txt (CAVS 14.1,
 * ecccdhtestvectors.zip), section [P-384], COUNT = 0.
 */
static const char p384_peer_x[] = "a7c76b970c3b5fe8b05d2838ae04ab47697b9eaf52e764592efda27fe7513272734466b400091adbf2d68c58e0c50066";
static const char p384_peer_y[] = "ac68f19f2e1cb879aed43a9969b91a0839c4c38a49749b661efedf243451915ed0905a32b060992b468c64766fc8437a";
static const char p384_private[] = "3cc3122a68f0d95027ad38c067916ba0eb8c38894d22e1b15618b6818a661774ad463b205da88cf699ab4d43c9cf98a1";
static const char p384_public_x[] = "9803807f2f6d2fd966cdd0290bd410c0190352fbec7ff6247de1302df86f25d34fe4a97bef60cff548355c015dbb3e5f";
static const char p384_public_y[] = "ba26ca69ec2f5b5d9dad20cc9da711383a9dbe34ea3fa5a2af75b46502629ad54dd8b7d73a8abb06a3a3be47d650cc99";
static const char p384_shared[] = "5f9d29dc5e31a163060356213669c8ce132e22f57c9a04f40ba7fcead493b457e5621e766c40a2e3d4d6a04b25e533f1";

/*
 * The code points the ClientHello is expected to carry, in its order.
 * Values referenced: RFC 8446 sections 4.2 and 4.2.7, Appendix B.4; RFC
 * 5289 section 3; RFC 7905 section 2; RFC 5746 section 3.3; RFC 8422
 * section 5.1.2; RFC 7627 section 5.1.
 */
static const uint16_t suites13[3] = { 0x1301, 0x1303, 0x1302 };
static const uint16_t suites12[6] = { 0xc02b, 0xcca9, 0xc02c, 0xc02f, 0xcca8, 0xc030 };
static const uint16_t schemes[8] = { 0x0403, 0x0503, 0x0804, 0x0805, 0x0806, 0x0401, 0x0501, 0x0601 };

/*
 * The test's random source: byte i of the stream is byte i mod 32 of
 * SHA-256("hs13 stream" || seed || be32(i / 32)); fail_at names the call
 * (1 first) that fails; sizes records each call's size and log the first
 * bytes drawn.
 */
struct stream {
	uint8_t seed;
	unsigned long position;
	unsigned calls;
	unsigned fail_at;
	size_t sizes[64];
	uint8_t log[1024];
};

/*
 * One client handshake and its peer: the handshake, its configuration,
 * store and random source; the last error; what the client's actions did
 * (a letter per action in log: S send, C change_cipher_spec, R read keys,
 * W write keys, A warning, D done); the keys it installed, compared with
 * the peer's; the ClientHellos it sent; the verification callback's
 * record.
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
	uint8_t hellos[2][SSLC_HS_OUT_MAX];
	size_t hello_size[2];
	unsigned hello_count;
	unsigned callback_calls;
	int callback_preverified;
	enum cryc_verify_code callback_code;
	int callback_answer;
};

/* What a server sent in one handshake, kept to be fed again (altered) to a client of the same random source. */
struct replay {
	unsigned kind[REPLAY_MAX];
	uint8_t data[REPLAY_MAX][REPLAY_SIZE];
	size_t size[REPLAY_MAX];
	size_t count;
};

/* A ServerHello or HelloRetryRequest made by hand: each field, and which extensions go in. */
struct sh_spec {
	uint16_t legacy_version;
	uint8_t random[32];
	uint8_t session_id[32];
	size_t session_id_size;
	uint16_t suite;
	unsigned compression;
	uint16_t version;
	int has_share;
	uint16_t group;
	uint8_t key[TLSPEER_SHARE_KEY_MAX];
	size_t key_size;
	int group_only;
	const uint8_t *cookie;
	size_t cookie_size;
	int has_extra;
	uint16_t extra_type;
	uint8_t extra[8];
	size_t extra_size;
};

/* The root and leaf of P-256 and of P-384, for host "server.test". */
static struct tlspeer_pki pki256;
static struct tlspeer_pki pki384;

/* The replay of the plain 1-RTT handshake, and of the one after a HelloRetryRequest. */
static struct replay replay_plain;
static struct replay replay_retry;

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

/* Feeds the peer's messages to the client (checking the client reads each under the peer's keys) until none wait or the client fails. */
static void pump(struct run *run);

/* Makes the run's peer from config. */
static int run_peer(struct run *run, const struct tlspeer_config *config);

/* Starts the client and runs the exchange until it ends; returns the client's error. */
static int go(struct run *run);

/* Returns 1 when size bytes at bytes are all zero. */
static int all_zero(const uint8_t *bytes, size_t size);

/* Checks that a run completed its handshake, and clears done so that a later failure can be checked. */
static void connected_then(struct run *run);

/* Checks that a run ended in a failure with error and alert, and that the failed handshake refuses more. */
static void expect_failed(struct run *run, int error, unsigned alert);

/* Checks that a run completed a TLS 1.3 handshake with the peer. */
static void check_connected(struct run *run, const char *log);

/* Writes the ClientHello the header promises, from its fields, into out; returns its size. */
static size_t expected_hello(uint8_t *out, size_t room, const uint8_t *random, const uint8_t *session_id, size_t session_id_size, int offer12, int offer13, const char *host, uint16_t share_group, const uint8_t *share, size_t share_size, const uint8_t *cookie, size_t cookie_size);

/* Fills a hand-made TLS 1.3 ServerHello for run: echoes its session id, AES-128-GCM, x25519 (RFC 7748's Bob). */
static void sh_default(struct sh_spec *spec, const struct run *run);

/* Makes it a HelloRetryRequest naming group (0: none). */
static void sh_retry(struct sh_spec *spec, uint16_t group);

/* Writes a hand-made ServerHello message into out; returns its size. */
static size_t sh_write(const struct sh_spec *spec, uint8_t *out, size_t room);

/* Starts a client without a peer and feeds it a hand-made ServerHello; returns the run. */
static struct run * feed_sh(uint16_t min_version, uint16_t max_version, const struct sh_spec *spec);

/* Feeds a hand-made ServerHello made from the default with change applied, and checks the failure. */
static void check_sh(uint16_t min_version, uint16_t max_version, const struct sh_spec *spec, int error, unsigned alert);

/* Makes a TLS 1.3 peer config over pki with the given changes. */
static void peer13(struct tlspeer_config *config, const struct tlspeer_pki *pki);

/* Runs a full handshake against config with a 1.2-1.3 client of host trusting pki; returns the run (to be freed). */
static struct run * handshake(const struct tlspeer_config *config, const struct tlspeer_pki *pki, uint16_t min_version, const char *host);

/* Records the server's messages of a run's peer from a fresh handshake of the same random source. */
static void record_replay(const struct tlspeer_config *config, struct replay *replay);

static struct run * replay_run(const struct replay *replay, size_t index, const uint8_t *message, size_t size);

/* Checks a handshake against a peer config that must fail with error and alert. */
static void check_peer_fails(const struct tlspeer_config *config, const struct tlspeer_pki *pki, uint16_t min_version, const char *host, int error, unsigned alert);

/* Replaces every occurrence of the size bytes from by to in data; returns how many. */
static unsigned patch(uint8_t *data, size_t data_size, const uint8_t *from, const uint8_t *to, size_t size);

static void test_altered(const struct replay *replay, unsigned step);

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
	memcpy(input, "hs13 stream", 11);
	input[11] = stream->seed;
	for (index = 0; index < size; index++) {
		position = stream->position + index;
		cryc_store_be32(input + 12, (uint32_t)(position / 32U));
		cryc_hash_digest(CRYC_HASH_SHA256, input, 16, block);
		out[index] = block[position % 32U];
		if (position < sizeof(stream->log))
			stream->log[position] = out[index];
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

				/* Counts the message and keeps the first two ClientHellos. */
				run->messages_sent++;
				if (bytes[0] == SSLC_HS_CLIENT_HELLO && run->hello_count < 2U) {
					memcpy(run->hellos[run->hello_count], bytes, length);
					run->hello_size[run->hello_count] = length;
					run->hello_count++;
				}

				/* Hands the message to the peer script. */
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
			CHECK(action->version13 == 1);
			run->read_keys = action->keys;
			run->read_keyed = 1;
			break;
		case SSLC_HS_SET_WRITE_KEYS:
			log_action(run, 'W');
			CHECK(action->version13 == 1);

			/* The first change is to the handshake keys, every later one to the application keys. */
			if (run->peer != NULL) {
				if (run->write_changes == 0U) {
					equal = keys_equal(&action->keys, &run->peer->client_handshake_keys);
					if (!equal)
						run->key_mismatch++;
				}

				/* Every later change is to the application keys. */
				if (run->write_changes > 0U) {
					equal = keys_equal(&action->keys, &run->peer->client_application_keys);
					if (!equal)
						run->key_mismatch++;
				}
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

/* Feeds the peer's messages to the client (checking the client reads each under the peer's keys) until none wait or the client fails. */
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

/* Checks that a run completed its handshake, and clears done so that a later failure can be checked. */
static void
connected_then(
	struct run *run)
{
	/* Done once; what follows is after the handshake. */
	CHECK(run->error == 0);
	CHECK(run->done == 1);
	CHECK(sslc_hs_connected(&run->hs) == 1);
	run->done = 0;
}

/* Checks that a run ended in a failure with error and alert, and that the failed handshake refuses more. */
static void
expect_failed(
	struct run *run,
	int error,
	unsigned alert)
{
	struct sslc_hs_actions actions;
	static const uint8_t finished[4 + 32] = { 0x14, 0x00, 0x00, 0x20 };

	/* The error and the alert, a failed state, no secret left. */
	CHECK(run->error == error);
	CHECK(run->hs.alert == alert);
	if (run->error != error || run->hs.alert != alert)
		printf("  got error %d alert %u, expected %d %u\n", run->error, run->hs.alert, error, alert);
	CHECK(run->hs.state == SSLC_HS_FAILED);
	CHECK(run->done == 0);
	CHECK(sslc_hs_connected(&run->hs) == 0);
	CHECK(all_zero(run->hs.x25519_private, sizeof(run->hs.x25519_private)));
	CHECK(all_zero(run->hs.schedule.secret, sizeof(run->hs.schedule.secret)));
	CHECK(all_zero(run->hs.schedule.client_application, sizeof(run->hs.schedule.client_application)));

	/* Later calls are refused, and the alert stays. */
	actions.count = 3;
	CHECK(sslc_hs_message(&run->hs, finished, sizeof(finished), &actions) == EPROTO);
	CHECK(actions.count == 0U);
	CHECK(sslc_hs_change_cipher_spec(&run->hs, &actions) == EPROTO);
	CHECK(run->hs.alert == alert);
}

/* Checks that a run completed a TLS 1.3 handshake with the peer. */
static void
check_connected(
	struct run *run,
	const char *log)
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
	CHECK(run->hs.state == SSLC_HS13_CONNECTED);
	CHECK(run->hs.version == SSLC_TLS13);
	CHECK(run->peer->state == TLSPEER_CONNECTED);
	CHECK(run->peer->error == 0);
	CHECK(run->peer->ccs_count == 1U);
	CHECK(run->key_mismatch == 0U);
	CHECK(strcmp(run->log, log) == 0);
	different = strcmp(run->log, log);
	if (different != 0)
		printf("  log %s, expected %s\n", run->log, log);

	/* The handshake's secrets are gone; the application secrets stay for KeyUpdate. */
	CHECK(all_zero(run->hs.x25519_private, sizeof(run->hs.x25519_private)));
	CHECK(all_zero(run->hs.ec_private, sizeof(run->hs.ec_private)));
	CHECK(all_zero(run->hs.schedule.secret, sizeof(run->hs.schedule.secret)));
	CHECK(all_zero(run->hs.schedule.client_handshake, sizeof(run->hs.schedule.client_handshake)));
	CHECK(all_zero(run->hs.schedule.server_handshake, sizeof(run->hs.schedule.server_handshake)));
	CHECK(!all_zero(run->hs.schedule.client_application, run->hs.schedule.size));
	CHECK(!all_zero(run->hs.schedule.server_application, run->hs.schedule.size));
}

/* Writes the ClientHello the header promises, from its fields, into out; returns its size. */
static size_t
expected_hello(
	uint8_t *out,
	size_t room,
	const uint8_t *random,
	const uint8_t *session_id,
	size_t session_id_size,
	int offer12,
	int offer13,
	const char *host,
	uint16_t share_group,
	const uint8_t *share,
	size_t share_size,
	const uint8_t *cookie,
	size_t cookie_size)
{
	struct tlspeer_writer writer;
	size_t index;

	/* Header, legacy_version, random, session id. */
	tlspeer_writer_init(&writer, out, room);
	tlspeer_put_int(&writer, 1, 1);
	tlspeer_begin(&writer, 3);
	tlspeer_put_int(&writer, 0x0303, 2);
	tlspeer_put(&writer, random, 32);
	tlspeer_begin(&writer, 1);
	tlspeer_put(&writer, session_id, session_id_size);
	tlspeer_end(&writer);

	/* The suites, then the renegotiation signalling value when TLS 1.2 is offered. */
	tlspeer_begin(&writer, 2);
	if (offer13) {
		for (index = 0; index < 3U; index++)
			tlspeer_put_int(&writer, suites13[index], 2);
	}

	/* Offers the TLS 1.2 suites and the signalling suite value. */
	if (offer12) {
		for (index = 0; index < 6U; index++)
			tlspeer_put_int(&writer, suites12[index], 2);
		tlspeer_put_int(&writer, 0x00ff, 2);
	}

	/* Ends the cipher_suites vector. */
	tlspeer_end(&writer);

	/* compression_methods: null. */
	tlspeer_put_int(&writer, 1, 1);
	tlspeer_put_int(&writer, 0, 1);

	/* The extensions, in the order sslc-msg.h gives. */
	tlspeer_begin(&writer, 2);
	if (host != NULL) {
		tlspeer_put_int(&writer, 0x0000, 2);
		tlspeer_begin(&writer, 2);
		tlspeer_begin(&writer, 2);
		tlspeer_put_int(&writer, 0, 1);
		tlspeer_begin(&writer, 2);
		tlspeer_put(&writer, (const uint8_t *)host, strlen(host));
		tlspeer_end(&writer);
		tlspeer_end(&writer);
		tlspeer_end(&writer);
	}

	/* Adds supported_groups. */
	tlspeer_put_int(&writer, 0x000a, 2);
	tlspeer_put_int(&writer, 8, 2);
	tlspeer_put_int(&writer, 6, 2);
	tlspeer_put_int(&writer, 0x001d, 2);
	tlspeer_put_int(&writer, 0x0017, 2);
	tlspeer_put_int(&writer, 0x0018, 2);
	if (offer12) {
		tlspeer_put_int(&writer, 0x000b, 2);
		tlspeer_put_int(&writer, 2, 2);
		tlspeer_put_int(&writer, 1, 1);
		tlspeer_put_int(&writer, 0, 1);
		tlspeer_put_int(&writer, 0x0017, 2);
		tlspeer_put_int(&writer, 0, 2);
	}

	/* Adds signature_algorithms. */
	tlspeer_put_int(&writer, 0x000d, 2);
	tlspeer_put_int(&writer, 18, 2);
	tlspeer_put_int(&writer, 16, 2);
	for (index = 0; index < 8U; index++)
		tlspeer_put_int(&writer, schemes[index], 2);
	if (offer13) {
		tlspeer_put_int(&writer, 0x002b, 2);
		tlspeer_begin(&writer, 2);
		tlspeer_begin(&writer, 1);
		tlspeer_put_int(&writer, 0x0304, 2);
		if (offer12)
			tlspeer_put_int(&writer, 0x0303, 2);
		tlspeer_end(&writer);
		tlspeer_end(&writer);
	}

	/* Adds the cookie extension when a cookie is to be echoed. */
	if (cookie_size > 0U) {
		tlspeer_put_int(&writer, 0x002c, 2);
		tlspeer_begin(&writer, 2);
		tlspeer_begin(&writer, 2);
		tlspeer_put(&writer, cookie, cookie_size);
		tlspeer_end(&writer);
		tlspeer_end(&writer);
	}

	/* Adds the key share when TLS 1.3 is offered. */
	if (offer13) {
		tlspeer_put_int(&writer, 0x0033, 2);
		tlspeer_begin(&writer, 2);
		tlspeer_begin(&writer, 2);
		tlspeer_put_int(&writer, share_group, 2);
		tlspeer_begin(&writer, 2);
		tlspeer_put(&writer, share, share_size);
		tlspeer_end(&writer);
		tlspeer_end(&writer);
		tlspeer_end(&writer);
	}

	/* Ends the extensions and the message. */
	tlspeer_end(&writer);
	tlspeer_end(&writer);
	CHECK(writer.error == 0);
	return writer.used;
}

/* Fills a hand-made TLS 1.3 ServerHello for run: echoes its session id, AES-128-GCM, x25519 (RFC 7748's Bob). */
static void
sh_default(
	struct sh_spec *spec,
	const struct run *run)
{
	size_t index;

	/* Every field that makes a well-formed ServerHello. */
	memset(spec, 0, sizeof(*spec));
	spec->legacy_version = 0x0303;
	for (index = 0; index < 32U; index++)
		spec->random[index] = (uint8_t)(0x40U + index);
	memcpy(spec->session_id, run->hs.hello.session_id, run->hs.hello.session_id_size);
	spec->session_id_size = run->hs.hello.session_id_size;
	spec->suite = 0x1301;
	spec->version = 0x0304;
	spec->has_share = 1;
	spec->group = 0x001d;
	spec->key_size = hex_decode(x25519_bob_public, spec->key, sizeof(spec->key));
}

/* Makes it a HelloRetryRequest naming group (0: none). */
static void
sh_retry(
	struct sh_spec *spec,
	uint16_t group)
{
	/* The special random, and the group alone in key_share. */
	memcpy(spec->random, retry_random, 32);
	spec->has_share = group != 0U;
	spec->group = group;
	spec->group_only = 1;
}

/* Writes a hand-made ServerHello message into out; returns its size. */
static size_t
sh_write(
	const struct sh_spec *spec,
	uint8_t *out,
	size_t room)
{
	struct tlspeer_writer writer;

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

	/* The extensions asked for. */
	tlspeer_begin(&writer, 2);
	if (spec->version != 0U) {
		tlspeer_put_int(&writer, 0x002b, 2);
		tlspeer_put_int(&writer, 2, 2);
		tlspeer_put_int(&writer, spec->version, 2);
	}

	/* Adds the key share when the spec has one. */
	if (spec->has_share) {
		tlspeer_put_int(&writer, 0x0033, 2);
		tlspeer_begin(&writer, 2);
		tlspeer_put_int(&writer, spec->group, 2);
		if (!spec->group_only) {
			tlspeer_begin(&writer, 2);
			tlspeer_put(&writer, spec->key, spec->key_size);
			tlspeer_end(&writer);
		}

		/* Ends the key_share extension. */
		tlspeer_end(&writer);
	}

	/* Adds the cookie when the spec has one. */
	if (spec->cookie != NULL) {
		tlspeer_put_int(&writer, 0x002c, 2);
		tlspeer_begin(&writer, 2);
		tlspeer_begin(&writer, 2);
		tlspeer_put(&writer, spec->cookie, spec->cookie_size);
		tlspeer_end(&writer);
		tlspeer_end(&writer);
	}

	/* Adds the extra extension when the spec has one. */
	if (spec->has_extra)
		tlspeer_put_extension(&writer, spec->extra_type, spec->extra, spec->extra_size);
	tlspeer_end(&writer);
	tlspeer_end(&writer);
	CHECK(writer.error == 0);
	return writer.used;
}

/* Starts a client without a peer and feeds it a hand-made ServerHello; returns the run. */
static struct run *
feed_sh(
	uint16_t min_version,
	uint16_t max_version,
	const struct sh_spec *spec)
{
	static uint8_t message[4096];
	struct run *run;
	size_t size;
	int error;

	/* A client, its ClientHello sent nowhere, the message. */
	run = run_new(min_version, max_version, &pki256, "server.test");
	error = go(run);
	if (error != 0)
		CHECK(0);
	size = sh_write(spec, message, sizeof(message));
	run->error = sslc_hs_message(&run->hs, message, size, &run->actions);
	return run;
}

/* Feeds a hand-made ServerHello made from the default with change applied, and checks the failure. */
static void
check_sh(
	uint16_t min_version,
	uint16_t max_version,
	const struct sh_spec *spec,
	int error,
	unsigned alert)
{
	struct run *run;

	/* The failure and its refusal of more. */
	run = feed_sh(min_version, max_version, spec);
	expect_failed(run, error, alert);
	CHECK(run->actions.count == 0U);
	run_free(run);
}

/* Makes a TLS 1.3 peer config over pki with the given changes. */
static void
peer13(
	struct tlspeer_config *config,
	const struct tlspeer_pki *pki)
{
	/* The tool's default script. */
	CHECK(tlspeer_config_init(config, SSLC_TLS13, pki) == 0);
}

/* Runs a full handshake against config with a 1.2-1.3 client of host trusting pki; returns the run (to be freed). */
static struct run *
handshake(
	const struct tlspeer_config *config,
	const struct tlspeer_pki *pki,
	uint16_t min_version,
	const char *host)
{
	struct run *run;
	int error;

	/* A client and its peer, run to the end. */
	run = run_new(min_version, SSLC_TLS13, pki, host);
	error = run_peer(run, config);
	if (error != 0)
		return run;
	go(run);
	return run;
}

/* Records the server's messages of a run's peer from a fresh handshake of the same random source. */
static void
record_replay(
	const struct tlspeer_config *config,
	struct replay *replay)
{
	struct tlspeer_item item;
	struct run *run;
	int more;
	int error;

	/* A client whose actions go to the peer, and every handshake item of the peer kept. */
	memset(replay, 0, sizeof(*replay));
	run = run_new(SSLC_TLS12, SSLC_TLS13, &pki256, "server.test");
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
	run = run_new(SSLC_TLS12, SSLC_TLS13, &pki256, "server.test");
	go(run);
	for (item = 0; item < replay->count && run->error == 0; item++) {
		if (item == index) {
			run->error = sslc_hs_message(&run->hs, message, size, &run->actions);
		} else if (replay->kind[item] == TLSPEER_ITEM_HANDSHAKE) {
			run->error = sslc_hs_message(&run->hs, replay->data[item], replay->size[item], &run->actions);
		} else {
			run->error = sslc_hs_change_cipher_spec(&run->hs, &run->actions);
		}

		/* Applies the actions unless the handshake already failed. */
		if (run->error == 0)
			apply(run);
	}

	/* Reports the finished run. */
	return run;
}

/*
 * Checks the ClientHello's bytes from the fixed random source against the
 * header's promise, for each set of versions, with and without a server
 * name; the order of the random draws; and that the same source gives the
 * same bytes.
 */
static void test_client_hello(void);

/* Checks the configuration's refusals and the random source's failures. */
static void test_config(void);

static void test_handshakes(void);

/* Checks each of the peer's breaks and unexpected extensions. */
static void test_breaks(void);

/* Checks the CertificateVerify checks with hand-made messages and mismatched schemes. */
static void test_certificate_verify(void);

/* Checks the certificate verdicts, the callback and the leaf checks. */
static void test_certificates(void);

/* Checks hand-made ServerHellos against each check of the header. */
static void test_server_hello(void);

/* Checks hand-made HelloRetryRequests and the ServerHellos after one. */
static void test_retry(void);

/* Checks the messages a state does not take, the header checks and the change_cipher_spec rules. */
static void test_states(void);

/* Feeds every replayed item, with the change_cipher_spec records in between, and checks each state's refusals. */
static void test_order(void);

/* Checks the messages after the handshake against the peer: tickets and KeyUpdates both ways. */
static void test_post_handshake(void);

/* Checks sslc_hs_queue_message and sslc_hs_queue_action directly. */
static void test_queue(void);

/* Checks sslc_hs_shared_secret with the RFC 7748 and CAVP values and the bad shares. */
static void test_shared_secret(void);

/* Checks sslc_hs_certificate directly: a second Certificate, a context, an empty list. */
static void test_certificate_direct(void);

static void
test_client_hello(void)
{
	static uint8_t expected[SSLC_HS_OUT_MAX];
	uint8_t share[CRYC_X25519_SIZE];
	struct tlspeer_hello parsed;
	struct run *run;
	struct run *again;
	size_t size;
	int pass;
	uint16_t min_version;
	uint16_t max_version;
	const char *host;

	/* Each pair of versions, with SNI and without. */
	for (pass = 0; pass < 6; pass++) {
		min_version = SSLC_TLS12;
		max_version = SSLC_TLS13;
		if (pass % 3 == 1)
			min_version = SSLC_TLS13;
		if (pass % 3 == 2)
			max_version = SSLC_TLS12;
		host = "server.test";
		if (pass >= 3)
			host = NULL;
		run = run_new(min_version, max_version, &pki256, host);
		CHECK(go(run) == 0);

		/* One SEND of the whole ClientHello, the first. */
		CHECK(run->actions.count == 1U);
		CHECK(run->actions.list[0].kind == SSLC_HS_SEND);
		CHECK(run->actions.list[0].offset == 0U);
		CHECK(run->actions.list[0].first_hello == 1);
		CHECK(run->hs.state == SSLC_HS_WAIT_SERVER_HELLO);
		CHECK(run->hello_count == 1U);

		/*
		 * The draws: the random (32), the session id (32) when TLS 1.3
		 * is offered, the x25519 key (32), and nothing more.
		 */
		CHECK(run->stream.sizes[0] == 32U);
		CHECK(run->stream.sizes[1] == 32U);
		if (max_version == SSLC_TLS13) {
			CHECK(run->stream.calls == 3U);
			CHECK(run->stream.sizes[2] == 32U);
			CHECK(memcmp(run->hs.x25519_private, run->stream.log + 64, 32) == 0);
		} else {
			CHECK(run->stream.calls == 2U);
			CHECK(memcmp(run->hs.x25519_private, run->stream.log + 32, 32) == 0);
		}

		/* Checks the x25519 public key that goes with the private key. */
		cryc_x25519_public(share, run->hs.x25519_private);
		CHECK_BYTES(run->hs.x25519_public, share, 32);

		/* The bytes, as the header and RFC 8446 4.1.2 write them. */
		if (max_version == SSLC_TLS13) {
			size = expected_hello(expected, sizeof(expected), run->stream.log, run->stream.log + 32, 32,
			    min_version == SSLC_TLS12, 1, host, 0x001d, share, 32, NULL, 0);
		} else {
			size = expected_hello(expected, sizeof(expected), run->stream.log, NULL, 0,
			    1, 0, host, 0, NULL, 0, NULL, 0);
		}

		/* Checks the ClientHello that was sent. */
		CHECK(run->actions.list[0].size == size);
		CHECK(run->hello_size[0] == size);
		CHECK_BYTES(run->hellos[0], expected, size);

		/* The test tool reads the same fields. */
		CHECK(tlspeer_parse_client_hello(run->hellos[0], run->hello_size[0], &parsed) == 0);
		CHECK_BYTES(parsed.random, run->stream.log, 32);
		CHECK(parsed.has_server_name == (host != NULL));

		/* The same source gives the same bytes. */
		again = run_new(min_version, max_version, &pki256, host);
		CHECK(go(again) == 0);
		CHECK(again->hello_size[0] == run->hello_size[0]);
		CHECK_BYTES(again->hellos[0], run->hellos[0], run->hello_size[0]);
		run_free(again);

		/* Another seed, other bytes. */
		again = run_new(min_version, max_version, &pki256, host);
		again->stream.seed = 1;
		CHECK(go(again) == 0);
		CHECK(memcmp(again->hellos[0] + 6, run->hellos[0] + 6, 32) != 0);
		run_free(again);
		run_free(run);
	}
}

/* Checks the configuration's refusals and the random source's failures. */
static void
test_config(void)
{
	static char long_name[300];
	struct run *run;
	struct sslc_hs_actions actions;
	struct sslc_hs_config config;
	unsigned fail;

	/* A reference configuration. */
	run = run_new(SSLC_TLS12, SSLC_TLS13, &pki256, "server.test");

	/* Arguments: no structure, no configuration. */
	CHECK(sslc_hs_start(NULL, &run->config, &actions) == EINVAL);
	CHECK(sslc_hs_start(&run->hs, &run->config, NULL) == EINVAL);
	CHECK(run->hs.state == SSLC_HS_FAILED);
	CHECK(sslc_hs_start(&run->hs, NULL, &actions) == EINVAL);
	CHECK(run->hs.state == SSLC_HS_FAILED);

	/* Versions out of range, a missing random source or store. */
	config = run->config;
	config.min_version = 0x0302;
	CHECK(sslc_hs_start(&run->hs, &config, &actions) == EINVAL);
	CHECK(run->hs.state == SSLC_HS_FAILED);
	config = run->config;
	config.max_version = 0x0305;
	CHECK(sslc_hs_start(&run->hs, &config, &actions) == EINVAL);
	config = run->config;
	config.min_version = SSLC_TLS13;
	config.max_version = SSLC_TLS12;
	CHECK(sslc_hs_start(&run->hs, &config, &actions) == EINVAL);
	config = run->config;
	config.random = NULL;
	CHECK(sslc_hs_start(&run->hs, &config, &actions) == EINVAL);
	config = run->config;
	config.store = NULL;
	CHECK(sslc_hs_start(&run->hs, &config, &actions) == EINVAL);

	/* A server name over 253 characters cannot be sent. */
	memset(long_name, 'a', 254);
	long_name[254] = '\0';
	config = run->config;
	config.server_name = long_name;
	CHECK(sslc_hs_start(&run->hs, &config, &actions) == EINVAL);
	CHECK(run->hs.state == SSLC_HS_FAILED);
	CHECK(actions.count == 0U);
	memset(long_name, 'a', 253);
	long_name[253] = '\0';
	CHECK(sslc_hs_start(&run->hs, &config, &actions) == 0);
	sslc_hs_free(&run->hs);
	run_free(run);

	/* Each of the three draws failing is EIO, with no alert and nothing to send. */
	for (fail = 1; fail <= 3U; fail++) {
		run = run_new(SSLC_TLS12, SSLC_TLS13, &pki256, "server.test");
		run->stream.fail_at = fail;
		CHECK(go(run) == EIO);
		CHECK(run->hs.state == SSLC_HS_FAILED);
		CHECK(run->hs.alert == 0U);
		CHECK(run->actions.count == 0U);
		CHECK(all_zero(run->hs.x25519_private, 32));
		run_free(run);
	}

	/* A TLS 1.2-only client draws twice; its second draw failing is EIO too. */
	run = run_new(SSLC_TLS12, SSLC_TLS12, &pki256, "server.test");
	run->stream.fail_at = 2;
	CHECK(go(run) == EIO);
	run_free(run);
}

/*
 * Checks full TLS 1.3 handshakes against the peer: each suite and key,
 * the HelloRetryRequest with and without a group and a cookie, the
 * options, and the second ClientHello's bytes.
 */
static void
test_handshakes(void)
{
	static uint8_t expected[SSLC_HS_OUT_MAX];
	static const uint8_t cookie[5] = { 'c', 'o', 'o', 'k', 'y' };
	static uint8_t big_cookie[SSLC_MSG_COOKIE_MAX];
	static const uint8_t context[3] = { 0xaa, 0xbb, 0xcc };
	static uint8_t long_context[255];
	static const uint16_t suite_ids[3] = { 0x1301, 0x1302, 0x1303 };
	struct tlspeer_config config;
	struct tlspeer_hello first;
	struct tlspeer_hello second;
	struct run *run;
	size_t size;
	size_t index;

	/* Each suite over x25519 with the P-256 leaf. */
	for (index = 0; index < 3U; index++) {
		peer13(&config, &pki256);
		config.suite = suite_ids[index];
		run = handshake(&config, &pki256, SSLC_TLS12, "server.test");
		check_connected(run, "SRCWSWRD");
		CHECK(run->hs.suite != NULL && run->hs.suite->id == suite_ids[index]);
		CHECK(run->hs.verify_code == CRYC_VERIFY_OK);
		CHECK(run->hs.verify.chain_count == 2U);
		CHECK(run->hs.peer_count == 1U);
		CHECK(run->messages_sent == 2U);
		CHECK(run->callback_calls == 0U);
		run_free(run);
	}

	/* The P-384 leaf (ecdsa_secp384r1_sha384), and a client of TLS 1.3 alone. */
	peer13(&config, &pki384);
	run = handshake(&config, &pki384, SSLC_TLS13, "server.test");
	check_connected(run, "SRCWSWRD");
	run_free(run);

	/* The server's own compatibility change_cipher_spec is dropped. */
	peer13(&config, &pki256);
	config.options = TLSPEER_OPT_CCS;
	run = handshake(&config, &pki256, SSLC_TLS12, "server.test");
	check_connected(run, "SRCWSWRD");
	run_free(run);

	/* The server_name acknowledgement answers the name sent; EncryptedExtensions may list groups. */
	peer13(&config, &pki256);
	config.options = TLSPEER_OPT_SNI_ACK;
	run = handshake(&config, &pki256, SSLC_TLS12, "server.test");
	check_connected(run, "SRCWSWRD");
	run_free(run);
	peer13(&config, &pki256);
	config.ee_raw = (const uint8_t *)"\x00\x0a\x00\x06\x00\x04\x00\x1d\x00\x17";
	config.ee_raw_size = 10;
	run = handshake(&config, &pki256, SSLC_TLS12, "server.test");
	check_connected(run, "SRCWSWRD");
	run_free(run);

	/* A CertificateRequest (empty context, then a context of 3 bytes) gets an empty Certificate with the Finished. */
	peer13(&config, &pki256);
	config.options = TLSPEER_OPT_CERT_REQUEST;
	run = handshake(&config, &pki256, SSLC_TLS12, "server.test");
	check_connected(run, "SRCWSWRD");
	CHECK(run->messages_sent == 3U);
	CHECK(run->hs.has_request == 1);
	run_free(run);
	peer13(&config, &pki256);
	config.options = TLSPEER_OPT_CERT_REQUEST | TLSPEER_OPT_CCS;
	config.request_context = context;
	config.request_context_size = sizeof(context);
	config.extra_type = 0x7a7a;
	config.extra_data = (const uint8_t *)"xy";
	config.extra_size = 2;
	config.extra_at = TLSPEER_AT_CERT_REQUEST;
	run = handshake(&config, &pki256, SSLC_TLS12, "server.test");
	check_connected(run, "SRCWSWRD");
	CHECK(run->hs.request_context_size == 3U);
	run_free(run);

	/* The longest context, 255 bytes, is echoed whole. */
	for (index = 0; index < sizeof(long_context); index++)
		long_context[index] = (uint8_t)index;
	peer13(&config, &pki256);
	config.options = TLSPEER_OPT_CERT_REQUEST;
	config.request_context = long_context;
	config.request_context_size = sizeof(long_context);
	run = handshake(&config, &pki256, SSLC_TLS12, "server.test");
	check_connected(run, "SRCWSWRD");
	CHECK(run->hs.request_context_size == 255U);
	run_free(run);

	/* No server name at all (an address): the chain is still verified, without a host. */
	peer13(&config, &pki256);
	run = handshake(&config, &pki256, SSLC_TLS12, NULL);
	check_connected(run, "SRCWSWRD");
	CHECK(run->peer->hello.has_server_name == 0);
	run_free(run);

	/* A HelloRetryRequest for secp256r1: the compatibility record before the second ClientHello, not again. */
	peer13(&config, &pki256);
	config.group = SSLC_GROUP_SECP256R1;
	config.hello_retry = 1;
	run = handshake(&config, &pki256, SSLC_TLS12, "server.test");
	check_connected(run, "SCSRWSWRD");
	CHECK(run->hs.retried == 1);
	CHECK(run->hs.has_ec == 1);
	CHECK(run->hello_count == 2U);
	CHECK(run->peer->retries == 1U);

	/* The second ClientHello: the same random and session id, the secp256r1 share alone, no cookie. */
	CHECK(tlspeer_parse_client_hello(run->hellos[0], run->hello_size[0], &first) == 0);
	CHECK(tlspeer_parse_client_hello(run->hellos[1], run->hello_size[1], &second) == 0);
	CHECK_BYTES(second.random, first.random, 32);
	CHECK(second.session_id_size == 32U);
	CHECK_BYTES(second.session_id, first.session_id, 32);
	CHECK(second.share_count == 1U);
	CHECK(second.shares[0].group == 0x0017);
	CHECK(second.shares[0].key_size == 65U);
	CHECK(second.shares[0].key[0] == 0x04);
	CHECK_BYTES(second.shares[0].key, run->hs.ec_public, 65);
	CHECK(second.has_cookie == 0);
	size = expected_hello(expected, sizeof(expected), first.random, first.session_id, 32, 1, 1, "server.test",
	    0x0017, run->hs.ec_public, 65, NULL, 0);
	CHECK(run->hello_size[1] == size);
	CHECK_BYTES(run->hellos[1], expected, size);

	/* The secp256r1 key came from the fourth draw on (after random, session id and x25519). */
	CHECK(run->stream.calls >= 4U);
	CHECK(run->stream.sizes[3] == 32U);
	run_free(run);

	/* A HelloRetryRequest for secp384r1: a 97-byte share alone, a 48-byte shared secret, the same keys as the peer. */
	peer13(&config, &pki256);
	config.group = SSLC_GROUP_SECP384R1;
	config.hello_retry = 1;
	run = handshake(&config, &pki256, SSLC_TLS12, "server.test");
	check_connected(run, "SCSRWSWRD");
	CHECK(run->hs.retried == 1);
	CHECK(run->hs.has_ec == 1);
	CHECK(run->hs.ec_group == SSLC_GROUP_SECP384R1);
	CHECK(run->hs.ec_public_size == 97U);
	CHECK(run->peer->shared_size == 48U);
	CHECK(tlspeer_parse_client_hello(run->hellos[0], run->hello_size[0], &first) == 0);
	CHECK(tlspeer_parse_client_hello(run->hellos[1], run->hello_size[1], &second) == 0);
	CHECK(second.share_count == 1U);
	CHECK(second.shares[0].group == 0x0018);
	CHECK(second.shares[0].key_size == 97U);
	CHECK(second.shares[0].key[0] == 0x04);
	CHECK_BYTES(second.shares[0].key, run->hs.ec_public, 97);
	size = expected_hello(expected, sizeof(expected), first.random, first.session_id, 32, 1, 1, "server.test",
	    0x0018, run->hs.ec_public, 97, NULL, 0);
	CHECK(run->hello_size[1] == size);
	CHECK_BYTES(run->hellos[1], expected, size);
	CHECK(all_zero(run->hs.ec_private, sizeof(run->hs.ec_private)));

	/* The secp384r1 key came from the fourth draw on, 48 bytes. */
	CHECK(run->stream.calls >= 4U);
	CHECK(run->stream.sizes[3] == 48U);
	run_free(run);

	/* A HelloRetryRequest with a group and a cookie, with the server's compatibility record. */
	peer13(&config, &pki256);
	config.group = SSLC_GROUP_SECP256R1;
	config.hello_retry = 1;
	config.cookie = cookie;
	config.cookie_size = sizeof(cookie);
	config.options = TLSPEER_OPT_CCS;
	config.suite = 0x1302;
	run = handshake(&config, &pki256, SSLC_TLS12, "server.test");
	check_connected(run, "SCSRWSWRD");
	CHECK(tlspeer_parse_client_hello(run->hellos[1], run->hello_size[1], &second) == 0);
	CHECK(second.has_cookie == 1);
	CHECK(second.cookie_size == sizeof(cookie));
	CHECK_BYTES(second.cookie, cookie, sizeof(cookie));
	CHECK(tlspeer_parse_client_hello(run->hellos[0], run->hello_size[0], &first) == 0);
	size = expected_hello(expected, sizeof(expected), first.random, first.session_id, 32, 1, 1, "server.test",
	    0x0017, run->hs.ec_public, 65, cookie, sizeof(cookie));
	CHECK(run->hello_size[1] == size);
	CHECK_BYTES(run->hellos[1], expected, size);
	run_free(run);

	/* A HelloRetryRequest with a cookie alone (of the largest size): the x25519 share is sent again. */
	for (index = 0; index < sizeof(big_cookie); index++)
		big_cookie[index] = (uint8_t)(index * 7U);
	peer13(&config, &pki256);
	config.hello_retry = 1;
	config.cookie = big_cookie;
	config.cookie_size = sizeof(big_cookie);
	config.options = TLSPEER_OPT_RETRY_NO_GROUP;
	run = handshake(&config, &pki256, SSLC_TLS12, "server.test");
	check_connected(run, "SCSRWSWRD");
	CHECK(run->hs.has_ec == 0);
	CHECK(tlspeer_parse_client_hello(run->hellos[1], run->hello_size[1], &second) == 0);
	CHECK(second.share_count == 1U);
	CHECK(second.shares[0].group == 0x001d);
	CHECK(second.cookie_size == sizeof(big_cookie));
	run_free(run);

	/* After a retry, a CertificateRequest and the P-384 leaf. */
	peer13(&config, &pki384);
	config.group = SSLC_GROUP_SECP256R1;
	config.hello_retry = 1;
	config.options = TLSPEER_OPT_CERT_REQUEST;
	run = handshake(&config, &pki384, SSLC_TLS13, "server.test");
	check_connected(run, "SCSRWSWRD");
	run_free(run);
}

/* Checks a handshake against a peer config that must fail with error and alert. */
static void
check_peer_fails(
	const struct tlspeer_config *config,
	const struct tlspeer_pki *pki,
	uint16_t min_version,
	const char *host,
	int error,
	unsigned alert)
{
	struct run *run;

	/* The client fails as expected, and refuses anything after. */
	run = handshake(config, pki, min_version, host);
	expect_failed(run, error, alert);
	run_free(run);
}

/* Checks each of the peer's breaks and unexpected extensions. */
static void
test_breaks(void)
{
	struct tlspeer_config config;
	struct run *run;

	/* A Finished that does not verify, a signature that does not verify. */
	peer13(&config, &pki256);
	config.breaks = TLSPEER_BREAK_FINISHED;
	check_peer_fails(&config, &pki256, SSLC_TLS12, "server.test", EBADMSG, SSLC_ALERT_DECRYPT_ERROR);
	peer13(&config, &pki256);
	config.breaks = TLSPEER_BREAK_SIGNATURE;
	check_peer_fails(&config, &pki256, SSLC_TLS12, "server.test", EBADMSG, SSLC_ALERT_DECRYPT_ERROR);
	peer13(&config, &pki384);
	config.breaks = TLSPEER_BREAK_SIGNATURE;
	check_peer_fails(&config, &pki384, SSLC_TLS12, "server.test", EBADMSG, SSLC_ALERT_DECRYPT_ERROR);

	/* A second HelloRetryRequest. */
	peer13(&config, &pki256);
	config.group = SSLC_GROUP_SECP256R1;
	config.hello_retry = 1;
	config.breaks = TLSPEER_BREAK_SECOND_HRR;
	check_peer_fails(&config, &pki256, SSLC_TLS12, "server.test", EPROTO, SSLC_ALERT_UNEXPECTED_MESSAGE);

	/* A TLS 1.2 server marking a downgrade to a client that offered TLS 1.3, either mark. */
	CHECK(tlspeer_config_init(&config, SSLC_TLS12, &pki256) == 0);
	config.breaks = TLSPEER_BREAK_DOWNGRADE12;
	check_peer_fails(&config, &pki256, SSLC_TLS12, "server.test", EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);
	CHECK(tlspeer_config_init(&config, SSLC_TLS12, &pki256) == 0);
	config.breaks = TLSPEER_BREAK_DOWNGRADE11;
	check_peer_fails(&config, &pki256, SSLC_TLS12, "server.test", EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);

	/* A TLS 1.2 server to a client of TLS 1.3 alone. */
	CHECK(tlspeer_config_init(&config, SSLC_TLS12, &pki256) == 0);
	check_peer_fails(&config, &pki256, SSLC_TLS13, "server.test", EPROTO, SSLC_ALERT_PROTOCOL_VERSION);

	/* The session id not echoed, a Certificate with a context. */
	peer13(&config, &pki256);
	config.breaks = TLSPEER_BREAK_SESSION_ID;
	check_peer_fails(&config, &pki256, SSLC_TLS12, "server.test", EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);
	peer13(&config, &pki256);
	config.breaks = TLSPEER_BREAK_CERT_CONTEXT;
	check_peer_fails(&config, &pki256, SSLC_TLS12, "server.test", EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);

	/* A ServerHello for secp256r1, for which no share was sent. */
	peer13(&config, &pki256);
	config.group = SSLC_GROUP_SECP256R1;
	config.breaks = TLSPEER_BREAK_UNSHARED;
	check_peer_fails(&config, &pki256, SSLC_TLS12, "server.test", EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);

	/* A share that gives an all-zero x25519 result, a secp256r1 point not on the curve. */
	peer13(&config, &pki256);
	config.breaks = TLSPEER_BREAK_ZERO_SHARE;
	check_peer_fails(&config, &pki256, SSLC_TLS12, "server.test", EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);
	peer13(&config, &pki256);
	config.group = SSLC_GROUP_SECP256R1;
	config.hello_retry = 1;
	config.breaks = TLSPEER_BREAK_ZERO_SHARE;
	check_peer_fails(&config, &pki256, SSLC_TLS12, "server.test", EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);

	/* A suite of TLS 1.2 in a TLS 1.3 ServerHello. */
	peer13(&config, &pki256);
	config.suite = 0xc02b;
	check_peer_fails(&config, &pki256, SSLC_TLS12, "server.test", EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);

	/* An extension never offered: in the ServerHello, the HelloRetryRequest, EncryptedExtensions, a CertificateEntry. */
	peer13(&config, &pki256);
	config.extra_type = 0x7a7a;
	config.extra_data = (const uint8_t *)"xy";
	config.extra_size = 2;
	config.extra_at = TLSPEER_AT_SERVER_HELLO;
	check_peer_fails(&config, &pki256, SSLC_TLS12, "server.test", EPROTO, SSLC_ALERT_UNSUPPORTED_EXTENSION);
	config.extra_at = TLSPEER_AT_ENCRYPTED_EXTENSIONS;
	check_peer_fails(&config, &pki256, SSLC_TLS12, "server.test", EPROTO, SSLC_ALERT_UNSUPPORTED_EXTENSION);
	config.extra_at = TLSPEER_AT_CERT_ENTRY;
	check_peer_fails(&config, &pki256, SSLC_TLS12, "server.test", EPROTO, SSLC_ALERT_UNSUPPORTED_EXTENSION);
	config.extra_at = TLSPEER_AT_RETRY;
	config.group = SSLC_GROUP_SECP256R1;
	config.hello_retry = 1;
	check_peer_fails(&config, &pki256, SSLC_TLS12, "server.test", EPROTO, SSLC_ALERT_UNSUPPORTED_EXTENSION);

	/* An extension offered for TLS 1.2 only, in a TLS 1.3 ServerHello or HelloRetryRequest. */
	peer13(&config, &pki256);
	config.extra_type = SSLC_EXT_EXTENDED_MASTER_SECRET;
	config.extra_data = NULL;
	config.extra_size = 0;
	config.extra_at = TLSPEER_AT_SERVER_HELLO;
	check_peer_fails(&config, &pki256, SSLC_TLS12, "server.test", EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);
	config.extra_type = SSLC_EXT_SERVER_NAME;
	check_peer_fails(&config, &pki256, SSLC_TLS12, "server.test", EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);
	config.extra_at = TLSPEER_AT_RETRY;
	config.group = SSLC_GROUP_SECP256R1;
	config.hello_retry = 1;
	check_peer_fails(&config, &pki256, SSLC_TLS12, "server.test", EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);
	config.extra_type = SSLC_EXT_EXTENDED_MASTER_SECRET;
	check_peer_fails(&config, &pki256, SSLC_TLS12, "server.test", EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);

	/* extended_master_secret in EncryptedExtensions: offered by the client, not specified for the message (RFC 8446 4.2): illegal_parameter. */
	peer13(&config, &pki256);
	config.extra_type = SSLC_EXT_EXTENDED_MASTER_SECRET;
	config.extra_at = TLSPEER_AT_ENCRYPTED_EXTENSIONS;
	check_peer_fails(&config, &pki256, SSLC_TLS12, "server.test", EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);

	/* A server_name acknowledgement to a client that sent no name. */
	peer13(&config, &pki256);
	config.ee_raw = (const uint8_t *)"\x00\x00\x00\x00";
	config.ee_raw_size = 4;
	check_peer_fails(&config, &pki256, SSLC_TLS12, NULL, EPROTO, SSLC_ALERT_UNSUPPORTED_EXTENSION);
	run = handshake(&config, &pki256, SSLC_TLS12, "server.test");
	check_connected(run, "SRCWSWRD");
	run_free(run);

	/* An EncryptedExtensions that does not parse. */
	peer13(&config, &pki256);
	config.ee_raw = (const uint8_t *)"\x00\x00\x00";
	config.ee_raw_size = 3;
	check_peer_fails(&config, &pki256, SSLC_TLS12, "server.test", EINVAL, SSLC_ALERT_DECODE_ERROR);
}

/* Checks the CertificateVerify checks with hand-made messages and mismatched schemes. */
static void
test_certificate_verify(void)
{
	static const uint8_t not_offered[12] = { 0x0f, 0, 0, 8, 0x08, 0x07, 0, 4, 1, 2, 3, 4 };
	static const uint8_t pkcs1[12] = { 0x0f, 0, 0, 8, 0x04, 0x01, 0, 4, 1, 2, 3, 4 };
	static const uint8_t pss[12] = { 0x0f, 0, 0, 8, 0x08, 0x04, 0, 4, 1, 2, 3, 4 };
	static const uint8_t p384_scheme[12] = { 0x0f, 0, 0, 8, 0x05, 0x03, 0, 4, 1, 2, 3, 4 };
	static const uint8_t garbage[12] = { 0x0f, 0, 0, 8, 0x04, 0x03, 0, 4, 1, 2, 3, 4 };
	static const uint8_t empty_signature[8] = { 0x0f, 0, 0, 4, 0x04, 0x03, 0, 0 };
	static const uint8_t truncated[7] = { 0x0f, 0, 0, 3, 0x04, 0x03, 0 };
	static const uint8_t finished_instead[36] = { 0x14, 0, 0, 32 };
	struct tlspeer_config config;

	/* A scheme not offered, RSA PKCS #1 (certificates only), RSA-PSS with an EC key, P-384's with a P-256 key. */
	peer13(&config, &pki256);
	config.cv_raw = not_offered;
	config.cv_raw_size = sizeof(not_offered);
	check_peer_fails(&config, &pki256, SSLC_TLS12, "server.test", EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);
	config.cv_raw = pkcs1;
	check_peer_fails(&config, &pki256, SSLC_TLS12, "server.test", EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);
	config.cv_raw = pss;
	check_peer_fails(&config, &pki256, SSLC_TLS12, "server.test", EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);
	config.cv_raw = p384_scheme;
	check_peer_fails(&config, &pki256, SSLC_TLS12, "server.test", EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);

	/* A signature that is not DER, an empty one: they do not verify. */
	config.cv_raw = garbage;
	check_peer_fails(&config, &pki256, SSLC_TLS12, "server.test", EBADMSG, SSLC_ALERT_DECRYPT_ERROR);
	config.cv_raw = empty_signature;
	config.cv_raw_size = sizeof(empty_signature);
	check_peer_fails(&config, &pki256, SSLC_TLS12, "server.test", EBADMSG, SSLC_ALERT_DECRYPT_ERROR);

	/* A CertificateVerify cut short, and a Finished in its place. */
	config.cv_raw = truncated;
	config.cv_raw_size = sizeof(truncated);
	check_peer_fails(&config, &pki256, SSLC_TLS12, "server.test", EINVAL, SSLC_ALERT_DECODE_ERROR);
	config.cv_raw = finished_instead;
	config.cv_raw_size = sizeof(finished_instead);
	check_peer_fails(&config, &pki256, SSLC_TLS12, "server.test", EPROTO, SSLC_ALERT_UNEXPECTED_MESSAGE);

	/* The peer signs with P-384's scheme and a P-256 key, and with P-256's and a P-384 key. */
	peer13(&config, &pki256);
	config.scheme = SSLC_SCHEME_ECDSA_SECP384R1_SHA384;
	check_peer_fails(&config, &pki256, SSLC_TLS12, "server.test", EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);
	peer13(&config, &pki384);
	config.scheme = SSLC_SCHEME_ECDSA_SECP256R1_SHA256;
	check_peer_fails(&config, &pki384, SSLC_TLS12, "server.test", EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);
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

/* Checks the certificate verdicts, the callback and the leaf checks. */
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
	struct tlspeer_config config;
	struct run *run;

	/* An empty store: unknown_ca when verifying, the verdict recorded either way. */
	peer13(&config, &pki256);
	check_peer_fails(&config, NULL, SSLC_TLS12, "server.test", EPROTO, SSLC_ALERT_UNKNOWN_CA);
	run = run_new(SSLC_TLS12, SSLC_TLS13, NULL, "server.test");
	run->config.verify_peer = 0;
	run->config.callback = verify_callback;
	run->config.callback_context = run;
	run_peer(run, &config);
	go(run);
	check_connected(run, "SRCWSWRD");
	CHECK(run->hs.verify_code == CRYC_VERIFY_UNABLE_TO_GET_ISSUER);
	CHECK(run->callback_calls == 0U);
	run_free(run);

	/* The callback lets an invalid chain through, or refuses it. */
	run = run_new(SSLC_TLS12, SSLC_TLS13, NULL, "server.test");
	run->config.callback = verify_callback;
	run->config.callback_context = run;
	run_peer(run, &config);
	go(run);
	check_connected(run, "SRCWSWRD");
	CHECK(run->callback_calls == 1U);
	CHECK(run->callback_preverified == 0);
	CHECK(run->callback_code == CRYC_VERIFY_UNABLE_TO_GET_ISSUER);
	CHECK(run->hs.verify_code == CRYC_VERIFY_UNABLE_TO_GET_ISSUER);
	run_free(run);
	run = run_new(SSLC_TLS12, SSLC_TLS13, NULL, "server.test");
	run->config.callback = verify_callback;
	run->config.callback_context = run;
	run->callback_answer = 0;
	run_peer(run, &config);
	go(run);
	expect_failed(run, EPROTO, SSLC_ALERT_UNKNOWN_CA);
	CHECK(run->callback_calls == 1U);
	run_free(run);

	/* A valid chain is offered to the callback, which may refuse it. */
	run = run_new(SSLC_TLS12, SSLC_TLS13, &pki256, "server.test");
	run->config.callback = verify_callback;
	run->config.callback_context = run;
	run_peer(run, &config);
	go(run);
	check_connected(run, "SRCWSWRD");
	CHECK(run->callback_calls == 1U);
	CHECK(run->callback_preverified == 1);
	CHECK(run->callback_code == CRYC_VERIFY_OK);
	run_free(run);
	run = run_new(SSLC_TLS12, SSLC_TLS13, &pki256, "server.test");
	run->config.callback = verify_callback;
	run->config.callback_context = run;
	run->callback_answer = 0;
	run_peer(run, &config);
	go(run);
	expect_failed(run, EPROTO, SSLC_ALERT_CERTIFICATE_UNKNOWN);
	run_free(run);

	/* Expired, not yet valid, another host. */
	run = run_new(SSLC_TLS12, SSLC_TLS13, &pki256, "server.test");
	run->config.params.time = TLSPEER_NOT_AFTER_TIME + 1;
	run_peer(run, &config);
	go(run);
	expect_failed(run, EPROTO, SSLC_ALERT_CERTIFICATE_EXPIRED);
	CHECK(run->hs.verify_code == CRYC_VERIFY_EXPIRED);
	run_free(run);
	run = run_new(SSLC_TLS12, SSLC_TLS13, &pki256, "server.test");
	run->config.params.time = TLSPEER_NOT_BEFORE_TIME - 1;
	run_peer(run, &config);
	go(run);
	expect_failed(run, EPROTO, SSLC_ALERT_CERTIFICATE_EXPIRED);
	CHECK(run->hs.verify_code == CRYC_VERIFY_NOT_YET_VALID);
	run_free(run);
	run = run_new(SSLC_TLS12, SSLC_TLS13, &pki256, "server.test");
	CHECK(cryc_name_host_normalize("other.test", run->config.params.host) == 0);
	run_peer(run, &config);
	go(run);
	expect_failed(run, EPROTO, SSLC_ALERT_BAD_CERTIFICATE);
	CHECK(run->hs.verify_code == CRYC_VERIFY_HOSTNAME_MISMATCH);
	run_free(run);

	/* The root alone as the server's certificate, and the root sent after the leaf to a client without it. */
	peer13(&config, &pki256);
	config.chain[0] = pki256.root;
	config.chain_size[0] = pki256.root_size;
	run = handshake(&config, NULL, SSLC_TLS12, "server.test");
	expect_failed(run, EPROTO, SSLC_ALERT_UNKNOWN_CA);
	CHECK(run->hs.verify_code == CRYC_VERIFY_DEPTH_ZERO_SELF_SIGNED);
	run_free(run);
	peer13(&config, &pki256);
	config.chain[1] = pki256.root;
	config.chain_size[1] = pki256.root_size;
	config.chain_count = 2;
	run = handshake(&config, NULL, SSLC_TLS12, "server.test");
	expect_failed(run, EPROTO, SSLC_ALERT_UNKNOWN_CA);
	CHECK(run->hs.verify_code == CRYC_VERIFY_SELF_SIGNED_IN_CHAIN);
	run_free(run);

	/* The root sent after the leaf to a client that trusts it: tolerated. */
	run = handshake(&config, &pki256, SSLC_TLS12, "server.test");
	check_connected(run, "SRCWSWRD");
	CHECK(run->hs.peer_count == 2U);
	run_free(run);

	/* A leaf signed with an algorithm libcrypto-compat does not support. */
	memcpy(leaf, pki256.leaf, pki256.leaf_size);
	CHECK(patch(leaf, pki256.leaf_size, sha256_oid, sha224_oid, sizeof(sha256_oid)) == 2U);
	peer13(&config, &pki256);
	config.chain[0] = leaf;
	config.chain_size[0] = pki256.leaf_size;
	run = handshake(&config, &pki256, SSLC_TLS12, "server.test");
	expect_failed(run, EPROTO, SSLC_ALERT_UNSUPPORTED_CERTIFICATE);
	CHECK(run->hs.verify_code == CRYC_VERIFY_UNSUPPORTED_ALGORITHM);
	run_free(run);

	/* A leaf of a curve libcrypto-compat does not support, without verification: unsupported_certificate. */
	memcpy(leaf, pki256.leaf, pki256.leaf_size);
	CHECK(patch(leaf, pki256.leaf_size, p256_oid, other_oid, sizeof(p256_oid)) == 1U);
	config.chain[0] = leaf;
	run = run_new(SSLC_TLS12, SSLC_TLS13, &pki256, "server.test");
	run->config.verify_peer = 0;
	run_peer(run, &config);
	go(run);
	expect_failed(run, ENOTSUP, SSLC_ALERT_UNSUPPORTED_CERTIFICATE);
	CHECK(run->hs.verify_code != CRYC_VERIFY_OK);
	run_free(run);

	/* A certificate that does not parse. */
	peer13(&config, &pki256);
	config.chain_size[0] = pki256.leaf_size - 1U;
	check_peer_fails(&config, &pki256, SSLC_TLS12, "server.test", EINVAL, SSLC_ALERT_BAD_CERTIFICATE);
}

/* Checks hand-made ServerHellos against each check of the header. */
static void
test_server_hello(void)
{
	static const uint8_t zero_key[32] = { 0 };
	static const uint8_t cookie[2] = { 1, 2 };
	struct sh_spec spec;
	struct run *run;
	struct run *model;

	/* A model client, for its session id. */
	model = run_new(SSLC_TLS12, SSLC_TLS13, &pki256, "server.test");
	CHECK(go(model) == 0);

	/* The default hand-made ServerHello is taken: the server's handshake keys are given, EncryptedExtensions awaited. */
	sh_default(&spec, model);
	run = feed_sh(SSLC_TLS12, SSLC_TLS13, &spec);
	CHECK(run->error == 0);
	CHECK(run->actions.count == 1U);
	CHECK(run->actions.list[0].kind == SSLC_HS_SET_READ_KEYS);
	CHECK(run->hs.state == SSLC_HS13_WAIT_ENCRYPTED_EXTENSIONS);
	CHECK(run->hs.version == SSLC_TLS13);
	CHECK(all_zero(run->hs.x25519_private, 32));
	CHECK(sslc_hs_connected(&run->hs) == 0);
	run_free(run);

	/* legacy_version other than 0x0303. */
	sh_default(&spec, model);
	spec.legacy_version = 0x0301;
	check_sh(SSLC_TLS12, SSLC_TLS13, &spec, EPROTO, SSLC_ALERT_PROTOCOL_VERSION);
	spec.legacy_version = 0x0304;
	check_sh(SSLC_TLS12, SSLC_TLS13, &spec, EPROTO, SSLC_ALERT_PROTOCOL_VERSION);

	/* supported_versions naming TLS 1.2, or TLS 1.3 to a client that did not offer it. */
	sh_default(&spec, model);
	spec.version = 0x0303;
	check_sh(SSLC_TLS12, SSLC_TLS13, &spec, EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);
	spec.version = 0x0305;
	check_sh(SSLC_TLS12, SSLC_TLS13, &spec, EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);
	sh_default(&spec, model);
	spec.session_id_size = 0;
	check_sh(SSLC_TLS12, SSLC_TLS12, &spec, EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);

	/* No supported_versions to a client of TLS 1.3 alone. */
	sh_default(&spec, model);
	spec.version = 0;
	check_sh(SSLC_TLS13, SSLC_TLS13, &spec, EPROTO, SSLC_ALERT_PROTOCOL_VERSION);

	/* A suite not offered, unknown, the signalling value, or of the other version. */
	sh_default(&spec, model);
	spec.suite = 0x1304;
	check_sh(SSLC_TLS12, SSLC_TLS13, &spec, EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);
	spec.suite = 0x00ff;
	check_sh(SSLC_TLS12, SSLC_TLS13, &spec, EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);
	spec.suite = 0xc02f;
	check_sh(SSLC_TLS12, SSLC_TLS13, &spec, EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);
	spec.suite = 0xc02b;
	check_sh(SSLC_TLS13, SSLC_TLS13, &spec, EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);

	/* A compression method. */
	sh_default(&spec, model);
	spec.compression = 1;
	check_sh(SSLC_TLS12, SSLC_TLS13, &spec, EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);

	/* The session id: another byte, empty, longer by one is a parse error. */
	sh_default(&spec, model);
	spec.session_id[31] ^= 0x80U;
	check_sh(SSLC_TLS12, SSLC_TLS13, &spec, EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);
	sh_default(&spec, model);
	spec.session_id_size = 0;
	check_sh(SSLC_TLS12, SSLC_TLS13, &spec, EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);
	spec.session_id_size = 31;
	check_sh(SSLC_TLS12, SSLC_TLS13, &spec, EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);

	/* No key share; one of a group with no share sent; one of a group never offered. */
	sh_default(&spec, model);
	spec.has_share = 0;
	check_sh(SSLC_TLS12, SSLC_TLS13, &spec, EPROTO, SSLC_ALERT_MISSING_EXTENSION);
	sh_default(&spec, model);
	spec.group = 0x0017;
	check_sh(SSLC_TLS12, SSLC_TLS13, &spec, EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);
	spec.group = 0x0018;
	check_sh(SSLC_TLS12, SSLC_TLS13, &spec, EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);
	spec.group = 0x0019;
	check_sh(SSLC_TLS12, SSLC_TLS13, &spec, EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);

	/* An x25519 share of the wrong length, and the zero point. */
	sh_default(&spec, model);
	spec.key_size = 31;
	check_sh(SSLC_TLS12, SSLC_TLS13, &spec, EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);
	spec.key_size = 33;
	check_sh(SSLC_TLS12, SSLC_TLS13, &spec, EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);
	memcpy(spec.key, zero_key, 32);
	spec.key_size = 32;
	check_sh(SSLC_TLS12, SSLC_TLS13, &spec, EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);

	/* Extensions of TLS 1.2 that have no place in a TLS 1.3 ServerHello (4.2's table). */
	sh_default(&spec, model);
	spec.has_extra = 1;
	spec.extra_type = SSLC_EXT_EXTENDED_MASTER_SECRET;
	check_sh(SSLC_TLS12, SSLC_TLS13, &spec, EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);
	spec.extra_type = SSLC_EXT_RENEGOTIATION_INFO;
	spec.extra_size = 1;
	check_sh(SSLC_TLS12, SSLC_TLS13, &spec, EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);
	spec.extra_type = SSLC_EXT_EC_POINT_FORMATS;
	spec.extra[0] = 1;
	spec.extra[1] = 0;
	spec.extra_size = 2;
	check_sh(SSLC_TLS12, SSLC_TLS13, &spec, EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);
	spec.extra_type = SSLC_EXT_SERVER_NAME;
	spec.extra_size = 0;
	check_sh(SSLC_TLS12, SSLC_TLS13, &spec, EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);

	/* A cookie outside a HelloRetryRequest (recognized, not specified there: illegal_parameter), and an extension never offered (unsupported_extension). */
	sh_default(&spec, model);
	spec.cookie = cookie;
	spec.cookie_size = sizeof(cookie);
	check_sh(SSLC_TLS12, SSLC_TLS13, &spec, EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);
	sh_default(&spec, model);
	spec.has_extra = 1;
	spec.extra_type = SSLC_EXT_ALPN;
	check_sh(SSLC_TLS12, SSLC_TLS13, &spec, EPROTO, SSLC_ALERT_UNSUPPORTED_EXTENSION);

	/* A TLS 1.2 ServerHello with a downgrade mark, either one, to a client that offered TLS 1.3. */
	sh_default(&spec, model);
	spec.version = 0;
	spec.has_share = 0;
	spec.session_id_size = 0;
	spec.suite = 0xc02b;
	memcpy(spec.random + 24, mark12, 8);
	check_sh(SSLC_TLS12, SSLC_TLS13, &spec, EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);
	memcpy(spec.random + 24, mark11, 8);
	check_sh(SSLC_TLS12, SSLC_TLS13, &spec, EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);

	/* A TLS 1.2 ServerHello with a TLS 1.3 suite, and with a suite not offered. */
	spec.random[31] ^= 0xffU;
	spec.suite = 0x1301;
	check_sh(SSLC_TLS12, SSLC_TLS13, &spec, EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);
	spec.suite = 0x009c;
	check_sh(SSLC_TLS12, SSLC_TLS13, &spec, EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);

	/* Frees the model run. */
	run_free(model);
}

/* Checks hand-made HelloRetryRequests and the ServerHellos after one. */
static void
test_retry(void)
{
	static uint8_t message[4096];
	static const uint8_t cookie[3] = { 9, 8, 7 };
	struct sh_spec spec;
	struct sh_spec retry;
	struct run *model;
	struct run *run;
	size_t size;

	/* A model client, for its session id. */
	model = run_new(SSLC_TLS12, SSLC_TLS13, &pki256, "server.test");
	CHECK(go(model) == 0);

	/* A group never offered (secp521r1), the group already shared, nothing to change. */
	sh_default(&spec, model);
	sh_retry(&spec, 0x0019);
	check_sh(SSLC_TLS12, SSLC_TLS13, &spec, EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);
	sh_retry(&spec, 0x001d);
	check_sh(SSLC_TLS12, SSLC_TLS13, &spec, EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);
	sh_retry(&spec, 0);
	check_sh(SSLC_TLS12, SSLC_TLS13, &spec, EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);

	/* Extensions that have no place in it. */
	sh_default(&spec, model);
	sh_retry(&spec, 0x0017);
	spec.has_extra = 1;
	spec.extra_type = SSLC_EXT_RENEGOTIATION_INFO;
	spec.extra_size = 1;
	check_sh(SSLC_TLS12, SSLC_TLS13, &spec, EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);
	spec.extra_type = SSLC_EXT_EC_POINT_FORMATS;
	spec.extra[0] = 1;
	spec.extra[1] = 0;
	spec.extra_size = 2;
	check_sh(SSLC_TLS12, SSLC_TLS13, &spec, EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);

	/* Its session id, suite and compression are checked as a ServerHello's. */
	sh_default(&spec, model);
	sh_retry(&spec, 0x0017);
	spec.session_id[0] ^= 1U;
	check_sh(SSLC_TLS12, SSLC_TLS13, &spec, EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);
	sh_default(&spec, model);
	sh_retry(&spec, 0x0017);
	spec.suite = 0xc02b;
	check_sh(SSLC_TLS12, SSLC_TLS13, &spec, EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);
	spec.suite = 0x1304;
	check_sh(SSLC_TLS12, SSLC_TLS13, &spec, EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);
	spec.suite = 0x1301;
	spec.compression = 1;
	check_sh(SSLC_TLS12, SSLC_TLS13, &spec, EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);

	/* Without supported_versions it is a TLS 1.2 ServerHello, whose TLS 1.3 suite does not fit. */
	sh_default(&spec, model);
	sh_retry(&spec, 0x0017);
	spec.version = 0;
	check_sh(SSLC_TLS12, SSLC_TLS13, &spec, EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);

	/* A good one: change_cipher_spec, then the second ClientHello (not first_hello). */
	sh_default(&retry, model);
	sh_retry(&retry, 0x0017);
	retry.cookie = cookie;
	retry.cookie_size = sizeof(cookie);
	retry.suite = 0x1303;
	run = feed_sh(SSLC_TLS12, SSLC_TLS13, &retry);
	CHECK(run->error == 0);
	CHECK(run->actions.count == 2U);
	CHECK(run->actions.list[0].kind == SSLC_HS_SEND_CCS);
	CHECK(run->actions.list[1].kind == SSLC_HS_SEND);
	CHECK(run->actions.list[1].offset == 0U);
	CHECK(run->actions.list[1].first_hello == 0);
	CHECK(run->hs.state == SSLC_HS_WAIT_SERVER_HELLO);
	CHECK(run->hs.retried == 1);
	CHECK(run->hs.hello.cookie_size == sizeof(cookie));
	CHECK(all_zero(run->hs.x25519_private, 32));
	apply(run);
	CHECK(run->hello_count == 2U);

	/* A second HelloRetryRequest. */
	size = sh_write(&retry, message, sizeof(message));
	run->error = sslc_hs_message(&run->hs, message, size, &run->actions);
	expect_failed(run, EPROTO, SSLC_ALERT_UNEXPECTED_MESSAGE);
	run_free(run);

	/* After it, a ServerHello of another suite, of the old group, without a version, of TLS 1.2. */
	sh_default(&spec, model);
	spec.suite = 0x1301;
	spec.group = 0x0017;
	spec.key[0] = 0x04;
	CHECK(hex_decode(p256_peer_x, spec.key + 1, 32) == 32U);
	CHECK(hex_decode(p256_peer_y, spec.key + 33, 32) == 32U);
	spec.key_size = 65;
	run = feed_sh(SSLC_TLS12, SSLC_TLS13, &retry);
	CHECK(run->error == 0);
	size = sh_write(&spec, message, sizeof(message));
	run->error = sslc_hs_message(&run->hs, message, size, &run->actions);
	expect_failed(run, EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);
	run_free(run);
	sh_default(&spec, model);
	spec.suite = 0x1303;
	run = feed_sh(SSLC_TLS12, SSLC_TLS13, &retry);
	size = sh_write(&spec, message, sizeof(message));
	run->error = sslc_hs_message(&run->hs, message, size, &run->actions);
	expect_failed(run, EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);
	run_free(run);
	sh_default(&spec, model);
	spec.suite = 0x1303;
	spec.version = 0;
	run = feed_sh(SSLC_TLS12, SSLC_TLS13, &retry);
	size = sh_write(&spec, message, sizeof(message));
	run->error = sslc_hs_message(&run->hs, message, size, &run->actions);
	expect_failed(run, EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);
	run_free(run);
	run = feed_sh(SSLC_TLS13, SSLC_TLS13, &retry);
	run->error = sslc_hs_message(&run->hs, message, size, &run->actions);
	expect_failed(run, EPROTO, SSLC_ALERT_PROTOCOL_VERSION);
	run_free(run);

	/* After it, a secp256r1 share that is not on the curve. */
	sh_default(&spec, model);
	spec.suite = 0x1303;
	spec.group = 0x0017;
	memset(spec.key, 0, 65);
	spec.key[0] = 0x04;
	spec.key_size = 65;
	run = feed_sh(SSLC_TLS12, SSLC_TLS13, &retry);
	size = sh_write(&spec, message, sizeof(message));
	run->error = sslc_hs_message(&run->hs, message, size, &run->actions);
	expect_failed(run, EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);
	run_free(run);

	/* The secp256r1 key generation failing on the HelloRetryRequest is EIO, with no alert. */
	run = run_new(SSLC_TLS12, SSLC_TLS13, &pki256, "server.test");
	run->stream.fail_at = 4;
	CHECK(go(run) == 0);
	size = sh_write(&retry, message, sizeof(message));
	run->error = sslc_hs_message(&run->hs, message, size, &run->actions);
	expect_failed(run, EIO, 0);
	run_free(run);
	run_free(model);
}

/* Checks the messages a state does not take, the header checks and the change_cipher_spec rules. */
static void
test_states(void)
{
	static const uint8_t hello_request[4] = { 0, 0, 0, 0 };
	static const uint8_t hello_request_body[5] = { 0, 0, 0, 1, 0 };
	static const uint8_t finished[36] = { 0x14, 0, 0, 32 };
	static const uint8_t bad_length[8] = { 0x02, 0, 0, 5, 0, 0, 0, 0 };
	static const uint8_t short_length[8] = { 0x02, 0, 0, 3, 0, 0, 0, 0 };
	static const uint8_t long_request[5] = { 0, 0, 0, 0, 0 };
	uint8_t *short_message;
	static uint8_t big[SSLC_HANDSHAKE_MAX + 1U];
	struct sslc_hs_actions actions;
	struct sslc_hs hs;
	struct run *run;
	unsigned count;

	/* A HelloRequest before the ServerHello is ignored when TLS 1.2 may follow; with a body it is decode_error. */
	run = run_new(SSLC_TLS12, SSLC_TLS13, &pki256, "server.test");
	CHECK(go(run) == 0);
	run->actions.count = 5;
	CHECK(sslc_hs_message(&run->hs, hello_request, sizeof(hello_request), &run->actions) == 0);
	CHECK(run->actions.count == 0U);
	CHECK(run->hs.state == SSLC_HS_WAIT_SERVER_HELLO);
	run->error = sslc_hs_message(&run->hs, hello_request_body, sizeof(hello_request_body), &run->actions);
	expect_failed(run, EINVAL, SSLC_ALERT_DECODE_ERROR);
	run_free(run);
	run = run_new(SSLC_TLS13, SSLC_TLS13, &pki256, "server.test");
	CHECK(go(run) == 0);
	run->error = sslc_hs_message(&run->hs, hello_request, sizeof(hello_request), &run->actions);
	expect_failed(run, EPROTO, SSLC_ALERT_UNEXPECTED_MESSAGE);
	run_free(run);

	/* Another message before the ServerHello. */
	run = run_new(SSLC_TLS12, SSLC_TLS13, &pki256, "server.test");
	CHECK(go(run) == 0);
	run->error = sslc_hs_message(&run->hs, finished, sizeof(finished), &run->actions);
	expect_failed(run, EPROTO, SSLC_ALERT_UNEXPECTED_MESSAGE);
	run_free(run);

	/* A header whose length is not the rest, a message shorter than a header, NULL, one over the limit. */
	run = run_new(SSLC_TLS12, SSLC_TLS13, &pki256, "server.test");
	CHECK(go(run) == 0);
	run->error = sslc_hs_message(&run->hs, bad_length, sizeof(bad_length), &run->actions);
	expect_failed(run, EINVAL, SSLC_ALERT_DECODE_ERROR);
	run_free(run);
	run = run_new(SSLC_TLS12, SSLC_TLS13, &pki256, "server.test");
	CHECK(go(run) == 0);
	short_message = malloc(3);
	CHECK(short_message != NULL);
	memcpy(short_message, bad_length, 3);
	run->error = sslc_hs_message(&run->hs, short_message, 3, &run->actions);
	free(short_message);
	expect_failed(run, EINVAL, SSLC_ALERT_DECODE_ERROR);
	run_free(run);
	run = run_new(SSLC_TLS12, SSLC_TLS13, &pki256, "server.test");
	CHECK(go(run) == 0);
	run->error = sslc_hs_message(&run->hs, short_length, sizeof(short_length), &run->actions);
	expect_failed(run, EINVAL, SSLC_ALERT_DECODE_ERROR);
	run_free(run);
	run = run_new(SSLC_TLS12, SSLC_TLS13, &pki256, "server.test");
	CHECK(go(run) == 0);
	run->error = sslc_hs_message(&run->hs, long_request, sizeof(long_request), &run->actions);
	expect_failed(run, EINVAL, SSLC_ALERT_DECODE_ERROR);
	run_free(run);
	run = run_new(SSLC_TLS12, SSLC_TLS13, &pki256, "server.test");
	CHECK(go(run) == 0);
	run->error = sslc_hs_message(&run->hs, NULL, 40, &run->actions);
	expect_failed(run, EINVAL, SSLC_ALERT_DECODE_ERROR);
	run_free(run);
	big[0] = 0x0b;
	cryc_store_be24(big + 1, SSLC_HANDSHAKE_MAX - 3U);
	run = run_new(SSLC_TLS12, SSLC_TLS13, &pki256, "server.test");
	CHECK(go(run) == 0);
	run->error = sslc_hs_message(&run->hs, big, SSLC_HANDSHAKE_MAX + 1U, &run->actions);
	expect_failed(run, EINVAL, SSLC_ALERT_DECODE_ERROR);
	run_free(run);

	/* A handshake that never started takes no message and no change_cipher_spec. */
	memset(&hs, 0, sizeof(hs));
	CHECK(sslc_hs_message(&hs, finished, sizeof(finished), &actions) == EPROTO);
	CHECK(hs.alert == SSLC_ALERT_UNEXPECTED_MESSAGE);
	CHECK(hs.state == SSLC_HS_FAILED);
	memset(&hs, 0, sizeof(hs));
	CHECK(sslc_hs_change_cipher_spec(&hs, &actions) == EPROTO);
	CHECK(hs.alert == SSLC_ALERT_UNEXPECTED_MESSAGE);
	CHECK(sslc_hs_connected(&hs) == 0);
	sslc_hs_free(&hs);
	sslc_hs_free(&hs);
	sslc_hs_free(NULL);
	CHECK(sslc_hs_connected(NULL) == 0);
	CHECK(sslc_hs_message(NULL, finished, sizeof(finished), &actions) == EINVAL);
	CHECK(sslc_hs_change_cipher_spec(NULL, &actions) == EINVAL);
	memset(&hs, 0, sizeof(hs));
	hs.state = SSLC_HS_WAIT_SERVER_HELLO;
	CHECK(sslc_hs_change_cipher_spec(&hs, NULL) == EINVAL);
	CHECK(hs.state == SSLC_HS_FAILED);

	/* Any number of change_cipher_spec records before the ServerHello is dropped. */
	run = run_new(SSLC_TLS12, SSLC_TLS13, &pki256, "server.test");
	CHECK(go(run) == 0);
	for (count = 0; count < 40U; count++) {
		run->actions.count = 2;
		CHECK(sslc_hs_change_cipher_spec(&run->hs, &run->actions) == 0);
		CHECK(run->actions.count == 0U);
	}

	/* Checks that the state waits for the ServerHello. */
	CHECK(run->hs.state == SSLC_HS_WAIT_SERVER_HELLO);
	run_free(run);

	/* A client of TLS 1.2 alone takes none before the ServerHello (RFC 5246 7.1): unexpected_message. */
	run = run_new(SSLC_TLS12, SSLC_TLS12, &pki256, "server.test");
	CHECK(go(run) == 0);
	CHECK(sslc_hs_change_cipher_spec(&run->hs, &run->actions) == EPROTO);
	CHECK(run->hs.alert == SSLC_ALERT_UNEXPECTED_MESSAGE);
	CHECK(run->hs.state == SSLC_HS_FAILED);
	run_free(run);
}

/* Feeds every replayed item, with the change_cipher_spec records in between, and checks each state's refusals. */
static void
test_order(void)
{
	static const uint8_t finished[36] = { 0x14, 0, 0, 32 };
	static const uint8_t ticket[18] = { 0x04, 0, 0, 14, 0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 1, 0x55, 0, 0 };
	static const uint8_t key_update[5] = { 0x18, 0, 0, 1, 0 };
	static const uint8_t certificate_request[9] = { 0x0d, 0, 0, 5, 0, 0, 2, 0, 0 };
	struct run *run;
	static uint8_t longer[REPLAY_SIZE + 1U];
	size_t stop;
	size_t item;
	size_t other;

	/*
	 * Each state, before every message of the flight: a change_cipher_spec
	 * is dropped until the Finished, and every other message of the flight,
	 * earlier or later, is unexpected there.
	 */
	for (stop = 0; stop < replay_plain.count; stop++) {
		for (other = 0; other < replay_plain.count; other++) {
			if (other == stop)
				continue;
			run = run_new(SSLC_TLS12, SSLC_TLS13, &pki256, "server.test");
			CHECK(go(run) == 0);
			for (item = 0; item < stop; item++)
				CHECK(sslc_hs_message(&run->hs, replay_plain.data[item], replay_plain.size[item], &run->actions) == 0);
			CHECK(sslc_hs_change_cipher_spec(&run->hs, &run->actions) == 0);
			CHECK(run->actions.count == 0U);
			run->error = sslc_hs_message(&run->hs, replay_plain.data[other], replay_plain.size[other], &run->actions);
			expect_failed(run, EPROTO, SSLC_ALERT_UNEXPECTED_MESSAGE);
			run_free(run);
		}
	}

	/* A Finished one byte longer than the hash, its header made to fit. */
	memcpy(longer, replay_plain.data[4], replay_plain.size[4]);
	longer[replay_plain.size[4]] = 0;
	cryc_store_be24(longer + 1, (uint32_t)(replay_plain.size[4] - 3U));
	run = replay_run(&replay_plain, 4, longer, replay_plain.size[4] + 1U);
	expect_failed(run, EINVAL, SSLC_ALERT_DECODE_ERROR);
	run_free(run);

	/* After the handshake: no change_cipher_spec, no Finished, no CertificateRequest; a ticket and a KeyUpdate are taken. */
	run = replay_run(&replay_plain, (size_t)-1, NULL, 0);
	connected_then(run);
	run->error = sslc_hs_change_cipher_spec(&run->hs, &run->actions);
	expect_failed(run, EPROTO, SSLC_ALERT_UNEXPECTED_MESSAGE);
	run_free(run);
	run = replay_run(&replay_plain, (size_t)-1, NULL, 0);
	connected_then(run);
	CHECK(sslc_hs_message(&run->hs, ticket, sizeof(ticket), &run->actions) == 0);
	CHECK(run->actions.count == 0U);
	CHECK(sslc_hs_message(&run->hs, key_update, sizeof(key_update), &run->actions) == 0);
	CHECK(run->actions.count == 1U);
	CHECK(run->actions.list[0].kind == SSLC_HS_SET_READ_KEYS);
	run->error = sslc_hs_message(&run->hs, finished, sizeof(finished), &run->actions);
	expect_failed(run, EPROTO, SSLC_ALERT_UNEXPECTED_MESSAGE);
	run_free(run);
	run = replay_run(&replay_plain, (size_t)-1, NULL, 0);
	connected_then(run);
	run->error = sslc_hs_message(&run->hs, certificate_request, sizeof(certificate_request), &run->actions);
	expect_failed(run, EPROTO, SSLC_ALERT_UNEXPECTED_MESSAGE);
	run_free(run);

	/* A CertificateRequest is taken only before the Certificate: after it, it is unexpected. */
	run = replay_run(&replay_plain, 3, certificate_request, sizeof(certificate_request));
	expect_failed(run, EPROTO, SSLC_ALERT_UNEXPECTED_MESSAGE);
	run_free(run);
}

/* Checks the messages after the handshake against the peer: tickets and KeyUpdates both ways. */
static void
test_post_handshake(void)
{
	static const uint8_t bad_update[5] = { 0x18, 0, 0, 1, 2 };
	static const uint8_t long_update[6] = { 0x18, 0, 0, 2, 0, 0 };
	static const uint8_t hello_request[4] = { 0, 0, 0, 0 };
	static const uint8_t bad_ticket[8] = { 0x04, 0, 0, 4, 0, 0, 0, 1 };
	struct tlspeer_config config;
	struct run *run;
	size_t log_size;

	/* A ticket, a KeyUpdate that does not ask, a ticket under the new keys. */
	peer13(&config, &pki256);
	run = handshake(&config, &pki256, SSLC_TLS12, "server.test");
	check_connected(run, "SRCWSWRD");
	log_size = run->log_size;
	CHECK(tlspeer_send_ticket(run->peer) == 0);
	CHECK(tlspeer_send_key_update(run->peer, 0) == 0);
	CHECK(tlspeer_send_ticket(run->peer) == 0);
	pump(run);
	CHECK(run->error == 0);
	CHECK(strcmp(run->log + log_size, "R") == 0);
	CHECK(run->key_mismatch == 0U);

	/* A KeyUpdate that asks: the client moves its read keys, answers, then moves its write keys. */
	log_size = run->log_size;
	CHECK(tlspeer_send_key_update(run->peer, 1) == 0);
	CHECK(tlspeer_send_ticket(run->peer) == 0);
	pump(run);
	CHECK(run->error == 0);
	CHECK(strcmp(run->log + log_size, "RSW") == 0);
	CHECK(run->key_mismatch == 0U);
	CHECK(run->peer_error == 0);
	CHECK(run->peer->error == 0);
	CHECK(run->write_changes == 3U);

	/* Again, twice in a row. */
	CHECK(tlspeer_send_key_update(run->peer, 1) == 0);
	CHECK(tlspeer_send_key_update(run->peer, 1) == 0);
	CHECK(tlspeer_send_ticket(run->peer) == 0);
	pump(run);
	CHECK(run->error == 0);
	CHECK(run->key_mismatch == 0U);
	CHECK(run->peer->error == 0);
	CHECK(sslc_hs_connected(&run->hs) == 1);

	/* A KeyUpdate of another value, of another length; a HelloRequest; a ticket that does not parse. */
	connected_then(run);
	CHECK(tlspeer_send_handshake(run->peer, bad_update, sizeof(bad_update)) == 0);
	pump(run);
	expect_failed(run, EPROTO, SSLC_ALERT_ILLEGAL_PARAMETER);
	run_free(run);
	run = handshake(&config, &pki256, SSLC_TLS12, "server.test");
	connected_then(run);
	CHECK(tlspeer_send_handshake(run->peer, long_update, sizeof(long_update)) == 0);
	pump(run);
	expect_failed(run, EINVAL, SSLC_ALERT_DECODE_ERROR);
	run_free(run);
	run = handshake(&config, &pki256, SSLC_TLS12, "server.test");
	connected_then(run);
	CHECK(tlspeer_send_handshake(run->peer, hello_request, sizeof(hello_request)) == 0);
	pump(run);
	expect_failed(run, EPROTO, SSLC_ALERT_UNEXPECTED_MESSAGE);
	run_free(run);
	run = handshake(&config, &pki256, SSLC_TLS12, "server.test");
	connected_then(run);
	CHECK(tlspeer_send_handshake(run->peer, bad_ticket, sizeof(bad_ticket)) == 0);
	pump(run);
	expect_failed(run, EINVAL, SSLC_ALERT_DECODE_ERROR);
	run_free(run);
}

/* Checks sslc_hs_queue_message and sslc_hs_queue_action directly. */
static void
test_queue(void)
{
	static uint8_t message[SSLC_HS_OUT_MAX];
	static const uint8_t hello[8] = { 0x01, 0, 0, 4, 1, 2, 3, 4 };
	static const uint8_t finished[8] = { 0x14, 0, 0, 4, 5, 6, 7, 8 };
	struct sslc_traffic_keys keys;
	struct sslc_transcript transcript;
	uint8_t got[CRYC_HASH_MAX_SIZE];
	uint8_t expected[CRYC_HASH_MAX_SIZE];
	struct run *run;
	unsigned index;

	/* Two messages in a row share one SEND; the transcript takes both. */
	run = run_new(SSLC_TLS12, SSLC_TLS13, &pki256, "server.test");
	CHECK(go(run) == 0);
	sslc_transcript_init(&run->hs.transcript);
	sslc_transcript_init(&transcript);
	run->actions.count = 0;
	CHECK(sslc_hs_queue_message(&run->hs, &run->actions, finished, sizeof(finished)) == 0);
	CHECK(sslc_hs_queue_message(&run->hs, &run->actions, finished, sizeof(finished)) == 0);
	CHECK(run->actions.count == 1U);
	CHECK(run->actions.list[0].offset == 0U);
	CHECK(run->actions.list[0].size == 16U);
	CHECK(run->actions.list[0].first_hello == 0);
	CHECK_BYTES(run->hs.out + 8, finished, 8);
	sslc_transcript_update(&transcript, finished, 8);
	sslc_transcript_update(&transcript, finished, 8);
	CHECK(sslc_transcript_select(&transcript, CRYC_HASH_SHA256) == 0);
	CHECK(sslc_transcript_select(&run->hs.transcript, CRYC_HASH_SHA256) == 0);
	CHECK(sslc_transcript_hash(&run->hs.transcript, got) == 32U);
	CHECK(sslc_transcript_hash(&transcript, expected) == 32U);
	CHECK_BYTES(got, expected, 32);

	/* An action between them ends the SEND; the next takes its own, after the bytes before. */
	CHECK(sslc_hs_queue_action(&run->actions, SSLC_HS_SEND_CCS, 0, NULL, 0) == 0);
	CHECK(sslc_hs_queue_message(&run->hs, &run->actions, finished, sizeof(finished)) == 0);
	CHECK(run->actions.count == 3U);
	CHECK(run->actions.list[2].kind == SSLC_HS_SEND);
	CHECK(run->actions.list[2].offset == 16U);
	CHECK(run->actions.list[2].size == 8U);

	/* A ClientHello before a retry is first_hello and never joins; after a retry it is not. */
	run->actions.count = 0;
	CHECK(sslc_hs_queue_message(&run->hs, &run->actions, finished, sizeof(finished)) == 0);
	CHECK(sslc_hs_queue_message(&run->hs, &run->actions, hello, sizeof(hello)) == 0);
	CHECK(run->actions.count == 2U);
	CHECK(run->actions.list[1].first_hello == 1);
	CHECK(sslc_hs_queue_message(&run->hs, &run->actions, finished, sizeof(finished)) == 0);
	CHECK(run->actions.count == 3U);
	run->hs.retried = 1;
	run->actions.count = 0;
	CHECK(sslc_hs_queue_message(&run->hs, &run->actions, finished, sizeof(finished)) == 0);
	CHECK(sslc_hs_queue_message(&run->hs, &run->actions, hello, sizeof(hello)) == 0);
	CHECK(run->actions.count == 1U);
	CHECK(run->actions.list[0].first_hello == 0);

	/* hs->out holds SSLC_HS_OUT_MAX bytes and no more; a message shorter than its header is refused. */
	run->actions.count = 0;
	memset(message, 0, sizeof(message));
	message[0] = 0x14;
	CHECK(sslc_hs_queue_message(&run->hs, &run->actions, message, SSLC_HS_OUT_MAX - 4U) == 0);
	CHECK(sslc_hs_queue_message(&run->hs, &run->actions, finished, 4) == 0);
	CHECK(sslc_hs_queue_message(&run->hs, &run->actions, finished, 4) == ENOBUFS);
	CHECK(run->actions.list[0].size == SSLC_HS_OUT_MAX);
	run->actions.count = 0;
	CHECK(sslc_hs_queue_message(&run->hs, &run->actions, message, SSLC_HS_OUT_MAX + 1U) == ENOBUFS);
	CHECK(sslc_hs_queue_message(&run->hs, &run->actions, finished, 3) == EINVAL);
	CHECK(sslc_hs_queue_message(&run->hs, &run->actions, NULL, 8) == EINVAL);
	CHECK(run->actions.count == 0U);

	/* The list holds SSLC_HS_MAX_ACTIONS; key actions copy their keys and need them. */
	memset(&keys, 0x5a, sizeof(keys));
	keys.suite = sslc_tls_suite(0x1301);
	keys.key_size = 16;
	keys.iv_size = 12;
	run->actions.count = 0;
	CHECK(sslc_hs_queue_action(&run->actions, SSLC_HS_SET_READ_KEYS, 1, NULL, 0) == EINVAL);
	CHECK(sslc_hs_queue_action(&run->actions, SSLC_HS_SET_WRITE_KEYS, 1, NULL, 0) == EINVAL);
	CHECK(sslc_hs_queue_action(&run->actions, SSLC_HS_SET_READ_KEYS, 1, &keys, 0) == 0);
	keys.key[0] = 0;
	CHECK(run->actions.list[0].keys.key[0] == 0x5a);
	CHECK(run->actions.list[0].version13 == 1);
	CHECK(sslc_hs_queue_action(&run->actions, SSLC_HS_SEND_WARNING, 0, NULL, SSLC_ALERT_NO_RENEGOTIATION) == 0);
	CHECK(run->actions.list[1].alert == SSLC_ALERT_NO_RENEGOTIATION);
	CHECK(run->actions.list[1].keys.key_size == 0U);
	for (index = 2; index < SSLC_HS_MAX_ACTIONS; index++)
		CHECK(sslc_hs_queue_action(&run->actions, SSLC_HS_DONE, 0, NULL, 0) == 0);
	CHECK(sslc_hs_queue_action(&run->actions, SSLC_HS_DONE, 0, NULL, 0) == ENOBUFS);
	CHECK(sslc_hs_queue_message(&run->hs, &run->actions, finished, sizeof(finished)) == ENOBUFS);
	CHECK(run->actions.count == SSLC_HS_MAX_ACTIONS);
	run_free(run);
}

/* Checks sslc_hs_shared_secret with the RFC 7748 and CAVP values and the bad shares. */
static void
test_shared_secret(void)
{
	uint8_t share[CRYC_ECP_MAX_POINT];
	uint8_t shared[SSLC_HS_SHARED_MAX];
	uint8_t expected[SSLC_HS_SHARED_MAX];
	uint8_t point[CRYC_ECP_MAX_POINT];
	uint8_t point384[CRYC_ECP_MAX_POINT];
	struct run *run;
	size_t shared_size;

	/* x25519: Alice's key with Bob's share gives K (32 bytes). */
	run = run_new(SSLC_TLS12, SSLC_TLS13, &pki256, "server.test");
	CHECK(go(run) == 0);
	CHECK(hex_decode(x25519_alice, run->hs.x25519_private, 32) == 32U);
	CHECK(hex_decode(x25519_bob_public, share, 32) == 32U);
	CHECK(hex_decode(x25519_shared, expected, 32) == 32U);
	shared_size = 99;
	CHECK(sslc_hs_shared_secret(&run->hs, SSLC_GROUP_X25519, share, 32, shared, &shared_size) == 0);
	CHECK(shared_size == 32U);
	CHECK_BYTES(shared, expected, 32);

	/* x25519: a wrong length, a missing share, the zero point (shared wiped, size 0). */
	CHECK(sslc_hs_shared_secret(&run->hs, SSLC_GROUP_X25519, share, 31, shared, &shared_size) == EPROTO);
	CHECK(run->hs.alert == SSLC_ALERT_ILLEGAL_PARAMETER);
	CHECK(all_zero(shared, sizeof(shared)));
	CHECK(shared_size == 0U);
	CHECK(sslc_hs_shared_secret(&run->hs, SSLC_GROUP_X25519, NULL, 32, shared, &shared_size) == EPROTO);
	memset(share, 0, 32);
	memset(shared, 0xee, sizeof(shared));
	CHECK(sslc_hs_shared_secret(&run->hs, SSLC_GROUP_X25519, share, 32, shared, &shared_size) == EPROTO);
	CHECK(all_zero(shared, sizeof(shared)));

	/* secp256r1 and secp384r1 without a key pair of our own, and a group never offered. */
	CHECK(hex_decode(p256_peer_x, point + 1, 32) == 32U);
	CHECK(hex_decode(p256_peer_y, point + 33, 32) == 32U);
	point[0] = 0x04;
	CHECK(hex_decode(p384_peer_x, point384 + 1, 48) == 48U);
	CHECK(hex_decode(p384_peer_y, point384 + 49, 48) == 48U);
	point384[0] = 0x04;
	run->hs.alert = 0;
	CHECK(sslc_hs_shared_secret(&run->hs, SSLC_GROUP_SECP256R1, point, 65, shared, &shared_size) == EPROTO);
	CHECK(run->hs.alert == SSLC_ALERT_ILLEGAL_PARAMETER);
	run->hs.alert = 0;
	CHECK(sslc_hs_shared_secret(&run->hs, SSLC_GROUP_SECP384R1, point384, 97, shared, &shared_size) == EPROTO);
	CHECK(run->hs.alert == SSLC_ALERT_ILLEGAL_PARAMETER);
	run->hs.alert = 0;
	CHECK(sslc_hs_shared_secret(&run->hs, 0x0019, point384, 97, shared, &shared_size) == EPROTO);
	CHECK(run->hs.alert == SSLC_ALERT_ILLEGAL_PARAMETER);
	run->hs.alert = 0;
	CHECK(sslc_hs_shared_secret(&run->hs, 0x0100, point, 65, shared, &shared_size) == EPROTO);
	CHECK(run->hs.alert == SSLC_ALERT_ILLEGAL_PARAMETER);

	/* secp256r1 with the CAVP key: dIUT with QCAVS gives ZIUT (32 bytes). */
	run->hs.ec = malloc(sizeof(*run->hs.ec));
	CHECK(run->hs.ec != NULL);
	CHECK(cryc_ecp_init(run->hs.ec, CRYC_CURVE_P256) == 0);
	CHECK(hex_decode(p256_private, run->hs.ec_private, 32) == 32U);
	run->hs.ec_public[0] = 0x04;
	CHECK(hex_decode(p256_public_x, run->hs.ec_public + 1, 32) == 32U);
	CHECK(hex_decode(p256_public_y, run->hs.ec_public + 33, 32) == 32U);
	run->hs.ec_public_size = 65;
	run->hs.ec_group = SSLC_GROUP_SECP256R1;
	CHECK(sslc_hs_shared_secret(&run->hs, SSLC_GROUP_SECP256R1, point, 65, shared, &shared_size) == EPROTO);
	run->hs.has_ec = 1;
	CHECK(hex_decode(p256_shared, expected, 32) == 32U);
	CHECK(sslc_hs_shared_secret(&run->hs, SSLC_GROUP_SECP256R1, point, 65, shared, &shared_size) == 0);
	CHECK(shared_size == 32U);
	CHECK_BYTES(shared, expected, 32);

	/* A key pair of one group does not answer a share of the other. */
	CHECK(sslc_hs_shared_secret(&run->hs, SSLC_GROUP_SECP384R1, point384, 97, shared, &shared_size) == EPROTO);
	CHECK(all_zero(shared, sizeof(shared)));

	/* secp256r1: off the curve, compressed, cut short, infinity. */
	point[64] ^= 1U;
	CHECK(sslc_hs_shared_secret(&run->hs, SSLC_GROUP_SECP256R1, point, 65, shared, &shared_size) == EPROTO);
	CHECK(all_zero(shared, sizeof(shared)));
	point[64] ^= 1U;
	point[0] = 0x02;
	CHECK(sslc_hs_shared_secret(&run->hs, SSLC_GROUP_SECP256R1, point, 33, shared, &shared_size) == EPROTO);
	point[0] = 0x04;
	CHECK(sslc_hs_shared_secret(&run->hs, SSLC_GROUP_SECP256R1, point, 64, shared, &shared_size) == EPROTO);
	CHECK(sslc_hs_shared_secret(&run->hs, SSLC_GROUP_SECP256R1, NULL, 65, shared, &shared_size) == EPROTO);
	point[0] = 0x00;
	CHECK(sslc_hs_shared_secret(&run->hs, SSLC_GROUP_SECP256R1, point, 1, shared, &shared_size) == EPROTO);

	/* secp384r1 with the CAVP key: dIUT with QCAVS gives ZIUT (48 bytes). */
	CHECK(cryc_ecp_init(run->hs.ec, CRYC_CURVE_P384) == 0);
	CHECK(hex_decode(p384_private, run->hs.ec_private, 48) == 48U);
	run->hs.ec_public[0] = 0x04;
	CHECK(hex_decode(p384_public_x, run->hs.ec_public + 1, 48) == 48U);
	CHECK(hex_decode(p384_public_y, run->hs.ec_public + 49, 48) == 48U);
	run->hs.ec_public_size = 97;
	run->hs.ec_group = SSLC_GROUP_SECP384R1;
	CHECK(hex_decode(p384_shared, expected, 48) == 48U);
	CHECK(sslc_hs_shared_secret(&run->hs, SSLC_GROUP_SECP384R1, point384, 97, shared, &shared_size) == 0);
	CHECK(shared_size == 48U);
	CHECK_BYTES(shared, expected, 48);

	/* secp384r1: off the curve, a secp256r1-sized point, the other group. */
	point384[96] ^= 1U;
	CHECK(sslc_hs_shared_secret(&run->hs, SSLC_GROUP_SECP384R1, point384, 97, shared, &shared_size) == EPROTO);
	CHECK(all_zero(shared, sizeof(shared)));
	CHECK(shared_size == 0U);
	point384[96] ^= 1U;
	CHECK(sslc_hs_shared_secret(&run->hs, SSLC_GROUP_SECP384R1, point384, 65, shared, &shared_size) == EPROTO);
	point[0] = 0x04;
	CHECK(sslc_hs_shared_secret(&run->hs, SSLC_GROUP_SECP256R1, point, 65, shared, &shared_size) == EPROTO);
	run_free(run);
}

/* Checks sslc_hs_certificate directly: a second Certificate, a context, an empty list. */
static void
test_certificate_direct(void)
{
	static uint8_t body[4096];
	static uint8_t with_context[4096];
	struct tlspeer_writer writer;
	struct run *run;
	size_t size;

	/* A TLS 1.3 Certificate body of the P-256 leaf, with an empty context. */
	tlspeer_writer_init(&writer, body, sizeof(body));
	tlspeer_begin(&writer, 1);
	tlspeer_end(&writer);
	tlspeer_begin(&writer, 3);
	tlspeer_begin(&writer, 3);
	tlspeer_put(&writer, pki256.leaf, pki256.leaf_size);
	tlspeer_end(&writer);
	tlspeer_put_int(&writer, 0, 2);
	tlspeer_end(&writer);
	size = writer.used;
	CHECK(writer.error == 0);

	/* Taken once, with a valid chain; a second is unexpected. */
	run = run_new(SSLC_TLS12, SSLC_TLS13, &pki256, "server.test");
	CHECK(go(run) == 0);
	CHECK(sslc_hs_certificate(&run->hs, 1, body, size) == 0);
	CHECK(run->hs.peer_count == 1U);
	CHECK(run->hs.peer_der_size[0] == pki256.leaf_size);
	CHECK_BYTES(run->hs.peer_der[0], pki256.leaf, pki256.leaf_size);
	CHECK(run->hs.verify_code == CRYC_VERIFY_OK);
	CHECK(sslc_hs_certificate(&run->hs, 1, body, size) == EPROTO);
	CHECK(run->hs.alert == SSLC_ALERT_UNEXPECTED_MESSAGE);
	run_free(run);

	/* A body cut short (the parser's decode_error), then one with a context of one byte. */
	run = run_new(SSLC_TLS12, SSLC_TLS13, &pki256, "server.test");
	CHECK(go(run) == 0);
	CHECK(sslc_hs_certificate(&run->hs, 1, body, size - 1U) == EINVAL);
	CHECK(run->hs.alert == SSLC_ALERT_DECODE_ERROR);
	CHECK(run->hs.peer_count == 0U);
	with_context[0] = 1;
	with_context[1] = 0x42;
	memcpy(with_context + 2, body + 1, size - 1U);
	CHECK(sslc_hs_certificate(&run->hs, 1, with_context, size + 1U) == EPROTO);
	CHECK(run->hs.alert == SSLC_ALERT_ILLEGAL_PARAMETER);
	CHECK(run->hs.peer_count == 0U);

	/* The TLS 1.2 form of the same list is not a TLS 1.3 body. */
	CHECK(sslc_hs_certificate(&run->hs, 1, body + 1, size - 1U) == EINVAL);
	run_free(run);
}

/*
 * Feeds the recorded flights again with each server message cut short
 * (its length fixed and not), with each byte altered, and with random
 * bytes: the client must fail without a fault (ASan and UBSan watch) and
 * never finish with an altered transcript.
 */
static void
test_altered(
	const struct replay *replay,
	unsigned step)
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

		/* Cut without fixing the header: always decode_error. */
		run = replay_run(replay, index, replay->data[index], replay->size[index] - 1U);
		CHECK(run->error == EINVAL);
		CHECK(run->hs.alert == SSLC_ALERT_DECODE_ERROR);
		run_free(run);
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

	/* The recorded flights: the plain 1-RTT one and the one after a HelloRetryRequest. */
	peer13(&config, &pki256);
	record_replay(&config, &replay_plain);
	CHECK(replay_plain.count == 5U);
	peer13(&config, &pki256);
	config.group = SSLC_GROUP_SECP256R1;
	config.hello_retry = 1;
	config.options = TLSPEER_OPT_CCS;
	record_replay(&config, &replay_retry);
	CHECK(replay_retry.count == 7U);

	/* The checks. */
	test_client_hello();
	test_config();
	test_handshakes();
	test_breaks();
	test_certificate_verify();
	test_certificates();
	test_server_hello();
	test_retry();
	test_states();
	test_order();
	test_post_handshake();
	test_queue();
	test_shared_secret();
	test_certificate_direct();
	test_altered(&replay_plain, 1);
	test_altered(&replay_retry, 3);
	return check_finish("test-hs13");
}
