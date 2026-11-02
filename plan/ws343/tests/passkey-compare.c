/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The driver of the comparison of libpasskey's cryptography on OpenSSL's
 * libcrypto and on libcrypto-compat (run-passkey-compare.sh builds it
 * twice with the same, unchanged crypto-openssl.c).  It calls every
 * pk_crypto_* function on the same inputs and prints one line per call:
 * the answer and every output that does not depend on random bytes.  The
 * two outputs must be the same, line for line.
 *
 *   passkey-compare            the deterministic calls (to compare)
 *   passkey-compare ecdh       ECDH with a peer whose private key is
 *                              known: our point and the shared secret,
 *                              one line each round (to check)
 *   passkey-compare check-ecdh read those lines on standard input and
 *                              check each shared secret with OpenSSL's
 *                              own derivation (OpenSSL build only)
 *
 * The inputs: a fixed pseudo-random sequence, the CAVP ECDSA PKV file,
 * and Wycheproof's ecdsa_secp256r1_sha256 and ecdh_secp256r1_ecpoint files
 * ($WS335_VECTORS/rec).
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "userland/base/libpasskey/crypto.h"

#include "check.h"
#include "vecfile.h"

#ifdef PASSKEY_COMPARE_OPENSSL
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/params.h>
#endif

/*
 * A P-256 key pair whose private key is known: RFC 6979 A.2.5's (x, and
 * U = xG).
 */
#ifdef PASSKEY_COMPARE_OPENSSL
static const char known_private[] = "c9afa9d845ba75166b5c215767b1d6934e50c3db36e89b127b8a622b120f6721";
#endif
static const char known_x[] = "60fed4ba255a9d31c961eb74c6356d68c049b8923b61fa6ce669622e60f29fb6";
static const char known_y[] = "7903fe1008b8bc99a41ae9e95628bc64f2f1b20c2d7e9f5177a3c294d4462299";

/* The state of the fixed pseudo-random sequence. */
static unsigned long sequence = 12345UL;

/* Returns the next byte of the fixed sequence. */
static uint8_t
next_byte(void)
{
	/* A linear congruential generator: only reproducibility matters. */
	sequence = sequence * 1103515245UL + 12345UL;
	return (uint8_t)((sequence >> 16) & 0xffUL);
}

/* Fills size bytes from the fixed sequence. */
static void
fill(
	uint8_t *out,
	size_t size)
{
	size_t index;

	/* One byte at a time. */
	for (index = 0U; index < size; index++)
		out[index] = next_byte();
}

/* Prints a label, an answer and size bytes in hex. */
static void
show(
	const char *label,
	int answer,
	const uint8_t *bytes,
	size_t size)
{
	size_t index;

	/* One line. */
	printf("%s %d ", label, answer);
	for (index = 0U; index < size; index++)
		printf("%02x", bytes[index]);
	printf("\n");
}

/* SHA-256 of 0 to 3 parts of various sizes. */
static void
compare_sha256(void)
{
	struct pk_crypto_part parts[3];
	uint8_t data[3][300];
	uint8_t hash[PK_SHA256_SIZE];
	char label[64];
	size_t count;
	size_t round;
	size_t part;
	int answer;

	for (round = 0U; round < 40U; round++) {
		count = round % 4U;
		for (part = 0U; part < count; part++) {
			parts[part].size = (round * 37U + part * 61U) % 300U;
			fill(data[part], parts[part].size);
			parts[part].data = data[part];
		}
		memset(hash, 0, sizeof(hash));
		answer = pk_crypto_sha256(parts, count, hash);
		sprintf(label, "sha256.%lu", (unsigned long)round);
		show(label, answer, hash, sizeof(hash));
	}
}

/* HMAC-SHA-256 with keys of 0 to 150 bytes and messages of 0 to 300. */
static void
compare_hmac(void)
{
	uint8_t key[150];
	uint8_t data[300];
	uint8_t mac[PK_SHA256_SIZE];
	char label[64];
	size_t key_size;
	size_t size;
	int answer;

	for (key_size = 0U; key_size < 150U; key_size += 7U) {
		size = (key_size * 13U) % 300U;
		fill(key, key_size);
		fill(data, size);
		memset(mac, 0, sizeof(mac));
		answer = pk_crypto_hmac_sha256(key, key_size, data, size, mac);
		sprintf(label, "hmac.%lu.%lu", (unsigned long)key_size, (unsigned long)size);
		show(label, answer, mac, sizeof(mac));
	}
}

