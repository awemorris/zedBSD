/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The constant-time checks of the public EVP_PKEY functions: the private
 * forms of keys (an Ed25519 seed, an X25519 scalar, an ML-KEM-768
 * decapsulation key) and the messages to sign are marked secret, so
 * memcheck reports any branch or address of api-pkey.c that depends on
 * their bytes.  The decisions api-pkey.c makes (key types, lengths,
 * object states, names) are on public values.  Outputs derived from
 * secrets are made public only after the calls, as a caller does.
 *
 * The all-zero test of EVP_PKEY_derive's result is a decision on a value
 * made from the secret, by design (RFC 7748 6.1 makes the caller abort on
 * it, so it is public); memcheck cannot tell, so the derivation runs with
 * the scalar declassified, and the primitive's arithmetic and its check
 * of the result are in ct-x25519.c.  Key generation and
 * encapsulation draw from the system's random source, whose bytes are
 * not marked, so they are covered by ct-mlkem.c for the primitive.
 */

#include "check.h"
#include "ct.h"

#include <compat/crypto/err.h>
#include <compat/crypto/evp.h>

#include "userland/base/libcrypto-compat/cryc-api.h"
#include "userland/base/libcrypto-compat/cryc-mlkem.h"

/* Fills size bytes at out with a fixed pattern. */
static void fill(uint8_t *out, size_t size, unsigned seed);

/* Folds a public output into one value, to keep the call from being dropped. */
static unsigned fold(const uint8_t *bytes, size_t size);

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
		out[index] = (uint8_t)(seed + index * 13U + (index >> 3));
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

	/* Reports the folded value, one byte wide. */
	return value & 0xffU;
}

