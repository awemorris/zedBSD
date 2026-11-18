/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * libpasskey's cryptography, behind one small interface (ws161-p004,
 * ws172): SHA-256, HMAC-SHA-256, the random bytes, the P-256 ECDSA
 * verification, and what the PIN/UV protocols need.  crypto-openssl.c
 * implements it with OpenSSL's libcrypto until the release (the
 * Guardrail's exception for /sbin/passkey); an implementation of zedBSD's
 * own replaces that file alone.
 */

#ifndef LIBPASSKEY_CRYPTO_H
#define LIBPASSKEY_CRYPTO_H

#include <stddef.h>
#include <stdint.h>

/* The sizes of a SHA-256 hash, a P-256 coordinate and an AES-256 key. */
#define PK_SHA256_SIZE		32U
#define PK_P256_SIZE		32U
#define PK_AES256_KEY_SIZE	32U
#define PK_AES_BLOCK_SIZE	16U

/* One part of a message to hash: size bytes at data. */
struct pk_crypto_part {
	const uint8_t *data;
	size_t size;
};

int pk_crypto_sha256(const struct pk_crypto_part *parts, size_t count, uint8_t *hash);
int pk_crypto_hmac_sha256(const uint8_t *key, size_t key_size, const uint8_t *data, size_t size, uint8_t *mac);
int pk_crypto_random(uint8_t *bytes, size_t size);
int pk_crypto_p256_valid(const uint8_t *x, const uint8_t *y);
int pk_crypto_p256_verify(const uint8_t *x, const uint8_t *y, const uint8_t *hash, const uint8_t *signature, size_t signature_size);
int pk_crypto_p256_ecdh(const uint8_t *peer_x, const uint8_t *peer_y, uint8_t *own_x, uint8_t *own_y, uint8_t *shared);
int pk_crypto_aes256_cbc(int encrypt, const uint8_t *key, const uint8_t *iv, const uint8_t *in, size_t size, uint8_t *out);
int pk_crypto_hkdf_sha256(const uint8_t *salt, size_t salt_size, const uint8_t *secret, size_t secret_size, const char *info, uint8_t *out, size_t out_size);
void pk_crypto_wipe(void *memory, size_t size);
int pk_crypto_equal(const uint8_t *left, const uint8_t *right, size_t size);

#endif
