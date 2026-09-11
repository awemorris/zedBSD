/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "wlan-crypto.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static unsigned checks;

#define CHECK(condition) do { assert(condition); checks++; } while (0)

static uint8_t
hex_nibble(char character)
{
	if (character >= '0' && character <= '9')
		return (uint8_t)(character - '0');
	if (character >= 'a' && character <= 'f')
		return (uint8_t)(character - 'a' + 10);
	assert(0);
	return 0U;
}

static void
check_hex(const uint8_t *actual, size_t length, const char *expected)
{
	size_t index;

	CHECK(strlen(expected) == length * 2U);
	for (index = 0U; index < length; index++)
		CHECK(actual[index] == (uint8_t)((hex_nibble(expected[index * 2U]) <<
		    4) | hex_nibble(expected[index * 2U + 1U])));
}

static int
all_zero(const void *data, size_t length)
{
	const uint8_t *bytes;
	size_t index;

	bytes = (const uint8_t *)data;
	for (index = 0U; index < length; index++) {
		if (bytes[index] != 0U)
			return 0;
	}
	return 1;
}

static void
test_sha1(void)
{
	struct wlan_sha1_context context;
	uint8_t digest[WLAN_SHA1_DIGEST_SIZE];
	uint8_t thousand_a[1000];
	uint8_t one;
	unsigned index;

	CHECK(wlan_sha1(NULL, 0U, digest) == 0);
	check_hex(digest, sizeof(digest),
	    "da39a3ee5e6b4b0d3255bfef95601890afd80709");
	CHECK(wlan_sha1("abc", 3U, digest) == 0);
	check_hex(digest, sizeof(digest),
	    "a9993e364706816aba3e25717850c26c9cd0d89d");
	/* FIPS 180-4's two-block padding example. */
	CHECK(wlan_sha1("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
	    56U, digest) == 0);
	check_hex(digest, sizeof(digest),
	    "84983e441c3bd26ebaae4aa1f95129e5e54670f1");

	CHECK(wlan_sha1_init(&context) == 0);
	CHECK(wlan_sha1_update(&context, "a", 1U) == 0);
	CHECK(wlan_sha1_update(&context, "bc", 2U) == 0);
	CHECK(wlan_sha1_update(&context, NULL, 0U) == 0);
	CHECK(wlan_sha1_final(&context, digest) == 0);
	CHECK(all_zero(&context, sizeof(context)));
	check_hex(digest, sizeof(digest),
	    "a9993e364706816aba3e25717850c26c9cd0d89d");

	memset(thousand_a, 'a', sizeof(thousand_a));
	CHECK(wlan_sha1_init(&context) == 0);
	for (index = 0U; index < 1000U; index++)
		CHECK(wlan_sha1_update(&context, thousand_a,
		    sizeof(thousand_a)) == 0);
	CHECK(wlan_sha1_final(&context, digest) == 0);
	check_hex(digest, sizeof(digest),
	    "34aa973cd4c4daa4f61eeb2bdbad27316534016f");

	CHECK(wlan_sha1_init(&context) == 0);
	context.total_bytes = UINT64_MAX / 8U;
	one = 1U;
	CHECK(wlan_sha1_update(&context, &one, 1U) == EOVERFLOW);
	CHECK(wlan_sha1_final(&context, digest) == EINVAL);
	CHECK(all_zero(&context, sizeof(context)));
	CHECK(wlan_sha1(NULL, 1U, digest) == EINVAL);
	CHECK(wlan_sha1(digest, 1U, digest) == EINVAL);
}