/* Random bytes: the answer, and whether a long output is not all zeros. */
static void
compare_random(void)
{
	static const size_t sizes[4] = { 0U, 1U, 32U, 1000U };
	uint8_t bytes[1000];
	size_t round;
	size_t index;
	int answer;
	int nonzero;

	for (round = 0U; round < 4U; round++) {
		memset(bytes, 0, sizeof(bytes));
		answer = pk_crypto_random(bytes, sizes[round]);
		nonzero = 0;
		for (index = 0U; index < sizes[round]; index++) {
			if (bytes[index] != 0U)
				nonzero = 1;
		}
		printf("random.%lu %d %d\n", (unsigned long)sizes[round], answer, sizes[round] >= 32U ? nonzero : -1);
	}
}

/* Puts a hex coordinate into 32 bytes; 0 when it does not fit. */
static int
coordinate(
	const char *text,
	uint8_t *out)
{
	char padded[80];
	uint8_t bytes[40];
	size_t length;
	size_t size;
	size_t skip;

	/* An odd number of digits gets a leading zero. */
	length = strlen(text);
	if (length + 2U > sizeof(padded))
		return 0;
	padded[0] = '0';
	memcpy(padded + 1, text, length + 1U);
	size = hex_decode(length % 2U != 0U ? padded : padded + 1, bytes, sizeof(bytes));
	if (size == (size_t)-1)
		return 0;
	for (skip = 0U; skip < size && size - skip > 32U && bytes[skip] == 0U; skip++)
		;
	if (size - skip > 32U)
		return 0;
	memset(out, 0, 32U);
	memcpy(out + 32U - (size - skip), bytes + skip, size - skip);
	return 1;
}

/* Point validity: the P-256 records of the PKV file, their changed forms, and the base point. */
static void
compare_valid(void)
{
	struct vec_file file;
	uint8_t x[32];
	uint8_t y[32];
	const char *qx;
	const char *qy;
	unsigned long record;
	int answer;

	/* The records (the converted file lists every section seen so far: a P-256 one has none later). */
	record = 0UL;
	if (vec_open(&file, "cavp-186-4ecdsatestvectors-PKV") != 0) {
		printf("valid FAIL no vectors\n");
		return;
	}
	while (vec_next(&file)) {
		qx = vec_get(&file, "Qx");
		qy = vec_get(&file, "Qy");
		if (vec_get(&file, "section.P-256") == NULL || vec_get(&file, "section.P-384") != NULL || qx == NULL || qy == NULL)
			continue;
		record++;
		if (!coordinate(qx, x) || !coordinate(qy, y)) {
			printf("valid.pkv.%lu wide\n", record);
			continue;
		}
		answer = pk_crypto_p256_valid(x, y);
		printf("valid.pkv.%lu %d\n", record, answer);

		/* The same point with one bit of y changed. */
		y[31] ^= 1U;
		answer = pk_crypto_p256_valid(x, y);
		printf("valid.pkv.%lu.flip %d\n", record, answer);
	}
	vec_close(&file);

	/* The known point, and (0, 0), and all ones. */
	hex_decode(known_x, x, sizeof(x));
	hex_decode(known_y, y, sizeof(y));
	printf("valid.known %d\n", pk_crypto_p256_valid(x, y));
	memset(x, 0, sizeof(x));
	memset(y, 0, sizeof(y));
	printf("valid.zero %d\n", pk_crypto_p256_valid(x, y));
	memset(x, 0xff, sizeof(x));
	memset(y, 0xff, sizeof(y));
	printf("valid.ones %d\n", pk_crypto_p256_valid(x, y));
}

