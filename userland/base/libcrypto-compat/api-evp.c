/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The public EVP digest, cipher and MAC functions.  The ciphers are
 * ChaCha20 and AES-256-CBC without padding (NIST SP 800-38A 6.2).
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
 * The AES-256-CBC algorithm object.
 *
 * EVP_aes_256_cbc returns this one constant object; a context is in CBC
 * mode exactly when its cipher pointer is this one.  Never written.
 */
static const struct evp_cipher_st aes256_cbc_object = {
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

/* Sets up AES-256-CBC on a context, as EVP_CipherInit_ex describes. */
static int cipher_cbc_init(EVP_CIPHER_CTX *ctx, const EVP_CIPHER *cipher, const unsigned char *key, const unsigned char *iv, int enc);

/* Encrypts or decrypts the whole blocks of the buffered bytes and size new bytes in CBC mode, keeping the rest. */
static int cipher_cbc_update(EVP_CIPHER_CTX *ctx, unsigned char *out, int *out_length, const unsigned char *in, int size);

/* Encrypts or decrypts one 16-byte block in CBC mode, chaining it (NIST SP 800-38A 6.2). */
static void cipher_cbc_block(EVP_CIPHER_CTX *ctx, const uint8_t *in, unsigned char *out);

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
 * Returns the AES-256-CBC cipher (a constant object): 32-byte key, 16-byte
 * IV, 16-byte blocks.
 */
const EVP_CIPHER *
EVP_aes_256_cbc(void)
{
	/* The one constant AES-256-CBC object. */
	return &aes256_cbc_object;
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
	int done;

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

	/* Refuses every cipher but ChaCha20 and AES-256-CBC. */
	if (cipher != NULL &&
	    cipher != &chacha20_object &&
	    cipher != &aes256_cbc_object) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_EVP_UNSUPPORTED_ALGORITHM);
		return 0;
	}

	/* Refuses a setup that names no cipher when the context has none. */
	if (cipher == NULL && ctx->cipher == NULL) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_PASSED_NULL_PARAMETER);
		return 0;
	}

	/* AES-256-CBC is set up on its own. */
	if (cipher == &aes256_cbc_object || (cipher == NULL && ctx->cipher == &aes256_cbc_object)) {
		done = cipher_cbc_init(ctx, cipher, key, iv, enc);
		return done;
	}

	/* A context that held AES-256-CBC starts afresh as ChaCha20. */
	if (ctx->cipher == &aes256_cbc_object)
		cryc_wipe(ctx, sizeof(*ctx));

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

	/* AES-256-CBC works on whole blocks. */
	if (ctx->cipher == &aes256_cbc_object) {
		done = cipher_cbc_update(ctx, out, out_length, in, in_length);
		return done;
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

	/* Only ChaCha20 is applied without the length output. */
	if (ctx->cipher == &aes256_cbc_object) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_UNSUPPORTED);
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
 * Ends a cipher: writes nothing and sets *out_length to 0; AES-256-CBC
 * fails with a partial block buffered or with padding on.
 */
int
EVP_CipherFinal_ex(
	EVP_CIPHER_CTX *ctx,
	unsigned char *out,
	int *out_length)
{
	/* Nothing is written: the ciphers here keep no output back. */
	UNUSED_PARAMETER(out);

	/* Refuses a missing context or length output. */
	if (ctx == NULL || out_length == NULL) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_PASSED_NULL_PARAMETER);
		return 0;
	}

	/* Refuses a context that was never set up. */
	if (ctx->cipher == NULL) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_EVP_OPERATION_NOT_INITIALIZED);
		return 0;
	}

	/* AES-256-CBC: padding is not implemented, and data must have been whole blocks (NIST SP 800-38A 6.2, Appendix A). */
	if (ctx->cipher == &aes256_cbc_object) {

		/* Refuses padding, which this mode does not implement. */
		if (ctx->padding) {
			cryc_err_put(CRYC_LIB_EVP, CRYC_R_UNSUPPORTED);
			return 0;
		}

		/* Refuses data that does not end on a block boundary. */
		if (ctx->partial_size != 0U) {
			cryc_err_put(CRYC_LIB_EVP, CRYC_R_EVP_DATA_NOT_MULTIPLE_OF_BLOCK_LENGTH);
			return 0;
		}
	}

	/* Succeeded: no bytes are left to write. */
	*out_length = 0;
	return 1;
}

/*
 * Turns the block cipher's padding on or off.
 */
int
EVP_CIPHER_CTX_set_padding(
	EVP_CIPHER_CTX *ctx,
	int pad)
{
	/* Refuses a missing context. */
	if (ctx == NULL) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_PASSED_NULL_PARAMETER);
		return 0;
	}

	/* Records the choice; ChaCha20 never reads it. */
	ctx->padding = 0;
	if (pad != 0)
		ctx->padding = 1;

	/* Succeeded: the padding is set. */
	return 1;
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

		/* Succeeded: the size of the tag is reported. */
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

		/* Refuses a request once the last key's counter range is in use. */
		if (ctx->key_set == CIPHER_KEY_LAST) {
			cryc_err_put(CRYC_LIB_EVP, CRYC_R_PASSED_INVALID_ARGUMENT);
			return 0;
		}

		/* Refuses a request that needs more blocks than the counter has left. */
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

/*
 * Sets up AES-256-CBC: a named cipher starts the context afresh (no key,
 * a zero IV, padding on, encrypting); then the key is expanded, the IV
 * taken and the direction set.  A key or an IV drops any buffered partial
 * block.
 */
