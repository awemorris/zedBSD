/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/net/wlan-wpa2-codec.h"

#include <errno.h>
#include <string.h>

#define IEEE80211_HEADER_LENGTH 24U
#define IEEE80211_AUTH_LENGTH 30U
#define IEEE80211_ASSOC_RESPONSE_LENGTH 30U

#define IEEE80211_FC_ASSOC_REQUEST 0x0000U
#define IEEE80211_FC_ASSOC_RESPONSE 0x0010U
#define IEEE80211_FC_AUTHENTICATION 0x00b0U
#define IEEE80211_FC_RETRY 0x0800U

#define IEEE80211_CAPABILITY_ESS 0x0001U
#define IEEE80211_CAPABILITY_IBSS 0x0002U
#define IEEE80211_CAPABILITY_PRIVACY 0x0010U

#define IEEE80211_IE_SSID 0U
#define IEEE80211_IE_SUPPORTED_RATES 1U
#define IEEE80211_IE_RSN 48U
#define IEEE80211_IE_EXTENDED_RATES 50U
#define IEEE80211_IE_VENDOR 221U

#define RSN_CAPABILITY_MFPR 0x0040U
#define RSN_CAPABILITY_MFPC 0x0080U
#define RSN_CAPABILITY_NO_PAIRWISE 0x0002U
#define RSN_CAPABILITY_SPP_REQUIRED 0x0800U
#define RSN_CAPABILITY_RESERVED 0xe000U

#define EAPOL_HEADER_LENGTH 4U
#define EAPOL_KEY_FIXED_LENGTH 95U
#define EAPOL_PACKET_KEY 3U
#define EAPOL_RSN_KEY_DESCRIPTOR 2U
#define EAPOL_CCMP_KEY_LENGTH 16U

#define EAPOL_KEY_INFO_MESSAGE_1 0x008aU
#define EAPOL_KEY_INFO_MESSAGE_2 0x010aU
#define EAPOL_KEY_INFO_MESSAGE_3 0x13caU
#define EAPOL_KEY_INFO_MESSAGE_4 0x030aU

static const uint8_t rsn_ccmp_suite[4] = { 0x00U, 0x0fU, 0xacU, 0x04U };
static const uint8_t rsn_psk_suite[4] = { 0x00U, 0x0fU, 0xacU, 0x02U };
static const uint8_t rsn_gtk_kde[4] = { 0x00U, 0x0fU, 0xacU, 0x01U };
static const uint8_t selected_rsn_ie[WLAN_WPA2_RSN_IE_LENGTH] = {
	48U, 20U, 1U, 0U, 0U, 15U, 172U, 4U,
	1U, 0U, 0U, 15U, 172U, 4U, 1U, 0U,
	0U, 15U, 172U, 2U, 0U, 0U
};

static uint16_t
get_le16(const uint8_t *bytes)
{
	return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
}

static uint16_t
get_be16(const uint8_t *bytes)
{
	return ((uint16_t)bytes[0] << 8) | (uint16_t)bytes[1];
}

static uint64_t
get_be64(const uint8_t *bytes)
{
	uint64_t value = 0U;
	unsigned index;

	for (index = 0U; index < 8U; index++)
		value = (value << 8) | bytes[index];
	return value;
}

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

static void
put_be64(uint8_t *bytes, uint64_t value)
{
	unsigned index;

	for (index = 0U; index < 8U; index++) {
		bytes[7U - index] = (uint8_t)value;
		value >>= 8;
	}
}

static int
all_zero(const uint8_t *bytes, size_t length)
{
	uint8_t value = 0U;
	size_t index;

	for (index = 0U; index < length; index++)
		value |= bytes[index];
	return value == 0U;
}

static int
valid_unicast_address(const uint8_t address[WLAN_WPA2_MAC_LENGTH])
{
	return address != NULL && (address[0] & 1U) == 0U &&
	    !all_zero(address, WLAN_WPA2_MAC_LENGTH);
}