/* Signature verification: every case of Wycheproof's ecdsa_secp256r1_sha256 file. */
static void
compare_verify(void)
{
	struct vec_file file;
	struct pk_crypto_part part;
	uint8_t point[200];
	uint8_t msg[2000];
	uint8_t sig[600];
	uint8_t hash[PK_SHA256_SIZE];
	const char *id;
	size_t size;
	size_t point_size;
	size_t msg_size;
	size_t sig_size;
	int answer;

	point_size = 0U;
	if (vec_open(&file, "wycheproof-ecdsa_secp256r1_sha256_test") != 0) {
		printf("verify FAIL no vectors\n");
		return;
	}
	while (vec_next(&file)) {
		/* The group's key comes in a record of its own; the tests after it use it. */
		size = vec_hex(&file, "group.publicKey.uncompressed", point, sizeof(point));
		if (size != (size_t)-1)
			point_size = size;
		id = vec_get(&file, "tcId");
		msg_size = vec_hex(&file, "msg", msg, sizeof(msg));
		sig_size = vec_hex(&file, "sig", sig, sizeof(sig));
		if (id == NULL || point_size != 65U || msg_size == (size_t)-1 || sig_size == (size_t)-1)
			continue;
		part.data = msg;
		part.size = msg_size;
		pk_crypto_sha256(&part, 1U, hash);
		answer = pk_crypto_p256_verify(point + 1, point + 33, hash, sig, sig_size);
		printf("verify.%s %d\n", id, answer);

		/* The same signature over another hash. */
		hash[0] ^= 0x80U;
		answer = pk_crypto_p256_verify(point + 1, point + 33, hash, sig, sig_size);
		printf("verify.%s.other %d\n", id, answer);
	}
	vec_close(&file);
}

/* ECDH: the answer for every uncompressed peer point of Wycheproof's ecdh file, and whether our point is valid. */
static void
compare_ecdh(void)
{
	struct vec_file file;
	uint8_t pub[200];
	uint8_t own_x[32];
	uint8_t own_y[32];
	uint8_t shared[32];
	const char *id;
	size_t size;
	int answer;
	int valid;

	if (vec_open(&file, "wycheproof-ecdh_secp256r1_ecpoint_test") != 0) {
		printf("ecdh FAIL no vectors\n");
		return;
	}
	while (vec_next(&file)) {
		id = vec_get(&file, "tcId");
		size = vec_hex(&file, "public", pub, sizeof(pub));
		if (id == NULL || size != 65U || pub[0] != 0x04U)
			continue;
		answer = pk_crypto_p256_ecdh(pub + 1, pub + 33, own_x, own_y, shared);
		valid = -1;
		if (answer == 0)
			valid = pk_crypto_p256_valid(own_x, own_y);
		printf("ecdh.%s %d %d\n", id, answer, valid);
	}
	vec_close(&file);
}

/* AES-256-CBC both ways, whole blocks and not. */
static void
compare_aes(void)
{
	uint8_t key[PK_AES256_KEY_SIZE];
	uint8_t iv[PK_AES_BLOCK_SIZE];
	uint8_t in[176];
	uint8_t out[176];
	char label[64];
	size_t size;
	int encrypt;
	int answer;

	for (size = 0U; size <= 176U; size += 8U) {
		for (encrypt = 0; encrypt < 2; encrypt++) {
			fill(key, sizeof(key));
			fill(iv, sizeof(iv));
			fill(in, size);
			memset(out, 0, sizeof(out));
			answer = pk_crypto_aes256_cbc(encrypt, key, iv, in, size, out);
			sprintf(label, "aes.%d.%lu", encrypt, (unsigned long)size);
			show(label, answer, out, size);
		}
	}
}