static int
cipher_cbc_init(
	EVP_CIPHER_CTX *ctx,
	const EVP_CIPHER *cipher,
	const unsigned char *key,
	const unsigned char *iv,
	int enc)
{
	int error;

	/* Expands the key first, so that a failure leaves the context as it was. */
	if (key != NULL) {
		error = cryc_aes_init(&ctx->aes, key, CRYC_AES256_KEY);
		if (error != 0) {
			cryc_err_put(CRYC_LIB_EVP, CRYC_R_INTERNAL_ERROR);
			return 0;
		}
	}

	/* A named cipher starts afresh: everything but a new key is forgotten. */
	if (cipher != NULL) {
		cryc_wipe(ctx->key, sizeof(ctx->key));
		cryc_wipe(ctx->keystream, sizeof(ctx->keystream));
		cryc_wipe(ctx->chain, sizeof(ctx->chain));
		cryc_wipe(ctx->partial, sizeof(ctx->partial));
		ctx->counter = 0;
		cryc_wipe(ctx->nonce, sizeof(ctx->nonce));
		ctx->available = 0U;
		ctx->partial_size = 0U;
		ctx->key_set = CIPHER_KEY_NONE;
		ctx->encrypt = 1;
		ctx->padding = 1;
		if (key == NULL)
			cryc_aes_clear(&ctx->aes);
		ctx->cipher = &aes256_cbc_object;
	}

	/* The key is ready; buffered bytes belonged to the old one. */
	if (key != NULL) {
		ctx->key_set = CIPHER_KEY_READY;
		cryc_wipe(ctx->partial, sizeof(ctx->partial));
		ctx->partial_size = 0U;
	}

	/* The IV starts the chain again. */
	if (iv != NULL) {
		memcpy(ctx->chain, iv, CRYC_AES_BLOCK);
		cryc_wipe(ctx->partial, sizeof(ctx->partial));
		ctx->partial_size = 0U;
	}

	/* The direction: 1 encrypts, 0 decrypts, anything else keeps it. */
	if (enc == 1)
		ctx->encrypt = 1;
	else if (enc == 0)
		ctx->encrypt = 0;

	/* Succeeded: the context holds what was given. */
	return 1;
}

/*
 * Takes size bytes after the buffered partial block, transforms every
 * whole block they make in CBC mode into out, and keeps the rest.  Each
 * block is gathered into a local buffer before its output is written.
 */
static int
cipher_cbc_update(
	EVP_CIPHER_CTX *ctx,
	unsigned char *out,
	int *out_length,
	const unsigned char *in,
	int size)
{
	uint8_t block[CRYC_AES_BLOCK];
	size_t remaining;
	size_t written;
	size_t take;

	/* Refuses a context without a key. */
	if (ctx->key_set == CIPHER_KEY_NONE) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_EVP_OPERATION_NOT_INITIALIZED);
		return 0;
	}

	/* Padding is not implemented: it must have been turned off. */
	if (ctx->padding) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_UNSUPPORTED);
		return 0;
	}

	/* No bytes: nothing written. */
	if (size == 0) {
		*out_length = 0;
		return 1;
	}

	/* Refuses missing buffers. */
	if (in == NULL || out == NULL) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_PASSED_NULL_PARAMETER);
		return 0;
	}

	/* Refuses a length whose output could pass INT_MAX. */
	if (size > INT_MAX - (int)CRYC_AES_BLOCK) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_PASSED_INVALID_ARGUMENT);
		return 0;
	}

	/* Each whole block of the buffered bytes and the new ones. */
	remaining = (size_t)size;
	written = 0U;
	while (ctx->partial_size + remaining >= CRYC_AES_BLOCK) {
		take = CRYC_AES_BLOCK - ctx->partial_size;
		memcpy(block, ctx->partial, ctx->partial_size);
		memcpy(block + ctx->partial_size, in, take);
		in += take;
		remaining -= take;
		ctx->partial_size = 0U;
		cipher_cbc_block(ctx, block, out + written);
		written += CRYC_AES_BLOCK;
	}

	/* Keeps the rest for the next call. */
	memcpy(ctx->partial + ctx->partial_size, in, remaining);
	ctx->partial_size += remaining;

	/* The block held plaintext or ciphertext. */
	cryc_wipe(block, sizeof(block));

	/* Succeeded: the whole blocks are written. */
	*out_length = (int)written;
	return 1;
}

/*
 * Transforms one block in CBC mode (NIST SP 800-38A 6.2): encrypting, C =
 * CIPH(P xor previous); decrypting, P = CIPH^-1(C) xor previous; the
 * ciphertext block becomes the previous one.  in is a private copy, so
 * out may be the caller's input.
 */
static void
cipher_cbc_block(
	EVP_CIPHER_CTX *ctx,
	const uint8_t *in,
	unsigned char *out)
{
	uint8_t work[CRYC_AES_BLOCK];
	size_t index;

	/* Chains and transforms the block in the direction the context was set up for. */
	if (ctx->encrypt) {
		/* Encrypts the block chained with the previous ciphertext, which it then replaces. */
		for (index = 0U; index < CRYC_AES_BLOCK; index++)
			work[index] = (uint8_t)(in[index] ^ ctx->chain[index]);
		cryc_aes_encrypt_blocks(&ctx->aes, work, ctx->chain, 1U);
		memcpy(out, ctx->chain, CRYC_AES_BLOCK);
	} else {
		/* Decrypts the block, unchains it, and keeps the ciphertext for the next one. */
		cryc_aes_decrypt_blocks(&ctx->aes, in, work, 1U);
		for (index = 0U; index < CRYC_AES_BLOCK; index++)
			work[index] = (uint8_t)(work[index] ^ ctx->chain[index]);
		memcpy(ctx->chain, in, CRYC_AES_BLOCK);
		memcpy(out, work, CRYC_AES_BLOCK);
	}

	/* The work block held plaintext. */
	cryc_wipe(work, sizeof(work));
}