static int
same_address(const uint8_t *left, const uint8_t *right)
{
	return memcmp(left, right, WLAN_WPA2_MAC_LENGTH) == 0;
}

static int
management_header_build(uint8_t *output, size_t capacity,
	uint16_t frame_control,
	const uint8_t destination[WLAN_WPA2_MAC_LENGTH],
	const uint8_t source[WLAN_WPA2_MAC_LENGTH],
	const uint8_t bssid[WLAN_WPA2_MAC_LENGTH], uint16_t sequence_number)
{
	if (output == NULL || !valid_unicast_address(destination) ||
	    !valid_unicast_address(source) || !valid_unicast_address(bssid) ||
	    sequence_number > 0x0fffU)
		return EINVAL;
	if (capacity < IEEE80211_HEADER_LENGTH)
		return ENOSPC;
	memset(output, 0, IEEE80211_HEADER_LENGTH);
	put_le16(output, frame_control);
	memcpy(output + 4U, destination, WLAN_WPA2_MAC_LENGTH);
	memcpy(output + 10U, source, WLAN_WPA2_MAC_LENGTH);
	memcpy(output + 16U, bssid, WLAN_WPA2_MAC_LENGTH);
	put_le16(output + 22U, (uint16_t)(sequence_number << 4));
	return 0;
}

static int
management_response_header_valid(const uint8_t *frame, size_t length,
	uint16_t expected_frame_control,
	const uint8_t station[WLAN_WPA2_MAC_LENGTH],
	const uint8_t bssid[WLAN_WPA2_MAC_LENGTH])
{
	uint16_t frame_control;

	if (frame == NULL || !valid_unicast_address(station) ||
	    !valid_unicast_address(bssid) || length < IEEE80211_HEADER_LENGTH)
		return 0;
	frame_control = get_le16(frame);
	if ((frame_control & (uint16_t)~IEEE80211_FC_RETRY) !=
	    expected_frame_control || (get_le16(frame + 22U) & 0x000fU) != 0U)
		return 0;
	return same_address(frame + 4U, station) &&
	    same_address(frame + 10U, bssid) &&
	    same_address(frame + 16U, bssid);
}

static int
suite_is(const uint8_t *suite, const uint8_t expected[4])
{
	return memcmp(suite, expected, 4U) == 0;
}

int
wlan_wpa2_rsn_select_ccmp_psk(const uint8_t *element, size_t length)
{
	size_t offset;
	uint16_t count;
	uint16_t capabilities = 0U;
	int have_ccmp = 0;
	int have_psk = 0;
	unsigned index;

	if (element == NULL || length < 2U || element[0] != IEEE80211_IE_RSN ||
	    (size_t)element[1] != length - 2U)
		return EINVAL;
	offset = 2U;
	if (length - offset < 2U || get_le16(element + offset) != 1U)
		return EINVAL;
	offset += 2U;
	if (length - offset < 4U ||
	    !suite_is(element + offset, rsn_ccmp_suite))
		return EINVAL;
	offset += 4U;
	if (length - offset < 2U)
		return EINVAL;
	count = get_le16(element + offset);
	offset += 2U;
	if (count == 0U || (size_t)count > (length - offset) / 4U)
		return EINVAL;
	for (index = 0U; index < count; index++) {
		if (suite_is(element + offset, rsn_ccmp_suite))
			have_ccmp = 1;
		offset += 4U;
	}
	if (!have_ccmp || length - offset < 2U)
		return EINVAL;
	count = get_le16(element + offset);
	offset += 2U;
	if (count == 0U || (size_t)count > (length - offset) / 4U)
		return EINVAL;
	for (index = 0U; index < count; index++) {
		if (suite_is(element + offset, rsn_psk_suite))
			have_psk = 1;
		offset += 4U;
	}
	if (!have_psk)
		return EINVAL;
	if (offset == length)
		return 0;
	if (length - offset < 2U)
		return EINVAL;
	capabilities = get_le16(element + offset);
	offset += 2U;
	if ((capabilities & (RSN_CAPABILITY_NO_PAIRWISE |
	    RSN_CAPABILITY_MFPR | RSN_CAPABILITY_SPP_REQUIRED |
	    RSN_CAPABILITY_RESERVED)) != 0U)
		return EINVAL;
	if (offset == length)
		return 0;
	if (length - offset < 2U)
		return EINVAL;
	count = get_le16(element + offset);
	offset += 2U;
	if ((size_t)count > (length - offset) / 16U)
		return EINVAL;
	offset += (size_t)count * 16U;
	if (offset == length)
		return 0;
	/* A final group-management cipher suite is structurally valid even when
	 * PMF is merely advertised; this profile does not select it. */
	if (length - offset != 4U ||
	    (capabilities & RSN_CAPABILITY_MFPC) == 0U)
		return EINVAL;
	return 0;
}

