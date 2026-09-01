/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/net/wlan.h"
#include "kern/net/wlan-wpa2-codec.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>

#define ARRAY_COUNT(values) (sizeof(values) / sizeof((values)[0]))

static const uint8_t station[6] = {
	0x02U, 0x11U, 0x22U, 0x33U, 0x44U, 0x55U
};
static const uint8_t bssid[6] = {
	0x02U, 0xaaU, 0xbbU, 0xccU, 0xddU, 0xeeU
};
static const uint8_t selected_rsn[WLAN_WPA2_RSN_IE_LENGTH] = {
	48U, 20U, 1U, 0U, 0U, 15U, 172U, 4U,
	1U, 0U, 0U, 15U, 172U, 4U, 1U, 0U,
	0U, 15U, 172U, 2U, 0U, 0U
};

static void
put_le16(uint8_t *bytes, uint16_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
}

static void
put_be16(uint8_t *bytes, uint16_t value)
{
	bytes[0] = (uint8_t)(value >> 8);
	bytes[1] = (uint8_t)value;
}

static size_t
mixed_rsn(uint8_t *element)
{
	static const uint8_t ccmp[4] = { 0U, 15U, 172U, 4U };
	static const uint8_t tkip[4] = { 0U, 15U, 172U, 2U };
	static const uint8_t unknown[4] = { 0U, 15U, 172U, 99U };
	static const uint8_t psk[4] = { 0U, 15U, 172U, 2U };
	size_t offset = 2U;

	element[0] = 48U;
	put_le16(element + offset, 1U);
	offset += 2U;
	memcpy(element + offset, ccmp, 4U);
	offset += 4U;
	put_le16(element + offset, 3U);
	offset += 2U;
	memcpy(element + offset, tkip, 4U);
	offset += 4U;
	memcpy(element + offset, unknown, 4U);
	offset += 4U;
	memcpy(element + offset, ccmp, 4U);
	offset += 4U;
	put_le16(element + offset, 1U);
	offset += 2U;
	memcpy(element + offset, psk, 4U);
	offset += 4U;
	put_le16(element + offset, 0U);
	offset += 2U;
	element[1] = (uint8_t)(offset - 2U);
	return offset;
}

static size_t
beacon_with_rsn(uint8_t *frame, const uint8_t *rsn, size_t rsn_length)
{
	const uint8_t ssid[] = { 'm', 'i', 'x', 'e', 'd' };
	size_t offset = 36U;

	memset(frame, 0, 256U);
	frame[0] = 0x80U;
	memcpy(frame + 10U, bssid, 6U);
	memcpy(frame + 16U, bssid, 6U);
	frame[32] = 100U;
	frame[34] = 0x11U;
	frame[offset++] = 0U;
	frame[offset++] = sizeof(ssid);
	memcpy(frame + offset, ssid, sizeof(ssid));
	offset += sizeof(ssid);
	frame[offset++] = 3U;
	frame[offset++] = 1U;
	frame[offset++] = 6U;
	memcpy(frame + offset, rsn, rsn_length);
	return offset + rsn_length;
}

