/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The public EVP digest, cipher and MAC functions.
 *
 * A function that fails queues exactly one error of the EVP library
 * (a null argument, an argument that must be NULL, an unsupported
 * algorithm, a context that is not set up, a length out of range, a
 * buffer too small, no memory) and returns the failure value of its
 * interface; a function that succeeds queues nothing.  A failed call
 * changes neither the context it was given nor the caller's output
 * buffers.
 */

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "cryc-api.h"

/* The states of struct evp_cipher_ctx_st's key_set: no key yet. */
#define CIPHER_KEY_NONE		0

/* A key is set and the keystream can still continue. */
#define CIPHER_KEY_READY	1

/*
 * A key is set and the last block of the 64-bit counter's range was
 * already generated: only the bytes still buffered can be used, and a new
 * IV is needed before another block can be.
 */
#define CIPHER_KEY_LAST		2

/*
 * The SHA-256 algorithm object.
 *
 * EVP_sha256 returns this one constant object; a digest context records
 * it in its md field, and a digest is only accepted when its algorithm
 * pointer is this one.  Never written.
 */
static const struct evp_md_st sha256_object = {
	CRYC_HASH_SHA256,
	32
};

/*
 * The ChaCha20 algorithm object.
 *
 * EVP_chacha20 returns this one constant object, and a cipher is only
 * accepted when its pointer is this one.  Never written.
 */
static const struct evp_cipher_st chacha20_object = {
	32,
	16
};

/*
 * The Poly1305 algorithm object.
 *
 * EVP_MAC_fetch returns this one object (its type has no const in the
 * interface), and a MAC context is only made for this pointer.  Never
 * written, so it is shared by every thread without locking.
 */
static struct evp_mac_st poly1305_object = {
	16
};

/* Compares a name with an uppercase ASCII name, ignoring the case of the letters of name. */
static int evp_name_equal(const char *name, const char *expected);

/* Gives up the key a signing or verification context holds. */
static void md_ctx_drop_key(EVP_MD_CTX *ctx);

/* Applies the ChaCha20 keystream to size bytes, continuing the keystream of ctx. */
static int cipher_stream(EVP_CIPHER_CTX *ctx, unsigned char *out, const unsigned char *in, size_t size);

/* Wipes the unused keystream a cipher context buffers. */
static void cipher_discard_keystream(EVP_CIPHER_CTX *ctx);

/*
 * Returns the SHA-256 digest algorithm (a constant object).
 */
const EVP_MD *
EVP_sha256(void)
{
	/* The one constant SHA-256 object. */
	return &sha256_object;
}

/*
 * Returns a digest's output size in bytes (32 for SHA-256), or -1 for
 * NULL.
 */
int
EVP_MD_get_size(
	const EVP_MD *md)
{
	/* A missing algorithm has no size. */
	if (md == NULL)
		return -1;

	/* The digest size of the algorithm. */
	return md->size;
}

/*
 * Makes an empty digest context; NULL without memory.
 */
EVP_MD_CTX *
EVP_MD_CTX_new(void)
{
	EVP_MD_CTX *ctx;

	/* Allocates the context zeroed: no mode, no algorithm, no key. */
	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_MALLOC_FAILURE);
		return NULL;
	}

	/* Succeeded: an empty context that must be started with EVP_DigestInit_ex. */
	return ctx;
}

/*
 * Wipes and frees a digest context (and drops its key, after a
 * DigestSign/VerifyInit); NULL is ignored.
 */
void
EVP_MD_CTX_free(
	EVP_MD_CTX *ctx)
{
	/* Nothing to free. */
	if (ctx == NULL)
		return;

	/* Drops the reference to the key of a signing or verification. */
	md_ctx_drop_key(ctx);

	/* The hash state may have absorbed secret data, so it is wiped before the memory is freed. */
	cryc_wipe(ctx, sizeof(*ctx));
	free(ctx);
}

/*
 * Starts a digest with type (EVP_sha256 only; impl NULL), discarding what
 * ctx held.
 */
