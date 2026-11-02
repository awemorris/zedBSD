/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The public EVP_PKEY functions: Ed25519, X25519 and ML-KEM-768.
 *
 * A function that fails queues exactly one error of the EVP library (a
 * null argument, an argument that must be NULL, an unsupported algorithm
 * or key type, a key that cannot do the operation, a context not set up,
 * a length out of range, a buffer too small, no memory) and returns the
 * failure value of its interface; a function that succeeds queues
 * nothing.  A failed call changes neither the objects it was given nor
 * the caller's output buffers, except that EVP_PKEY_derive zeroes its
 * output when the shared secret is all zeros, as its interface says.  A
 * signature that does not verify is a verdict, not an error: it queues
 * nothing.
 *
 * Secrets (the private key of a key object, the shared secrets, the
 * private copies made on the stack) are wiped before their memory is
 * released.  Nothing here branches on a secret: the algorithms are the
 * constant-time ones of cryc-ed25519.h, cryc-x25519.h and cryc-mlkem.h,
 * and the decisions made in this file depend on key types, lengths and
 * the state of the objects, which are public.
 */

#include <stdlib.h>
#include <string.h>

#include "cryc-api.h"
#include "cryc-os.h"

/* The size in bytes of a raw Ed25519 or X25519 key, public or private (RFC 8032 5.1.5, RFC 7748 5). */
#define PKEY_RAW_SIZE		32U

/* The size in bytes of an Ed25519 signature (RFC 8032 5.1.6). */
#define PKEY_SIGNATURE_SIZE	64U

/* Looks up a key type by the name a program gives ("ED25519", "X25519", "ML-KEM-768"); 0 when it is none. */
static int pkey_type_from_name(const char *name);

/* Compares a name with an uppercase ASCII name, ignoring the case of the letters of name. */
static int pkey_name_equal(const char *name, const char *expected);

/* Makes a key object of a type with one reference and the private form's memory; NULL without memory. */
static EVP_PKEY *pkey_new(int type, size_t public_size, size_t private_size);

/* Makes a public-only key of a type from its raw form, checking the length and (ML-KEM) the modulus. */
static EVP_PKEY *pkey_new_public(int type, const unsigned char *key, size_t length);

/* Takes one more reference to a key. */
static void pkey_take_reference(EVP_PKEY *pkey);

/* Makes a digest context hold pkey for the signing or verification mode, replacing what it held. */
static int md_ctx_start_key(EVP_MD_CTX *ctx, EVP_PKEY_CTX **pctx, const EVP_MD *type, ENGINE *e, EVP_PKEY *pkey, int mode);

/* Makes an operation context holding a reference to pkey (which may be NULL for a key-generation context). */
static EVP_PKEY_CTX *pkey_ctx_make(EVP_PKEY *pkey, int keygen_type);

/*
 * Makes a key from its raw private form: for EVP_PKEY_ED25519 the 32-byte
 * seed of RFC 8032 5.1.5, for EVP_PKEY_X25519 the 32-byte scalar of RFC
 * 7748 (unclamped bytes, as received).  The public key is computed.
 */
