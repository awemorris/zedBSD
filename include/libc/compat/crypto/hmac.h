/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * libcrypto-compat's one-shot HMAC (RFC 2104), with OpenSSL's name and
 * signature.  Only SHA-256 (EVP_sha256) is supported.
 */

#ifndef KERN_COMPAT_CRYPTO_HMAC_H
#define KERN_COMPAT_CRYPTO_HMAC_H

#include <compat/crypto/evp.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Computes HMAC(key, data) with evp_md (EVP_sha256 only) into md (32
 * bytes) and sets *md_len to 32 when md_len is not NULL.  key may be NULL
 * only when key_len is 0, data only when data_len is 0.  Returns md, or
 * NULL (an error queued) for another digest, a negative key_len, a NULL
 * buffer with a nonzero length, or md NULL: unlike OpenSSL, there is no
 * static output buffer.
 */
unsigned char *HMAC(const EVP_MD *evp_md, const void *key, int key_len, const unsigned char *data, size_t data_len, unsigned char *md, unsigned int *md_len);

#ifdef __cplusplus
}
#endif

#endif