int
EVP_DigestInit_ex(
	EVP_MD_CTX *ctx,
	const EVP_MD *type,
	ENGINE *impl)
{
	int error;

	/* Refuses a missing context. */
	if (ctx == NULL) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_PASSED_NULL_PARAMETER);
		return 0;
	}

	/* Refuses an engine: none is implemented. */
	if (impl != NULL) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_PASSED_INVALID_ARGUMENT);
		return 0;
	}

	/* Refuses a missing algorithm. */
	if (type == NULL) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_PASSED_NULL_PARAMETER);
		return 0;
	}

	/* Refuses every algorithm but SHA-256. */
	if (type != &sha256_object) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_EVP_UNSUPPORTED_ALGORITHM);
		return 0;
	}

	/* Gives up the key of a signing or verification the context was used for. */
	md_ctx_drop_key(ctx);

	/* Wipes the previous computation, which may have absorbed secret data. */
	cryc_wipe(&ctx->hash, sizeof(ctx->hash));

	/* Starts the SHA-256 computation. */
	error = cryc_hash_init(&ctx->hash, type->hash);
	if (error != 0) {
		ctx->mode = CRYC_MD_NONE;
		ctx->md = NULL;
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_EVP_UNSUPPORTED_ALGORITHM);
		return 0;
	}

	/* The context now holds a digest in progress. */
	ctx->mode = CRYC_MD_DIGEST;
	ctx->md = type;

	/* Succeeded: ready for EVP_DigestUpdate. */
	return 1;
}

/*
 * Absorbs count bytes.
 */
int
EVP_DigestUpdate(
	EVP_MD_CTX *ctx,
	const void *data,
	size_t count)
{
	/* Refuses a missing context. */
	if (ctx == NULL) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_PASSED_NULL_PARAMETER);
		return 0;
	}

	/* Refuses a context that holds no digest in progress (never started, finished, or used for a signature). */
	if (ctx->mode != CRYC_MD_DIGEST) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_EVP_OPERATION_NOT_INITIALIZED);
		return 0;
	}

	/* Refuses missing bytes when some were announced. */
	if (data == NULL && count != 0) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_PASSED_NULL_PARAMETER);
		return 0;
	}

	/* Absorbs the bytes. */
	cryc_hash_update(&ctx->hash, (const uint8_t *)data, count);

	/* Succeeded: the bytes are part of the digest. */
	return 1;
}

/*
 * Writes the digest to md, its size to *size when size is not NULL; ctx
 * must be started again for reuse.
 */
int
EVP_DigestFinal_ex(
	EVP_MD_CTX *ctx,
	unsigned char *md,
	unsigned int *size)
{
	/* Refuses a missing context. */
	if (ctx == NULL) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_PASSED_NULL_PARAMETER);
		return 0;
	}

	/* Refuses a context that holds no digest in progress. */
	if (ctx->mode != CRYC_MD_DIGEST) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_EVP_OPERATION_NOT_INITIALIZED);
		return 0;
	}

	/* Refuses a missing output, leaving the computation in progress. */
	if (md == NULL) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_PASSED_NULL_PARAMETER);
		return 0;
	}

	/* Finishes the digest into the caller's buffer; the hash state is wiped by the finish. */
	cryc_hash_final(&ctx->hash, (uint8_t *)md);

	/* The context is finished: it needs EVP_DigestInit_ex before it is used again. */
	ctx->mode = CRYC_MD_NONE;
	ctx->md = NULL;

	/* Reports the digest size when the caller asked for it. */
	if (size != NULL)
		*size = (unsigned int)sha256_object.size;

	/* Succeeded: md holds the digest. */
	return 1;
}

/*
 * Makes out a copy of the digest in progress in in (a snapshot); out need
 * not be started.
 */
