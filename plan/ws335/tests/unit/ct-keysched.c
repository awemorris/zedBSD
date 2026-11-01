/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The constant-time checks of the key schedules (sslc-keysched.h): the
 * (EC)DHE shared secret, the pre-master secret and every secret derived
 * from them are marked secret, so memcheck reports any branch or address
 * that depends on their bytes.  The transcript hashes, randoms, labels and
 * sizes are public.  The secrets are carried through the whole chain
 * (handshake, master, keys, Finished, update; pre-master, master, key
 * block, Finished) without being marked public in between.
 */

#include "check.h"
#include "ct.h"

#include "userland/base/libssl-compat/sslc-keysched.h"

/* Fills size bytes at out with a fixed pattern. */
static void
fill(
	uint8_t *out,
	size_t size,
	unsigned seed)
{
	size_t index;

	/* One byte at a time. */
	for (index = 0; index < size; index++)
		out[index] = (uint8_t)(seed + index * 7U + (index >> 2));
}

/* Folds a public output into one value, to keep the call from being dropped. */
static unsigned
fold(
	const uint8_t *bytes,
	size_t size)
{
	unsigned value;
	size_t index;

	/* Exclusive-or of the bytes, rotated. */
	value = 0;
	for (index = 0; index < size; index++)
		value = ((value << 1) | (value >> 7)) ^ bytes[index];
	return value & 0xffU;
}

/* Runs the TLS 1.3 schedule for one suite from a secret shared secret. */
static unsigned
run_tls13(
	uint16_t suite_id)
{
	struct sslc_tls13_schedule schedule;
	struct sslc_traffic_keys keys;
	const struct sslc_suite *suite;
	uint8_t shared[32];
	uint8_t transcript_hash[CRYC_HASH_MAX_SIZE];
	uint8_t verify[CRYC_HASH_MAX_SIZE];
	uint8_t label_out[40];
	size_t size;
	unsigned total;

	/* The suite and the public inputs. */
	suite = sslc_tls_suite(suite_id);
	CHECK(suite != NULL);
	if (suite == NULL)
		return 0;
	size = cryc_hash_size(suite->hash);
	fill(transcript_hash, sizeof(transcript_hash), 11U);
	fill(shared, sizeof(shared), 3U);
	CT_SECRET(shared, sizeof(shared));
	total = 0;

	/* The schedule from the shared secret to the application secrets. */
	CHECK(sslc_tls13_start(&schedule, suite->hash) == 0);
	CHECK(sslc_tls13_handshake(&schedule, shared, sizeof(shared), transcript_hash) == 0);
	CHECK(sslc_tls13_master(&schedule, transcript_hash) == 0);

	/* Keys and Finished from the secret traffic secrets, then the update. */
	CHECK(sslc_tls13_traffic_keys(suite->hash, suite, schedule.client_handshake, &keys) == 0);
	CT_PUBLIC(keys.key, keys.key_size);
	CT_PUBLIC(keys.iv, keys.iv_size);
	total += fold(keys.key, keys.key_size) + fold(keys.iv, keys.iv_size);
	CHECK(sslc_tls13_finished(suite->hash, schedule.server_handshake, transcript_hash, verify) == 0);
	CT_PUBLIC(verify, size);
	total += fold(verify, size);
	CHECK(sslc_tls13_update_secret(suite->hash, schedule.client_application) == 0);
	CHECK(sslc_tls13_update_secret(suite->hash, schedule.client_application) == 0);
	CT_PUBLIC(schedule.client_application, size);
	CT_PUBLIC(schedule.server_application, size);
	total += fold(schedule.client_application, size) + fold(schedule.server_application, size);

	/* A secret in an HKDF-Expand-Label of its own, with a public context. */
	CT_SECRET(schedule.server_handshake, size);
	CHECK(sslc_tls13_expand_label(suite->hash, schedule.server_handshake, "key", transcript_hash, size, label_out, sizeof(label_out)) == 0);
	CT_PUBLIC(label_out, sizeof(label_out));
	total += fold(label_out, sizeof(label_out));
	CT_PUBLIC(shared, sizeof(shared));
	CT_PUBLIC(schedule.server_handshake, size);
	sslc_tls13_clear(&schedule);
	return total;
}

