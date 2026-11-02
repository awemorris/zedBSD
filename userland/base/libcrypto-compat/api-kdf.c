/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The one-shot HMAC (<compat/crypto/hmac.h>) and HKDF through OpenSSL 3's
 * EVP_KDF interface (<compat/crypto/kdf.h>), over cryc-hash.h.
 *
 * A function that fails queues exactly one error of the EVP library and
 * returns the failure value of its interface; a function that succeeds
 * queues nothing.  A failed EVP_KDF_derive changes neither the context
 * nor the caller's output.
 *
 * The HMAC key, the HKDF input keying material and the pseudorandom key
 * are secret: they are wiped before their memory is released.  The
 * decisions made here depend only on lengths, names and the state of the
 * objects, which are public.
 */

#include <stdlib.h>
#include <string.h>

#include "cryc-api.h"

/* The size of a SHA-256 digest, HMAC-SHA-256's and HKDF-SHA-256's HashLen. */
#define KDF_SHA256_SIZE		32U

/*
 * The HKDF algorithm object.
 *
 * EVP_KDF_fetch returns this one object (its type has no const in the
 * interface), and a context is only made for this pointer.  Never written,
 * so it is shared by every thread without locking.
 */
static struct evp_kdf_st hkdf_object = {
	1
};

/*
 * The settings one EVP_KDF_derive call's parameters bring, before they
 * are applied to the context: each has_ flag says the parameter was
 * present; info is the joined info parameters (malloc'd, NULL when none
 * or empty).
 */
struct kdf_settings {
	int has_digest;
	enum cryc_hash_id hash;
	int has_key;
	const uint8_t *key;
	size_t key_size;
	int has_salt;
	const uint8_t *salt;
	size_t salt_size;
	int has_info;
	uint8_t *info;
	size_t info_size;
};

/* Reads an array of parameters into settings; 0 and an error queued when one is not acceptable. */
static int kdf_read_params(struct kdf_settings *settings, const OSSL_PARAM params[]);

/* Joins every info parameter of params into one buffer of settings; 0 and an error queued on failure. */
static int kdf_join_info(struct kdf_settings *settings, const OSSL_PARAM params[]);

/* Copies size bytes into a new buffer (NULL for size 0); 0 and an error queued without memory. */
static int kdf_copy(uint8_t **copy, const uint8_t *data, size_t size);

/* Wipes and frees a buffer of size bytes; NULL is ignored. */
static void kdf_release(uint8_t *buffer, size_t size);

/*
 * Computes HMAC(key, data) with evp_md (EVP_sha256 only) into md.
 */
unsigned char *
HMAC(
	const EVP_MD *evp_md,
	const void *key,
	int key_len,
	const unsigned char *data,
	size_t data_len,
	unsigned char *md,
	unsigned int *md_len)
{
	const EVP_MD *sha256;
	static const uint8_t empty[1] = { 0 };
	int error;

	/* Refuses a missing digest or output: there is no static output buffer. */
	if (evp_md == NULL || md == NULL) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_PASSED_NULL_PARAMETER);
		return NULL;
	}

	/* Refuses every digest but SHA-256. */
	sha256 = EVP_sha256();
	if (evp_md != sha256) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_EVP_UNSUPPORTED_ALGORITHM);
		return NULL;
	}

	/* Refuses a negative key length. */
	if (key_len < 0) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_PASSED_INVALID_ARGUMENT);
		return NULL;
	}

	/* Refuses a missing key or message that is not empty. */
	if ((key == NULL && key_len != 0) || (data == NULL && data_len != 0U)) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_PASSED_NULL_PARAMETER);
		return NULL;
	}

	/* An empty key or message may come as a NULL pointer; the hash is given a real one. */
	if (key == NULL)
		key = empty;
	if (data == NULL)
		data = empty;

	/* The MAC (RFC 2104 section 2). */
	error = cryc_hmac(CRYC_HASH_SHA256, (const uint8_t *)key, (size_t)key_len, data, data_len, md);
	if (error != 0) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_INTERNAL_ERROR);
		return NULL;
	}

	/* Reports the size when asked. */
	if (md_len != NULL)
		*md_len = KDF_SHA256_SIZE;

	/* Succeeded: md holds the MAC. */
	return md;
}

