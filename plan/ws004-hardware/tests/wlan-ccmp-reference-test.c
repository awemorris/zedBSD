/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

/*
 * Test-only CCMP reference codec.  This deliberately does not share framing
 * code with the production WLAN path: it checks the bytes produced there
 * against RFC 3610 CCM and the IEEE 802.11 CCMP nonce/AAD construction.
 */
#include "../../../src/kern/net/wlan-crypto.h"
#include "../../../src/kern/net/wlan-l2.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CCM_NONCE_SIZE 13U
#define CCM_TAG_SIZE 8U
#define CCM_AAD_SIZE 22U
#define CCM_BLOCK_SIZE 16U
#define CCM_MAX_TEXT 1600U

static uint16_t
load_le16(const uint8_t value[2])
{
	return (uint16_t)value[0] | ((uint16_t)value[1] << 8);
}

static void
store_be16(uint8_t value[2], size_t number)
{
	value[0] = (uint8_t)(number >> 8);
	value[1] = (uint8_t)number;
}

static void
xor_block(uint8_t left[CCM_BLOCK_SIZE], const uint8_t right[CCM_BLOCK_SIZE])
{
	unsigned index;

	for (index = 0U; index < CCM_BLOCK_SIZE; index++)
		left[index] ^= right[index];
}

static void
ccm_mac_block(const uint8_t key[WLAN_AES128_KEY_SIZE],
	uint8_t state[CCM_BLOCK_SIZE], const uint8_t block[CCM_BLOCK_SIZE])
{
	uint8_t input[CCM_BLOCK_SIZE];

	memcpy(input, state, sizeof(input));
	xor_block(input, block);
	assert(wlan_aes128_encrypt_block(key, input, state) == 0);
	wlan_crypto_erase(input, sizeof(input));
}

static void
ccm_mac_bytes(const uint8_t key[WLAN_AES128_KEY_SIZE],
	uint8_t state[CCM_BLOCK_SIZE], const uint8_t *bytes, size_t length)
{
	uint8_t block[CCM_BLOCK_SIZE];
	size_t amount;

	while (length != 0U) {
		amount = length < sizeof(block) ? length : sizeof(block);
		memset(block, 0, sizeof(block));
		memcpy(block, bytes, amount);
		ccm_mac_block(key, state, block);
		bytes += amount;
		length -= amount;
	}
	wlan_crypto_erase(block, sizeof(block));
}

static void
ccm_tag(const uint8_t key[WLAN_AES128_KEY_SIZE],
	const uint8_t nonce[CCM_NONCE_SIZE], const uint8_t *aad,
	size_t aad_length, const uint8_t *plaintext, size_t plaintext_length,
	uint8_t tag[CCM_TAG_SIZE])
{
	uint8_t block[CCM_BLOCK_SIZE], state[CCM_BLOCK_SIZE];
	size_t prefix_length, amount;

	assert(aad_length != 0U && aad_length < 0xff00U);
	assert(plaintext_length <= 0xffffU);
	memset(state, 0, sizeof(state));
	memset(block, 0, sizeof(block));
	/* Adata=1, M=8, L=2. */
	block[0] = 0x59U;
	memcpy(block + 1U, nonce, CCM_NONCE_SIZE);
	store_be16(block + 14U, plaintext_length);
	ccm_mac_block(key, state, block);

	/* The short-form associated-data length is part of its first block. */
	memset(block, 0, sizeof(block));
	store_be16(block, aad_length);
	prefix_length = aad_length < 14U ? aad_length : 14U;
	memcpy(block + 2U, aad, prefix_length);
	ccm_mac_block(key, state, block);
	aad += prefix_length;
	aad_length -= prefix_length;
	ccm_mac_bytes(key, state, aad, aad_length);

	while (plaintext_length != 0U) {
		amount = plaintext_length < sizeof(block) ? plaintext_length :
		    sizeof(block);
		memset(block, 0, sizeof(block));
		memcpy(block, plaintext, amount);
		ccm_mac_block(key, state, block);
		plaintext += amount;
		plaintext_length -= amount;
	}
	memcpy(tag, state, CCM_TAG_SIZE);
	wlan_crypto_erase(block, sizeof(block));
	wlan_crypto_erase(state, sizeof(state));
}