EVP_PKEY *
EVP_PKEY_new_raw_private_key(
	int type,
	ENGINE *e,
	const unsigned char *key,
	size_t length)
{
	EVP_PKEY *pkey;
	int pkey_type;

	/* Refuses an engine: none is implemented. */
	if (e != NULL) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_PASSED_INVALID_ARGUMENT);
		return NULL;
	}

	/* Refuses a missing key. */
	if (key == NULL) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_PASSED_NULL_PARAMETER);
		return NULL;
	}

	/* Maps the interface's type number to the key type; ML-KEM has no raw private form. */
	pkey_type = 0;
	if (type == EVP_PKEY_ED25519)
		pkey_type = CRYC_PKEY_ED25519;
	else if (type == EVP_PKEY_X25519)
		pkey_type = CRYC_PKEY_X25519;
	if (pkey_type == 0) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_EVP_UNSUPPORTED_ALGORITHM);
		return NULL;
	}

	/* Both private forms are 32 bytes. */
	if (length != PKEY_RAW_SIZE) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_EVP_INVALID_KEY_LENGTH);
		return NULL;
	}

	/* Makes the key object with room for the 32-byte public and private forms. */
	pkey = pkey_new(pkey_type, PKEY_RAW_SIZE, PKEY_RAW_SIZE);
	if (pkey == NULL)
		return NULL;

	/* Keeps the private form exactly as received. */
	memcpy(pkey->private_key, key, PKEY_RAW_SIZE);
	pkey->has_private = 1;

	/* Computes the public key that belongs to the private one. */
	if (pkey_type == CRYC_PKEY_ED25519)
		cryc_ed25519_public(pkey->public_key, pkey->private_key);
	else
		cryc_x25519_public(pkey->public_key, pkey->private_key);

	/* Succeeded: a key with one reference, owned by the caller. */
	return pkey;
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
	EVP_PKEY *pkey;
	int pkey_type;

	/* Refuses an engine: none is implemented. */
	if (e != NULL) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_PASSED_INVALID_ARGUMENT);
		return NULL;
	}

	/* Refuses a missing key. */
	if (key == NULL) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_PASSED_NULL_PARAMETER);
		return NULL;
	}

	/* Maps the interface's type number to the key type; ML-KEM is made by name. */
	pkey_type = 0;
	if (type == EVP_PKEY_ED25519)
		pkey_type = CRYC_PKEY_ED25519;
	else if (type == EVP_PKEY_X25519)
		pkey_type = CRYC_PKEY_X25519;
	if (pkey_type == 0) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_EVP_UNSUPPORTED_ALGORITHM);
		return NULL;
	}

	/* Makes the public-only key. */
	pkey = pkey_new_public(pkey_type, key, length);
	if (pkey == NULL)
		return NULL;

	/* Succeeded: a public key with one reference, owned by the caller. */
	return pkey;
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
	EVP_PKEY *pkey;
	int pkey_type;

	/* Refuses a library context and a property query: neither is implemented. */
	if (libctx != NULL || propq != NULL) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_PASSED_INVALID_ARGUMENT);
		return NULL;
	}

	/* Refuses a missing type name or key. */
	if (keytype == NULL || key == NULL) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_PASSED_NULL_PARAMETER);
		return NULL;
	}

	/* Finds the key type the name stands for. */
	pkey_type = pkey_type_from_name(keytype);
	if (pkey_type == 0) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_EVP_UNSUPPORTED_ALGORITHM);
		return NULL;
	}

	/* Makes the public-only key. */
	pkey = pkey_new_public(pkey_type, key, length);
	if (pkey == NULL)
		return NULL;

	/* Succeeded: a public key with one reference, owned by the caller. */
	return pkey;
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
	/* Refuses a missing key or length. */
	if (pkey == NULL || length == NULL) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_PASSED_NULL_PARAMETER);
		return 0;
	}

	/* A query for the size writes no key. */
	if (key == NULL) {
		*length = pkey->public_size;
		return 1;
	}

	/* Refuses a buffer that is too small, leaving it and the length as they were. */
	if (*length < pkey->public_size) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_EVP_BUFFER_TOO_SMALL);
		return 0;
	}

	/* Copies the public form out. */
	memcpy(key, pkey->public_key, pkey->public_size);
	*length = pkey->public_size;

	/* Succeeded: the raw public key is written. */
	return 1;
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
	/* Refuses a missing key or length. */
	if (pkey == NULL || length == NULL) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_PASSED_NULL_PARAMETER);
		return 0;
	}

	/* The private form of an ML-KEM-768 key is never exported. */
	if (pkey->type == CRYC_PKEY_MLKEM768) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_UNSUPPORTED);
		return 0;
	}

	/* A public-only key has no private form to give. */
	if (!pkey->has_private) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_EVP_INVALID_KEY);
		return 0;
	}

	/* A query for the size writes no key. */
	if (key == NULL) {
		*length = pkey->private_size;
		return 1;
	}

	/* Refuses a buffer that is too small, leaving it and the length as they were. */
	if (*length < pkey->private_size) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_EVP_BUFFER_TOO_SMALL);
		return 0;
	}

	/* Copies the private form out. */
	memcpy(key, pkey->private_key, pkey->private_size);
	*length = pkey->private_size;

	/* Succeeded: the raw private key is written. */
	return 1;
}

/*
 * Drops one reference to a key, wiping and freeing it at the last; NULL is
 * ignored.
 */