int
EVP_MD_CTX_copy_ex(
	EVP_MD_CTX *out,
	const EVP_MD_CTX *in)
{
	/* Refuses a missing context on either side. */
	if (out == NULL || in == NULL) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_PASSED_NULL_PARAMETER);
		return 0;
	}

	/* Refuses a source that holds no digest in progress. */
	if (in->mode != CRYC_MD_DIGEST) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_EVP_OPERATION_NOT_INITIALIZED);
		return 0;
	}

	/* A copy onto itself changes nothing. */
	if (out == in)
		return 1;

	/* Gives up the key of a signing or verification the destination was used for. */
	md_ctx_drop_key(out);

	/* Wipes the destination's previous computation. */
	cryc_wipe(&out->hash, sizeof(out->hash));

	/* Copies the digest in progress: a snapshot the two contexts continue independently. */
	out->hash = in->hash;
	out->md = in->md;
	out->mode = CRYC_MD_DIGEST;

	/* Succeeded: out holds a copy of the digest in progress. */
	return 1;
}

/*
 * Hashes count bytes in one call into md (and its size into *size when not
 * NULL).
 */
int
EVP_Digest(
	const void *data,
	size_t count,
	unsigned char *md,
	unsigned int *size,
	const EVP_MD *type,
	ENGINE *impl)
{
	int error;

	/* Refuses an engine: none is implemented. */
	if (impl != NULL) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_PASSED_INVALID_ARGUMENT);
		return 0;
	}

	/* Refuses a missing algorithm. */
	if (type == NULL) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_PASSED_NULL_PARAMETER);
		return 0;
	}

	/* Refuses every algorithm but SHA-256. */
	if (type != &sha256_object) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_EVP_UNSUPPORTED_ALGORITHM);
		return 0;
	}

	/* Refuses a missing output. */
	if (md == NULL) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_PASSED_NULL_PARAMETER);
		return 0;
	}

	/* Refuses missing bytes when some were announced. */
	if (data == NULL && count != 0) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_PASSED_NULL_PARAMETER);
		return 0;
	}

	/* Hashes the bytes in one call. */
	error = cryc_hash_digest(type->hash, (const uint8_t *)data, count, (uint8_t *)md);
	if (error != 0) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_EVP_UNSUPPORTED_ALGORITHM);
		return 0;
	}

	/* Reports the digest size when the caller asked for it. */
	if (size != NULL)
		*size = (unsigned int)type->size;

	/* Succeeded: md holds the digest. */
	return 1;
}

/*
 * Returns the ChaCha20 cipher (a constant object): 32-byte key, 16-byte
 * IV.
 */
const EVP_CIPHER *
EVP_chacha20(void)
{
	/* The one constant ChaCha20 object. */
	return &chacha20_object;
}

/*
 * Makes an empty cipher context; NULL without memory.
 */
EVP_CIPHER_CTX *
EVP_CIPHER_CTX_new(void)
{
	EVP_CIPHER_CTX *ctx;

	/* Allocates the context zeroed: no cipher, no key, a zero IV, no buffered keystream. */
	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_MALLOC_FAILURE);
		return NULL;
	}

	/* Succeeded: an empty context that must be set up with EVP_CipherInit_ex. */
	return ctx;
}

/*
 * Wipes and frees a cipher context; NULL is ignored.
 */
void
EVP_CIPHER_CTX_free(
	EVP_CIPHER_CTX *ctx)
{
	/* Nothing to free. */
	if (ctx == NULL)
		return;

	/* The key and the buffered keystream are secret, so the whole context is wiped before the memory is freed. */
	cryc_wipe(ctx, sizeof(*ctx));
	free(ctx);
}

/*
 * Sets up ChaCha20: cipher EVP_chacha20 (or NULL to keep the one set), key
 * 32 bytes (or NULL to keep it), iv 16 bytes (or NULL to keep it), enc 1,
 * 0 or -1 (keep; ChaCha20 is the same both ways).  The IV is OpenSSL's
 * layout: bytes 0 to 15 are the block function's state words 12 to 15,
 * little-endian, and the block counter is words 12 and 13 taken as one
 * 64-bit little-endian counter.  Setting an IV restarts the keystream.
 */
