/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * libcrypto-compat's memory helpers, with OpenSSL's names.
 */

#ifndef KERN_COMPAT_CRYPTO_CRYPTO_H
#define KERN_COMPAT_CRYPTO_CRYPTO_H

#include <compat/crypto/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Overwrites length bytes at memory with zeros in a way the compiler keeps (a NULL memory with length 0 is allowed). */
void OPENSSL_cleanse(void *memory, size_t length);

/*
 * Compares length bytes in constant time: returns 0 when equal and
 * non-zero otherwise (not an ordering).
 */
int CRYPTO_memcmp(const void *left, const void *right, size_t length);

#ifdef __cplusplus
}
#endif

#endif