static void
ccm_stream_block(const uint8_t key[WLAN_AES128_KEY_SIZE],
	const uint8_t nonce[CCM_NONCE_SIZE], uint16_t counter,
	uint8_t stream[CCM_BLOCK_SIZE])
{
	uint8_t block[CCM_BLOCK_SIZE];

	memset(block, 0, sizeof(block));
	block[0] = 0x01U; /* L=2. */
	memcpy(block + 1U, nonce, CCM_NONCE_SIZE);
	block[14U] = (uint8_t)(counter >> 8);
	block[15U] = (uint8_t)counter;
	assert(wlan_aes128_encrypt_block(key, block, stream) == 0);
	wlan_crypto_erase(block, sizeof(block));
}

static void
ccm_encrypt(const uint8_t key[WLAN_AES128_KEY_SIZE],
	const uint8_t nonce[CCM_NONCE_SIZE], const uint8_t *aad,
	size_t aad_length, const uint8_t *plaintext, size_t plaintext_length,
	uint8_t *ciphertext, uint8_t tag[CCM_TAG_SIZE])
{
	uint8_t raw_tag[CCM_TAG_SIZE], stream[CCM_BLOCK_SIZE];
	size_t offset, amount, index;
	uint16_t counter;

	assert(plaintext_length <= CCM_MAX_TEXT);
	ccm_tag(key, nonce, aad, aad_length, plaintext, plaintext_length, raw_tag);
	for (offset = 0U, counter = 1U; offset < plaintext_length;
	    offset += amount, counter++) {
		ccm_stream_block(key, nonce, counter, stream);
		amount = plaintext_length - offset;
		if (amount > sizeof(stream))
			amount = sizeof(stream);
		for (index = 0U; index < amount; index++)
			ciphertext[offset + index] = plaintext[offset + index] ^
			    stream[index];
	}
	ccm_stream_block(key, nonce, 0U, stream);
	for (index = 0U; index < CCM_TAG_SIZE; index++)
		tag[index] = raw_tag[index] ^ stream[index];
	wlan_crypto_erase(raw_tag, sizeof(raw_tag));
	wlan_crypto_erase(stream, sizeof(stream));
}

static int
ccm_decrypt(const uint8_t key[WLAN_AES128_KEY_SIZE],
	const uint8_t nonce[CCM_NONCE_SIZE], const uint8_t *aad,
	size_t aad_length, const uint8_t *ciphertext, size_t ciphertext_length,
	const uint8_t tag[CCM_TAG_SIZE], uint8_t *plaintext)
{
	uint8_t calculated[CCM_TAG_SIZE], raw_tag[CCM_TAG_SIZE];
	uint8_t stream[CCM_BLOCK_SIZE];
	size_t offset, amount, index;
	uint16_t counter;

	if (ciphertext_length > CCM_MAX_TEXT)
		return EINVAL;
	for (offset = 0U, counter = 1U; offset < ciphertext_length;
	    offset += amount, counter++) {
		ccm_stream_block(key, nonce, counter, stream);
		amount = ciphertext_length - offset;
		if (amount > sizeof(stream))
			amount = sizeof(stream);
		for (index = 0U; index < amount; index++)
			plaintext[offset + index] = ciphertext[offset + index] ^
			    stream[index];
	}
	ccm_tag(key, nonce, aad, aad_length, plaintext, ciphertext_length,
	    raw_tag);
	ccm_stream_block(key, nonce, 0U, stream);
	for (index = 0U; index < CCM_TAG_SIZE; index++)
		calculated[index] = raw_tag[index] ^ stream[index];
	if (!wlan_crypto_equal(calculated, tag, CCM_TAG_SIZE)) {
		wlan_crypto_erase(plaintext, ciphertext_length);
		wlan_crypto_erase(calculated, sizeof(calculated));
		wlan_crypto_erase(raw_tag, sizeof(raw_tag));
		wlan_crypto_erase(stream, sizeof(stream));
		return EACCES;
	}
	wlan_crypto_erase(calculated, sizeof(calculated));
	wlan_crypto_erase(raw_tag, sizeof(raw_tag));
	wlan_crypto_erase(stream, sizeof(stream));
	return 0;
}

