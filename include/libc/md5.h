/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef LIBC_MD5_H
#define LIBC_MD5_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MD5_BLOCK_LENGTH  64
#define MD5_DIGEST_LENGTH 16
#define MD5_DIGEST_STRING_LENGTH (MD5_DIGEST_LENGTH * 2 + 1)

/*
 * MD5, as RFC 1321 describes it.
 *
 * It is kept because file formats and protocols that were written around it
 * still have to be read.  It must not be used where a collision would
 * matter: one can be produced at will.
 */
typedef struct MD5Context {
	uint32_t state[4];
	uint64_t count;
	uint8_t buffer[MD5_BLOCK_LENGTH];
} MD5_CTX;

void MD5Init(MD5_CTX *);
void MD5Update(MD5_CTX *, const uint8_t *, size_t);
void MD5Final(uint8_t [MD5_DIGEST_LENGTH], MD5_CTX *);
char *MD5End(MD5_CTX *, char *);
char *MD5Data(const uint8_t *, size_t, char *);

#ifdef __cplusplus
}
#endif

#endif
