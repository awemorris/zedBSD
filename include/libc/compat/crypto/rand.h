/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * libcrypto-compat's random bytes, with OpenSSL's name: taken from the
 * operating system each call (getrandom on zedBSD, BCryptGenRandom on
 * Windows), no user-space generator in between.
 */

#ifndef KERN_COMPAT_CRYPTO_RAND_H
#define KERN_COMPAT_CRYPTO_RAND_H

#include <compat/crypto/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Fills count bytes of buffer with random bytes.  Returns 1, or 0 when
 * the system refused (buffer is then zeroed, an error is queued) or count
 * is negative.  count 0 returns 1.
 */
int RAND_bytes(unsigned char *buffer, int count);

#ifdef __cplusplus
}
#endif

#endif