/*
 * Returns the KDF named "HKDF" (libctx and properties NULL), or NULL.
 */
EVP_KDF *
EVP_KDF_fetch(
	OSSL_LIB_CTX *libctx,
	const char *algorithm,
	const char *properties)
{
	int matches;

	/* Refuses a library context and a property query: neither is implemented. */
	if (libctx != NULL || properties != NULL) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_PASSED_INVALID_ARGUMENT);
		return NULL;
	}

	/* Refuses a missing name. */
	if (algorithm == NULL) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_PASSED_NULL_PARAMETER);
		return NULL;
	}

	/* Refuses every name but HKDF (names are not case sensitive). */
	matches = cryc_param_name_equal(algorithm, "HKDF");
	if (!matches) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_EVP_UNSUPPORTED_ALGORITHM);
		return NULL;
	}

	/* Succeeded: the one HKDF object. */
	return &hkdf_object;
}

/*
 * Releases a KDF; NULL is ignored.
 */
void
EVP_KDF_free(
	EVP_KDF *kdf)
{
	/* The algorithm is a constant object that is never freed. */
	UNUSED_PARAMETER(kdf);
}

/*
 * Makes a derivation context for kdf with nothing set; NULL on failure.
 */
EVP_KDF_CTX *
EVP_KDF_CTX_new(
	EVP_KDF *kdf)
{
	EVP_KDF_CTX *ctx;

	/* Refuses a missing algorithm. */
	if (kdf == NULL) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_PASSED_NULL_PARAMETER);
		return NULL;
	}

	/* Refuses an algorithm that EVP_KDF_fetch did not return. */
	if (kdf != &hkdf_object) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_EVP_UNSUPPORTED_ALGORITHM);
		return NULL;
	}

	/* Allocates the context zeroed: no digest, no key, no salt, no info. */
	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_MALLOC_FAILURE);
		return NULL;
	}

	/* No digest is chosen until one is given. */
	ctx->hash = CRYC_HASH_NONE;

	/* Succeeded: an empty context. */
	return ctx;
}

/*
 * Wipes and frees a context; NULL is ignored.
 */
void
EVP_KDF_CTX_free(
	EVP_KDF_CTX *ctx)
{
	/* Nothing to free. */
	if (ctx == NULL)
		return;

	/* The key is secret; the salt and the info are wiped too. */
	kdf_release(ctx->key, ctx->key_size);
	kdf_release(ctx->salt, ctx->salt_size);
	kdf_release(ctx->info, ctx->info_size);

	/* Wipes the object, then releases it. */
	cryc_wipe(ctx, sizeof(*ctx));
	free(ctx);
}

/*
 * Applies params to the context, then derives keylen bytes into key with
 * HKDF (RFC 5869 2.2 and 2.3).
 */
