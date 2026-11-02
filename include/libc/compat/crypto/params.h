/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * libcrypto-compat's constructors of OpenSSL 3's parameters (OSSL_PARAM,
 * <compat/crypto/types.h>), with OpenSSL's names: a UTF-8 string, an
 * octet string, and the end of an array.  A constructor only fills the
 * structure; it copies nothing, so the buffer it points to must live as
 * long as the parameter is used.
 */

#ifndef KERN_COMPAT_CRYPTO_PARAMS_H
#define KERN_COMPAT_CRYPTO_PARAMS_H

#include <compat/crypto/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The data types of a parameter, and the return size of a parameter that
 * was not written.
 * Values referenced: OpenSSL 3.5.8 include/openssl/core.h
 * (OSSL_PARAM_UTF8_STRING, OSSL_PARAM_OCTET_STRING) and
 * include/openssl/params.h (OSSL_PARAM_UNMODIFIED).
 */
#define OSSL_PARAM_UTF8_STRING		4
#define OSSL_PARAM_OCTET_STRING		5
#define OSSL_PARAM_UNMODIFIED		((size_t)-1)

/*
 * Makes a UTF-8 string parameter named key pointing to buf: its size is
 * bsize bytes, or strlen(buf) when bsize is 0 and buf is not NULL (the
 * terminating NUL is not counted).  The return size is
 * OSSL_PARAM_UNMODIFIED.
 */
OSSL_PARAM OSSL_PARAM_construct_utf8_string(const char *key, char *buf, size_t bsize);

/* Makes an octet string parameter named key: bsize bytes at buf (the return size OSSL_PARAM_UNMODIFIED). */
OSSL_PARAM OSSL_PARAM_construct_octet_string(const char *key, void *buf, size_t bsize);

/* Makes the parameter that ends an array: every member zero, the key NULL. */
OSSL_PARAM OSSL_PARAM_construct_end(void);

#ifdef __cplusplus
}
#endif

#endif