static void
test_rsn_selection(void)
{
	uint8_t element[96];
	uint8_t frame[256];
	struct wlan_bss_record record;
	size_t length;
	size_t frame_length;

	memset(element, 0, sizeof(element));
	assert(wlan_wpa2_rsn_build_ccmp_psk(element, sizeof(element),
	    &length) == 0);
	assert(length == sizeof(selected_rsn));
	assert(memcmp(element, selected_rsn, sizeof(selected_rsn)) == 0);
	assert(wlan_wpa2_rsn_select_ccmp_psk(element, length) == 0);
	assert(wlan_wpa2_rsn_build_ccmp_psk(element, length - 1U,
	    &length) == ENOSPC);
	assert(wlan_wpa2_rsn_build_ccmp_psk(NULL, 0U, &length) == EINVAL);

	length = mixed_rsn(element);
	assert(wlan_wpa2_rsn_select_ccmp_psk(element, length) == 0);
	frame_length = beacon_with_rsn(frame, element, length);
	assert(wlan_frame_parse_bss(frame, frame_length, -31, 0U,
	    &record) == 0);
	assert((record.security & (WLAN_SECURITY_WPA2 |
	    WLAN_SECURITY_CCMP | WLAN_SECURITY_TKIP | WLAN_SECURITY_PSK)) ==
	    (WLAN_SECURITY_WPA2 | WLAN_SECURITY_CCMP |
	    WLAN_SECURITY_TKIP | WLAN_SECURITY_PSK));
	assert((record.security & WLAN_SECURITY_UNSUPPORTED_SUITE) == 0U);

	/* A non-CCMP group cipher is mandatory and therefore not selectable. */
	memcpy(element, selected_rsn, sizeof(selected_rsn));
	element[7] = 2U;
	assert(wlan_wpa2_rsn_select_ccmp_psk(element,
	    sizeof(selected_rsn)) == EINVAL);
	frame_length = beacon_with_rsn(frame, element, sizeof(selected_rsn));
	assert(wlan_frame_parse_bss(frame, frame_length, -31, 0U,
	    &record) == 0);
	assert((record.security & WLAN_SECURITY_UNSUPPORTED_SUITE) != 0U);

	/* A pairwise list without CCMP cannot be repaired by a CCMP group suite. */
	memcpy(element, selected_rsn, sizeof(selected_rsn));
	element[13] = 2U;
	assert(wlan_wpa2_rsn_select_ccmp_psk(element,
	    sizeof(selected_rsn)) == EINVAL);
	frame_length = beacon_with_rsn(frame, element, sizeof(selected_rsn));
	assert(wlan_frame_parse_bss(frame, frame_length, -31, 0U,
	    &record) == 0);
	assert((record.security & WLAN_SECURITY_UNSUPPORTED_SUITE) != 0U);

	/* This strict first profile does not negotiate another AKM. */
	memcpy(element, selected_rsn, sizeof(selected_rsn));
	element[19] = 1U;
	assert(wlan_wpa2_rsn_select_ccmp_psk(element,
	    sizeof(selected_rsn)) == EINVAL);
	memcpy(element, selected_rsn, sizeof(selected_rsn));
	element[20] = 0x40U;
	assert(wlan_wpa2_rsn_select_ccmp_psk(element,
	    sizeof(selected_rsn)) == EINVAL);
	memcpy(element, selected_rsn, sizeof(selected_rsn));
	element[20] = 0x80U;
	assert(wlan_wpa2_rsn_select_ccmp_psk(element,
	    sizeof(selected_rsn)) == 0);
	memcpy(element, selected_rsn, sizeof(selected_rsn));
	element[20] = 0x02U;
	assert(wlan_wpa2_rsn_select_ccmp_psk(element,
	    sizeof(selected_rsn)) == EINVAL);
	memcpy(element, selected_rsn, sizeof(selected_rsn));
	element[21] = 0x04U;
	assert(wlan_wpa2_rsn_select_ccmp_psk(element,
	    sizeof(selected_rsn)) == 0);
	memcpy(element, selected_rsn, sizeof(selected_rsn));
	element[21] = 0x08U;
	assert(wlan_wpa2_rsn_select_ccmp_psk(element,
	    sizeof(selected_rsn)) == EINVAL);
	memcpy(element, selected_rsn, sizeof(selected_rsn));
	element[1] = 26U;
	element[20] = 0x80U;
	element[22] = 0U;
	element[23] = 0U;
	element[24] = 0U;
	element[25] = 15U;
	element[26] = 172U;
	element[27] = 6U;
	assert(wlan_wpa2_rsn_select_ccmp_psk(element, 28U) == 0);
	element[20] = 0U;
	assert(wlan_wpa2_rsn_select_ccmp_psk(element, 28U) == EINVAL);

	memcpy(element, selected_rsn, sizeof(selected_rsn));
	element[1]++;
	assert(wlan_wpa2_rsn_select_ccmp_psk(element,
	    sizeof(selected_rsn)) == EINVAL);
	memcpy(element, selected_rsn, sizeof(selected_rsn));
	element[8] = 0xffU;
	assert(wlan_wpa2_rsn_select_ccmp_psk(element,
	    sizeof(selected_rsn)) == EINVAL);
	assert(wlan_wpa2_rsn_select_ccmp_psk(NULL, 0U) == EINVAL);
}