static void
ccmp_parameters(const uint8_t *mpdu, size_t length,
	uint8_t nonce[CCM_NONCE_SIZE], uint8_t aad[CCM_AAD_SIZE],
	uint8_t *key_index)
{
	const uint8_t *ccmp;
	uint16_t frame_control, sequence_control;

	assert(length >= WLAN_L2_DATA_HEADER_SIZE + WLAN_L2_CCMP_HEADER_SIZE);
	ccmp = mpdu + WLAN_L2_DATA_HEADER_SIZE;
	assert(ccmp[2U] == 0U);
	assert((ccmp[3U] & 0x20U) != 0U);
	*key_index = (uint8_t)((ccmp[3U] >> 6) & 3U);

	/* Priority zero for this non-QoS data frame, transmitter address, PN. */
	nonce[0U] = 0U;
	memcpy(nonce + 1U, mpdu + 10U, 6U);
	nonce[7U] = ccmp[7U];
	nonce[8U] = ccmp[6U];
	nonce[9U] = ccmp[5U];
	nonce[10U] = ccmp[4U];
	nonce[11U] = ccmp[1U];
	nonce[12U] = ccmp[0U];

	/* IEEE 802.11i AAD: masked FC, A1/A2/A3, masked sequence control. */
	frame_control = load_le16(mpdu);
	aad[0U] = (uint8_t)frame_control & 0x8fU;
	aad[1U] = (uint8_t)(frame_control >> 8) & 0xc7U;
	memcpy(aad + 2U, mpdu + 4U, 18U);
	sequence_control = load_le16(mpdu + 22U);
	aad[20U] = (uint8_t)sequence_control & 0x0fU;
	aad[21U] = 0U;
}

static void
test_rfc3610_packet_vector_1(void)
{
	static const uint8_t key[16] = {
		0xc0U, 0xc1U, 0xc2U, 0xc3U, 0xc4U, 0xc5U, 0xc6U, 0xc7U,
		0xc8U, 0xc9U, 0xcaU, 0xcbU, 0xccU, 0xcdU, 0xceU, 0xcfU
	};
	static const uint8_t nonce[13] = {
		0x00U, 0x00U, 0x00U, 0x03U, 0x02U, 0x01U, 0x00U,
		0xa0U, 0xa1U, 0xa2U, 0xa3U, 0xa4U, 0xa5U
	};
	static const uint8_t aad[8] = {
		0x00U, 0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U, 0x07U
	};
	static const uint8_t plaintext[23] = {
		0x08U, 0x09U, 0x0aU, 0x0bU, 0x0cU, 0x0dU, 0x0eU, 0x0fU,
		0x10U, 0x11U, 0x12U, 0x13U, 0x14U, 0x15U, 0x16U, 0x17U,
		0x18U, 0x19U, 0x1aU, 0x1bU, 0x1cU, 0x1dU, 0x1eU
	};
	static const uint8_t expected[31] = {
		0x58U, 0x8cU, 0x97U, 0x9aU, 0x61U, 0xc6U, 0x63U, 0xd2U,
		0xf0U, 0x66U, 0xd0U, 0xc2U, 0xc0U, 0xf9U, 0x89U, 0x80U,
		0x6dU, 0x5fU, 0x6bU, 0x61U, 0xdaU, 0xc3U, 0x84U, 0x17U,
		0xe8U, 0xd1U, 0x2cU, 0xfdU, 0xf9U, 0x26U, 0xe0U
	};
	uint8_t encrypted[31], decrypted[23], damaged_tag[8];

	ccm_encrypt(key, nonce, aad, sizeof(aad), plaintext, sizeof(plaintext),
	    encrypted, encrypted + sizeof(plaintext));
	assert(memcmp(encrypted, expected, sizeof(expected)) == 0);
	assert(ccm_decrypt(key, nonce, aad, sizeof(aad), encrypted,
	    sizeof(plaintext), encrypted + sizeof(plaintext), decrypted) == 0);
	assert(memcmp(decrypted, plaintext, sizeof(plaintext)) == 0);
	memcpy(damaged_tag, encrypted + sizeof(plaintext), sizeof(damaged_tag));
	damaged_tag[3U] ^= 0x80U;
	memset(decrypted, 0xa5, sizeof(decrypted));
	assert(ccm_decrypt(key, nonce, aad, sizeof(aad), encrypted,
	    sizeof(plaintext), damaged_tag, decrypted) == EACCES);
	for (size_t index = 0U; index < sizeof(decrypted); index++)
		assert(decrypted[index] == 0U);
}

