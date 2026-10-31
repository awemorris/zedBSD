/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The public EVP_PKEY functions: Ed25519, X25519 and ML-KEM-768.
 */

#include "cryc-api.h"

/*
 * Makes a key from its raw private form: for EVP_PKEY_ED25519 the 32-byte
 * seed of RFC 8032 5.1.5, for EVP_PKEY_X25519 the 32-byte scalar of RFC
 * 7748 (unclamped bytes, as received).
 */
EVP_PKEY *
EVP_PKEY_new_raw_private_key(
	int type,
	ENGINE *e,
	const unsigned char *key,
	size_t length)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(type);
	UNUSED_PARAMETER(e);
	UNUSED_PARAMETER(key);
	UNUSED_PARAMETER(length);

	/* Not implemented: the failure value. */
	return NULL;
}

/*
 * Makes a public key from its raw form (32 bytes for both types), or NULL.
 */
EVP_PKEY *
EVP_PKEY_new_raw_public_key(
	int type,
	ENGINE *e,
	const unsigned char *key,
	size_t length)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(type);
	UNUSED_PARAMETER(e);
	UNUSED_PARAMETER(key);
	UNUSED_PARAMETER(length);

	/* Not implemented: the failure value. */
	return NULL;
}

/*
 * Makes a public key of the named type from its raw form: "ML-KEM-768"
 * with the 1184-byte encapsulation key, which must pass the modulus check
 * of FIPS 203 7.2 (else NULL); "ED25519" and "X25519" as
 * EVP_PKEY_new_raw_public_key.
 */
EVP_PKEY *
EVP_PKEY_new_raw_public_key_ex(
	OSSL_LIB_CTX *libctx,
	const char *keytype,
	const char *propq,
	const unsigned char *key,
	size_t length)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(libctx);
	UNUSED_PARAMETER(keytype);
	UNUSED_PARAMETER(propq);
	UNUSED_PARAMETER(key);
	UNUSED_PARAMETER(length);

	/* Not implemented: the failure value. */
	return NULL;
}

/*
 * Writes a key's raw public form (32 bytes; 1184 for ML-KEM-768's
 * encapsulation key).
 */
int
EVP_PKEY_get_raw_public_key(
	const EVP_PKEY *pkey,
	unsigned char *key,
	size_t *length)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(pkey);
	UNUSED_PARAMETER(key);
	UNUSED_PARAMETER(length);

	/* Not implemented: the failure value. */
	return 0;
}

/*
 * Writes a key's raw private form (32 bytes: Ed25519's seed, X25519's
 * scalar), as EVP_PKEY_get_raw_public_key; 0 for a public-only key and for
 * ML-KEM-768 (its private form is not exported).
 */
int
EVP_PKEY_get_raw_private_key(
	const EVP_PKEY *pkey,
	unsigned char *key,
	size_t *length)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(pkey);
	UNUSED_PARAMETER(key);
	UNUSED_PARAMETER(length);

	/* Not implemented: the failure value. */
	return 0;
}

/*
 * Drops one reference to a key, wiping and freeing it at the last; NULL is
 * ignored.
 */
void
EVP_PKEY_free(
	EVP_PKEY *pkey)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(pkey);
}

/*
 * Starts an Ed25519 signature with pkey (which must have its private key):
 * type must be NULL (pure Ed25519 hashes internally), pctx NULL or
 * receives NULL, e NULL.
 */
int
EVP_DigestSignInit(
	EVP_MD_CTX *ctx,
	EVP_PKEY_CTX **pctx,
	const EVP_MD *type,
	ENGINE *e,
	EVP_PKEY *pkey)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(ctx);
	UNUSED_PARAMETER(pctx);
	UNUSED_PARAMETER(type);
	UNUSED_PARAMETER(e);
	UNUSED_PARAMETER(pkey);

	/* Not implemented: the failure value. */
	return 0;
}

/*
 * Signs tbs (one shot, RFC 8032 5.1.6).
 */
int
EVP_DigestSign(
	EVP_MD_CTX *ctx,
	unsigned char *signature,
	size_t *length,
	const unsigned char *tbs,
	size_t tbs_length)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(ctx);
	UNUSED_PARAMETER(signature);
	UNUSED_PARAMETER(length);
	UNUSED_PARAMETER(tbs);
	UNUSED_PARAMETER(tbs_length);

	/* Not implemented: the failure value. */
	return 0;
}

/*
 * Starts an Ed25519 verification with pkey (public key enough); arguments
 * as EVP_DigestSignInit.
 */
int
EVP_DigestVerifyInit(
	EVP_MD_CTX *ctx,
	EVP_PKEY_CTX **pctx,
	const EVP_MD *type,
	ENGINE *e,
	EVP_PKEY *pkey)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(ctx);
	UNUSED_PARAMETER(pctx);
	UNUSED_PARAMETER(type);
	UNUSED_PARAMETER(e);
	UNUSED_PARAMETER(pkey);

	/* Not implemented: the failure value. */
	return 0;
}

/*
 * Verifies a signature over tbs (one shot, RFC 8032 5.1.7).
 */