int
wlan_wpa2_rsn_build_ccmp_psk(uint8_t *output, size_t capacity,
	size_t *result_length)
{
	if (output == NULL || result_length == NULL)
		return EINVAL;
	if (capacity < WLAN_WPA2_RSN_IE_LENGTH)
		return ENOSPC;
	output[0] = IEEE80211_IE_RSN;
	output[1] = 20U;
	put_le16(output + 2U, 1U);
	memcpy(output + 4U, rsn_ccmp_suite, 4U);
	put_le16(output + 8U, 1U);
	memcpy(output + 10U, rsn_ccmp_suite, 4U);
	put_le16(output + 14U, 1U);
	memcpy(output + 16U, rsn_psk_suite, 4U);
	put_le16(output + 20U, 0U);
	*result_length = WLAN_WPA2_RSN_IE_LENGTH;
	return 0;
}

int
wlan_wpa2_auth_request_build(uint8_t *output, size_t capacity,
	const uint8_t station[WLAN_WPA2_MAC_LENGTH],
	const uint8_t bssid[WLAN_WPA2_MAC_LENGTH], uint16_t sequence_number,
	size_t *result_length)
{
	int error;

	if (output == NULL || result_length == NULL)
		return EINVAL;
	if (capacity < IEEE80211_AUTH_LENGTH)
		return ENOSPC;
	error = management_header_build(output, capacity,
	    IEEE80211_FC_AUTHENTICATION, bssid, station, bssid,
	    sequence_number);
	if (error != 0)
		return error;
	put_le16(output + 24U, 0U);
	put_le16(output + 26U, 1U);
	put_le16(output + 28U, 0U);
	*result_length = IEEE80211_AUTH_LENGTH;
	return 0;
}

int
wlan_wpa2_auth_response_parse(const uint8_t *frame, size_t length,
	const uint8_t station[WLAN_WPA2_MAC_LENGTH],
	const uint8_t bssid[WLAN_WPA2_MAC_LENGTH], uint16_t *status)
{
	if (status == NULL || length != IEEE80211_AUTH_LENGTH ||
	    !management_response_header_valid(frame, length,
	    IEEE80211_FC_AUTHENTICATION, station, bssid) ||
	    get_le16(frame + 24U) != 0U || get_le16(frame + 26U) != 2U)
		return EINVAL;
	*status = get_le16(frame + 28U);
	return 0;
}

static int
rates_valid(const uint8_t *rates, size_t rate_count)
{
	size_t index;

	if (rates == NULL || rate_count == 0U ||
	    rate_count > WLAN_WPA2_RATE_MAX)
		return 0;
	for (index = 0U; index < rate_count; index++) {
		if ((rates[index] & 0x7fU) == 0U)
			return 0;
	}
	return 1;
}