/* Runs the TLS 1.2 chain for one suite from a secret pre-master secret. */
static unsigned
run_tls12(
	uint16_t suite_id)
{
	struct sslc_traffic_keys client;
	struct sslc_traffic_keys server;
	const struct sslc_suite *suite;
	uint8_t pre_master[32];
	uint8_t master[SSLC_TLS12_MASTER];
	uint8_t randoms[64];
	uint8_t session_hash[CRYC_HASH_MAX_SIZE];
	uint8_t verify[SSLC_TLS12_VERIFY_DATA];
	uint8_t prf_out[100];
	size_t size;
	unsigned total;
	int extended;

	/* The suite and the public inputs. */
	suite = sslc_tls_suite(suite_id);
	CHECK(suite != NULL);
	if (suite == NULL)
		return 0;
	size = cryc_hash_size(suite->hash);
	fill(randoms, sizeof(randoms), 5U);
	fill(session_hash, sizeof(session_hash), 21U);
	total = 0;

	/* Both forms of the master secret, then the key block and the Finished messages from it. */
	for (extended = 0; extended < 2; extended++) {
		fill(pre_master, sizeof(pre_master), 13U + (unsigned)extended);
		CT_SECRET(pre_master, sizeof(pre_master));
		if (extended)
			CHECK(sslc_tls12_master(suite->hash, 1, pre_master, sizeof(pre_master), session_hash, size, master) == 0);
		else
			CHECK(sslc_tls12_master(suite->hash, 0, pre_master, sizeof(pre_master), randoms, sizeof(randoms), master) == 0);
		CHECK(sslc_tls12_key_block(suite, master, randoms, randoms + 32, &client, &server) == 0);
		CT_PUBLIC(client.key, client.key_size);
		CT_PUBLIC(client.iv, client.iv_size);
		CT_PUBLIC(server.key, server.key_size);
		CT_PUBLIC(server.iv, server.iv_size);
		total += fold(client.key, client.key_size) + fold(server.key, server.key_size);
		total += fold(client.iv, client.iv_size) + fold(server.iv, server.iv_size);
		CHECK(sslc_tls12_finished(suite->hash, master, 1, session_hash, verify) == 0);
		CT_PUBLIC(verify, sizeof(verify));
		total += fold(verify, sizeof(verify));
		CHECK(sslc_tls12_prf(suite->hash, master, sizeof(master), "key expansion", randoms, sizeof(randoms), prf_out, sizeof(prf_out)) == 0);
		CT_PUBLIC(prf_out, sizeof(prf_out));
		total += fold(prf_out, sizeof(prf_out));
		CT_PUBLIC(pre_master, sizeof(pre_master));
		CT_PUBLIC(master, sizeof(master));
	}
	return total;
}

/* Runs the checks. */
int
main(void)
{
	unsigned total;

	/* Every suite of both versions, so both hashes and the three key sizes. */
	total = 0;
	total += run_tls13(SSLC_SUITE_AES_128_GCM_SHA256);
	total += run_tls13(SSLC_SUITE_AES_256_GCM_SHA384);
	total += run_tls13(SSLC_SUITE_CHACHA20_POLY1305_SHA256);
	total += run_tls12(SSLC_SUITE_ECDHE_RSA_WITH_AES_128_GCM_SHA256);
	total += run_tls12(SSLC_SUITE_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384);
	total += run_tls12(SSLC_SUITE_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256);

	/* The folded value is used so that no call can be dropped. */
	CHECK(total < 1000000U);

	/* The result. */
	return check_finish("ct-keysched");
}