int
EVP_CipherInit_ex(
	EVP_CIPHER_CTX *ctx,
	const EVP_CIPHER *cipher,
	ENGINE *impl,
	const unsigned char *key,
	const unsigned char *iv,
	int enc)
{
	uint64_t low_half;
	uint64_t high_half;

	/* The direction does not matter: the keystream is the same both ways. */
	UNUSED_PARAMETER(enc);

	/* Refuses a missing context. */
	if (ctx == NULL) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_PASSED_NULL_PARAMETER);
		return 0;
	}

	/* Refuses an engine: none is implemented. */
	if (impl != NULL) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_PASSED_INVALID_ARGUMENT);
		return 0;
	}

	/* Refuses every cipher but ChaCha20. */
	if (cipher != NULL && cipher != &chacha20_object) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_EVP_UNSUPPORTED_ALGORITHM);
		return 0;
	}

	/* Refuses a setup that names no cipher when the context has none. */
	if (cipher == NULL && ctx->cipher == NULL) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_PASSED_NULL_PARAMETER);
		return 0;
	}

	/* The context is for ChaCha20 from here on. */
	ctx->cipher = &chacha20_object;

	/*
	 * Takes the key.  The buffered keystream belongs to the old key, so
	 * it is dropped; the counter and the nonce stay.
	 */
	if (key != NULL) {
		memcpy(ctx->key, key, sizeof(ctx->key));
		ctx->key_set = CIPHER_KEY_READY;
		cipher_discard_keystream(ctx);
	}

	/*
	 * Takes the IV and restarts the keystream: the first eight bytes are
	 * the 64-bit little-endian block counter (state words 12 and 13), the
	 * last eight the nonce (words 14 and 15).  A context whose counter
	 * range was used up can go on.
	 */
	if (iv != NULL) {
		low_half = cryc_load_le32(iv);
		high_half = cryc_load_le32(iv + 4);
		ctx->counter = (high_half << 32) | low_half;
		memcpy(ctx->nonce, iv + 8, sizeof(ctx->nonce));
		cipher_discard_keystream(ctx);
		if (ctx->key_set == CIPHER_KEY_LAST)
			ctx->key_set = CIPHER_KEY_READY;
	}

	/* Succeeded: the context holds what was given. */
	return 1;
}

/*
 * XORs in_length bytes of in with the keystream into out (out may be in),
 * continuing the keystream across calls; *out_length becomes in_length.
 */
int
EVP_CipherUpdate(
	EVP_CIPHER_CTX *ctx,
	unsigned char *out,
	int *out_length,
	const unsigned char *in,
	int in_length)
{
	int done;

	/* Refuses a missing context or length output. */
	if (ctx == NULL || out_length == NULL) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_PASSED_NULL_PARAMETER);
		return 0;
	}

	/* Refuses a negative length. */
	if (in_length < 0) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_PASSED_INVALID_ARGUMENT);
		return 0;
	}

	/* Applies the keystream. */
	done = cipher_stream(ctx, out, in, (size_t)in_length);
	if (done == 0)
		return 0;

	/* Reports the bytes written: as many as were given. */
	*out_length = in_length;

	/* Succeeded: out holds the transformed bytes. */
	return 1;
}

/*
 * As EVP_CipherUpdate without the length output.  Returns length (as int)
 * on success, and -1 on failure (a context not set up, length over
 * INT_MAX), as OpenSSL 3.5 does for provider ciphers.
 */
int
EVP_Cipher(
	EVP_CIPHER_CTX *ctx,
	unsigned char *out,
	const unsigned char *in,
	unsigned int length)
{
	int done;

	/* Refuses a missing context. */
	if (ctx == NULL) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_PASSED_NULL_PARAMETER);
		return -1;
	}

	/* Refuses a length that does not fit the int that is returned. */
	if (length > (unsigned int)INT_MAX) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_PASSED_INVALID_ARGUMENT);
		return -1;
	}

	/* Applies the keystream. */
	done = cipher_stream(ctx, out, in, (size_t)length);
	if (done == 0)
		return -1;

	/* Succeeded: the number of bytes transformed. */
	return (int)length;
}