static void
test_hmac_sha1(void)
{
	struct wlan_hmac_sha1_context context;
	uint8_t key[80];
	uint8_t data[50];
	uint8_t digest[WLAN_SHA1_DIGEST_SIZE];

	/* RFC 2202 cases 1, 2, 3, and 6. */
	memset(key, 0x0b, 20U);
	CHECK(wlan_hmac_sha1(key, 20U, "Hi There", 8U, digest) == 0);
	check_hex(digest, sizeof(digest),
	    "b617318655057264e28bc0b6fb378c8ef146be00");
	CHECK(wlan_hmac_sha1("Jefe", 4U, "what do ya want for nothing?",
	    28U, digest) == 0);
	check_hex(digest, sizeof(digest),
	    "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79");
	memset(key, 0xaa, 20U);
	memset(data, 0xdd, sizeof(data));
	CHECK(wlan_hmac_sha1(key, 20U, data, sizeof(data), digest) == 0);
	check_hex(digest, sizeof(digest),
	    "125d7342b9ac11cd91a39af48aa17b4f63f175d3");
	memset(key, 0xaa, sizeof(key));
	CHECK(wlan_hmac_sha1(key, sizeof(key),
	    "Test Using Larger Than Block-Size Key - Hash Key First", 54U,
	    digest) == 0);
	check_hex(digest, sizeof(digest),
	    "aa4ae5e15272d00e95705637ce8a3b55ed402112");

	CHECK(wlan_hmac_sha1_init(&context, "Jefe", 4U) == 0);
	CHECK(wlan_hmac_sha1_update(&context, "what do ya want ", 16U) == 0);
	CHECK(wlan_hmac_sha1_update(&context, "for nothing?", 12U) == 0);
	CHECK(wlan_hmac_sha1_final(&context, digest) == 0);
	CHECK(all_zero(&context, sizeof(context)));
	check_hex(digest, sizeof(digest),
	    "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79");
	CHECK(wlan_hmac_sha1("key", 3U, digest, sizeof(digest), digest) ==
	    EINVAL);
}

static void
test_pbkdf2(void)
{
	uint8_t output[32];
	uint8_t binary_password[] = { 'p', 'a', 's', 's', 0U, 'w', 'o', 'r', 'd' };
	uint8_t binary_salt[] = { 's', 'a', 0U, 'l', 't' };

	/* RFC 6070 cases 1, 2, 3, and 5. */
	CHECK(wlan_pbkdf2_hmac_sha1("password", 8U, "salt", 4U, 1U,
	    output, 20U) == 0);
	check_hex(output, 20U, "0c60c80f961f0e71f3a9b524af6012062fe037a6");
	CHECK(wlan_pbkdf2_hmac_sha1("password", 8U, "salt", 4U, 2U,
	    output, 20U) == 0);
	check_hex(output, 20U, "ea6c014dc72d6f8ccd1ed92ace1d41f0d8de8957");
	CHECK(wlan_pbkdf2_hmac_sha1("password", 8U, "salt", 4U, 4096U,
	    output, 20U) == 0);
	check_hex(output, 20U, "4b007901b765489abead49d926f721d065a429c1");
	CHECK(wlan_pbkdf2_hmac_sha1(binary_password,
	    sizeof(binary_password), binary_salt, sizeof(binary_salt), 4096U,
	    output, 16U) == 0);
	check_hex(output, 16U, "56fa6aa75548099dcc37d7f03425e0c3");

	/* Wi-Fi Alliance-style PSK vector: password/IEEE. */
	CHECK(wlan_pbkdf2_hmac_sha1("password", 8U, "IEEE", 4U, 4096U,
	    output, sizeof(output)) == 0);
	check_hex(output, sizeof(output),
	    "f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e");
	CHECK(wlan_pbkdf2_hmac_sha1("password", 8U, "salt", 4U, 0U,
	    output, sizeof(output)) == EINVAL);
	CHECK(wlan_pbkdf2_hmac_sha1(output, 8U, "salt", 4U, 1U,
	    output, sizeof(output)) == EINVAL);
#if SIZE_MAX > UINT32_MAX
	CHECK(wlan_pbkdf2_hmac_sha1("p", 1U, "s", 1U, 1U, output,
	    (size_t)UINT32_MAX * WLAN_SHA1_DIGEST_SIZE + 1U) == EOVERFLOW);
#endif
}

static void
test_wpa_prf(void)
{
	uint8_t key[32];
	uint8_t data[76];
	uint8_t output[64];
	unsigned index;

	for (index = 0U; index < sizeof(key); index++)
		key[index] = (uint8_t)index;
	for (index = 0U; index < sizeof(data); index++)
		data[index] = (uint8_t)(0x20U + index);
	CHECK(wlan_crypto_prf_sha1(key, sizeof(key), "Pairwise key expansion",
	    22U, data, sizeof(data), output, sizeof(output)) == 0);
	check_hex(output, sizeof(output),
	    "7bf5fd62e99cd496a1138246b2a1a4578effb6a37e059d52314baf3d479792ad"
	    "71c1f41a710c1aba2361c3169f5fef24717fb21d064d8575326dde87dfdafd9c");
	CHECK(wlan_crypto_prf_sha1(key, sizeof(key), "x", 1U, data,
	    sizeof(data), output, 256U * WLAN_SHA1_DIGEST_SIZE + 1U) ==
	    EOVERFLOW);
}