int
wlan_wpa2_assoc_request_build(uint8_t *output, size_t capacity,
	const uint8_t station[WLAN_WPA2_MAC_LENGTH],
	const uint8_t bssid[WLAN_WPA2_MAC_LENGTH], uint16_t sequence_number,
	uint16_t capability, uint16_t listen_interval, const uint8_t *ssid,
	size_t ssid_length, const uint8_t *rates, size_t rate_count,
	size_t *result_length)
{
	size_t length;
	size_t first_rates;
	size_t rsn_length;
	int error;

	if (output == NULL || result_length == NULL || ssid == NULL ||
	    ssid_length == 0U ||
	    ssid_length > 32U || !rates_valid(rates, rate_count) ||
	    listen_interval == 0U ||
	    (capability & (IEEE80211_CAPABILITY_ESS |
	    IEEE80211_CAPABILITY_PRIVACY)) !=
	    (IEEE80211_CAPABILITY_ESS | IEEE80211_CAPABILITY_PRIVACY) ||
	    (capability & IEEE80211_CAPABILITY_IBSS) != 0U)
		return EINVAL;
	first_rates = rate_count < 8U ? rate_count : 8U;
	length = IEEE80211_HEADER_LENGTH + 4U + 2U + ssid_length +
	    2U + first_rates + WLAN_WPA2_RSN_IE_LENGTH;
	if (rate_count > first_rates)
		length += 2U + rate_count - first_rates;
	if (capacity < length)
		return ENOSPC;
	error = management_header_build(output, capacity,
	    IEEE80211_FC_ASSOC_REQUEST, bssid, station, bssid,
	    sequence_number);
	if (error != 0)
		return error;
	put_le16(output + 24U, capability);
	put_le16(output + 26U, listen_interval);
	length = 28U;
	output[length++] = IEEE80211_IE_SSID;
	output[length++] = (uint8_t)ssid_length;
	memcpy(output + length, ssid, ssid_length);
	length += ssid_length;
	output[length++] = IEEE80211_IE_SUPPORTED_RATES;
	output[length++] = (uint8_t)first_rates;
	memcpy(output + length, rates, first_rates);
	length += first_rates;
	if (rate_count > first_rates) {
		output[length++] = IEEE80211_IE_EXTENDED_RATES;
		output[length++] = (uint8_t)(rate_count - first_rates);
		memcpy(output + length, rates + first_rates,
		    rate_count - first_rates);
		length += rate_count - first_rates;
	}
	error = wlan_wpa2_rsn_build_ccmp_psk(output + length,
	    capacity - length, &rsn_length);
	if (error != 0)
		return error;
	length += rsn_length;
	*result_length = length;
	return 0;
}

int
wlan_wpa2_assoc_response_parse(const uint8_t *frame, size_t length,
	const uint8_t station[WLAN_WPA2_MAC_LENGTH],
	const uint8_t bssid[WLAN_WPA2_MAC_LENGTH],
	struct wlan_wpa2_assoc_response *result)
{
	struct wlan_wpa2_assoc_response parsed;
	size_t offset;
	int have_rates = 0;
	int have_extended = 0;

	if (result == NULL || length < IEEE80211_ASSOC_RESPONSE_LENGTH ||
	    !management_response_header_valid(frame, length,
	    IEEE80211_FC_ASSOC_RESPONSE, station, bssid))
		return EINVAL;
	parsed.capability = get_le16(frame + 24U);
	parsed.status = get_le16(frame + 26U);
	parsed.aid = 0U;
	if (parsed.status == 0U) {
		uint16_t aid = get_le16(frame + 28U);

		if ((aid & 0xc000U) != 0xc000U || (aid & 0x3fffU) == 0U ||
		    (aid & 0x3fffU) > 2007U)
			return EINVAL;
		parsed.aid = aid & 0x3fffU;
	}
	offset = IEEE80211_ASSOC_RESPONSE_LENGTH;
	while (offset < length) {
		uint8_t identifier;
		uint8_t ie_length;
		size_t index;

		if (length - offset < 2U)
			return EINVAL;
		identifier = frame[offset];
		ie_length = frame[offset + 1U];
		offset += 2U;
		if ((size_t)ie_length > length - offset)
			return EINVAL;
		if (identifier == IEEE80211_IE_SUPPORTED_RATES ||
		    identifier == IEEE80211_IE_EXTENDED_RATES) {
			if (ie_length == 0U ||
			    (identifier == IEEE80211_IE_SUPPORTED_RATES &&
			    (have_rates || ie_length > 8U)) ||
			    (identifier == IEEE80211_IE_EXTENDED_RATES &&
			    have_extended))
				return EINVAL;
			for (index = 0U; index < ie_length; index++) {
				if ((frame[offset + index] & 0x7fU) == 0U)
					return EINVAL;
			}
			if (identifier == IEEE80211_IE_SUPPORTED_RATES)
				have_rates = 1;
			else
				have_extended = 1;
		}
		offset += ie_length;
	}
	if (parsed.status == 0U && !have_rates)
		return EINVAL;
	*result = parsed;
	return 0;
}