void
EVP_PKEY_free(
	EVP_PKEY *pkey)
{
	long count;

	/* Nothing to free. */
	if (pkey == NULL)
		return;

	/* Drops this holder's reference; the call that brings the count to zero frees the key. */
	count = cryc_os_atomic_add(&pkey->references, -1L);
	if (count != 0L)
		return;

	/* The private form is secret: wipes it before its memory is released. */
	if (pkey->private_key != NULL) {
		cryc_wipe(pkey->private_key, pkey->private_size);
		free(pkey->private_key);
	}

	/* Wipes the rest of the object, then releases it. */
	cryc_wipe(pkey, sizeof(*pkey));
	free(pkey);
}

/*
 * Starts an Ed25519 signature with pkey (which must have its private key):
 * type must be NULL (pure Ed25519 hashes internally), pctx NULL or
 * receives NULL, e NULL.  ctx takes a reference to pkey.
 */
int
EVP_DigestSignInit(
	EVP_MD_CTX *ctx,
	EVP_PKEY_CTX **pctx,
	const EVP_MD *type,
	ENGINE *e,
	EVP_PKEY *pkey)
{
	int started;

	/* Starts the context in signing mode with the key. */
	started = md_ctx_start_key(ctx, pctx, type, e, pkey, CRYC_MD_SIGN);
	if (!started)
		return 0;

	/* Succeeded: ctx signs with pkey. */
	return 1;
}

/*
 * Signs tbs (one shot, RFC 8032 5.1.6).  With signature NULL sets
 * *length to 64 and returns 1; otherwise *length is the room (at least
 * 64) and becomes 64.
 */
int
EVP_DigestSign(
	EVP_MD_CTX *ctx,
	unsigned char *signature,
	size_t *length,
	const unsigned char *tbs,
	size_t tbs_length)
{
	uint8_t made[CRYC_ED25519_SIGNATURE];
	static const uint8_t empty[1] = { 0 };

	/* Refuses a missing context or length. */
	if (ctx == NULL || length == NULL) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_PASSED_NULL_PARAMETER);
		return 0;
	}

	/* Refuses a context that was not started for signing. */
	if (ctx->mode != CRYC_MD_SIGN || ctx->key == NULL) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_EVP_OPERATION_NOT_INITIALIZED);
		return 0;
	}

	/* A query for the size writes no signature. */
	if (signature == NULL) {
		*length = PKEY_SIGNATURE_SIZE;
		return 1;
	}

	/* Refuses a buffer that is too small. */
	if (*length < PKEY_SIGNATURE_SIZE) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_EVP_BUFFER_TOO_SMALL);
		return 0;
	}

	/* Refuses a missing message, except the empty one. */
	if (tbs == NULL && tbs_length != 0U) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_PASSED_NULL_PARAMETER);
		return 0;
	}

	/* An empty message may come as a NULL pointer; the signer is given a real one. */
	if (tbs == NULL)
		tbs = empty;

	/* Signs into a private buffer first, so that a buffer overlapping the message is safe. */
	cryc_ed25519_sign(made, tbs, tbs_length, ctx->key->private_key);

	/* Writes the 64-byte signature out. */
	memcpy(signature, made, PKEY_SIGNATURE_SIZE);
	*length = PKEY_SIGNATURE_SIZE;

	/* Succeeded: the signature is written. */
	return 1;
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
	int started;

	/* Starts the context in verification mode with the key. */
	started = md_ctx_start_key(ctx, pctx, type, e, pkey, CRYC_MD_VERIFY);
	if (!started)
		return 0;

	/* Succeeded: ctx verifies with pkey. */
	return 1;
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
	static const uint8_t empty[1] = { 0 };
	int error;

	/* A missing context is not a verdict on the signature. */
	if (ctx == NULL) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_PASSED_NULL_PARAMETER);
		return -1;
	}

	/* A context that was not started for verification gives the negative answer. */
	if (ctx->mode != CRYC_MD_VERIFY || ctx->key == NULL) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_EVP_OPERATION_NOT_INITIALIZED);
		return -1;
	}

	/* A signature of another length than 64 does not verify (RFC 8032 5.1.7 gives it no meaning). */
	if (length != PKEY_SIGNATURE_SIZE)
		return 0;

	/* Refuses a missing signature of the right length, or a missing message that is not empty. */
	if (signature == NULL || (tbs == NULL && tbs_length != 0U)) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_PASSED_NULL_PARAMETER);
		return 0;
	}

	/* An empty message may come as a NULL pointer; the verifier is given a real one. */
	if (tbs == NULL)
		tbs = empty;

	/* Checks the equation [S]B = R + [k]A of RFC 8032 5.1.7 step 3. */
	error = cryc_ed25519_verify(signature, tbs, tbs_length, ctx->key->public_key);
	if (error != 0)
		return 0;

	/* Succeeded: the signature verifies. */
	return 1;
}

