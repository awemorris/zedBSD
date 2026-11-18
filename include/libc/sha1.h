/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef LIBC_SHA1_H
#define LIBC_SHA1_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SHA1_BLOCK_LENGTH  64
#define SHA1_DIGEST_LENGTH 20
#define SHA1_DIGEST_STRING_LENGTH (SHA1_DIGEST_LENGTH * 2 + 1)

/*
 * SHA-1, as FIPS 180-4 describes it.
 *
 * Like MD5 it is kept for what already depends on it, and like MD5 a
 * collision can be produced, so it must not be relied on for one.
 */
typedef struct {
	uint32_t state[5];
	uint64_t count;
	uint8_t buffer[SHA1_BLOCK_LENGTH];
} SHA1_CTX;

void SHA1Init(SHA1_CTX *);
void SHA1Update(SHA1_CTX *, const uint8_t *, size_t);
void SHA1Final(uint8_t [SHA1_DIGEST_LENGTH], SHA1_CTX *);
char *SHA1End(SHA1_CTX *, char *);
char *SHA1Data(const uint8_t *, size_t, char *);

#ifdef __cplusplus
}
#endif

#endif