static uint16_t
message_key_info(enum wlan_wpa2_eapol_message message)
{
	switch (message) {
	case WLAN_WPA2_EAPOL_MESSAGE_1:
		return EAPOL_KEY_INFO_MESSAGE_1;
	case WLAN_WPA2_EAPOL_MESSAGE_2:
		return EAPOL_KEY_INFO_MESSAGE_2;
	case WLAN_WPA2_EAPOL_MESSAGE_3:
		return EAPOL_KEY_INFO_MESSAGE_3;
	case WLAN_WPA2_EAPOL_MESSAGE_4:
		return EAPOL_KEY_INFO_MESSAGE_4;
	default:
		return 0U;
	}
}

static enum wlan_wpa2_eapol_message
key_info_message(uint16_t key_info)
{
	switch (key_info) {
	case EAPOL_KEY_INFO_MESSAGE_1:
		return WLAN_WPA2_EAPOL_MESSAGE_1;
	case EAPOL_KEY_INFO_MESSAGE_2:
		return WLAN_WPA2_EAPOL_MESSAGE_2;
	case EAPOL_KEY_INFO_MESSAGE_3:
		return WLAN_WPA2_EAPOL_MESSAGE_3;
	case EAPOL_KEY_INFO_MESSAGE_4:
		return WLAN_WPA2_EAPOL_MESSAGE_4;
	default:
		return 0;
	}
}

static int
eapol_key_fields_valid(const struct wlan_wpa2_eapol_key *key,
	int parsing)
{
	if (key == NULL || (key->protocol_version != 1U &&
	    key->protocol_version != 2U) || message_key_info(key->message) == 0U ||
	    key->key_data_length > WLAN_WPA2_EAPOL_KEY_DATA_MAX ||
	    (key->key_data_length != 0U && key->key_data == NULL) ||
	    !all_zero(key->iv, sizeof(key->iv)))
		return 0;
	if (key->message != WLAN_WPA2_EAPOL_MESSAGE_3 &&
	    !all_zero(key->rsc, sizeof(key->rsc)))
		return 0;
	switch (key->message) {
	case WLAN_WPA2_EAPOL_MESSAGE_1:
		return key->key_length == EAPOL_CCMP_KEY_LENGTH &&
		    key->key_data_length == 0U &&
		    !all_zero(key->nonce, sizeof(key->nonce)) &&
		    all_zero(key->mic, sizeof(key->mic));
	case WLAN_WPA2_EAPOL_MESSAGE_2:
		return key->key_length == 0U &&
		    key->key_data_length == WLAN_WPA2_RSN_IE_LENGTH &&
		    !all_zero(key->nonce, sizeof(key->nonce)) &&
		    (!parsing || !all_zero(key->mic, sizeof(key->mic))) &&
		    wlan_wpa2_rsn_select_ccmp_psk(key->key_data,
		    key->key_data_length) == 0 &&
		    memcmp(key->key_data, selected_rsn_ie,
		    WLAN_WPA2_RSN_IE_LENGTH) == 0;
	case WLAN_WPA2_EAPOL_MESSAGE_3:
		return key->key_length == EAPOL_CCMP_KEY_LENGTH &&
		    key->key_data_length >= 24U &&
		    (key->key_data_length & 7U) == 0U &&
		    !all_zero(key->nonce, sizeof(key->nonce)) &&
		    (!parsing || !all_zero(key->mic, sizeof(key->mic)));
	case WLAN_WPA2_EAPOL_MESSAGE_4:
		return key->key_length == 0U && key->key_data_length == 0U &&
		    all_zero(key->nonce, sizeof(key->nonce)) &&
		    (!parsing || !all_zero(key->mic, sizeof(key->mic)));
	default:
		return 0;
	}
}