int
EVP_KDF_derive(
	EVP_KDF_CTX *ctx,
	unsigned char *key,
	size_t keylen,
	const OSSL_PARAM params[])
{
	struct kdf_settings settings;
	uint8_t prk[KDF_SHA256_SIZE];
	uint8_t *key_copy;
	uint8_t *salt_copy;
	int copied;
	int good;
	int error;

	/* Refuses a missing context. */
	if (ctx == NULL) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_PASSED_NULL_PARAMETER);
		return 0;
	}

	/* Reads the parameters without touching the context yet. */
	memset(&settings, 0, sizeof(settings));
	good = kdf_read_params(&settings, params);
	if (!good)
		return 0;

	/* Copies the new key. */
	key_copy = NULL;
	if (settings.has_key) {
		copied = kdf_copy(&key_copy, settings.key, settings.key_size);
		if (!copied) {
			kdf_release(settings.info, settings.info_size);
			return 0;
		}
	}

	/* Copies the new salt. */
	salt_copy = NULL;
	if (settings.has_salt) {
		copied = kdf_copy(&salt_copy, settings.salt, settings.salt_size);
		if (!copied) {
			kdf_release(key_copy, settings.key_size);
			kdf_release(settings.info, settings.info_size);
			return 0;
		}
	}

	/* Applies the digest, replacing the one the context held. */
	if (settings.has_digest)
		ctx->hash = settings.hash;

	/* Applies the key, replacing the one the context held. */
	if (settings.has_key) {
		kdf_release(ctx->key, ctx->key_size);
		ctx->key = key_copy;
		ctx->key_size = settings.key_size;
		ctx->key_set = 1;
	}

	/* Applies the salt, replacing the one the context held. */
	if (settings.has_salt) {
		kdf_release(ctx->salt, ctx->salt_size);
		ctx->salt = salt_copy;
		ctx->salt_size = settings.salt_size;
	}

	/* Applies the info, replacing the one the context held. */
	if (settings.has_info) {
		kdf_release(ctx->info, ctx->info_size);
		ctx->info = settings.info;
		ctx->info_size = settings.info_size;
	}

	/* A derivation needs the digest and the key. */
	if (ctx->hash == CRYC_HASH_NONE || !ctx->key_set) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_EVP_OPERATION_NOT_INITIALIZED);
		return 0;
	}

	/* Refuses a missing output. */
	if (key == NULL) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_PASSED_NULL_PARAMETER);
		return 0;
	}

	/* Refuses an output of no bytes, or of more than HKDF-Expand can make. */
	if (keylen == 0U || keylen > 255U * KDF_SHA256_SIZE) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_EVP_INVALID_KEY_LENGTH);
		return 0;
	}

	/* HKDF-Extract: PRK = HMAC-SHA-256(salt, IKM), the empty salt meaning HashLen zeros. */
	error = cryc_hkdf_extract(ctx->hash, ctx->salt, ctx->salt_size, ctx->key, ctx->key_size, prk);
	if (error != 0) {
		cryc_wipe(prk, sizeof(prk));
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_INTERNAL_ERROR);
		return 0;
	}

	/* HKDF-Expand: keylen bytes of OKM from PRK and info. */
	error = cryc_hkdf_expand(ctx->hash, prk, sizeof(prk), ctx->info, ctx->info_size, key, keylen);
	cryc_wipe(prk, sizeof(prk));
	if (error != 0) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_INTERNAL_ERROR);
		return 0;
	}

	/* Succeeded: key holds the derived bytes. */
	return 1;
}

/*
 * Reads params into settings: the digest (a UTF-8 string naming SHA-256),
 * the key and the salt (octet strings; the first of each counts), and the
 * info (every octet string named info, joined).  Any other name or data
 * type is refused.
 */
static int
kdf_read_params(
	struct kdf_settings *settings,
	const OSSL_PARAM params[])
{
	const OSSL_PARAM *param;
	int digest_order;
	int key_order;
	int salt_order;
	int info_order;
	int known;
	int joined;

	/* No parameters: nothing changes. */
	if (params == NULL)
		return 1;

	/* Each parameter until the one with no name. */
	for (param = params; param->key != NULL; param++) {
		/* Compares the name of the parameter with each name that is known. */
		digest_order = strcmp(param->key, OSSL_KDF_PARAM_DIGEST);
		key_order = strcmp(param->key, OSSL_KDF_PARAM_KEY);
		salt_order = strcmp(param->key, OSSL_KDF_PARAM_SALT);
		info_order = strcmp(param->key, OSSL_KDF_PARAM_INFO);

		/* The digest's name (the first one counts): only SHA-256's names are known. */
		if (digest_order == 0) {
			if (settings->has_digest)
				continue;
			known = cryc_param_string_equal(param, "SHA256");
			if (!known)
				known = cryc_param_string_equal(param, "SHA2-256");
			if (!known)
				known = cryc_param_string_equal(param, "SHA-256");
			if (!known) {
				cryc_err_put(CRYC_LIB_EVP, CRYC_R_EVP_UNSUPPORTED_ALGORITHM);
				return 0;
			}

			/* Records that the digest is SHA-256. */
			settings->has_digest = 1;
			settings->hash = CRYC_HASH_SHA256;
			continue;
		}

		/* Any other parameter than the key, the salt and the info (a mode among them) is not supported. */
		if (key_order != 0 &&
		    salt_order != 0 &&
		    info_order != 0) {
			cryc_err_put(CRYC_LIB_EVP, CRYC_R_UNSUPPORTED);
			return 0;
		}

		/* The three are octet strings, whose bytes must be there. */
		if (param->data_type != OSSL_PARAM_OCTET_STRING ||
		    (param->data == NULL && param->data_size != 0U)) {
			cryc_err_put(CRYC_LIB_EVP, CRYC_R_PASSED_INVALID_ARGUMENT);
			return 0;
		}

		/* The input keying material: the first one counts. */
		if (key_order == 0) {
			if (!settings->has_key) {
				settings->has_key = 1;
				settings->key = (const uint8_t *)param->data;
				settings->key_size = param->data_size;
			}

			/* Moves on to the next parameter. */
			continue;
		}

		/* The salt: the first one counts. */
		if (salt_order == 0) {
			if (!settings->has_salt) {
				settings->has_salt = 1;
				settings->salt = (const uint8_t *)param->data;
				settings->salt_size = param->data_size;
			}

			/* Moves on to the next parameter. */
			continue;
		}

		/* An info: all of them are joined below. */
		settings->has_info = 1;
	}

	/* Joins the info parameters. */
	if (settings->has_info) {
		joined = kdf_join_info(settings, params);
		if (!joined)
			return 0;
	}

	/* Succeeded: settings holds the parameters. */
	return 1;
}

