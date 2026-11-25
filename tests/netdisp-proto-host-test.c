/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Host unit test for Network Display wire format and X25519 + HKDF-SHA256 +
 * ChaCha20 + HMAC-SHA256 roundtrip.
 */

#include "apps/mikuterm-common/netdisp-proto.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int
main(
	void)
{
	struct netdisp_adv adv;
	struct netdisp_adv decoded;
	struct netdisp_offer_plain plain;
	struct netdisp_offer_plain out_plain;
	uint8_t sk[NETDISP_KEY_LEN];
	uint8_t pk[NETDISP_KEY_LEN];
	uint8_t nonce[NETDISP_NONCE_LEN];
	uint8_t adv_buf[NETDISP_ADV_MAX_LEN];
	uint8_t offer_buf[NETDISP_OFFER_MAX_LEN];
	uint8_t tampered[NETDISP_OFFER_MAX_LEN];
	uint8_t bad_nonce[NETDISP_NONCE_LEN];
	char fp[10];
	char id_hex[33];
	uint8_t id_back[NETDISP_DEVICE_ID_LEN];
	size_t adv_len;
	size_t offer_len;
	size_t i;

	assert(netdisp_generate_keypair(sk, pk, nonce) == 0);
	netdisp_fingerprint_hex(pk, nonce, fp);
	assert(strlen(fp) == 9U && fp[4] == '-');

	memset(&adv, 0, sizeof(adv));
	for (i = 0U; i < NETDISP_DEVICE_ID_LEN; i++)
		adv.device_id[i] = (uint8_t)(0xA0U + i);
	memcpy(adv.nonce, nonce, NETDISP_NONCE_LEN);
	memcpy(adv.pk, pk, NETDISP_KEY_LEN);
	adv.reply_port = 41234U;
	adv.pref_width = 1920U;
	adv.pref_height = 1080U;
	adv.pref_scale120 = 180U;
	snprintf(adv.device_name, sizeof(adv.device_name), "Tablet\x01" "Display");

	assert(netdisp_adv_encode(&adv, adv_buf, sizeof(adv_buf), &adv_len) == 0);
	assert(netdisp_adv_decode(adv_buf, adv_len, &decoded) == 0);
	assert(memcmp(decoded.device_id, adv.device_id, NETDISP_DEVICE_ID_LEN) == 0);
	assert(memcmp(decoded.nonce, nonce, NETDISP_NONCE_LEN) == 0);
	assert(memcmp(decoded.pk, pk, NETDISP_KEY_LEN) == 0);
	assert(decoded.reply_port == 41234U);
	assert(decoded.pref_width == 1920U);
	assert(decoded.pref_height == 1080U);
	assert(decoded.pref_scale120 == 180U);
	assert(strcmp(decoded.device_name, "TabletDisplay") == 0);

	netdisp_device_id_hex(decoded.device_id, id_hex);
	assert(netdisp_device_id_from_hex(id_hex, id_back) == 0);
	assert(memcmp(id_back, decoded.device_id, NETDISP_DEVICE_ID_LEN) == 0);

	memset(&plain, 0, sizeof(plain));
	memcpy(plain.nonce, decoded.nonce, NETDISP_NONCE_LEN);
	plain.rdp_port = 3389U;
	snprintf(plain.username, sizeof(plain.username), "awe+extend1");
	snprintf(plain.password, sizeof(plain.password), "s3cr3t-one-shot-pass");

	assert(netdisp_offer_encrypt(decoded.device_id, decoded.pk, &plain,
				     offer_buf, sizeof(offer_buf), &offer_len) == 0);
	assert(netdisp_offer_decrypt(offer_buf, offer_len, adv.device_id,
				     sk, pk, nonce, &out_plain) == 0);
	assert(out_plain.rdp_port == 3389U);
	assert(strcmp(out_plain.username, "awe+extend1") == 0);
	assert(strcmp(out_plain.password, "s3cr3t-one-shot-pass") == 0);

	/* Tampered ciphertext or MAC must be rejected. */
	memcpy(tampered, offer_buf, offer_len);
	tampered[NETDISP_OFFER_HDR_LEN + 2U] ^= 0x55U;
	assert(netdisp_offer_decrypt(tampered, offer_len, adv.device_id,
				     sk, pk, nonce, &out_plain) != 0);

	/* Mismatched nonce must be rejected. */
	memcpy(bad_nonce, nonce, NETDISP_NONCE_LEN);
	bad_nonce[0] ^= 0x01U;
	assert(netdisp_offer_decrypt(offer_buf, offer_len, adv.device_id,
				     sk, pk, bad_nonce, &out_plain) != 0);

	printf("netdisp-proto-host-test: PASS\n");
	return 0;
}