int
wlan_wpa2_eapol_key_parse(const uint8_t *frame, size_t length,
	struct wlan_wpa2_eapol_key *result)
{
	struct wlan_wpa2_eapol_key parsed;
	size_t body_length;
	size_t key_data_length;

	if (frame == NULL || result == NULL || length <
	    EAPOL_HEADER_LENGTH + EAPOL_KEY_FIXED_LENGTH ||
	    (frame[0] != 1U && frame[0] != 2U) ||
	    frame[1] != EAPOL_PACKET_KEY)
		return EINVAL;
	body_length = get_be16(frame + 2U);
	if (body_length != length - EAPOL_HEADER_LENGTH ||
	    body_length < EAPOL_KEY_FIXED_LENGTH ||
	    frame[4] != EAPOL_RSN_KEY_DESCRIPTOR)
		return EINVAL;
	memset(&parsed, 0, sizeof(parsed));
	parsed.protocol_version = frame[0];
	parsed.message = key_info_message(get_be16(frame + 5U));
	parsed.key_length = get_be16(frame + 7U);
	parsed.replay_counter = get_be64(frame + 9U);
	memcpy(parsed.nonce, frame + 17U, sizeof(parsed.nonce));
	memcpy(parsed.iv, frame + 49U, sizeof(parsed.iv));
	memcpy(parsed.rsc, frame + 65U, sizeof(parsed.rsc));
	/* The 8-octet Key ID/reserved field is unused by RSN and must be zero. */
	if (!all_zero(frame + 73U, 8U))
		return EINVAL;
	memcpy(parsed.mic, frame + 81U, sizeof(parsed.mic));
	key_data_length = get_be16(frame + 97U);
	if (key_data_length != body_length - EAPOL_KEY_FIXED_LENGTH)
		return EINVAL;
	parsed.key_data = frame + 99U;
	parsed.key_data_length = key_data_length;
	if (!eapol_key_fields_valid(&parsed, 1))
		return EINVAL;
	*result = parsed;
	return 0;
}

int
wlan_wpa2_eapol_key_build(uint8_t *output, size_t capacity,
	const struct wlan_wpa2_eapol_key *key, size_t *result_length)
{
	size_t length;

	if (output == NULL || result_length == NULL ||
	    !eapol_key_fields_valid(key, 0))
		return EINVAL;
	length = EAPOL_HEADER_LENGTH + EAPOL_KEY_FIXED_LENGTH +
	    key->key_data_length;
	if (capacity < length)
		return ENOSPC;
	memset(output, 0, length);
	output[0] = key->protocol_version;
	output[1] = EAPOL_PACKET_KEY;
	put_be16(output + 2U,
	    (uint16_t)(EAPOL_KEY_FIXED_LENGTH + key->key_data_length));
	output[4] = EAPOL_RSN_KEY_DESCRIPTOR;
	put_be16(output + 5U, message_key_info(key->message));
	put_be16(output + 7U, key->key_length);
	put_be64(output + 9U, key->replay_counter);
	memcpy(output + 17U, key->nonce, sizeof(key->nonce));
	memcpy(output + 49U, key->iv, sizeof(key->iv));
	memcpy(output + 65U, key->rsc, sizeof(key->rsc));
	memcpy(output + 81U, key->mic, sizeof(key->mic));
	put_be16(output + 97U, (uint16_t)key->key_data_length);
	if (key->key_data_length != 0U)
		memcpy(output + 99U, key->key_data, key->key_data_length);
	*result_length = length;
	return 0;
}