static void
test_aes(void)
{
	uint8_t key[16] = {
		0x00U, 0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U, 0x07U,
		0x08U, 0x09U, 0x0aU, 0x0bU, 0x0cU, 0x0dU, 0x0eU, 0x0fU
	};
	uint8_t plaintext[16] = {
		0x00U, 0x11U, 0x22U, 0x33U, 0x44U, 0x55U, 0x66U, 0x77U,
		0x88U, 0x99U, 0xaaU, 0xbbU, 0xccU, 0xddU, 0xeeU, 0xffU
	};
	uint8_t output[16];

	/* FIPS 197 Appendix C.1. */
	CHECK(wlan_aes128_encrypt_block(key, plaintext, output) == 0);
	check_hex(output, sizeof(output),
	    "69c4e0d86a7b0430d8cdb78070b4c55a");
	CHECK(wlan_aes128_encrypt_block(key, plaintext, plaintext) == EINVAL);
	CHECK(wlan_aes128_encrypt_block(key, plaintext, key) == EINVAL);
	memset(key, 0, sizeof(key));
	memset(plaintext, 0, sizeof(plaintext));
	CHECK(wlan_aes128_encrypt_block(key, plaintext, output) == 0);
	check_hex(output, sizeof(output),
	    "66e94bd4ef8a2c3b884cfa59ca342b2e");
}

static void
test_key_unwrap(void)
{
	uint8_t kek[16] = {
		0x00U, 0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U, 0x07U,
		0x08U, 0x09U, 0x0aU, 0x0bU, 0x0cU, 0x0dU, 0x0eU, 0x0fU
	};
	uint8_t wrapped[24] = {
		0x1fU, 0xa6U, 0x8bU, 0x0aU, 0x81U, 0x12U, 0xb4U, 0x47U,
		0xaeU, 0xf3U, 0x4bU, 0xd8U, 0xfbU, 0x5aU, 0x7bU, 0x82U,
		0x9dU, 0x3eU, 0x86U, 0x23U, 0x71U, 0xd2U, 0xcfU, 0xe5U
	};
	uint8_t output[32];
	size_t output_length;

	/* RFC 3394 section 4.1. */
	memset(output, 0x5a, sizeof(output));
	output_length = 99U;
	CHECK(wlan_rfc3394_unwrap(kek, wrapped, sizeof(wrapped), output,
	    sizeof(output), &output_length) == 0);
	CHECK(output_length == 16U);
	check_hex(output, output_length,
	    "00112233445566778899aabbccddeeff");

	wrapped[23] ^= 1U;
	memset(output, 0x5a, sizeof(output));
	output_length = 99U;
	CHECK(wlan_rfc3394_unwrap(kek, wrapped, sizeof(wrapped), output,
	    sizeof(output), &output_length) == EACCES);
	CHECK(output_length == 0U);
	CHECK(all_zero(output, 16U));
	wrapped[23] ^= 1U;
	CHECK(wlan_rfc3394_unwrap(kek, wrapped, sizeof(wrapped), output, 15U,
	    &output_length) == ENOSPC);
	CHECK(wlan_rfc3394_unwrap(kek, wrapped, 16U, output, sizeof(output),
	    &output_length) == EINVAL);
	CHECK(wlan_rfc3394_unwrap(kek, wrapped, sizeof(wrapped), wrapped,
	    sizeof(wrapped), &output_length) == EINVAL);
}

static void
test_constant_time_and_erase(void)
{
	uint8_t left[32];
	uint8_t right[32];

	memset(left, 0xa5, sizeof(left));
	memcpy(right, left, sizeof(right));
	CHECK(wlan_crypto_equal(left, right, sizeof(left)));
	right[0] ^= 1U;
	CHECK(!wlan_crypto_equal(left, right, sizeof(left)));
	right[0] ^= 1U;
	right[31] ^= 1U;
	CHECK(!wlan_crypto_equal(left, right, sizeof(left)));
	CHECK(wlan_crypto_equal(NULL, NULL, 0U));
	CHECK(!wlan_crypto_equal(NULL, right, 1U));
	wlan_crypto_erase(left, sizeof(left));
	CHECK(all_zero(left, sizeof(left)));
	wlan_crypto_erase(NULL, 17U);
}

int
main(void)
{
	test_sha1();
	test_hmac_sha1();
	test_pbkdf2();
	test_wpa_prf();
	test_aes();
	test_key_unwrap();
	test_constant_time_and_erase();
	printf("wlan crypto: %u checks PASS\n", checks);
	return 0;
}