/*
 * Joins the info parameters of params in order into a new buffer of at
 * most CRYC_KDF_MAX_INFO bytes.
 */
static int
kdf_join_info(
	struct kdf_settings *settings,
	const OSSL_PARAM params[])
{
	const OSSL_PARAM *param;
	uint8_t *joined;
	size_t total;
	size_t offset;
	int info_order;

	/* The total size, refused above the limit (checked before each addition, so it cannot wrap). */
	total = 0U;
	for (param = params; param->key != NULL; param++) {
		/* Skips a parameter that is not an info. */
		info_order = strcmp(param->key, OSSL_KDF_PARAM_INFO);
		if (info_order != 0)
			continue;

		/* Refuses a total above the limit. */
		if (param->data_size > CRYC_KDF_MAX_INFO - total) {
			cryc_err_put(CRYC_LIB_EVP, CRYC_R_PASSED_INVALID_ARGUMENT);
			return 0;
		}

		/* Counts the bytes this info adds. */
		total += param->data_size;
	}

	/* Empty info needs no buffer. */
	if (total == 0U) {
		settings->info = NULL;
		settings->info_size = 0U;
		return 1;
	}

	/* The buffer of the joined info. */
	joined = malloc(total);
	if (joined == NULL) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_MALLOC_FAILURE);
		return 0;
	}

	/* Copies each info in order. */
	offset = 0U;
	for (param = params; param->key != NULL; param++) {
		/* Skips a parameter that is not an info. */
		info_order = strcmp(param->key, OSSL_KDF_PARAM_INFO);
		if (info_order != 0)
			continue;

		/* Skips an info with no bytes. */
		if (param->data_size == 0U)
			continue;

		/* Appends the bytes of this info. */
		memcpy(joined + offset, param->data, param->data_size);
		offset += param->data_size;
	}

	/* Succeeded: settings owns the joined info. */
	settings->info = joined;
	settings->info_size = total;
	return 1;
}

/*
 * Copies size bytes into a new buffer, or sets *copy to NULL for size 0.
 */
static int
kdf_copy(
	uint8_t **copy,
	const uint8_t *data,
	size_t size)
{
	uint8_t *buffer;

	/* No bytes, no buffer. */
	if (size == 0U) {
		*copy = NULL;
		return 1;
	}

	/* The new buffer. */
	buffer = malloc(size);
	if (buffer == NULL) {
		cryc_err_put(CRYC_LIB_EVP, CRYC_R_MALLOC_FAILURE);
		return 0;
	}

	/* Succeeded: the copy. */
	memcpy(buffer, data, size);
	*copy = buffer;
	return 1;
}

/*
 * Wipes and frees a buffer of size bytes; NULL is ignored.
 */
static void
kdf_release(
	uint8_t *buffer,
	size_t size)
{
	/* Nothing to release. */
	if (buffer == NULL)
		return;

	/* The bytes may be secret. */
	cryc_wipe(buffer, size);
	free(buffer);
}
