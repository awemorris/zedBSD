/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The constant-time checks of the public EVP digest, cipher and MAC
 * functions: keys, plaintexts and messages are marked secret, so memcheck
 * reports any branch or address of api-evp.c that depends on their bytes.
 * The IV of EVP_chacha20 (the block counter and the nonce) is public, as
 * in every use of the interface: the length of the keystream the counter
 * allows is a decision the function makes on it.  Outputs are made public
 * only after the calls, as a caller does.
 */

#include "check.h"
#include "ct.h"

#include <compat/crypto/err.h>
#include <compat/crypto/evp.h>

/* Fills size bytes at out with a fixed pattern. */
static void fill(uint8_t *out, size_t size, unsigned seed);

/* Folds a public output into one value, to keep the call from being dropped. */
static unsigned fold(const uint8_t *bytes, size_t size);

/* Fills size bytes at out with a fixed pattern. */
static void
fill(
	uint8_t *out,
	size_t size,
	unsigned seed)
{
	size_t index;

	/* One byte at a time. */
	for (index = 0; index < size; index++)
		out[index] = (uint8_t)(seed + index * 13U + (index >> 3));
}

/* Folds a public output into one value, to keep the call from being dropped. */
static unsigned
fold(
	const uint8_t *bytes,
	size_t size)
{
	unsigned value;
	size_t index;

	/* Exclusive-or of the bytes, rotated. */
	value = 0;
	for (index = 0; index < size; index++)
		value = ((value << 1) | (value >> 7)) ^ bytes[index];

	/* Reports the folded value, one byte wide. */
	return value & 0xffU;
}

