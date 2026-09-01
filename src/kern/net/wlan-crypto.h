/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_KERN_NET_WLAN_CRYPTO_H
#define ZEDBSD_KERN_NET_WLAN_CRYPTO_H

#include <stddef.h>
#include <stdint.h>

#define WLAN_SHA1_BLOCK_SIZE 64U
#define WLAN_SHA1_DIGEST_SIZE 20U
#define WLAN_AES128_BLOCK_SIZE 16U
#define WLAN_AES128_KEY_SIZE 16U

/* The phase-029 EAPOL-Key frame is bounded below this private kernel limit. */
#define WLAN_RFC3394_WRAPPED_MAX 4096U

struct wlan_sha1_context {
	uint32_t state[5];
	uint64_t total_bytes;
	uint8_t block[WLAN_SHA1_BLOCK_SIZE];
	size_t block_length;
	int failed;
};

struct wlan_hmac_sha1_context {
	struct wlan_sha1_context inner;
	struct wlan_sha1_context outer;
	int initialized;
	int failed;
};

int wlan_sha1_init(struct wlan_sha1_context *context);
int wlan_sha1_update(struct wlan_sha1_context *context,
	const void *data, size_t length);
int wlan_sha1_final(struct wlan_sha1_context *context,
	uint8_t digest[WLAN_SHA1_DIGEST_SIZE]);
int wlan_sha1(const void *data, size_t length,
	uint8_t digest[WLAN_SHA1_DIGEST_SIZE]);

int wlan_hmac_sha1_init(struct wlan_hmac_sha1_context *context,
	const void *key, size_t key_length);
int wlan_hmac_sha1_update(struct wlan_hmac_sha1_context *context,
	const void *data, size_t length);
int wlan_hmac_sha1_final(struct wlan_hmac_sha1_context *context,
	uint8_t digest[WLAN_SHA1_DIGEST_SIZE]);
int wlan_hmac_sha1(const void *key, size_t key_length,
	const void *data, size_t data_length,
	uint8_t digest[WLAN_SHA1_DIGEST_SIZE]);

int wlan_pbkdf2_hmac_sha1(const void *password, size_t password_length,
	const void *salt, size_t salt_length, uint32_t iterations,
	uint8_t *output, size_t output_length);

/* IEEE 802.11's HMAC-SHA1 PRF: label || NUL || data || octet(counter). */
int wlan_crypto_prf_sha1(const void *key, size_t key_length,
	const void *label, size_t label_length,
	const void *data, size_t data_length,
	uint8_t *output, size_t output_length);

int wlan_aes128_encrypt_block(const uint8_t key[WLAN_AES128_KEY_SIZE],
	const uint8_t input[WLAN_AES128_BLOCK_SIZE],
	uint8_t output[WLAN_AES128_BLOCK_SIZE]);

/* On an integrity failure output is erased and EACCES is returned. */
int wlan_rfc3394_unwrap(const uint8_t kek[WLAN_AES128_KEY_SIZE],
	const uint8_t *wrapped, size_t wrapped_length,
	uint8_t *output, size_t output_capacity, size_t *output_length);

int wlan_crypto_equal(const void *left, const void *right, size_t length);
void wlan_crypto_erase(void *data, size_t length);

#endif