/*
 * Makes an operation context on pkey (taking a reference); e must be NULL.
 */
EVP_PKEY_CTX *
EVP_PKEY_CTX_new(
	EVP_PKEY *pkey,
	ENGINE *e)
{
	EVP_PKEY_CTX *ctx;

	/* Refuses an engine: none is implemented. */
	if (e != NULL) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_PASSED_INVALID_ARGUMENT);
		return NULL;
	}

	/* Refuses a missing key. */
	if (pkey == NULL) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_PASSED_NULL_PARAMETER);
		return NULL;
	}

	/* Makes the context holding a reference to the key. */
	ctx = pkey_ctx_make(pkey, 0);
	if (ctx == NULL)
		return NULL;

	/* Succeeded: a context for operations on pkey. */
	return ctx;
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
	EVP_PKEY_CTX *ctx;
	int pkey_type;

	/* Refuses a library context and a property query: neither is implemented. */
	if (libctx != NULL || propquery != NULL) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_PASSED_INVALID_ARGUMENT);
		return NULL;
	}

	/* Refuses a missing name. */
	if (name == NULL) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_PASSED_NULL_PARAMETER);
		return NULL;
	}

	/* Only ML-KEM-768 is generated by name. */
	pkey_type = pkey_type_from_name(name);
	if (pkey_type != CRYC_PKEY_MLKEM768) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_EVP_UNSUPPORTED_ALGORITHM);
		return NULL;
	}

	/* Makes a context that holds no key yet, only the type to generate. */
	ctx = pkey_ctx_make(NULL, pkey_type);
	if (ctx == NULL)
		return NULL;

	/* Succeeded: a key-generation context. */
	return ctx;
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
	EVP_PKEY_CTX *ctx;

	/* Refuses a library context and a property query: neither is implemented. */
	if (libctx != NULL || propquery != NULL) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_PASSED_INVALID_ARGUMENT);
		return NULL;
	}

	/* Refuses a missing key. */
	if (pkey == NULL) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_PASSED_NULL_PARAMETER);
		return NULL;
	}

	/* Makes the context holding a reference to the key. */
	ctx = pkey_ctx_make(pkey, 0);
	if (ctx == NULL)
		return NULL;

	/* Succeeded: a context for operations on pkey. */
	return ctx;
}

/*
 * Frees a context and drops its keys; NULL is ignored.
 */
void
EVP_PKEY_CTX_free(
	EVP_PKEY_CTX *ctx)
{
	/* Nothing to free. */
	if (ctx == NULL)
		return;

	/* Gives up the context's references to its key and its peer. */
	EVP_PKEY_free(ctx->key);
	EVP_PKEY_free(ctx->peer);

	/* Wipes the object, then releases it. */
	cryc_wipe(ctx, sizeof(*ctx));
	free(ctx);
}

/*
 * Prepares an X25519 derivation with the context's private key.
 */
int
EVP_PKEY_derive_init(
	EVP_PKEY_CTX *ctx)
{
	/* Refuses a missing context. */
	if (ctx == NULL) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_PASSED_NULL_PARAMETER);
		return 0;
	}

	/* Only an X25519 key derives; a key-generation context holds no key. */
	if (ctx->key == NULL || ctx->key->type != CRYC_PKEY_X25519) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_EVP_UNSUPPORTED_ALGORITHM);
		return 0;
	}

	/* Derivation needs the private key. */
	if (!ctx->key->has_private) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_EVP_INVALID_KEY);
		return 0;
	}

	/* A new derivation needs its own peer: the earlier peer is let go. */
	EVP_PKEY_free(ctx->peer);
	ctx->peer = NULL;
	ctx->operation = CRYC_OP_DERIVE;

	/* Succeeded: the context derives. */
	return 1;
}

/*
 * Sets the peer's X25519 public key (the context takes a reference).
 */
