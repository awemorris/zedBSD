/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * libcrypto-compat's key derivation, with OpenSSL 3's EVP_KDF names and
 * signatures.  Only HKDF (RFC 5869) with SHA-256, in its default mode
 * (extract, then expand), is supported.  A context may be used by one
 * thread at a time.
 */

#ifndef KERN_COMPAT_CRYPTO_KDF_H
#define KERN_COMPAT_CRYPTO_KDF_H

#include <compat/crypto/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Returns the KDF named "HKDF" (the name is not case sensitive; libctx and
 * properties must be NULL), or NULL (an error queued) for another name.
 * The object is constant; EVP_KDF_free on it does nothing.
 */
EVP_KDF *EVP_KDF_fetch(OSSL_LIB_CTX *libctx, const char *algorithm, const char *properties);

/* Releases a KDF; NULL is ignored. */
void EVP_KDF_free(EVP_KDF *kdf);

/* Makes a derivation context for kdf with nothing set; NULL on failure (the kdf may be freed afterwards). */
EVP_KDF_CTX *EVP_KDF_CTX_new(EVP_KDF *kdf);

/* Wipes and frees a context (its key, salt and info); NULL is ignored. */
void EVP_KDF_CTX_free(EVP_KDF_CTX *ctx);

/*
 * Applies params (NULL for none) to the context, then derives keylen bytes
 * into key.  The parameters, kept by the context for later derivations:
 * OSSL_KDF_PARAM_DIGEST, a UTF-8 string "SHA256" (or "SHA2-256",
 * "SHA-256"; not case sensitive), required; OSSL_KDF_PARAM_KEY, an octet
 * string, the input keying material, required; OSSL_KDF_PARAM_SALT, an
 * octet string (absent or empty: HashLen zeros, RFC 5869 2.2);
 * OSSL_KDF_PARAM_INFO, an octet string, several of which in one array are
 * joined in order (32768 bytes at most; an array with info replaces the
 * info set before).  When a name other than info appears twice, the first
 * counts.  Any other parameter name, a wrong data type or another digest
 * fails the call and changes nothing.
 *
 * keylen is 1 to 8160 (255 * 32).  Returns 1, or 0 with an error queued
 * (nothing written to key).
 */
int EVP_KDF_derive(EVP_KDF_CTX *ctx, unsigned char *key, size_t keylen, const OSSL_PARAM params[]);

#ifdef __cplusplus
}
#endif

#endif