static void
response_header(uint8_t *frame, uint16_t frame_control)
{
	memset(frame, 0, 256U);
	put_le16(frame, frame_control);
	memcpy(frame + 4U, station, 6U);
	memcpy(frame + 10U, bssid, 6U);
	memcpy(frame + 16U, bssid, 6U);
}

static void
test_authentication_codec(void)
{
	uint8_t frame[256];
	uint8_t modified[256];
	size_t length;
	uint16_t status;

	assert(wlan_wpa2_auth_request_build(frame, sizeof(frame), station,
	    bssid, 0x321U, &length) == 0);
	assert(length == 30U && frame[0] == 0xb0U && frame[1] == 0U);
	assert(memcmp(frame + 4U, bssid, 6U) == 0);
	assert(memcmp(frame + 10U, station, 6U) == 0);
	assert(memcmp(frame + 16U, bssid, 6U) == 0);
	assert(frame[22] == 0x10U && frame[23] == 0x32U);
	assert(frame[24] == 0U && frame[26] == 1U && frame[28] == 0U);
	assert(wlan_wpa2_auth_request_build(frame, 29U, station, bssid,
	    0U, &length) == ENOSPC);
	assert(wlan_wpa2_auth_request_build(NULL, 0U, station, bssid,
	    0U, &length) == EINVAL);
	assert(wlan_wpa2_auth_request_build(frame, sizeof(frame), station,
	    bssid, 0x1000U, &length) == EINVAL);

	response_header(frame, 0x00b0U);
	put_le16(frame + 24U, 0U);
	put_le16(frame + 26U, 2U);
	put_le16(frame + 28U, 0U);
	assert(wlan_wpa2_auth_response_parse(frame, 30U, station, bssid,
	    &status) == 0 && status == 0U);
	put_le16(frame, 0x08b0U);
	put_le16(frame + 28U, 17U);
	assert(wlan_wpa2_auth_response_parse(frame, 30U, station, bssid,
	    &status) == 0 && status == 17U);

	memcpy(modified, frame, 30U);
	modified[0] = 0xc0U;
	assert(wlan_wpa2_auth_response_parse(modified, 30U, station, bssid,
	    &status) == EINVAL);
	memcpy(modified, frame, 30U);
	modified[1] |= 1U;
	assert(wlan_wpa2_auth_response_parse(modified, 30U, station, bssid,
	    &status) == EINVAL);
	memcpy(modified, frame, 30U);
	modified[4] ^= 2U;
	assert(wlan_wpa2_auth_response_parse(modified, 30U, station, bssid,
	    &status) == EINVAL);
	memcpy(modified, frame, 30U);
	modified[10] ^= 2U;
	assert(wlan_wpa2_auth_response_parse(modified, 30U, station, bssid,
	    &status) == EINVAL);
	memcpy(modified, frame, 30U);
	modified[22] |= 1U;
	assert(wlan_wpa2_auth_response_parse(modified, 30U, station, bssid,
	    &status) == EINVAL);
	memcpy(modified, frame, 30U);
	modified[26] = 1U;
	assert(wlan_wpa2_auth_response_parse(modified, 30U, station, bssid,
	    &status) == EINVAL);
	assert(wlan_wpa2_auth_response_parse(frame, 29U, station, bssid,
	    &status) == EINVAL);
}

