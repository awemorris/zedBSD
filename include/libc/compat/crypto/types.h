/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * libcrypto-compat: zedBSD's own implementation of the parts of OpenSSL's
 * libcrypto interface that its programs use.  It holds no OpenSSL code;
 * the names, the types' tags and the numeric values are OpenSSL's so that
 * a program written for OpenSSL reads the same.  Programs of the packages
 * use the real OpenSSL (/usr/include/openssl); base and desktop programs
 * include <compat/crypto/...> and <compat/ssl/...> and link
 * libcrypto-compat and libssl-compat.
 *
 * This file names the opaque object types.  The struct tags are
 * OpenSSL's (a program may forward-declare "struct x509_st"); the
 * structures themselves are libcrypto-compat's own and are not visible.
 */

#ifndef KERN_COMPAT_CRYPTO_TYPES_H
#define KERN_COMPAT_CRYPTO_TYPES_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A certificate: reference counted; X509_free drops one reference. */
typedef struct x509_st X509;

/* A set of trusted certificates (an SSL_CTX owns one). */
typedef struct x509_store_st X509_STORE;

/* The state of one chain validation, as a verify callback sees it. */
typedef struct x509_store_ctx_st X509_STORE_CTX;

/* What a validation checks: the host name, the address. */
typedef struct X509_VERIFY_PARAM_st X509_VERIFY_PARAM;

/* A message digest algorithm (EVP_sha256 returns a constant one). */
typedef struct evp_md_st EVP_MD;

/* A digest, signing or verification in progress. */
typedef struct evp_md_ctx_st EVP_MD_CTX;

/* A cipher algorithm (EVP_chacha20 returns a constant one). */
typedef struct evp_cipher_st EVP_CIPHER;

/* A cipher in use: its key and position. */
typedef struct evp_cipher_ctx_st EVP_CIPHER_CTX;

/* A key: Ed25519, X25519 or ML-KEM-768; reference counted. */
typedef struct evp_pkey_st EVP_PKEY;

/* An operation on a key: derivation, generation, encapsulation. */
typedef struct evp_pkey_ctx_st EVP_PKEY_CTX;

/* A MAC algorithm (EVP_MAC_fetch returns it; only POLY1305). */
typedef struct evp_mac_st EVP_MAC;

/* A MAC computation in progress. */
typedef struct evp_mac_ctx_st EVP_MAC_CTX;

/* A key derivation algorithm (EVP_KDF_fetch returns it; only HKDF). */
typedef struct evp_kdf_st EVP_KDF;

/* A key derivation's settings (the digest, the key, the salt, the info). */
typedef struct evp_kdf_ctx_st EVP_KDF_CTX;

/*
 * Types that appear in OpenSSL's signatures but that libcrypto-compat
 * does not implement: every argument of these types must be NULL (an
 * ENGINE, a library context), or the call fails.
 */
typedef struct engine_st ENGINE;
typedef struct ossl_lib_ctx_st OSSL_LIB_CTX;

/*
 * A parameter of OpenSSL 3's parameter arrays (made with the constructors
 * of <compat/crypto/params.h>; an array ends with a parameter whose key is
 * NULL).  Only EVP_PKEY_fromdata and EVP_KDF_derive take parameters, and
 * only the ones their comments name; every other OSSL_PARAM array argument
 * must be NULL.
 * Values referenced: OpenSSL 3.5.8 include/openssl/core.h (the members
 * of struct ossl_param_st, in order).
 */
typedef struct ossl_param_st {
	const char *key;
	unsigned int data_type;
	void *data;
	size_t data_size;
	size_t return_size;
} OSSL_PARAM;

#ifdef __cplusplus
}
#endif

#endif
