/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A small program that uses libcrypto-compat.so through its public headers only, on zedBSD: SHA-256 (FIPS 180-4,
 * the "abc" example), X25519 (RFC 7748 6.1), Ed25519 (RFC 8032 7.1 TEST 1), 32 random bytes, and an empty error
 * queue at the end.  The last line is "crypto-smoke: PASS" or "crypto-smoke: FAIL (<n> failed)".
 */

#include <compat/crypto/crypto.h>
#include <compat/crypto/err.h>
#include <compat/crypto/evp.h>
#include <compat/crypto/rand.h>

#include <stdio.h>
#include <string.h>

/* The number of checks that failed. */
static int failed;

/* The number of checks made. */
static int made;

/* Records one check of a condition. */
static void
check(
	int good,
	const char *what)
{
	/* Counted either way. */
	made++;
	if (!good) {
		failed++;
		printf("FAIL %s\n", what);
	}
}

/* Turns a hex string into bytes; returns the count. */
static size_t
hex_decode(
	const char *hex,
	unsigned char *out)
{
	size_t count;
	int high;
	int value;

	count = 0;
	high = -1;
	for (; *hex != '\0'; hex++) {
		/* One hex digit. */
		if (*hex >= '0' && *hex <= '9')
			value = *hex - '0';
		else
			value = *hex - 'a' + 10;
		if (high < 0) {
			high = value;
		} else {
			out[count++] = (unsigned char)(high * 16 + value);
			high = -1;
		}
	}
	return count;
}

/* SHA-256 of "abc" through EVP_Digest and through the context calls. */
static void
check_sha256(void)
{
	unsigned char expected[32];
	unsigned char digest[32];
	unsigned int size;
	EVP_MD_CTX *ctx;

	hex_decode("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", expected);
	size = 0;
	memset(digest, 0, sizeof(digest));
	check(EVP_Digest("abc", 3, digest, &size, EVP_sha256(), NULL) == 1, "EVP_Digest");
	check(size == 32, "digest size");
	check(memcmp(digest, expected, 32) == 0, "SHA-256(abc)");
	check(EVP_MD_get_size(EVP_sha256()) == 32, "EVP_MD_get_size");

	/* The same in pieces. */
	ctx = EVP_MD_CTX_new();
	check(ctx != NULL, "EVP_MD_CTX_new");
	if (ctx != NULL) {
		size = 0;
		memset(digest, 0, sizeof(digest));
		check(EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) == 1, "EVP_DigestInit_ex");
		check(EVP_DigestUpdate(ctx, "a", 1) == 1, "EVP_DigestUpdate a");
		check(EVP_DigestUpdate(ctx, "bc", 2) == 1, "EVP_DigestUpdate bc");
		check(EVP_DigestFinal_ex(ctx, digest, &size) == 1, "EVP_DigestFinal_ex");
		check(size == 32 && memcmp(digest, expected, 32) == 0, "SHA-256(abc) in pieces");
		EVP_MD_CTX_free(ctx);
	}
}

/* The X25519 exchange of RFC 7748 6.1: both sides reach the same secret. */
static void
check_x25519(void)
{
	unsigned char alice_private[32];
	unsigned char alice_public[32];
	unsigned char bob_private[32];
	unsigned char bob_public[32];
	unsigned char secret[32];
	unsigned char got[32];
	unsigned char other[32];
	size_t length;
	EVP_PKEY *alice;
	EVP_PKEY *bob;
	EVP_PKEY *bob_only;
	EVP_PKEY_CTX *ctx;

	hex_decode("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a", alice_private);
	hex_decode("8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a", alice_public);
	hex_decode("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb", bob_private);
	hex_decode("de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f", bob_public);
	hex_decode("4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742", secret);

	alice = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, NULL, alice_private, 32);
	bob = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, NULL, bob_private, 32);
	bob_only = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, NULL, bob_public, 32);
	check(alice != NULL && bob != NULL && bob_only != NULL, "X25519 keys");
	if (alice == NULL || bob == NULL || bob_only == NULL) {
		EVP_PKEY_free(alice);
		EVP_PKEY_free(bob);
		EVP_PKEY_free(bob_only);
		return;
	}

	/* The public values. */
	length = 32;
	memset(got, 0, sizeof(got));
	check(EVP_PKEY_get_raw_public_key(alice, got, &length) == 1 && length == 32, "Alice public");
	check(memcmp(got, alice_public, 32) == 0, "Alice public value");
	length = 32;
	memset(got, 0, sizeof(got));
	check(EVP_PKEY_get_raw_public_key(bob, got, &length) == 1 && length == 32, "Bob public");
	check(memcmp(got, bob_public, 32) == 0, "Bob public value");

	/* Alice with Bob's public key. */
	ctx = EVP_PKEY_CTX_new(alice, NULL);
	check(ctx != NULL, "EVP_PKEY_CTX_new Alice");
	if (ctx != NULL) {
		length = 32;
		memset(got, 0, sizeof(got));
		check(EVP_PKEY_derive_init(ctx) == 1, "derive_init Alice");
		check(EVP_PKEY_derive_set_peer(ctx, bob_only) == 1, "set_peer Bob");
		check(EVP_PKEY_derive(ctx, got, &length) == 1 && length == 32, "derive Alice");
		check(memcmp(got, secret, 32) == 0, "shared secret (Alice)");
		EVP_PKEY_CTX_free(ctx);
	}

	/* Bob with Alice's key. */
	ctx = EVP_PKEY_CTX_new(bob, NULL);
	check(ctx != NULL, "EVP_PKEY_CTX_new Bob");
	if (ctx != NULL) {
		length = 32;
		memset(other, 0, sizeof(other));
		check(EVP_PKEY_derive_init(ctx) == 1, "derive_init Bob");
		check(EVP_PKEY_derive_set_peer(ctx, alice) == 1, "set_peer Alice");
		check(EVP_PKEY_derive(ctx, other, &length) == 1 && length == 32, "derive Bob");
		check(memcmp(other, secret, 32) == 0, "shared secret (Bob)");
		EVP_PKEY_CTX_free(ctx);
	}

	EVP_PKEY_free(alice);
	EVP_PKEY_free(bob);
	EVP_PKEY_free(bob_only);
}