int
EVP_PKEY_derive_set_peer(
	EVP_PKEY_CTX *ctx,
	EVP_PKEY *peer)
{
	/* Refuses a missing context or peer. */
	if (ctx == NULL || peer == NULL) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_PASSED_NULL_PARAMETER);
		return 0;
	}

	/* The context must have been prepared for derivation. */
	if (ctx->operation != CRYC_OP_DERIVE) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_EVP_OPERATION_NOT_INITIALIZED);
		return 0;
	}

	/* The peer must be of the same type as the context's key. */
	if (peer->type != CRYC_PKEY_X25519) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_EVP_DIFFERENT_KEY_TYPES);
		return 0;
	}

	/* Takes the new reference before letting go of the old one, so that setting the same peer again is safe. */
	pkey_take_reference(peer);
	EVP_PKEY_free(ctx->peer);
	ctx->peer = peer;

	/* Succeeded: the context holds the peer. */
	return 1;
}

/*
 * Derives the X25519 shared secret (RFC 7748 6.1).  With key NULL sets
 * *length to 32 and returns 1.  An all-zero result fails (returns 0,
 * nothing usable written: key is zeroed).
 */
int
EVP_PKEY_derive(
	EVP_PKEY_CTX *ctx,
	unsigned char *key,
	size_t *length)
{
	uint8_t shared[CRYC_X25519_SIZE];
	int error;

	/* Refuses a missing context or length. */
	if (ctx == NULL || length == NULL) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_PASSED_NULL_PARAMETER);
		return 0;
	}

	/* The context must have been prepared and given a peer. */
	if (ctx->operation != CRYC_OP_DERIVE || ctx->peer == NULL) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_EVP_OPERATION_NOT_INITIALIZED);
		return 0;
	}

	/* A query for the size writes no secret. */
	if (key == NULL) {
		*length = CRYC_X25519_SIZE;
		return 1;
	}

	/* Refuses a buffer that is too small. */
	if (*length < CRYC_X25519_SIZE) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_EVP_BUFFER_TOO_SMALL);
		return 0;
	}

	/* Computes X25519(own private key, peer public key) into a private buffer. */
	error = cryc_x25519(shared, ctx->key->private_key, ctx->peer->public_key);
	if (error != 0) {
		/* An all-zero result means the peer's key is of small order: nothing usable is given out. */
		cryc_wipe(shared, sizeof(shared));
		cryc_wipe(key, CRYC_X25519_SIZE);
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_EVP_INVALID_KEY);
		return 0;
	}

	/* Writes the shared secret out and wipes the private copy. */
	memcpy(key, shared, CRYC_X25519_SIZE);
	*length = CRYC_X25519_SIZE;
	cryc_wipe(shared, sizeof(shared));

	/* Succeeded: the shared secret is written. */
	return 1;
}

/*
 * Prepares a key generation on a context from EVP_PKEY_CTX_new_from_name.
 */
