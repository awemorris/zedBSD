/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The public EVP digest, cipher and MAC functions.
 */

#include "cryc-api.h"

/*
 * Returns the SHA-256 digest algorithm (a constant object).
 */
const EVP_MD *
EVP_sha256(void)
{
	/* Not implemented: the failure value. */
	return NULL;
}

/*
 * Returns a digest's output size in bytes (32 for SHA-256), or -1 for
 * NULL.
 */
int
EVP_MD_get_size(
	const EVP_MD *md)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(md);

	/* Not implemented: the failure value. */
	return 0;
}

/*
 * Makes an empty digest context; NULL without memory.
 */
EVP_MD_CTX *
EVP_MD_CTX_new(void)
{
	/* Not implemented: the failure value. */
	return NULL;
}

/*
 * Wipes and frees a digest context (and drops its key, after a
 * DigestSign/VerifyInit); NULL is ignored.
 */
void
EVP_MD_CTX_free(
	EVP_MD_CTX *ctx)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(ctx);
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
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(ctx);
	UNUSED_PARAMETER(type);
	UNUSED_PARAMETER(impl);

	/* Not implemented: the failure value. */
	return 0;
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
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(ctx);
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(count);

	/* Not implemented: the failure value. */
	return 0;
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
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(ctx);
	UNUSED_PARAMETER(md);
	UNUSED_PARAMETER(size);

	/* Not implemented: the failure value. */
	return 0;
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
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(out);
	UNUSED_PARAMETER(in);

	/* Not implemented: the failure value. */
	return 0;
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
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(count);
	UNUSED_PARAMETER(md);
	UNUSED_PARAMETER(size);
	UNUSED_PARAMETER(type);
	UNUSED_PARAMETER(impl);

	/* Not implemented: the failure value. */
	return 0;
}

/*
 * Returns the ChaCha20 cipher (a constant object): 32-byte key, 16-byte
 * IV.
 */
const EVP_CIPHER *
EVP_chacha20(void)
{
	/* Not implemented: the failure value. */
	return NULL;
}

/*
 * Makes an empty cipher context; NULL without memory.
 */
EVP_CIPHER_CTX *
EVP_CIPHER_CTX_new(void)
{
	/* Not implemented: the failure value. */
	return NULL;
}

/*
 * Wipes and frees a cipher context; NULL is ignored.
 */
void
EVP_CIPHER_CTX_free(
	EVP_CIPHER_CTX *ctx)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(ctx);
}

/*
 * Sets up ChaCha20: cipher EVP_chacha20 (or NULL to keep the one set), key
 * 32 bytes (or NULL to keep it), iv 16 bytes (or NULL to keep it), enc 1,
 * 0 or -1 (keep; ChaCha20 is the same both ways).
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
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(ctx);
	UNUSED_PARAMETER(cipher);
	UNUSED_PARAMETER(impl);
	UNUSED_PARAMETER(key);
	UNUSED_PARAMETER(iv);
	UNUSED_PARAMETER(enc);

	/* Not implemented: the failure value. */
	return 0;
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
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(ctx);
	UNUSED_PARAMETER(out);
	UNUSED_PARAMETER(out_length);
	UNUSED_PARAMETER(in);
	UNUSED_PARAMETER(in_length);

	/* Not implemented: the failure value. */
	return 0;
}

/*
 * As EVP_CipherUpdate without the length output; returns 1 or 0 (OpenSSL's
 * convention for stream ciphers).
 */
int
EVP_Cipher(
	EVP_CIPHER_CTX *ctx,
	unsigned char *out,
	const unsigned char *in,
	unsigned int length)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(ctx);
	UNUSED_PARAMETER(out);
	UNUSED_PARAMETER(in);
	UNUSED_PARAMETER(length);

	/* Not implemented: the failure value. */
	return 0;
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
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(libctx);
	UNUSED_PARAMETER(algorithm);
	UNUSED_PARAMETER(properties);

	/* Not implemented: the failure value. */
	return NULL;
}

/*
 * Releases a MAC algorithm; NULL is ignored.
 */
void
EVP_MAC_free(
	EVP_MAC *mac)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(mac);
}

/*
 * Makes a MAC context for mac; NULL on failure.
 */
EVP_MAC_CTX *
EVP_MAC_CTX_new(
	EVP_MAC *mac)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(mac);

	/* Not implemented: the failure value. */
	return NULL;
}

/*
 * Wipes and frees a MAC context; NULL is ignored.
 */
void
EVP_MAC_CTX_free(
	EVP_MAC_CTX *ctx)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(ctx);
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
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(ctx);
	UNUSED_PARAMETER(key);
	UNUSED_PARAMETER(length);
	UNUSED_PARAMETER(params);

	/* Not implemented: the failure value. */
	return 0;
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
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(ctx);
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(length);

	/* Not implemented: the failure value. */
	return 0;
}

/*
 * Writes the 16-byte tag (out_size at least 16; *out_length 16); the
 * context needs EVP_MAC_init before reuse.
 */
int
EVP_MAC_final(
	EVP_MAC_CTX *ctx,
	unsigned char *out,
	size_t *out_length,
	size_t out_size)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(ctx);
	UNUSED_PARAMETER(out);
	UNUSED_PARAMETER(out_length);
	UNUSED_PARAMETER(out_size);

	/* Not implemented: the failure value. */
	return 0;
}
