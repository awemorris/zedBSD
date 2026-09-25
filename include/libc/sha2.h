/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef LIBC_SHA2_H
#define LIBC_SHA2_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SHA256_BLOCK_LENGTH   64
#define SHA256_DIGEST_LENGTH  32
#define SHA256_DIGEST_STRING_LENGTH (SHA256_DIGEST_LENGTH * 2 + 1)
#define SHA384_BLOCK_LENGTH   128
#define SHA384_DIGEST_LENGTH  48
#define SHA384_DIGEST_STRING_LENGTH (SHA384_DIGEST_LENGTH * 2 + 1)
#define SHA512_BLOCK_LENGTH   128
#define SHA512_DIGEST_LENGTH  64
#define SHA512_DIGEST_STRING_LENGTH (SHA512_DIGEST_LENGTH * 2 + 1)

/*
 * The SHA-2 family, as FIPS 180-4 describes it.
 *
 * One context serves all of them: the 256-bit form works on 32-bit words
 * and 64-byte blocks, the 384- and 512-bit forms on 64-bit words and
 * 128-byte blocks, and which was started decides which is continued.
 */
typedef struct {
	union {
		uint32_t state32[8];
		uint64_t state64[8];
	} state;
	uint64_t count[2];
	uint8_t buffer[SHA512_BLOCK_LENGTH];
} SHA2_CTX;

void SHA256Init(SHA2_CTX *);
void SHA256Update(SHA2_CTX *, const uint8_t *, size_t);
void SHA256Final(uint8_t [SHA256_DIGEST_LENGTH], SHA2_CTX *);
char *SHA256End(SHA2_CTX *, char *);
char *SHA256Data(const uint8_t *, size_t, char *);

void SHA384Init(SHA2_CTX *);
void SHA384Update(SHA2_CTX *, const uint8_t *, size_t);
void SHA384Final(uint8_t [SHA384_DIGEST_LENGTH], SHA2_CTX *);
char *SHA384End(SHA2_CTX *, char *);
char *SHA384Data(const uint8_t *, size_t, char *);

void SHA512Init(SHA2_CTX *);
void SHA512Update(SHA2_CTX *, const uint8_t *, size_t);
void SHA512Final(uint8_t [SHA512_DIGEST_LENGTH], SHA2_CTX *);
char *SHA512End(SHA2_CTX *, char *);
char *SHA512Data(const uint8_t *, size_t, char *);

#ifdef __cplusplus
}
#endif

#endif