int
EVP_PKEY_keygen_init(
	EVP_PKEY_CTX *ctx)
{
	/* Refuses a missing context. */
	if (ctx == NULL) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_PASSED_NULL_PARAMETER);
		return 0;
	}

	/* Only a context made from a type name generates a key. */
	if (ctx->key != NULL || ctx->keygen_type != CRYC_PKEY_MLKEM768) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_EVP_UNSUPPORTED_ALGORITHM);
		return 0;
	}

	/* The context is now a key generator. */
	ctx->operation = CRYC_OP_KEYGEN;

	/* Succeeded: the context generates keys. */
	return 1;
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
	EVP_PKEY *made;
	int error;

	/* Refuses a missing context or output. */
	if (ctx == NULL || pkey == NULL) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_PASSED_NULL_PARAMETER);
		return 0;
	}

	/* The context must have been prepared for key generation. */
	if (ctx->operation != CRYC_OP_KEYGEN) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_EVP_OPERATION_NOT_INITIALIZED);
		return 0;
	}

	/* Makes the key object with room for the encapsulation key and the decapsulation key. */
	made = pkey_new(CRYC_PKEY_MLKEM768, CRYC_MLKEM768_EK, CRYC_MLKEM768_DK);
	if (made == NULL)
		return 0;

	/* Generates the pair from random seeds (FIPS 203 Algorithm 19). */
	error = cryc_mlkem768_keygen(made->public_key, made->private_key, cryc_os_random, NULL);
	if (error != 0) {
		/* The system's random source refused: no key is made. */
		EVP_PKEY_free(made);
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_INTERNAL_ERROR);
		return 0;
	}
	made->has_private = 1;

	/* Hands the new key to the caller; whatever *pkey held before is not touched. */
	*pkey = made;

	/* Succeeded: *pkey is a key pair with one reference. */
	return 1;
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
	/* Refuses parameters: none are implemented. */
	if (params != NULL) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_PASSED_INVALID_ARGUMENT);
		return 0;
	}

	/* Refuses a missing context. */
	if (ctx == NULL) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_PASSED_NULL_PARAMETER);
		return 0;
	}

	/* Only an ML-KEM-768 key encapsulates. */
	if (ctx->key == NULL || ctx->key->type != CRYC_PKEY_MLKEM768) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_EVP_UNSUPPORTED_ALGORITHM);
		return 0;
	}

	/* The context now encapsulates. */
	ctx->operation = CRYC_OP_ENCAPSULATE;

	/* Succeeded: the context encapsulates to its key. */
	return 1;
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
	uint8_t ciphertext[CRYC_MLKEM768_CT];
	uint8_t shared[CRYC_MLKEM_SHARED];
	int error;

	/* Refuses a missing context. */
	if (ctx == NULL) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_PASSED_NULL_PARAMETER);
		return 0;
	}

	/* The context must have been prepared for encapsulation. */
	if (ctx->operation != CRYC_OP_ENCAPSULATE) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_EVP_OPERATION_NOT_INITIALIZED);
		return 0;
	}

	/* The two lengths are required whether the buffers are given or not. */
	if (wrapped_length == NULL || key_length == NULL) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_PASSED_NULL_PARAMETER);
		return 0;
	}

	/* A query for the sizes gives neither buffer. */
	if (wrapped == NULL && key == NULL) {
		*wrapped_length = CRYC_MLKEM768_CT;
		*key_length = CRYC_MLKEM_SHARED;
		return 1;
	}

	/* One buffer without the other is a mistake in the call. */
	if (wrapped == NULL || key == NULL) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_PASSED_NULL_PARAMETER);
		return 0;
	}

	/* Refuses buffers that are too small before any random bytes are used. */
	if (*wrapped_length < CRYC_MLKEM768_CT || *key_length < CRYC_MLKEM_SHARED) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_EVP_BUFFER_TOO_SMALL);
		return 0;
	}

	/* Encapsulates to the key with a fresh random message (FIPS 203 Algorithm 20). */
	error = cryc_mlkem768_encaps(shared, ciphertext, ctx->key->public_key, cryc_os_random, NULL);
	if (error != 0) {
		/* Either the encapsulation key fails the modulus check or the random source refused. */
		cryc_wipe(shared, sizeof(shared));
		if (error == EINVAL)
			cryc_err_put(CRYC_LIB_EVP, CRYC_R_EVP_INVALID_KEY);
		else
			cryc_err_put(CRYC_LIB_EVP, CRYC_R_INTERNAL_ERROR);
		return 0;
	}

	/* Writes the ciphertext and the shared key out, and wipes the private copy of the key. */
	memcpy(wrapped, ciphertext, CRYC_MLKEM768_CT);
	memcpy(key, shared, CRYC_MLKEM_SHARED);
	*wrapped_length = CRYC_MLKEM768_CT;
	*key_length = CRYC_MLKEM_SHARED;
	cryc_wipe(shared, sizeof(shared));

	/* Succeeded: the ciphertext and the shared key are written. */
	return 1;
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
	/* Refuses parameters: none are implemented. */
	if (params != NULL) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_PASSED_INVALID_ARGUMENT);
		return 0;
	}

	/* Refuses a missing context. */
	if (ctx == NULL) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_PASSED_NULL_PARAMETER);
		return 0;
	}

	/* Only an ML-KEM-768 key decapsulates. */
	if (ctx->key == NULL || ctx->key->type != CRYC_PKEY_MLKEM768) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_EVP_UNSUPPORTED_ALGORITHM);
		return 0;
	}

	/* Decapsulation needs the private key. */
	if (!ctx->key->has_private) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_EVP_INVALID_KEY);
		return 0;
	}

	/* The context now decapsulates. */
	ctx->operation = CRYC_OP_DECAPSULATE;

	/* Succeeded: the context decapsulates with its key. */
	return 1;
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
	uint8_t shared[CRYC_MLKEM_SHARED];

	/* Refuses a missing context or length. */
	if (ctx == NULL || length == NULL) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_PASSED_NULL_PARAMETER);
		return 0;
	}

	/* The context must have been prepared for decapsulation. */
	if (ctx->operation != CRYC_OP_DECAPSULATE) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_EVP_OPERATION_NOT_INITIALIZED);
		return 0;
	}

	/* A query for the size writes no key. */
	if (key == NULL) {
		*length = CRYC_MLKEM_SHARED;
		return 1;
	}

	/* Refuses a buffer that is too small. */
	if (*length < CRYC_MLKEM_SHARED) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_EVP_BUFFER_TOO_SMALL);
		return 0;
	}

	/* Refuses a missing ciphertext. */
	if (wrapped == NULL) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_PASSED_NULL_PARAMETER);
		return 0;
	}

	/* Only a ciphertext of exactly 1088 bytes is decapsulated (FIPS 203 7.3 input check). */
	if (wrapped_length != CRYC_MLKEM768_CT) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_PASSED_INVALID_ARGUMENT);
		return 0;
	}

	/* Decapsulates; a ciphertext that does not re-encrypt gives the implicit-rejection key, not an error. */
	cryc_mlkem768_decaps(shared, wrapped, ctx->key->private_key);

	/* Writes the shared key out and wipes the private copy. */
	memcpy(key, shared, CRYC_MLKEM_SHARED);
	*length = CRYC_MLKEM_SHARED;
	cryc_wipe(shared, sizeof(shared));

	/* Succeeded: the shared key is written. */
	return 1;
}