/* HKDF-SHA-256 with libpasskey's info texts and others, salts and secrets of several sizes, outputs of 0 to 8161. */
static void
compare_hkdf(void)
{
	static const char *const infos[4] = { "CTAP2 HMAC key", "CTAP2 AES key", "", "a longer info text, of more than one block's worth of bytes in all, to be sure" };
	static const size_t outs[7] = { 0U, 1U, 32U, 33U, 64U, 8160U, 8161U };
	static uint8_t out[8161];
	uint8_t salt[80];
	uint8_t secret[100];
	char label[80];
	size_t round;
	size_t salt_size;
	size_t secret_size;
	size_t out_size;
	int answer;

	for (round = 0U; round < 28U; round++) {
		salt_size = (round * 11U) % 80U;
		secret_size = (round * 17U) % 100U;
		out_size = outs[round % 7U];
		fill(salt, salt_size);
		fill(secret, secret_size);
		memset(out, 0, sizeof(out));
		answer = pk_crypto_hkdf_sha256(salt, salt_size, secret, secret_size, infos[round % 4U], out, out_size);
		sprintf(label, "hkdf.%lu.%lu.%lu.%lu", (unsigned long)round, (unsigned long)salt_size,
		    (unsigned long)secret_size, (unsigned long)out_size);
		show(label, answer, out, out_size > 96U ? 96U : out_size);
	}
}

/* The comparison and the wipe. */
static void
compare_equal_wipe(void)
{
	uint8_t left[64];
	uint8_t right[64];
	size_t size;

	for (size = 0U; size <= 64U; size += 16U) {
		fill(left, size);
		memcpy(right, left, size);
		printf("equal.%lu.same %d\n", (unsigned long)size, pk_crypto_equal(left, right, size));
		if (size > 0U) {
			right[size - 1U] ^= 1U;
			printf("equal.%lu.last %d\n", (unsigned long)size, pk_crypto_equal(left, right, size));
			right[size - 1U] ^= 1U;
			right[0] ^= 0x80U;
			printf("equal.%lu.first %d\n", (unsigned long)size, pk_crypto_equal(left, right, size));
		}
	}
	fill(left, sizeof(left));
	pk_crypto_wipe(left, sizeof(left));
	memset(right, 0, sizeof(right));
	printf("wipe %d\n", memcmp(left, right, sizeof(left)) == 0);
}

/* ECDH with the known key as the peer: our point and the shared secret, rounds lines. */
static void
emit_ecdh(
	int rounds)
{
	uint8_t x[32];
	uint8_t y[32];
	uint8_t own_x[32];
	uint8_t own_y[32];
	uint8_t shared[32];
	int round;
	int answer;

	hex_decode(known_x, x, sizeof(x));
	hex_decode(known_y, y, sizeof(y));
	for (round = 0; round < rounds; round++) {
		answer = pk_crypto_p256_ecdh(x, y, own_x, own_y, shared);
		printf("%d ", answer);
		show("own_x", 0, own_x, sizeof(own_x));
		show("own_y", 0, own_y, sizeof(own_y));
		show("shared", 0, shared, sizeof(shared));
	}
}