static size_t
valid_assoc_response(uint8_t *frame)
{
	size_t offset = 30U;
	const uint8_t rates[] = { 0x82U, 0x84U, 0x8bU, 0x96U };

	response_header(frame, 0x0010U);
	put_le16(frame + 24U, 0x0431U);
	put_le16(frame + 26U, 0U);
	put_le16(frame + 28U, 0xc02aU);
	frame[offset++] = 1U;
	frame[offset++] = sizeof(rates);
	memcpy(frame + offset, rates, sizeof(rates));
	return offset + sizeof(rates);
}

static void
test_association_codec(void)
{
	const uint8_t ssid[] = { 'z', 'e', 'd', 'B', 'S', 'D' };
	const uint8_t rates[] = {
		0x82U, 0x84U, 0x8bU, 0x96U, 0x0cU, 0x12U,
		0x18U, 0x24U, 0x30U, 0x48U
	};
	uint8_t frame[256];
	uint8_t modified[256];
	struct wlan_wpa2_assoc_response response;
	size_t length;
	size_t offset;

	assert(wlan_wpa2_assoc_request_build(frame, sizeof(frame), station,
	    bssid, 9U, 0x0431U, 10U, ssid, sizeof(ssid), rates,
	    ARRAY_COUNT(rates), &length) == 0);
	assert(frame[0] == 0U && frame[1] == 0U);
	assert(frame[24] == 0x31U && frame[25] == 0x04U);
	offset = 28U;
	assert(frame[offset++] == 0U && frame[offset++] == sizeof(ssid));
	assert(memcmp(frame + offset, ssid, sizeof(ssid)) == 0);
	offset += sizeof(ssid);
	assert(frame[offset++] == 1U && frame[offset++] == 8U);
	assert(memcmp(frame + offset, rates, 8U) == 0);
	offset += 8U;
	assert(frame[offset++] == 50U && frame[offset++] == 2U);
	assert(memcmp(frame + offset, rates + 8U, 2U) == 0);
	offset += 2U;
	assert(length - offset == sizeof(selected_rsn));
	assert(memcmp(frame + offset, selected_rsn, sizeof(selected_rsn)) == 0);
	assert(wlan_wpa2_assoc_request_build(frame, length - 1U, station,
	    bssid, 9U, 0x0431U, 10U, ssid, sizeof(ssid), rates,
	    ARRAY_COUNT(rates), &length) == ENOSPC);
	assert(wlan_wpa2_assoc_request_build(NULL, 0U, station, bssid, 9U,
	    0x0431U, 10U, ssid, sizeof(ssid), rates, ARRAY_COUNT(rates),
	    &length) == EINVAL);
	assert(wlan_wpa2_assoc_request_build(frame, sizeof(frame), station,
	    bssid, 9U, 0x0421U, 10U, ssid, sizeof(ssid), rates,
	    ARRAY_COUNT(rates), &length) == EINVAL);
	assert(wlan_wpa2_assoc_request_build(frame, sizeof(frame), station,
	    bssid, 9U, 0x0431U, 10U, ssid, 0U, rates,
	    ARRAY_COUNT(rates), &length) == EINVAL);

	length = valid_assoc_response(frame);
	assert(wlan_wpa2_assoc_response_parse(frame, length, station, bssid,
	    &response) == 0);
	assert(response.status == 0U && response.aid == 42U &&
	    response.capability == 0x0431U);
	memcpy(modified, frame, length);
	put_le16(modified + 28U, 0xc000U);
	assert(wlan_wpa2_assoc_response_parse(modified, length, station,
	    bssid, &response) == EINVAL);
	memcpy(modified, frame, length);
	put_le16(modified + 28U, 0xc7d8U);
	assert(wlan_wpa2_assoc_response_parse(modified, length, station,
	    bssid, &response) == EINVAL);
	assert(wlan_wpa2_assoc_response_parse(frame, 30U, station, bssid,
	    &response) == EINVAL);
	memcpy(modified, frame, length);
	modified[length] = 1U;
	modified[length + 1U] = 1U;
	modified[length + 2U] = 0x82U;
	assert(wlan_wpa2_assoc_response_parse(modified, length + 3U,
	    station, bssid, &response) == EINVAL);
	memcpy(modified, frame, length);
	modified[31] = 8U;
	assert(wlan_wpa2_assoc_response_parse(modified, length, station,
	    bssid, &response) == EINVAL);
	memcpy(modified, frame, length);
	put_le16(modified + 26U, 17U);
	assert(wlan_wpa2_assoc_response_parse(modified, 30U, station, bssid,
	    &response) == 0 && response.status == 17U && response.aid == 0U);
}