int
EVP_DigestVerify(
	EVP_MD_CTX *ctx,
	const unsigned char *signature,
	size_t length,
	const unsigned char *tbs,
	size_t tbs_length)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(ctx);
	UNUSED_PARAMETER(signature);
	UNUSED_PARAMETER(length);
	UNUSED_PARAMETER(tbs);
	UNUSED_PARAMETER(tbs_length);

	/* Not implemented: the failure value. */
	return 0;
}

/*
 * Makes an operation context on pkey (taking a reference); e must be NULL.
 */
EVP_PKEY_CTX *
EVP_PKEY_CTX_new(
	EVP_PKEY *pkey,
	ENGINE *e)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(pkey);
	UNUSED_PARAMETER(e);

	/* Not implemented: the failure value. */
	return NULL;
}

/*
 * Makes a key-generation context for a type named "ML-KEM-768" (libctx and
 * propquery NULL).
 */
EVP_PKEY_CTX *
EVP_PKEY_CTX_new_from_name(
	OSSL_LIB_CTX *libctx,
	const char *name,
	const char *propquery)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(libctx);
	UNUSED_PARAMETER(name);
	UNUSED_PARAMETER(propquery);

	/* Not implemented: the failure value. */
	return NULL;
}

/*
 * As EVP_PKEY_CTX_new (libctx and propquery NULL).
 */
EVP_PKEY_CTX *
EVP_PKEY_CTX_new_from_pkey(
	OSSL_LIB_CTX *libctx,
	EVP_PKEY *pkey,
	const char *propquery)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(libctx);
	UNUSED_PARAMETER(pkey);
	UNUSED_PARAMETER(propquery);

	/* Not implemented: the failure value. */
	return NULL;
}

/*
 * Frees a context and drops its keys; NULL is ignored.
 */
void
EVP_PKEY_CTX_free(
	EVP_PKEY_CTX *ctx)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(ctx);
}

/*
 * Prepares an X25519 derivation with the context's private key.
 */
int
EVP_PKEY_derive_init(
	EVP_PKEY_CTX *ctx)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(ctx);

	/* Not implemented: the failure value. */
	return 0;
}

/*
 * Sets the peer's X25519 public key (the context takes a reference).
 */
int
EVP_PKEY_derive_set_peer(
	EVP_PKEY_CTX *ctx,
	EVP_PKEY *peer)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(ctx);
	UNUSED_PARAMETER(peer);

	/* Not implemented: the failure value. */
	return 0;
}

/*
 * Derives the X25519 shared secret (RFC 7748 6.1).
 */
int
EVP_PKEY_derive(
	EVP_PKEY_CTX *ctx,
	unsigned char *key,
	size_t *length)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(ctx);
	UNUSED_PARAMETER(key);
	UNUSED_PARAMETER(length);

	/* Not implemented: the failure value. */
	return 0;
}

/*
 * Prepares a key generation on a context from EVP_PKEY_CTX_new_from_name.
 */
int
EVP_PKEY_keygen_init(
	EVP_PKEY_CTX *ctx)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(ctx);

	/* Not implemented: the failure value. */
	return 0;
}

/*
 * Generates an ML-KEM-768 key pair (FIPS 203 Algorithm 19) into a new key
 * in *pkey.
 */
int
EVP_PKEY_keygen(
	EVP_PKEY_CTX *ctx,
	EVP_PKEY **pkey)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(ctx);
	UNUSED_PARAMETER(pkey);

	/* Not implemented: the failure value. */
	return 0;
}

/*
 * Prepares an encapsulation to the context's ML-KEM-768 public key; params
 * must be NULL.
 */
int
EVP_PKEY_encapsulate_init(
	EVP_PKEY_CTX *ctx,
	const OSSL_PARAM params[])
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(ctx);
	UNUSED_PARAMETER(params);

	/* Not implemented: the failure value. */
	return 0;
}

/*
 * Encapsulates (FIPS 203 Algorithm 20): writes the 1088-byte ciphertext to
 * wrapped and the 32-byte shared key to key.
 */
int
EVP_PKEY_encapsulate(
	EVP_PKEY_CTX *ctx,
	unsigned char *wrapped,
	size_t *wrapped_length,
	unsigned char *key,
	size_t *key_length)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(ctx);
	UNUSED_PARAMETER(wrapped);
	UNUSED_PARAMETER(wrapped_length);
	UNUSED_PARAMETER(key);
	UNUSED_PARAMETER(key_length);

	/* Not implemented: the failure value. */
	return 0;
}

/*
 * Prepares a decapsulation with the context's ML-KEM-768 private key;
 * params must be NULL.
 */
int
EVP_PKEY_decapsulate_init(
	EVP_PKEY_CTX *ctx,
	const OSSL_PARAM params[])
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(ctx);
	UNUSED_PARAMETER(params);

	/* Not implemented: the failure value. */
	return 0;
}

/*
 * Decapsulates (FIPS 203 Algorithm 21): writes the 32-byte shared key.
 */
int
EVP_PKEY_decapsulate(
	EVP_PKEY_CTX *ctx,
	unsigned char *key,
	size_t *length,
	const unsigned char *wrapped,
	size_t wrapped_length)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(ctx);
	UNUSED_PARAMETER(key);
	UNUSED_PARAMETER(length);
	UNUSED_PARAMETER(wrapped);
	UNUSED_PARAMETER(wrapped_length);

	/* Not implemented: the failure value. */
	return 0;
}