#ifdef PASSKEY_COMPARE_OPENSSL
/* Derives x(d * P) with OpenSSL for the known private key d and a point P; 1 when it worked. */
static int
openssl_shared(
	const uint8_t *own_x,
	const uint8_t *own_y,
	uint8_t *shared)
{
	OSSL_PARAM params[4];
	OSSL_PARAM peer_params[3];
	EVP_PKEY_CTX *ctx;
	EVP_PKEY *known;
	EVP_PKEY *peer;
	uint8_t d[32];
	uint8_t d_native[32];
	uint8_t point[65];
	uint8_t peer_point[65];
	size_t index;
	size_t length;
	int good;

	/* The known key pair: d in the platform's order for a BN parameter, and its point. */
	hex_decode(known_private, d, sizeof(d));
	for (index = 0U; index < 32U; index++)
		d_native[index] = d[31U - index];
	point[0] = 0x04U;
	hex_decode(known_x, point + 1, 32U);
	hex_decode(known_y, point + 33, 32U);
	params[0] = OSSL_PARAM_construct_utf8_string(OSSL_PKEY_PARAM_GROUP_NAME, (char *)"prime256v1", 0U);
	params[1] = OSSL_PARAM_construct_BN(OSSL_PKEY_PARAM_PRIV_KEY, d_native, sizeof(d_native));
	params[2] = OSSL_PARAM_construct_octet_string(OSSL_PKEY_PARAM_PUB_KEY, point, sizeof(point));
	params[3] = OSSL_PARAM_construct_end();
	ctx = EVP_PKEY_CTX_new_from_name(NULL, "EC", NULL);
	known = NULL;
	good = ctx != NULL && EVP_PKEY_fromdata_init(ctx) == 1 && EVP_PKEY_fromdata(ctx, &known, EVP_PKEY_KEYPAIR, params) == 1;
	EVP_PKEY_CTX_free(ctx);
	if (!good)
		return 0;

	/* The other point. */
	peer_point[0] = 0x04U;
	memcpy(peer_point + 1, own_x, 32U);
	memcpy(peer_point + 33, own_y, 32U);
	peer_params[0] = OSSL_PARAM_construct_utf8_string(OSSL_PKEY_PARAM_GROUP_NAME, (char *)"prime256v1", 0U);
	peer_params[1] = OSSL_PARAM_construct_octet_string(OSSL_PKEY_PARAM_PUB_KEY, peer_point, sizeof(peer_point));
	peer_params[2] = OSSL_PARAM_construct_end();
	ctx = EVP_PKEY_CTX_new_from_name(NULL, "EC", NULL);
	peer = NULL;
	good = ctx != NULL && EVP_PKEY_fromdata_init(ctx) == 1 && EVP_PKEY_fromdata(ctx, &peer, EVP_PKEY_PUBLIC_KEY, peer_params) == 1;
	EVP_PKEY_CTX_free(ctx);
	if (!good) {
		EVP_PKEY_free(known);
		return 0;
	}

	/* The derivation. */
	ctx = EVP_PKEY_CTX_new(known, NULL);
	length = 32U;
	good = ctx != NULL && EVP_PKEY_derive_init(ctx) == 1 && EVP_PKEY_derive_set_peer(ctx, peer) == 1 &&
	    EVP_PKEY_derive(ctx, shared, &length) == 1 && length == 32U;
	EVP_PKEY_CTX_free(ctx);
	EVP_PKEY_free(peer);
	EVP_PKEY_free(known);
	return good;
}

/* Checks the lines of emit_ecdh on standard input with OpenSSL; prints "check-ecdh: PASS (n)" or FAIL. */
static int
check_ecdh(void)
{
	char answer_text[16];
	char label[16];
	char value[3][80];
	uint8_t own_x[32];
	uint8_t own_y[32];
	uint8_t shared[32];
	uint8_t expected[32];
	int dummy;
	int count;
	int failed;

	count = 0;
	failed = 0;
	while (scanf("%15s %15s %d %79s", answer_text, label, &dummy, value[0]) == 4) {
		if (scanf("%15s %d %79s", label, &dummy, value[1]) != 3 || scanf("%15s %d %79s", label, &dummy, value[2]) != 3) {
			failed++;
			break;
		}
		count++;
		if (strcmp(answer_text, "0") != 0 || hex_decode(value[0], own_x, 32U) != 32U ||
		    hex_decode(value[1], own_y, 32U) != 32U || hex_decode(value[2], shared, 32U) != 32U ||
		    !openssl_shared(own_x, own_y, expected) || memcmp(shared, expected, 32U) != 0) {
			printf("check-ecdh: round %d differs\n", count);
			failed++;
		}
	}
	if (failed != 0 || count == 0) {
		printf("check-ecdh: FAIL (%d of %d)\n", failed, count);
		return 1;
	}
	printf("check-ecdh: PASS (%d)\n", count);
	return 0;
}
#endif

/* Runs the mode asked for. */
int
main(
	int argc,
	char **argv)
{
	/* ECDH lines for the check. */
	if (argc > 1 && strcmp(argv[1], "ecdh") == 0) {
		emit_ecdh(argc > 2 ? atoi(argv[2]) : 20);
		return 0;
	}

#ifdef PASSKEY_COMPARE_OPENSSL
	/* The check of those lines. */
	if (argc > 1 && strcmp(argv[1], "check-ecdh") == 0)
		return check_ecdh();
#endif

	/* Every deterministic call. */
	compare_sha256();
	compare_hmac();
	compare_random();
	compare_valid();
	compare_verify();
	compare_ecdh();
	compare_aes();
	compare_hkdf();
	compare_equal_wipe();
	return 0;
}