/*
 * Returns the MAC algorithm named "POLY1305" (libctx and properties NULL),
 * or NULL for another name.
 */
EVP_MAC *
EVP_MAC_fetch(
	OSSL_LIB_CTX *libctx,
	const char *algorithm,
	const char *properties)
{
	int matches;

	/* Refuses a library context and a property query: none is implemented. */
	if (libctx != NULL || properties != NULL) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_PASSED_INVALID_ARGUMENT);
		return NULL;
	}

	/* Refuses a missing name. */
	if (algorithm == NULL) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_PASSED_NULL_PARAMETER);
		return NULL;
	}

	/* Refuses every name but POLY1305 (names are not case sensitive). */
	matches = evp_name_equal(algorithm, "POLY1305");
	if (matches == 0) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_EVP_UNSUPPORTED_ALGORITHM);
		return NULL;
	}

	/* Succeeded: the one Poly1305 object. */
	return &poly1305_object;
}

/*
 * Releases a MAC algorithm; NULL is ignored.
 */
void
EVP_MAC_free(
	EVP_MAC *mac)
{
	/* The algorithm is a constant object that is never freed. */
	UNUSED_PARAMETER(mac);
}

/*
 * Makes a MAC context for mac; NULL on failure.
 */
EVP_MAC_CTX *
EVP_MAC_CTX_new(
	EVP_MAC *mac)
{
	EVP_MAC_CTX *ctx;

	/* Refuses a missing algorithm. */
	if (mac == NULL) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_PASSED_NULL_PARAMETER);
		return NULL;
	}

	/* Refuses an algorithm that EVP_MAC_fetch did not return. */
	if (mac != &poly1305_object) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_EVP_UNSUPPORTED_ALGORITHM);
		return NULL;
	}

	/* Allocates the context zeroed: not started. */
	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_MALLOC_FAILURE);
		return NULL;
	}

	/* Succeeded: a context that must be started with EVP_MAC_init. */
	return ctx;
}

/*
 * Wipes and frees a MAC context; NULL is ignored.
 */
void
EVP_MAC_CTX_free(
	EVP_MAC_CTX *ctx)
{
	/* Nothing to free. */
	if (ctx == NULL)
		return;

	/* The key material of a computation in progress is secret, so the context is wiped before the memory is freed. */
	cryc_wipe(ctx, sizeof(*ctx));
	free(ctx);
}

/*
 * (Re)starts Poly1305 with a 32-byte one-time key (params NULL); may be
 * called again on the same context.
 */
int
EVP_MAC_init(
	EVP_MAC_CTX *ctx,
	const unsigned char *key,
	size_t length,
	const OSSL_PARAM params[])
{
	/* Refuses a missing context. */
	if (ctx == NULL) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_PASSED_NULL_PARAMETER);
		return 0;
	}

	/* Refuses a parameter array: none is implemented. */
	if (params != NULL) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_PASSED_INVALID_ARGUMENT);
		return 0;
	}

	/* Refuses a missing key: Poly1305 keeps no key between computations. */
	if (key == NULL) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_PASSED_NULL_PARAMETER);
		return 0;
	}

	/* Refuses a key that is not the 32 bytes of r and s. */
	if (length != CRYC_POLY1305_KEY) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_EVP_INVALID_KEY_LENGTH);
		return 0;
	}

	/* Starts the computation, discarding one that was in progress. */
	cryc_poly1305_init(&ctx->poly, (const uint8_t *)key);

	/* The context now holds a computation that accepts bytes. */
	ctx->started = 1;

	/* Succeeded: ready for EVP_MAC_update. */
	return 1;
}

/*
 * Absorbs length bytes.
 */