static void
test_production_ccmp_framing(void)
{
	static const uint8_t station[6] =
	    { 0x02U, 0x11U, 0x22U, 0x33U, 0x44U, 0x55U };
	static const uint8_t bssid[6] =
	    { 0x02U, 0xaaU, 0xbbU, 0xccU, 0xddU, 0xeeU };
	static const uint8_t peer[6] =
	    { 0x02U, 0x90U, 0x91U, 0x92U, 0x93U, 0x94U };
	static const uint8_t key[16] = {
		0x00U, 0x11U, 0x22U, 0x33U, 0x44U, 0x55U, 0x66U, 0x77U,
		0x88U, 0x99U, 0xaaU, 0xbbU, 0xccU, 0xddU, 0xeeU, 0xffU
	};
	static const uint8_t expected_nonce[13] = {
		0x00U, 0x02U, 0x11U, 0x22U, 0x33U, 0x44U, 0x55U,
		0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U
	};
	uint8_t ethernet[78], mpdu[WLAN_L2_MPDU_MAX + CCM_TAG_SIZE];
	uint8_t nonce[CCM_NONCE_SIZE], aad[CCM_AAD_SIZE];
	uint8_t ciphertext[CCM_MAX_TEXT], recovered[CCM_MAX_TEXT], tag[CCM_TAG_SIZE];
	size_t mpdu_length, plaintext_length, index;
	uint8_t key_index;

	memcpy(ethernet, peer, 6U);
	memcpy(ethernet + 6U, station, 6U);
	ethernet[12U] = 0x08U;
	ethernet[13U] = 0x00U;
	for (index = 14U; index < sizeof(ethernet); index++)
		ethernet[index] = (uint8_t)(index * 7U);
	assert(wlan_l2_build_data(station, bssid, ethernet, sizeof(ethernet), 1,
	    0U, 0x010203040506ULL, mpdu, sizeof(mpdu), &mpdu_length) == 0);
	assert(mpdu_length == WLAN_L2_DATA_HEADER_SIZE +
	    WLAN_L2_CCMP_HEADER_SIZE + WLAN_L2_LLC_SNAP_SIZE +
	    sizeof(ethernet) - WLAN_L2_ETHERNET_HEADER_SIZE);
	ccmp_parameters(mpdu, mpdu_length, nonce, aad, &key_index);
	assert(key_index == 0U);
	assert(memcmp(nonce, expected_nonce, sizeof(nonce)) == 0);
	assert(aad[0U] == 0x08U && aad[1U] == 0x41U);
	assert(memcmp(aad + 2U, bssid, 6U) == 0);
	assert(memcmp(aad + 8U, station, 6U) == 0);
	assert(memcmp(aad + 14U, peer, 6U) == 0);

	plaintext_length = mpdu_length - WLAN_L2_DATA_HEADER_SIZE -
	    WLAN_L2_CCMP_HEADER_SIZE;
	ccm_encrypt(key, nonce, aad, sizeof(aad),
	    mpdu + WLAN_L2_DATA_HEADER_SIZE + WLAN_L2_CCMP_HEADER_SIZE,
	    plaintext_length, ciphertext, tag);
	assert(ccm_decrypt(key, nonce, aad, sizeof(aad), ciphertext,
	    plaintext_length, tag, recovered) == 0);
	assert(memcmp(recovered,
	    mpdu + WLAN_L2_DATA_HEADER_SIZE + WLAN_L2_CCMP_HEADER_SIZE,
	    plaintext_length) == 0);
	assert(plaintext_length + CCM_TAG_SIZE ==
	    WLAN_L2_LLC_SNAP_SIZE + sizeof(ethernet) -
	    WLAN_L2_ETHERNET_HEADER_SIZE + WLAN_L2_CCMP_MIC_SIZE);
}

int
main(void)
{
	test_rfc3610_packet_vector_1();
	test_production_ccmp_framing();
	puts("wlan CCMP reference fixture: PASS");
	return 0;
}