/* The Ed25519 signature of RFC 8032 7.1 TEST 1 (the empty message), and its check. */
static void
check_ed25519(void)
{
	unsigned char seed[32];
	unsigned char public_key[32];
	unsigned char expected[64];
	unsigned char signature[64];
	unsigned char got[32];
	size_t length;
	EVP_PKEY *secret_key;
	EVP_PKEY *verify_key;
	EVP_MD_CTX *ctx;

	hex_decode("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60", seed);
	hex_decode("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", public_key);
	hex_decode("e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b", expected);

	secret_key = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, seed, 32);
	verify_key = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, NULL, public_key, 32);
	check(secret_key != NULL && verify_key != NULL, "Ed25519 keys");
	if (secret_key == NULL || verify_key == NULL) {
		EVP_PKEY_free(secret_key);
		EVP_PKEY_free(verify_key);
		return;
	}

	/* The public key made from the seed. */
	length = 32;
	memset(got, 0, sizeof(got));
	check(EVP_PKEY_get_raw_public_key(secret_key, got, &length) == 1 && length == 32, "Ed25519 public");
	check(memcmp(got, public_key, 32) == 0, "Ed25519 public value");

	/* The signature. */
	ctx = EVP_MD_CTX_new();
	check(ctx != NULL, "EVP_MD_CTX_new (sign)");
	if (ctx != NULL) {
		length = 64;
		memset(signature, 0, sizeof(signature));
		check(EVP_DigestSignInit(ctx, NULL, NULL, NULL, secret_key) == 1, "EVP_DigestSignInit");
		check(EVP_DigestSign(ctx, signature, &length, NULL, 0) == 1 && length == 64, "EVP_DigestSign");
		check(memcmp(signature, expected, 64) == 0, "Ed25519 signature");
		EVP_MD_CTX_free(ctx);
	}

	/* The check of the signature, and of one flipped bit. */
	ctx = EVP_MD_CTX_new();
	check(ctx != NULL, "EVP_MD_CTX_new (verify)");
	if (ctx != NULL) {
		check(EVP_DigestVerifyInit(ctx, NULL, NULL, NULL, verify_key) == 1, "EVP_DigestVerifyInit");
		check(EVP_DigestVerify(ctx, expected, 64, NULL, 0) == 1, "EVP_DigestVerify");
		EVP_MD_CTX_free(ctx);
	}
	ctx = EVP_MD_CTX_new();
	if (ctx != NULL) {
		expected[10] ^= 1;
		check(EVP_DigestVerifyInit(ctx, NULL, NULL, NULL, verify_key) == 1, "EVP_DigestVerifyInit (bad)");
		check(EVP_DigestVerify(ctx, expected, 64, NULL, 0) == 0, "EVP_DigestVerify (bad signature)");
		EVP_MD_CTX_free(ctx);
		expected[10] ^= 1;
	}

	/* A different message does not verify. */
	ctx = EVP_MD_CTX_new();
	if (ctx != NULL) {
		check(EVP_DigestVerifyInit(ctx, NULL, NULL, NULL, verify_key) == 1, "EVP_DigestVerifyInit (message)");
		check(EVP_DigestVerify(ctx, expected, 64, (const unsigned char *)"x", 1) == 0, "EVP_DigestVerify (other message)");
		EVP_MD_CTX_free(ctx);
	}

	EVP_PKEY_free(secret_key);
	EVP_PKEY_free(verify_key);
}

/* 32 random bytes: not all zero, and two calls differ. */
static void
check_random(void)
{
	unsigned char first[32];
	unsigned char second[32];
	unsigned char zero[32];

	memset(first, 0, sizeof(first));
	memset(second, 0, sizeof(second));
	memset(zero, 0, sizeof(zero));
	check(RAND_bytes(first, 32) == 1, "RAND_bytes first");
	check(RAND_bytes(second, 32) == 1, "RAND_bytes second");
	check(memcmp(first, zero, 32) != 0, "first random bytes are not all zero");
	check(memcmp(first, second, 32) != 0, "two random byte strings differ");
}

/* The constant-time helpers. */
static void
check_helpers(void)
{
	unsigned char one[8];
	unsigned char two[8];

	memset(one, 7, sizeof(one));
	memset(two, 7, sizeof(two));
	check(CRYPTO_memcmp(one, two, 8) == 0, "CRYPTO_memcmp equal");
	two[7] = 8;
	check(CRYPTO_memcmp(one, two, 8) != 0, "CRYPTO_memcmp different");
	OPENSSL_cleanse(one, sizeof(one));
	check(one[0] == 0 && one[7] == 0, "OPENSSL_cleanse");
}

int
main(void)
{
	check_sha256();
	check_x25519();
	check_ed25519();
	check_random();
	check_helpers();

	/* Every call above succeeded, so the error queue is empty. */
	check(ERR_get_error() == 0, "ERR_get_error is 0");

	printf("crypto-smoke: %d checks\n", made);
	if (failed != 0) {
		printf("crypto-smoke: FAIL (%d failed)\n", failed);
		return 1;
	}
	printf("crypto-smoke: PASS\n");
	return 0;
}