int
EVP_MAC_update(
	EVP_MAC_CTX *ctx,
	const unsigned char *data,
	size_t length)
{
	/* Refuses a missing context. */
	if (ctx == NULL) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_PASSED_NULL_PARAMETER);
		return 0;
	}

	/* Refuses a context that is not started (never initialized, or finished). */
	if (ctx->started == 0) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_EVP_OPERATION_NOT_INITIALIZED);
		return 0;
	}

	/* Refuses missing bytes when some were announced. */
	if (data == NULL && length != 0) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_PASSED_NULL_PARAMETER);
		return 0;
	}

	/* Absorbs the bytes. */
	cryc_poly1305_update(&ctx->poly, (const uint8_t *)data, length);

	/* Succeeded: the bytes are part of the tag. */
	return 1;
}

/*
 * Writes the 16-byte tag (out_size at least 16; *out_length 16); the
 * context needs EVP_MAC_init before reuse.  With out NULL it only sets
 * *out_length to 16 and returns 1, leaving the computation in progress.
 */
int
EVP_MAC_final(
	EVP_MAC_CTX *ctx,
	unsigned char *out,
	size_t *out_length,
	size_t out_size)
{
	/* Refuses a missing context. */
	if (ctx == NULL) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_PASSED_NULL_PARAMETER);
		return 0;
	}

	/* Answers a query for the size of the tag: reports it and computes nothing. */
	if (out == NULL) {
		if (out_length == NULL) {
			cryc_err_put(CRYC_LIB_EVP, CRYC_R_PASSED_NULL_PARAMETER);
			return 0;
		}
		*out_length = CRYC_POLY1305_TAG;
		return 1;
	}

	/* Refuses a context that is not started. */
	if (ctx->started == 0) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_EVP_OPERATION_NOT_INITIALIZED);
		return 0;
	}

	/* Refuses a buffer that cannot hold the tag, leaving the computation in progress. */
	if (out_size < CRYC_POLY1305_TAG) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_EVP_BUFFER_TOO_SMALL);
		return 0;
	}

	/* Finishes the tag into the caller's buffer; the state, which holds the key, is wiped by the finish. */
	cryc_poly1305_final(&ctx->poly, (uint8_t *)out);

	/* The context is finished: it needs EVP_MAC_init before it is used again. */
	ctx->started = 0;

	/* Reports the tag size when the caller asked for it. */
	if (out_length != NULL)
		*out_length = CRYC_POLY1305_TAG;

	/* Succeeded: out holds the tag. */
	return 1;
}

/*
 * Compares a name with an uppercase ASCII name, ignoring the case of the
 * letters of name.
 */