/* Runs the checks. */
int
main(void)
{
	uint8_t key[32];
	uint8_t iv[16];
	uint8_t plain[300];
	uint8_t out[300];
	uint8_t digest[32];
	uint8_t tag[16];
	EVP_CIPHER_CTX *cipher_ctx;
	EVP_MD_CTX *md_ctx;
	EVP_MD_CTX *copy_ctx;
	EVP_MAC *mac;
	EVP_MAC_CTX *mac_ctx;
	size_t tag_length;
	unsigned total;
	int out_length;
	int status;

	/* Starts the folded value at zero. */
	total = 0;

	/* ChaCha20 with a secret key and plaintext: a partial block, whole blocks, a partial block, an in-place call. */
	fill(key, sizeof(key), 3U);
	fill(iv, sizeof(iv), 7U);
	fill(plain, sizeof(plain), 11U);
	CT_SECRET(key, sizeof(key));
	CT_SECRET(plain, sizeof(plain));
	cipher_ctx = EVP_CIPHER_CTX_new();
	CHECK(cipher_ctx != NULL);
	status = EVP_CipherInit_ex(cipher_ctx, EVP_chacha20(), NULL, key, iv, 1);
	CHECK(status == 1);
	status = EVP_CipherUpdate(cipher_ctx, out, &out_length, plain, 10);
	CHECK(status == 1);
	status = EVP_CipherUpdate(cipher_ctx, out + 10, &out_length, plain + 10, 130);
	CHECK(status == 1);
	status = EVP_CipherUpdate(cipher_ctx, out + 140, &out_length, plain + 140, 21);
	CHECK(status == 1);
	status = EVP_Cipher(cipher_ctx, out + 161, plain + 161, 139U);
	CHECK(status == 139);
	CT_PUBLIC(out, sizeof(out));
	total += fold(out, sizeof(out));

	/* The same context with a new key and IV, in place. */
	status = EVP_CipherInit_ex(cipher_ctx, NULL, NULL, key, iv, -1);
	CHECK(status == 1);
	memcpy(out, plain, 77U);
	status = EVP_CipherUpdate(cipher_ctx, out, &out_length, out, 77);
	CHECK(status == 1);
	CT_PUBLIC(out, 77U);
	total += fold(out, 77U);
	EVP_CIPHER_CTX_free(cipher_ctx);
	CT_PUBLIC(plain, sizeof(plain));
	CT_PUBLIC(key, sizeof(key));

	/* Poly1305 with a secret key and message, in pieces. */
	fill(key, sizeof(key), 17U);
	fill(plain, sizeof(plain), 19U);
	CT_SECRET(key, sizeof(key));
	CT_SECRET(plain, sizeof(plain));
	mac = EVP_MAC_fetch(NULL, "POLY1305", NULL);
	CHECK(mac != NULL);
	mac_ctx = EVP_MAC_CTX_new(mac);
	CHECK(mac_ctx != NULL);
	status = EVP_MAC_init(mac_ctx, key, 32U, NULL);
	CHECK(status == 1);
	status = EVP_MAC_update(mac_ctx, plain, 5U);
	CHECK(status == 1);
	status = EVP_MAC_update(mac_ctx, plain + 5, 40U);
	CHECK(status == 1);
	status = EVP_MAC_update(mac_ctx, plain + 45, 200U);
	CHECK(status == 1);
	tag_length = 0;
	status = EVP_MAC_final(mac_ctx, tag, &tag_length, sizeof(tag));
	CHECK(status == 1);
	CT_PUBLIC(tag, sizeof(tag));
	total += fold(tag, sizeof(tag));

	/* A second computation on the same context after a first one that was abandoned. */
	status = EVP_MAC_init(mac_ctx, key, 32U, NULL);
	CHECK(status == 1);
	status = EVP_MAC_update(mac_ctx, plain, 17U);
	CHECK(status == 1);
	status = EVP_MAC_init(mac_ctx, key, 32U, NULL);
	CHECK(status == 1);
	status = EVP_MAC_update(mac_ctx, plain, 33U);
	CHECK(status == 1);
	status = EVP_MAC_final(mac_ctx, tag, NULL, sizeof(tag));
	CHECK(status == 1);
	CT_PUBLIC(tag, sizeof(tag));
	total += fold(tag, sizeof(tag));
	EVP_MAC_CTX_free(mac_ctx);
	EVP_MAC_free(mac);
	CT_PUBLIC(plain, sizeof(plain));
	CT_PUBLIC(key, sizeof(key));

	/* SHA-256 of a secret message: one shot, in pieces, and a snapshot. */
	fill(plain, sizeof(plain), 23U);
	CT_SECRET(plain, sizeof(plain));
	status = EVP_Digest(plain, 200U, digest, NULL, EVP_sha256(), NULL);
	CHECK(status == 1);
	CT_PUBLIC(digest, sizeof(digest));
	total += fold(digest, sizeof(digest));
	md_ctx = EVP_MD_CTX_new();
	copy_ctx = EVP_MD_CTX_new();
	CHECK(md_ctx != NULL && copy_ctx != NULL);
	status = EVP_DigestInit_ex(md_ctx, EVP_sha256(), NULL);
	CHECK(status == 1);
	status = EVP_DigestUpdate(md_ctx, plain, 70U);
	CHECK(status == 1);
	status = EVP_MD_CTX_copy_ex(copy_ctx, md_ctx);
	CHECK(status == 1);
	status = EVP_DigestUpdate(md_ctx, plain + 70, 130U);
	CHECK(status == 1);
	status = EVP_DigestUpdate(copy_ctx, plain + 70, 130U);
	CHECK(status == 1);
	status = EVP_DigestFinal_ex(md_ctx, digest, NULL);
	CHECK(status == 1);
	CT_PUBLIC(digest, sizeof(digest));
	total += fold(digest, sizeof(digest));
	status = EVP_DigestFinal_ex(copy_ctx, digest, NULL);
	CHECK(status == 1);
	CT_PUBLIC(digest, sizeof(digest));
	total += fold(digest, sizeof(digest));
	EVP_MD_CTX_free(md_ctx);
	EVP_MD_CTX_free(copy_ctx);
	CT_PUBLIC(plain, sizeof(plain));

	/* The folded value is used so that no call can be dropped. */
	CHECK(total < 1000000U);
	CHECK(ERR_peek_error() == 0UL);

	/* The result. */
	return check_finish("ct-api-evp");
}