/*
 * Compares a name with an uppercase ASCII name, ignoring the case of the
 * letters of name.
 */
static int
pkey_name_equal(
	const char *name,
	const char *expected)
{
	unsigned char letter;
	size_t index;

	/* Compares the two names letter by letter, folding a lowercase ASCII letter of name to uppercase. */
	for (index = 0;
	     name[index] != '\0';
	     index++) {
		letter = (unsigned char)name[index];
		if (letter >= (unsigned char)'a' && letter <= (unsigned char)'z')
			letter = (unsigned char)(letter - (unsigned char)'a' + (unsigned char)'A');
		if (letter != (unsigned char)expected[index])
			return 0;
	}

	/* The names are equal when expected ends where name does. */
	if (expected[index] != '\0')
		return 0;

	/* Equal. */
	return 1;
}

/*
 * Looks up a key type by name; 0 when the name is none of the supported
 * ones.
 */
static int
pkey_type_from_name(
	const char *name)
{
	int equal;

	/* The Ed25519 name. */
	equal = pkey_name_equal(name, "ED25519");
	if (equal)
		return CRYC_PKEY_ED25519;

	/* The X25519 name. */
	equal = pkey_name_equal(name, "X25519");
	if (equal)
		return CRYC_PKEY_X25519;

	/* The ML-KEM-768 name. */
	equal = pkey_name_equal(name, "ML-KEM-768");
	if (equal)
		return CRYC_PKEY_MLKEM768;

	/* No supported type has this name. */
	return 0;
}

/*
 * Makes a key object of a type with one reference and zeroed fields; the
 * private form's memory is allocated when private_size is not 0.
 */
static EVP_PKEY *
pkey_new(
	int type,
	size_t public_size,
	size_t private_size)
{
	EVP_PKEY *pkey;

	/* Allocates the object zeroed: no private key, no reference yet. */
	pkey = calloc(1, sizeof(*pkey));
	if (pkey == NULL) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_MALLOC_FAILURE);
		return NULL;
	}

	/* Allocates the room for the private form, zeroed. */
	if (private_size != 0U) {
		pkey->private_key = calloc(1, private_size);
		if (pkey->private_key == NULL) {
			cryc_err_put(CRYC_LIB_EVP, CRYC_R_MALLOC_FAILURE);
			free(pkey);
			return NULL;
		}
	}

	/* Initializes the object: the caller's reference is the only one. */
	pkey->references = 1L;
	pkey->type = type;
	pkey->public_size = public_size;
	pkey->private_size = private_size;
	pkey->has_private = 0;

	/* Succeeded: the new object. */
	return pkey;
}

/*
 * Makes a public-only key of a type from its raw form.  Ed25519 and
 * X25519 keys are 32 bytes and are taken as given (an Ed25519 key that
 * does not decode fails at verification, an X25519 key is any 32 bytes
 * per RFC 7748 5); an ML-KEM-768 key is 1184 bytes and must pass the
 * modulus check of FIPS 203 7.2.
 */