static int
evp_name_equal(
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
 * Gives up the reference a signing or verification context holds to its
 * key.
 */
static void
md_ctx_drop_key(
	EVP_MD_CTX *ctx)
{
	/* A context without a key has nothing to drop. */
	if (ctx->key == NULL)
		return;

	/* Drops the context's reference to the key. */
	EVP_PKEY_free(ctx->key);
	ctx->key = NULL;
}

/*
 * Wipes the buffered keystream bytes of a cipher context and forgets them.
 */
static void
cipher_discard_keystream(
	EVP_CIPHER_CTX *ctx)
{
	/* The buffered keystream is secret: it is overwritten, not just forgotten. */
	cryc_wipe(ctx->keystream, sizeof(ctx->keystream));
	ctx->available = 0;
}

/*
 * XORs size bytes of in with the ChaCha20 keystream into out, continuing
 * from the buffered rest of the current block and the next block counter.
 * Nothing is written when it fails.
 */
static int
cipher_stream(
	EVP_CIPHER_CTX *ctx,
	unsigned char *out,
	const unsigned char *in,
	size_t size)
{
	uint64_t blocks;
	uint64_t next_counter;
	uint64_t blocks_left;
	size_t reuse;
	size_t need;
	size_t whole;
	size_t tail;
	size_t index;
	size_t first;
	int error;

	/* Refuses a context that has no cipher or no key. */
	if (ctx->cipher == NULL || ctx->key_set == CIPHER_KEY_NONE) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_EVP_OPERATION_NOT_INITIALIZED);
		return 0;
	}

	/* No bytes: nothing to do, whatever the pointers. */
	if (size == 0)
		return 1;

	/* Refuses missing buffers for a nonzero length. */
	if (in == NULL || out == NULL) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_PASSED_NULL_PARAMETER);
		return 0;
	}

	/* Splits the request into bytes from the buffered block and bytes that need new blocks. */
	reuse = size;
	if (reuse > ctx->available)
		reuse = ctx->available;
	need = size - reuse;

	/* Counts the new blocks the request needs: the whole ones and a part of one. */
	blocks = (uint64_t)need / CRYC_CHACHA20_BLOCK;
	if (((uint64_t)need % CRYC_CHACHA20_BLOCK) != 0)
		blocks++;

	/*
	 * Refuses a request that would run the 64-bit block counter past
	 * 2^64 - 1 before anything is written.  When the counter is 0 and
	 * the range is not used up, 2^64 blocks remain, more than any
	 * request can need.
	 */
	if (blocks > 0) {
		if (ctx->key_set == CIPHER_KEY_LAST) {
			cryc_err_put(CRYC_LIB_EVP, CRYC_R_PASSED_INVALID_ARGUMENT);
			return 0;
		}
		if (ctx->counter != 0) {
			blocks_left = (uint64_t)0 - ctx->counter;
			if (blocks > blocks_left) {
				cryc_err_put(CRYC_LIB_EVP, CRYC_R_PASSED_INVALID_ARGUMENT);
				return 0;
			}
		}
	}

	/* Uses the rest of the buffered block first. */
	first = CRYC_CHACHA20_BLOCK - ctx->available;
	for (index = 0;
	     index < reuse;
	     index++)
		out[index] = (unsigned char)(in[index] ^ ctx->keystream[first + index]);
	ctx->available -= reuse;
	if (ctx->available == 0)
		cipher_discard_keystream(ctx);

	/* Applies the keystream of the whole blocks straight to the caller's bytes. */
	whole = (need / CRYC_CHACHA20_BLOCK) * CRYC_CHACHA20_BLOCK;
	if (whole > 0) {
		error = cryc_chacha20_xor64(ctx->key, ctx->counter, ctx->nonce, in + reuse, out + reuse, whole);
		if (error != 0) {
			cryc_err_put(CRYC_LIB_EVP, CRYC_R_INTERNAL_ERROR);
			return 0;
		}

		/*
		 * Moves the counter past the blocks used.  A counter that
		 * wrapped to a smaller value used the last block of its range.
		 */
		next_counter = ctx->counter + (uint64_t)(whole / CRYC_CHACHA20_BLOCK);
		if (next_counter < ctx->counter)
			ctx->key_set = CIPHER_KEY_LAST;
		ctx->counter = next_counter;
	}

	/* Makes a new block for the last partial block (by encrypting zeros in place in the buffer) and keeps the rest of it for the next call. */
	tail = need - whole;
	if (tail > 0) {
		memset(ctx->keystream, 0, sizeof(ctx->keystream));
		error = cryc_chacha20_xor64(ctx->key, ctx->counter, ctx->nonce, ctx->keystream, ctx->keystream, sizeof(ctx->keystream));
		if (error != 0) {
			cryc_err_put(CRYC_LIB_EVP, CRYC_R_INTERNAL_ERROR);
			return 0;
		}

		/* Applies the beginning of the new block to the caller's last bytes. */
		for (index = 0;
		     index < tail;
		     index++)
			out[reuse + whole + index] = (unsigned char)(in[reuse + whole + index] ^ ctx->keystream[index]);
		ctx->available = CRYC_CHACHA20_BLOCK - tail;

		/* Moves the counter past the block; wrapping to 0 means the last block of its range was used. */
		next_counter = ctx->counter + 1U;
		if (next_counter == 0)
			ctx->key_set = CIPHER_KEY_LAST;
		ctx->counter = next_counter;
	}

	/* Succeeded: the bytes are transformed and the keystream position is advanced. */
	return 1;
}