static void
fill_nonzero(uint8_t *bytes, size_t length, uint8_t seed)
{
	size_t index;

	for (index = 0U; index < length; index++)
		bytes[index] = (uint8_t)(seed + index);
}

static struct wlan_wpa2_eapol_key
key_message(enum wlan_wpa2_eapol_message message)
{
	struct wlan_wpa2_eapol_key key;

	memset(&key, 0, sizeof(key));
	key.message = message;
	key.protocol_version = 2U;
	key.key_length = message == WLAN_WPA2_EAPOL_MESSAGE_1 ||
	    message == WLAN_WPA2_EAPOL_MESSAGE_3 ? 16U : 0U;
	key.replay_counter = UINT64_C(0x0102030405060708);
	if (message != WLAN_WPA2_EAPOL_MESSAGE_4)
		fill_nonzero(key.nonce, sizeof(key.nonce), 1U);
	if (message != WLAN_WPA2_EAPOL_MESSAGE_1)
		fill_nonzero(key.mic, sizeof(key.mic), 0xa0U);
	return key;
}

static void
test_eapol_codec(void)
{
	uint8_t frame[1024];
	uint8_t modified[1024];
	uint8_t encrypted[24];
	struct wlan_wpa2_eapol_key key;
	struct wlan_wpa2_eapol_key parsed;
	size_t length;

	key = key_message(WLAN_WPA2_EAPOL_MESSAGE_1);
	assert(wlan_wpa2_eapol_key_build(frame, sizeof(frame), &key,
	    &length) == 0);
	assert(length == 99U && frame[0] == 2U && frame[1] == 3U &&
	    frame[2] == 0U && frame[3] == 95U && frame[4] == 2U &&
	    frame[5] == 0U && frame[6] == 0x8aU && frame[7] == 0U &&
	    frame[8] == 16U);
	assert(wlan_wpa2_eapol_key_parse(frame, length, &parsed) == 0);
	assert(parsed.message == WLAN_WPA2_EAPOL_MESSAGE_1 &&
	    parsed.replay_counter == key.replay_counter &&
	    memcmp(parsed.nonce, key.nonce, sizeof(key.nonce)) == 0);
	assert(wlan_wpa2_eapol_key_build(frame, length - 1U, &key,
	    &length) == ENOSPC);

	key = key_message(WLAN_WPA2_EAPOL_MESSAGE_2);
	key.key_data = selected_rsn;
	key.key_data_length = sizeof(selected_rsn);
	assert(wlan_wpa2_eapol_key_build(frame, sizeof(frame), &key,
	    &length) == 0);
	assert(length == 121U && frame[5] == 1U && frame[6] == 0x0aU &&
	    frame[7] == 0U && frame[8] == 0U);
	assert(wlan_wpa2_eapol_key_parse(frame, length, &parsed) == 0 &&
	    parsed.message == WLAN_WPA2_EAPOL_MESSAGE_2 &&
	    parsed.key_data_length == sizeof(selected_rsn));
	memcpy(modified, frame, length);
	modified[6] |= 0x80U;
	assert(wlan_wpa2_eapol_key_parse(modified, length, &parsed) == EINVAL);
	memcpy(modified, frame, length);
	modified[4] = 254U;
	assert(wlan_wpa2_eapol_key_parse(modified, length, &parsed) == EINVAL);
	memcpy(modified, frame, length);
	modified[81] = 0U;
	memset(modified + 81U, 0, 16U);
	assert(wlan_wpa2_eapol_key_parse(modified, length, &parsed) == EINVAL);
	memcpy(modified, frame, length);
	put_be16(modified + 97U, 21U);
	assert(wlan_wpa2_eapol_key_parse(modified, length, &parsed) == EINVAL);
	memcpy(modified, frame, length);
	put_be16(modified + 2U, 94U);
	assert(wlan_wpa2_eapol_key_parse(modified, length, &parsed) == EINVAL);
	memcpy(modified, frame, length);
	modified[73] = 1U;
	assert(wlan_wpa2_eapol_key_parse(modified, length, &parsed) == EINVAL);
	assert(wlan_wpa2_eapol_key_parse(frame, length + 1U, &parsed) == EINVAL);

	fill_nonzero(encrypted, sizeof(encrypted), 0x31U);
	key = key_message(WLAN_WPA2_EAPOL_MESSAGE_3);
	key.key_data = encrypted;
	key.key_data_length = sizeof(encrypted);
	key.rsc[0] = 7U;
	assert(wlan_wpa2_eapol_key_build(frame, sizeof(frame), &key,
	    &length) == 0);
	assert(frame[5] == 0x13U && frame[6] == 0xcaU);
	assert(wlan_wpa2_eapol_key_parse(frame, length, &parsed) == 0 &&
	    parsed.message == WLAN_WPA2_EAPOL_MESSAGE_3 && parsed.rsc[0] == 7U);
	key.key_data_length = 23U;
	assert(wlan_wpa2_eapol_key_build(frame, sizeof(frame), &key,
	    &length) == EINVAL);

	key = key_message(WLAN_WPA2_EAPOL_MESSAGE_4);
	assert(wlan_wpa2_eapol_key_build(frame, sizeof(frame), &key,
	    &length) == 0);
	assert(frame[5] == 3U && frame[6] == 0x0aU && frame[7] == 0U &&
	    frame[8] == 0U);
	assert(wlan_wpa2_eapol_key_parse(frame, length, &parsed) == 0 &&
	    parsed.message == WLAN_WPA2_EAPOL_MESSAGE_4);
	key.nonce[0] = 1U;
	assert(wlan_wpa2_eapol_key_build(frame, sizeof(frame), &key,
	    &length) == EINVAL);
	key.nonce[0] = 0U;
	key.key_length = 16U;
	assert(wlan_wpa2_eapol_key_build(frame, sizeof(frame), &key,
	    &length) == EINVAL);
}