/* Runs the checks. */
int
main(void)
{
	static uint8_t message[300];
	static uint8_t ek[CRYC_MLKEM768_EK];
	static uint8_t dk[CRYC_MLKEM768_DK];
	static uint8_t ciphertext[CRYC_MLKEM768_CT];
	uint8_t seed[32];
	uint8_t scalar[32];
	uint8_t peer_raw[32];
	uint8_t d[32];
	uint8_t z[32];
	uint8_t m[32];
	uint8_t shared[32];
	uint8_t signature[64];
	uint8_t raw[32];
	size_t length;
	unsigned total;
	EVP_PKEY *key;
	EVP_PKEY *peer;
	EVP_PKEY *pair;
	EVP_PKEY_CTX *ctx;
	EVP_MD_CTX *md;
	int status;

	/* Starts the folded value at zero. */
	total = 0;

	/* Ed25519: a secret seed and message; the signing key's public form is public. */
	fill(seed, sizeof(seed), 3U);
	fill(message, sizeof(message), 11U);
	CT_SECRET(seed, sizeof(seed));
	CT_SECRET(message, sizeof(message));
	key = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, seed, 32U);
	CHECK(key != NULL);
	if (key != NULL) {
		CT_PUBLIC(key->public_key, 32U);
		total += fold(key->public_key, 32U);
		md = EVP_MD_CTX_new();
		CHECK(md != NULL);
		status = EVP_DigestSignInit(md, NULL, NULL, NULL, key);
		CHECK(status == 1);
		length = sizeof(signature);
		status = EVP_DigestSign(md, signature, &length, message, 0U);
		CHECK(status == 1);
		CT_PUBLIC(signature, sizeof(signature));
		total += fold(signature, sizeof(signature));
		length = sizeof(signature);
		status = EVP_DigestSign(md, signature, &length, message, 77U);
		CHECK(status == 1);
		length = sizeof(signature);
		status = EVP_DigestSign(md, signature, &length, message, sizeof(message));
		CHECK(status == 1);
		CT_PUBLIC(signature, sizeof(signature));
		total += fold(signature, sizeof(signature));

		/* The raw private form is copied out without looking at it. */
		length = sizeof(raw);
		status = EVP_PKEY_get_raw_private_key(key, raw, &length);
		CHECK(status == 1);
		CT_PUBLIC(raw, sizeof(raw));
		total += fold(raw, sizeof(raw));
		EVP_MD_CTX_free(md);

		/* Freeing wipes the secret. */
		EVP_PKEY_free(key);
	}

	/* The seed and the message are public again once the calls are done. */
	CT_PUBLIC(seed, sizeof(seed));
	CT_PUBLIC(message, sizeof(message));

	/* X25519: a secret scalar, a public peer; the peer's key is a public one. */
	fill(scalar, sizeof(scalar), 7U);
	fill(peer_raw, sizeof(peer_raw), 29U);
	peer_raw[31] &= 0x7fU;
	CT_SECRET(scalar, sizeof(scalar));
	key = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, NULL, scalar, 32U);
	peer = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, NULL, peer_raw, 32U);
	CHECK(key != NULL && peer != NULL);
	if (key != NULL && peer != NULL) {
		CT_PUBLIC(key->public_key, 32U);
		total += fold(key->public_key, 32U);
		ctx = EVP_PKEY_CTX_new(key, NULL);
		CHECK(ctx != NULL);
		status = EVP_PKEY_derive_init(ctx);
		CHECK(status == 1);
		status = EVP_PKEY_derive_set_peer(ctx, peer);
		CHECK(status == 1);

		/* The all-zero test of the result is a decision on the secret by design, so the scalar is declassified here: derivation's own arithmetic is checked in ct-x25519.c. */
		CT_PUBLIC(key->private_key, 32U);
		length = sizeof(shared);
		status = EVP_PKEY_derive(ctx, shared, &length);
		CHECK(status == 1);
		CT_PUBLIC(shared, sizeof(shared));
		total += fold(shared, sizeof(shared));
		EVP_PKEY_CTX_free(ctx);
	}

	/* Frees both keys; the scalar is public again once the calls are done. */
	EVP_PKEY_free(key);
	EVP_PKEY_free(peer);
	CT_PUBLIC(scalar, sizeof(scalar));

	/* ML-KEM-768: a key pair whose seeds are secret, decapsulation of a ciphertext made for it (a valid one and a changed one). */
	fill(d, sizeof(d), 41U);
	fill(z, sizeof(z), 43U);
	fill(m, sizeof(m), 47U);
	cryc_mlkem768_keygen_internal(ek, dk, d, z);
	cryc_mlkem768_encaps_internal(shared, ciphertext, ek, m);
	pair = calloc(1, sizeof(*pair));
	CHECK(pair != NULL);
	if (pair != NULL) {
		pair->private_key = malloc(CRYC_MLKEM768_DK);
		CHECK(pair->private_key != NULL);
		if (pair->private_key != NULL) {
			pair->references = 1;
			pair->type = CRYC_PKEY_MLKEM768;
			memcpy(pair->public_key, ek, sizeof(ek));
			pair->public_size = sizeof(ek);
			memcpy(pair->private_key, dk, sizeof(dk));
			pair->private_size = sizeof(dk);
			pair->has_private = 1;

			/* The secret part of dk: everything but the embedded ek and its hash, as ct-mlkem.c marks it. */
			CT_SECRET(pair->private_key, 1152U);
			CT_SECRET(pair->private_key + 2368, 32U);
			ctx = EVP_PKEY_CTX_new(pair, NULL);
			CHECK(ctx != NULL);
			status = EVP_PKEY_decapsulate_init(ctx, NULL);
			CHECK(status == 1);
			length = sizeof(shared);
			status = EVP_PKEY_decapsulate(ctx, shared, &length, ciphertext, sizeof(ciphertext));
			CHECK(status == 1);
			CT_PUBLIC(shared, sizeof(shared));
			total += fold(shared, sizeof(shared));
			ciphertext[17] ^= 0x04U;
			length = sizeof(shared);
			status = EVP_PKEY_decapsulate(ctx, shared, &length, ciphertext, sizeof(ciphertext));
			CHECK(status == 1);
			CT_PUBLIC(shared, sizeof(shared));
			total += fold(shared, sizeof(shared));
			EVP_PKEY_CTX_free(ctx);
			CT_PUBLIC(pair->private_key, 1152U);
			CT_PUBLIC(pair->private_key + 2368, 32U);
		}

		/* Frees the key pair. */
		EVP_PKEY_free(pair);
	}

	/* The folded value is used so that no call can be dropped. */
	CHECK(total < 1000000U);
	CHECK(ERR_peek_error() == 0UL);

	/* The result. */
	return check_finish("ct-api-pkey");
}