static int
padding_valid(const uint8_t *bytes, size_t length)
{
	size_t offset;

	if (length == 0U || bytes[0] != IEEE80211_IE_VENDOR)
		return 0;
	if (length != 1U && bytes[1] != 0U)
		return 0;
	for (offset = length == 1U ? 1U : 2U; offset < length; offset++) {
		if (bytes[offset] != 0U)
			return 0;
	}
	return 1;
}

int
wlan_wpa2_m3_plaintext_parse(const uint8_t *plaintext, size_t length,
	struct wlan_wpa2_gtk *result)
{
	struct wlan_wpa2_gtk parsed;
	size_t offset = 0U;
	int have_rsn = 0;
	int have_gtk = 0;

	if (plaintext == NULL || result == NULL || length < 16U ||
	    (length & 7U) != 0U || length > WLAN_WPA2_EAPOL_KEY_DATA_MAX)
		return EINVAL;
	memset(&parsed, 0, sizeof(parsed));
	while (offset < length) {
		uint8_t identifier;
		uint8_t ie_length;
		const uint8_t *body;

		if (plaintext[offset] == IEEE80211_IE_VENDOR &&
		    padding_valid(plaintext + offset, length - offset)) {
			offset = length;
			break;
		}
		if (length - offset < 2U)
			return EINVAL;
		identifier = plaintext[offset];
		ie_length = plaintext[offset + 1U];
		if ((size_t)ie_length > length - offset - 2U)
			return EINVAL;
		body = plaintext + offset + 2U;
		if (identifier == IEEE80211_IE_RSN) {
			if (have_rsn || wlan_wpa2_rsn_select_ccmp_psk(
			    plaintext + offset, (size_t)ie_length + 2U) != 0)
				return EINVAL;
			have_rsn = 1;
		} else if (identifier == IEEE80211_IE_VENDOR &&
		    ie_length >= 4U && suite_is(body, rsn_gtk_kde)) {
			uint8_t key_info;

			if (have_gtk || ie_length != 22U)
				return EINVAL;
			key_info = body[4];
			/* Key IDs 1--3 are group slots.  The Tx/reserved bits and
			 * second reserved octet are not part of this station profile. */
			if ((key_info & 0xfcU) != 0U || (key_info & 3U) == 0U ||
			    body[5] != 0U)
				return EINVAL;
			parsed.key_index = key_info & 3U;
			memcpy(parsed.key, body + 6U, sizeof(parsed.key));
			have_gtk = 1;
		} else {
			return EINVAL;
		}
		offset += (size_t)ie_length + 2U;
	}
	if (!have_rsn || !have_gtk)
		return EINVAL;
	*result = parsed;
	return 0;
}

int
wlan_wpa2_m3_plaintext_build(uint8_t *output, size_t capacity,
	uint8_t key_index, const uint8_t gtk[WLAN_WPA2_GTK_LENGTH],
	size_t *result_length)
{
	const size_t length = 48U;

	if (output == NULL || gtk == NULL || result_length == NULL ||
	    key_index == 0U || key_index > 3U)
		return EINVAL;
	if (capacity < length)
		return ENOSPC;
	memcpy(output, selected_rsn_ie, sizeof(selected_rsn_ie));
	output[22] = IEEE80211_IE_VENDOR;
	output[23] = 22U;
	memcpy(output + 24U, rsn_gtk_kde, sizeof(rsn_gtk_kde));
	output[28] = key_index;
	output[29] = 0U;
	memcpy(output + 30U, gtk, WLAN_WPA2_GTK_LENGTH);
	/* Canonical KDE padding: vendor-specific ID, zero length. */
	output[46] = IEEE80211_IE_VENDOR;
	output[47] = 0U;
	*result_length = length;
	return 0;
}