static void
test_m3_plaintext_codec(void)
{
	uint8_t plaintext[64];
	uint8_t modified[64];
	uint8_t extended[64];
	uint8_t rsn_extended[64];
	uint8_t gtk[WLAN_WPA2_GTK_LENGTH];
	struct wlan_wpa2_gtk parsed;
	size_t length;

	fill_nonzero(gtk, sizeof(gtk), 0x51U);
	assert(wlan_wpa2_m3_plaintext_build(plaintext, sizeof(plaintext), 1U,
	    gtk, &length) == 0);
	assert(length == 48U && memcmp(plaintext, selected_rsn,
	    sizeof(selected_rsn)) == 0 && plaintext[22] == 221U &&
	    plaintext[23] == 22U && plaintext[28] == 1U &&
	    plaintext[46] == 221U && plaintext[47] == 0U);
	assert(wlan_wpa2_m3_plaintext_parse(plaintext, length, &parsed) == 0);
	assert(parsed.key_index == 1U && memcmp(parsed.key, gtk,
	    sizeof(gtk)) == 0);
	/* Transition-mode APs may authenticate an RSN Extension Element in M3.
	 * Preserve it as ignorable metadata without relaxing the selected RSN
	 * profile or the unique GTK requirement. */
	memcpy(rsn_extended, plaintext, 22U);
	rsn_extended[22] = 244U;
	rsn_extended[23] = 2U;
	rsn_extended[24] = 0x20U;
	rsn_extended[25] = 0x00U;
	memcpy(rsn_extended + 26U, plaintext + 22U, 24U);
	rsn_extended[50] = 221U;
	rsn_extended[51] = 0U;
	memset(rsn_extended + 52U, 0, 4U);
	assert(wlan_wpa2_m3_plaintext_parse(rsn_extended, 56U, &parsed) == 0);
	assert(parsed.key_index == 1U && memcmp(parsed.key, gtk,
	    sizeof(gtk)) == 0);
	rsn_extended[23] = 0U;
	assert(wlan_wpa2_m3_plaintext_parse(rsn_extended, 56U, &parsed) ==
	    EINVAL);
	/* Authenticated M3 key data may carry extension KDEs that this profile
	 * does not consume.  Preserve strict IE framing while skipping them. */
	memcpy(extended, plaintext, 22U);
	extended[22] = 221U;
	extended[23] = 6U;
	extended[24] = 0x00U;
	extended[25] = 0x0fU;
	extended[26] = 0xacU;
	extended[27] = 9U;
	extended[28] = 0U;
	extended[29] = 0U;
	memcpy(extended + 30U, plaintext + 22U, 24U);
	extended[54] = 221U;
	extended[55] = 0U;
	assert(wlan_wpa2_m3_plaintext_parse(extended, 56U, &parsed) == 0);
	assert(parsed.key_index == 1U && memcmp(parsed.key, gtk,
	    sizeof(gtk)) == 0);
	extended[24] = 1U;
	assert(wlan_wpa2_m3_plaintext_parse(extended, 56U, &parsed) == EINVAL);
	extended[24] = 0U;
	extended[22] = 1U;
	assert(wlan_wpa2_m3_plaintext_parse(extended, 56U, &parsed) == EINVAL);
	assert(wlan_wpa2_m3_plaintext_build(plaintext, 47U, 1U, gtk,
	    &length) == ENOSPC);
	assert(wlan_wpa2_m3_plaintext_build(plaintext, sizeof(plaintext), 0U,
	    gtk, &length) == EINVAL);

	memcpy(modified, plaintext, length);
	modified[28] = 0U;
	assert(wlan_wpa2_m3_plaintext_parse(modified, length, &parsed) == EINVAL);
	memcpy(modified, plaintext, length);
	modified[28] = 5U;
	assert(wlan_wpa2_m3_plaintext_parse(modified, length, &parsed) == EINVAL);
	memcpy(modified, plaintext, length);
	modified[29] = 1U;
	assert(wlan_wpa2_m3_plaintext_parse(modified, length, &parsed) == EINVAL);
	memcpy(modified, plaintext, length);
	modified[27] = 2U;
	assert(wlan_wpa2_m3_plaintext_parse(modified, length, &parsed) == EINVAL);
	memcpy(modified, plaintext, length);
	memcpy(modified + 22U, selected_rsn, sizeof(selected_rsn));
	assert(wlan_wpa2_m3_plaintext_parse(modified, length, &parsed) == EINVAL);
	memcpy(modified, plaintext, length);
	modified[47] = 1U;
	assert(wlan_wpa2_m3_plaintext_parse(modified, length, &parsed) == EINVAL);
	assert(wlan_wpa2_m3_plaintext_parse(plaintext, length - 1U,
	    &parsed) == EINVAL);
}

int
main(void)
{
	test_rsn_selection();
	test_authentication_codec();
	test_association_codec();
	test_eapol_codec();
	test_m3_plaintext_codec();
	return 0;
}