static EVP_PKEY *
pkey_new_public(
	int type,
	const unsigned char *key,
	size_t length)
{
	EVP_PKEY *pkey;
	size_t expected;
	int error;

	/* The length of the raw public form of the type. */
	expected = PKEY_RAW_SIZE;
	if (type == CRYC_PKEY_MLKEM768)
		expected = CRYC_MLKEM768_EK;
	if (length != expected) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_EVP_INVALID_KEY_LENGTH);
		return NULL;
	}

	/* An encapsulation key whose coefficients are not all reduced is refused (FIPS 203 7.2). */
	if (type == CRYC_PKEY_MLKEM768) {
		error = cryc_mlkem768_check_ek(key);
		if (error != 0) {
			cryc_err_put(CRYC_LIB_EVP, CRYC_R_EVP_INVALID_KEY);
			return NULL;
		}
	}

	/* Makes the key object without a private form. */
	pkey = pkey_new(type, expected, 0U);
	if (pkey == NULL)
		return NULL;

	/* Keeps the public form as received. */
	memcpy(pkey->public_key, key, expected);

	/* Succeeded: a public-only key. */
	return pkey;
}

/*
 * Takes one more reference to a key, which stays alive until each holder
 * has dropped its own.
 */
static void
pkey_take_reference(
	EVP_PKEY *pkey)
{
	/* The count is changed only atomically: other threads may hold the key too. */
	cryc_os_atomic_add(&pkey->references, 1L);
}

/*
 * Makes a digest context hold pkey for signing (mode CRYC_MD_SIGN) or
 * verification (CRYC_MD_VERIFY), replacing what it held; 0 and an error
 * queued when it cannot, with ctx unchanged.
 */
static int
md_ctx_start_key(
	EVP_MD_CTX *ctx,
	EVP_PKEY_CTX **pctx,
	const EVP_MD *type,
	ENGINE *e,
	EVP_PKEY *pkey,
	int mode)
{
	/* Refuses a missing context or key. */
	if (ctx == NULL || pkey == NULL) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_PASSED_NULL_PARAMETER);
		return 0;
	}

	/* Refuses an engine: none is implemented. */
	if (e != NULL) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_PASSED_INVALID_ARGUMENT);
		return 0;
	}

	/* Pure Ed25519 hashes inside the algorithm: a digest here is a mistake. */
	if (type != NULL) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_PASSED_INVALID_ARGUMENT);
		return 0;
	}

	/* Only an Ed25519 key signs and verifies. */
	if (pkey->type != CRYC_PKEY_ED25519) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_EVP_UNSUPPORTED_ALGORITHM);
		return 0;
	}

	/* Signing needs the private key. */
	if (mode == CRYC_MD_SIGN && !pkey->has_private) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_EVP_INVALID_KEY);
		return 0;
	}

	/* The caller gets no operation context: this implementation has none to give. */
	if (pctx != NULL)
		*pctx = NULL;

	/* Takes the new reference before letting go of the old key, so that starting again with the same key is safe. */
	pkey_take_reference(pkey);
	EVP_PKEY_free(ctx->key);

	/* Wipes any digest the context was computing, which may have absorbed secret data. */
	cryc_wipe(&ctx->hash, sizeof(ctx->hash));

	/* The context now signs or verifies with the key. */
	ctx->key = pkey;
	ctx->mode = mode;
	ctx->md = NULL;

	/* Succeeded: the context is started. */
	return 1;
}

/*
 * Makes an operation context holding a reference to pkey (which may be
 * NULL for a key-generation context of the type keygen_type).
 */
static EVP_PKEY_CTX *
pkey_ctx_make(
	EVP_PKEY *pkey,
	int keygen_type)
{
	EVP_PKEY_CTX *ctx;

	/* Allocates the context zeroed: no key, no peer, no operation. */
	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_MALLOC_FAILURE);
		return NULL;
	}

	/* The context keeps its key alive with a reference of its own. */
	if (pkey != NULL)
		pkey_take_reference(pkey);
	ctx->key = pkey;
	ctx->keygen_type = keygen_type;
	ctx->peer = NULL;
	ctx->operation = CRYC_OP_NONE;

	/* Succeeded: the new context. */
	return ctx;
}
