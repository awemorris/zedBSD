/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The WPA2-PSK station codec.
 *
 * The station profile is frozen to CCMP with pre-shared key
 * authentication.  This file validates and builds the RSN element for
 * that profile, the authentication and association management frames,
 * the EAPOL-Key messages of the four-way and group handshakes, and the
 * key data plaintext that carries the group temporal key.
 */

#include "kern/net/wifi/wlan-wpa2-codec.h"

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
#define IEEE80211_IE_RSN_EXTENSION 244U

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
#define EAPOL_KEY_INFO_GROUP_MESSAGE_1 0x1382U
#define EAPOL_KEY_INFO_GROUP_MESSAGE_2 0x0302U

static const uint8_t rsn_ccmp_suite[4] = { 0x00U, 0x0fU, 0xacU, 0x04U };
static const uint8_t rsn_psk_suite[4] = { 0x00U, 0x0fU, 0xacU, 0x02U };
static const uint8_t rsn_gtk_kde[4] = { 0x00U, 0x0fU, 0xacU, 0x01U };
static const uint8_t selected_rsn_ie[WLAN_WPA2_RSN_IE_LENGTH] = {
	48U, 20U, 1U, 0U, 0U, 15U, 172U, 4U,
	1U, 0U, 0U, 15U, 172U, 4U, 1U, 0U,
	0U, 15U, 172U, 2U, 0U, 0U
};

static uint16_t get_le16(const uint8_t *bytes);
static uint16_t get_be16(const uint8_t *bytes);
static uint64_t get_be64(const uint8_t *bytes);
static void put_le16(uint8_t *bytes, uint16_t value);
static void put_be16(uint8_t *bytes, uint16_t value);
static void put_be64(uint8_t *bytes, uint64_t value);
static int all_zero(const uint8_t *bytes, size_t length);
static int valid_unicast_address(const uint8_t address[WLAN_WPA2_MAC_LENGTH]);
static int same_address(const uint8_t *left, const uint8_t *right);
static int management_header_build(uint8_t *output, size_t capacity, uint16_t frame_control, const uint8_t destination[WLAN_WPA2_MAC_LENGTH], const uint8_t source[WLAN_WPA2_MAC_LENGTH], const uint8_t bssid[WLAN_WPA2_MAC_LENGTH], uint16_t sequence_number);
static int management_response_header_valid(const uint8_t *frame, size_t length, uint16_t expected_frame_control, const uint8_t station[WLAN_WPA2_MAC_LENGTH], const uint8_t bssid[WLAN_WPA2_MAC_LENGTH]);
static int suite_is(const uint8_t *suite, const uint8_t expected[4]);
static int rates_valid(const uint8_t *rates, size_t rate_count);
static uint16_t message_key_info(enum wlan_wpa2_eapol_message message);
static enum wlan_wpa2_eapol_message key_info_message(uint16_t key_info);
static int eapol_key_fields_valid(const struct wlan_wpa2_eapol_key *key, int parsing);
static int padding_valid(const uint8_t *bytes, size_t length);
static int key_plaintext_parse(const uint8_t *plaintext, size_t length, struct wlan_wpa2_gtk *result, int require_rsn);

/*
 * Checks that an RSN element offers the CCMP with PSK profile.
 *
 * The group cipher must be CCMP, CCMP must be among the pairwise ciphers,
 * and PSK among the key management suites.  Capabilities that the
 * profile cannot honour, such as required management frame protection,
 * are refused.
 */
int
wlan_wpa2_rsn_select_ccmp_psk(
	const uint8_t *element,
	size_t length)
{
	size_t offset;
	uint16_t count;
	uint16_t capabilities;
	unsigned index;
	int have_ccmp;
	int have_psk;

	capabilities = 0U;
	have_ccmp = 0;
	have_psk = 0;

	/* Rejects anything but a version 1 RSN element with a matching length. */
	if (element == NULL ||
	    length < 2U ||
	    element[0] != IEEE80211_IE_RSN ||
	    (size_t)element[1] != length - 2U)
		return EINVAL;
	offset = 2U;
	if (length - offset < 2U || get_le16(element + offset) != 1U)
		return EINVAL;
	offset += 2U;

	/* The group cipher must be CCMP. */
	if (length - offset < 4U ||
	    !suite_is(element + offset, rsn_ccmp_suite))
		return EINVAL;
	offset += 4U;

	/* CCMP must be offered as a pairwise cipher. */
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

	/* PSK must be offered as a key management suite. */
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

	/* The capabilities are optional but must not demand the unsupported. */
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

	/* The PMKID list is optional and skipped. */
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

	/*
	 * A final group-management cipher suite is structurally valid even
	 * when PMF is merely advertised; this profile does not select it.
	 */
	if (length - offset != 4U ||
	    (capabilities & RSN_CAPABILITY_MFPC) == 0U)
		return EINVAL;

	/* Reports an acceptable element. */
	return 0;
}

/*
 * Builds the RSN element the station selects: CCMP group and pairwise, PSK.
 */
int
wlan_wpa2_rsn_build_ccmp_psk(
	uint8_t *output,
	size_t capacity,
	size_t *result_length)
{
	/* Rejects a missing output or result. */
	if (output == NULL || result_length == NULL)
		return EINVAL;
	if (capacity < WLAN_WPA2_RSN_IE_LENGTH)
		return ENOSPC;

	/* Writes version 1, the ciphers, one PSK suite, and no capabilities. */
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

	/* Reports the built element. */
	return 0;
}

/*
 * Builds the open system authentication request.
 */
int
wlan_wpa2_auth_request_build(
	uint8_t *output,
	size_t capacity,
	const uint8_t station[WLAN_WPA2_MAC_LENGTH],
	const uint8_t bssid[WLAN_WPA2_MAC_LENGTH],
	uint16_t sequence_number,
	size_t *result_length)
{
	int error;

	/* Rejects a missing output or result. */
	if (output == NULL || result_length == NULL)
		return EINVAL;
	if (capacity < IEEE80211_AUTH_LENGTH)
		return ENOSPC;

	/* Addresses the frame from the station to the access point. */
	error = management_header_build(output, capacity,
	    IEEE80211_FC_AUTHENTICATION, bssid, station, bssid,
	    sequence_number);
	if (error != 0)
		return error;

	/* Open system, first transaction, no status. */
	put_le16(output + 24U, 0U);
	put_le16(output + 26U, 1U);
	put_le16(output + 28U, 0U);
	*result_length = IEEE80211_AUTH_LENGTH;

	/* Reports the built frame. */
	return 0;
}

/*
 * Parses the open system authentication response and extracts its status.
 */
int
wlan_wpa2_auth_response_parse(
	const uint8_t *frame,
	size_t length,
	const uint8_t station[WLAN_WPA2_MAC_LENGTH],
	const uint8_t bssid[WLAN_WPA2_MAC_LENGTH],
	uint16_t *status)
{
	/* Accepts only the second open system transaction addressed to us. */
	if (status == NULL ||
	    length != IEEE80211_AUTH_LENGTH ||
	    !management_response_header_valid(frame, length,
	    IEEE80211_FC_AUTHENTICATION, station, bssid) ||
	    get_le16(frame + 24U) != 0U ||
	    get_le16(frame + 26U) != 2U)
		return EINVAL;

	*status = get_le16(frame + 28U);

	/* Reports the parsed status. */
	return 0;
}

/*
 * Builds the association request with the selected RSN element.
 *
 * The capability must announce an ESS with privacy, the rates are split
 * into the supported and extended rate elements, and the RSN element
 * closes the frame.
 */
int
wlan_wpa2_assoc_request_build(
	uint8_t *output,
	size_t capacity,
	const uint8_t station[WLAN_WPA2_MAC_LENGTH],
	const uint8_t bssid[WLAN_WPA2_MAC_LENGTH],
	uint16_t sequence_number,
	uint16_t capability,
	uint16_t listen_interval,
	const uint8_t *ssid,
	size_t ssid_length,
	const uint8_t *rates,
	size_t rate_count,
	size_t *result_length)
{
	size_t length;
	size_t first_rates;
	size_t rsn_length;
	int error;

	/* Rejects a missing operand, a bad SSID or rate set, or a wrong capability. */
	if (output == NULL ||
	    result_length == NULL ||
	    ssid == NULL ||
	    ssid_length == 0U ||
	    ssid_length > 32U ||
	    !rates_valid(rates, rate_count) ||
	    listen_interval == 0U ||
	    (capability & (IEEE80211_CAPABILITY_ESS |
	    IEEE80211_CAPABILITY_PRIVACY)) !=
	    (IEEE80211_CAPABILITY_ESS | IEEE80211_CAPABILITY_PRIVACY) ||
	    (capability & IEEE80211_CAPABILITY_IBSS) != 0U)
		return EINVAL;

	/* Sizes the frame: at most eight rates fit the supported rates element. */
	if (rate_count < 8U)
		first_rates = rate_count;
	else
		first_rates = 8U;
	length = IEEE80211_HEADER_LENGTH + 4U + 2U + ssid_length +
	    2U + first_rates + WLAN_WPA2_RSN_IE_LENGTH;
	if (rate_count > first_rates)
		length += 2U + rate_count - first_rates;
	if (capacity < length)
		return ENOSPC;

	/* Addresses the frame and writes the fixed fields. */
	error = management_header_build(output, capacity,
	    IEEE80211_FC_ASSOC_REQUEST, bssid, station, bssid,
	    sequence_number);
	if (error != 0)
		return error;
	put_le16(output + 24U, capability);
	put_le16(output + 26U, listen_interval);
	length = 28U;

	/* Appends the SSID and the rate elements. */
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

	/* Appends the selected RSN element. */
	error = wlan_wpa2_rsn_build_ccmp_psk(output + length,
	    capacity - length, &rsn_length);
	if (error != 0)
		return error;
	length += rsn_length;
	*result_length = length;

	/* Reports the built frame. */
	return 0;
}

/*
 * Parses an association response.
 *
 * A successful response must carry a valid association identifier and a
 * supported rates element; the rate elements are checked for sanity
 * and everything else is ignored.
 */
int
wlan_wpa2_assoc_response_parse(
	const uint8_t *frame,
	size_t length,
	const uint8_t station[WLAN_WPA2_MAC_LENGTH],
	const uint8_t bssid[WLAN_WPA2_MAC_LENGTH],
	struct wlan_wpa2_assoc_response *result)
{
	struct wlan_wpa2_assoc_response parsed;
	size_t offset;
	size_t index;
	uint16_t aid;
	uint8_t identifier;
	uint8_t ie_length;
	int have_rates;
	int have_extended;

	have_rates = 0;
	have_extended = 0;

	/* Accepts only a complete response addressed to us. */
	if (result == NULL ||
	    length < IEEE80211_ASSOC_RESPONSE_LENGTH ||
	    !management_response_header_valid(frame, length,
	    IEEE80211_FC_ASSOC_RESPONSE, station, bssid))
		return EINVAL;

	/* Takes the fixed fields; a success carries a 14-bit identifier. */
	parsed.capability = get_le16(frame + 24U);
	parsed.status = get_le16(frame + 26U);
	parsed.aid = 0U;
	if (parsed.status == 0U) {
		aid = get_le16(frame + 28U);
		if ((aid & 0xc000U) != 0xc000U ||
		    (aid & 0x3fffU) == 0U ||
		    (aid & 0x3fffU) > 2007U)
			return EINVAL;
		parsed.aid = aid & 0x3fffU;
	}

	/* Walks the elements, checking the rate elements. */
	offset = IEEE80211_ASSOC_RESPONSE_LENGTH;
	while (offset < length) {
		if (length - offset < 2U)
			return EINVAL;
		identifier = frame[offset];
		ie_length = frame[offset + 1U];
		offset += 2U;
		if ((size_t)ie_length > length - offset)
			return EINVAL;
		if (identifier == IEEE80211_IE_SUPPORTED_RATES ||
		    identifier == IEEE80211_IE_EXTENDED_RATES) {
			/* Each rate element appears once, non-empty, with non-zero rates. */
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

	/* A success without rates is malformed. */
	if (parsed.status == 0U && !have_rates)
		return EINVAL;

	*result = parsed;

	/* Reports the parsed response. */
	return 0;
}

/*
 * Parses an EAPOL-Key frame of the RSN descriptor type.
 *
 * The key information must identify one of the six handshake messages,
 * and every field must satisfy that message's constraints.
 */
int
wlan_wpa2_eapol_key_parse(
	const uint8_t *frame,
	size_t length,
	struct wlan_wpa2_eapol_key *result)
{
	struct wlan_wpa2_eapol_key parsed;
	size_t body_length;
	size_t key_data_length;

	/* Accepts only an EAPOL version 1 or 2 key packet of full length. */
	if (frame == NULL ||
	    result == NULL ||
	    length < EAPOL_HEADER_LENGTH + EAPOL_KEY_FIXED_LENGTH ||
	    (frame[0] != 1U && frame[0] != 2U) ||
	    frame[1] != EAPOL_PACKET_KEY)
		return EINVAL;
	body_length = get_be16(frame + 2U);
	if (body_length != length - EAPOL_HEADER_LENGTH ||
	    body_length < EAPOL_KEY_FIXED_LENGTH ||
	    frame[4] != EAPOL_RSN_KEY_DESCRIPTOR)
		return EINVAL;

	/* Takes the fixed fields. */
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

	/* The key data must fill the rest of the body exactly. */
	key_data_length = get_be16(frame + 97U);
	if (key_data_length != body_length - EAPOL_KEY_FIXED_LENGTH)
		return EINVAL;
	parsed.key_data = frame + 99U;
	parsed.key_data_length = key_data_length;

	/* Applies the per-message constraints. */
	if (!eapol_key_fields_valid(&parsed, 1))
		return EINVAL;

	*result = parsed;

	/* Reports the parsed message. */
	return 0;
}

/*
 * Builds an EAPOL-Key frame of the RSN descriptor type.
 */
int
wlan_wpa2_eapol_key_build(
	uint8_t *output,
	size_t capacity,
	const struct wlan_wpa2_eapol_key *key,
	size_t *result_length)
{
	size_t length;

	/* Rejects a missing output or result, or a message that violates its constraints. */
	if (output == NULL ||
	    result_length == NULL ||
	    !eapol_key_fields_valid(key, 0))
		return EINVAL;
	length = EAPOL_HEADER_LENGTH + EAPOL_KEY_FIXED_LENGTH +
	    key->key_data_length;
	if (capacity < length)
		return ENOSPC;

	/* Writes the header and the fixed fields. */
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

	/* Appends the key data. */
	put_be16(output + 97U, (uint16_t)key->key_data_length);
	if (key->key_data_length != 0U)
		memcpy(output + 99U, key->key_data, key->key_data_length);
	*result_length = length;

	/* Reports the built frame. */
	return 0;
}

/*
 * Parses the decrypted key data of message 3, which must carry the RSN element.
 */
int
wlan_wpa2_m3_plaintext_parse(
	const uint8_t *plaintext,
	size_t length,
	struct wlan_wpa2_gtk *result)
{
	int error;

	error = key_plaintext_parse(plaintext, length, result, 1);

	/* Reports the parse result. */
	return error;
}

/*
 * Parses the decrypted key data of group message 1, which carries no RSN element.
 */
int
wlan_wpa2_group_plaintext_parse(
	const uint8_t *plaintext,
	size_t length,
	struct wlan_wpa2_gtk *result)
{
	int error;

	error = key_plaintext_parse(plaintext, length, result, 0);

	/* Reports the parse result. */
	return error;
}

/*
 * Builds the key data plaintext of message 3: the RSN element and a GTK KDE.
 */
int
wlan_wpa2_m3_plaintext_build(
	uint8_t *output,
	size_t capacity,
	uint8_t key_index,
	const uint8_t gtk[WLAN_WPA2_GTK_LENGTH],
	size_t *result_length)
{
	const size_t length = 48U;

	/* Rejects a missing operand or a key index outside the group slots. */
	if (output == NULL ||
	    gtk == NULL ||
	    result_length == NULL ||
	    key_index == 0U ||
	    key_index > 3U)
		return EINVAL;
	if (capacity < length)
		return ENOSPC;

	/* Writes the selected RSN element and the GTK KDE. */
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

	/* Reports the built plaintext. */
	return 0;
}

/*
 * Builds the key data plaintext of group message 1: a GTK KDE alone.
 */
int
wlan_wpa2_group_plaintext_build(
	uint8_t *output,
	size_t capacity,
	uint8_t key_index,
	const uint8_t gtk[WLAN_WPA2_GTK_LENGTH],
	size_t *result_length)
{
	const size_t length = 32U;

	/* Rejects a missing operand or a key index outside the group slots. */
	if (output == NULL ||
	    gtk == NULL ||
	    result_length == NULL ||
	    key_index == 0U ||
	    key_index > 3U)
		return EINVAL;
	if (capacity < length)
		return ENOSPC;

	/* Writes the GTK KDE followed by the canonical padding. */
	output[0] = IEEE80211_IE_VENDOR;
	output[1] = 22U;
	memcpy(output + 2U, rsn_gtk_kde, sizeof(rsn_gtk_kde));
	output[6] = key_index;
	output[7] = 0U;
	memcpy(output + 8U, gtk, WLAN_WPA2_GTK_LENGTH);
	output[24] = IEEE80211_IE_VENDOR;
	output[25] = 0U;
	memset(output + 26U, 0, length - 26U);
	*result_length = length;

	/* Reports the built plaintext. */
	return 0;
}

/* Reads a little-endian 16-bit field. */
static uint16_t
get_le16(
	const uint8_t *bytes)
{
	return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
}

/* Reads a big-endian 16-bit field. */
static uint16_t
get_be16(
	const uint8_t *bytes)
{
	return ((uint16_t)bytes[0] << 8) | (uint16_t)bytes[1];
}

/* Reads a big-endian 64-bit field. */
static uint64_t
get_be64(
	const uint8_t *bytes)
{
	uint64_t value;
	unsigned index;

	/* Folds the bytes in from the most significant. */
	value = 0U;
	for (index = 0U; index < 8U; index++)
		value = (value << 8) | bytes[index];

	/* Reports the read value. */
	return value;
}

/* Writes a little-endian 16-bit field. */
static void
put_le16(
	uint8_t *bytes,
	uint16_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
}

/* Writes a big-endian 16-bit field. */
static void
put_be16(
	uint8_t *bytes,
	uint16_t value)
{
	bytes[0] = (uint8_t)(value >> 8);
	bytes[1] = (uint8_t)value;
}

/* Writes a big-endian 64-bit field. */
static void
put_be64(
	uint8_t *bytes,
	uint64_t value)
{
	unsigned index;

	/* Stores the bytes from the least significant backwards. */
	for (index = 0U; index < 8U; index++) {
		bytes[7U - index] = (uint8_t)value;
		value >>= 8;
	}
}

/* Tests whether every byte of a field is zero. */
static int
all_zero(
	const uint8_t *bytes,
	size_t length)
{
	uint8_t value;
	size_t index;

	/* Accumulates every set bit. */
	value = 0U;
	for (index = 0U; index < length; index++)
		value |= bytes[index];

	/* Reports an all-zero field when no bit was set. */
	if (value != 0U)
		return 0;
	return 1;
}

/* Tests whether an address is a non-zero individual address. */
static int
valid_unicast_address(
	const uint8_t address[WLAN_WPA2_MAC_LENGTH])
{
	/* The address must exist, not be a group address, and not be zero. */
	if (address == NULL)
		return 0;
	if ((address[0] & 1U) != 0U)
		return 0;
	if (all_zero(address, WLAN_WPA2_MAC_LENGTH))
		return 0;

	/* Reports a usable address. */
	return 1;
}

/* Compares two MAC addresses. */
static int
same_address(
	const uint8_t *left,
	const uint8_t *right)
{
	/* Reports equality of every byte. */
	if (memcmp(left, right, WLAN_WPA2_MAC_LENGTH) != 0)
		return 0;
	return 1;
}

/* Writes a management frame header with three addresses and a sequence number. */
static int
management_header_build(
	uint8_t *output,
	size_t capacity,
	uint16_t frame_control,
	const uint8_t destination[WLAN_WPA2_MAC_LENGTH],
	const uint8_t source[WLAN_WPA2_MAC_LENGTH],
	const uint8_t bssid[WLAN_WPA2_MAC_LENGTH],
	uint16_t sequence_number)
{
	/* Rejects a missing output, an unusable address, or a bad sequence number. */
	if (output == NULL ||
	    !valid_unicast_address(destination) ||
	    !valid_unicast_address(source) ||
	    !valid_unicast_address(bssid) ||
	    sequence_number > 0x0fffU)
		return EINVAL;
	if (capacity < IEEE80211_HEADER_LENGTH)
		return ENOSPC;

	/* Writes the header with a zero duration and fragment number. */
	memset(output, 0, IEEE80211_HEADER_LENGTH);
	put_le16(output, frame_control);
	memcpy(output + 4U, destination, WLAN_WPA2_MAC_LENGTH);
	memcpy(output + 10U, source, WLAN_WPA2_MAC_LENGTH);
	memcpy(output + 16U, bssid, WLAN_WPA2_MAC_LENGTH);
	put_le16(output + 22U, (uint16_t)(sequence_number << 4));

	/* Reports the written header. */
	return 0;
}

/* Tests whether a frame is an unfragmented response of a type from the AP to us. */
static int
management_response_header_valid(
	const uint8_t *frame,
	size_t length,
	uint16_t expected_frame_control,
	const uint8_t station[WLAN_WPA2_MAC_LENGTH],
	const uint8_t bssid[WLAN_WPA2_MAC_LENGTH])
{
	uint16_t frame_control;

	/* Rejects a missing frame, an unusable address, or a short frame. */
	if (frame == NULL ||
	    !valid_unicast_address(station) ||
	    !valid_unicast_address(bssid) ||
	    length < IEEE80211_HEADER_LENGTH)
		return 0;

	/* The type must match apart from the retry bit; no fragments. */
	frame_control = get_le16(frame);
	if ((frame_control & (uint16_t)~IEEE80211_FC_RETRY) !=
	    expected_frame_control || (get_le16(frame + 22U) & 0x000fU) != 0U)
		return 0;

	/* The frame must go from the BSSID to the station. */
	if (!same_address(frame + 4U, station))
		return 0;
	if (!same_address(frame + 10U, bssid))
		return 0;
	if (!same_address(frame + 16U, bssid))
		return 0;

	/* Reports a valid header. */
	return 1;
}

/* Compares a suite selector with an expected one. */
static int
suite_is(
	const uint8_t *suite,
	const uint8_t expected[4])
{
	/* Reports equality of all four bytes. */
	if (memcmp(suite, expected, 4U) != 0)
		return 0;
	return 1;
}

/* Tests whether a rate set is non-empty, bounded, and free of zero rates. */
static int
rates_valid(
	const uint8_t *rates,
	size_t rate_count)
{
	size_t index;

	/* Rejects a missing, empty, or oversized rate set. */
	if (rates == NULL ||
	    rate_count == 0U ||
	    rate_count > WLAN_WPA2_RATE_MAX)
		return 0;

	/* Every rate must be non-zero apart from its basic rate bit. */
	for (index = 0U; index < rate_count; index++) {
		if ((rates[index] & 0x7fU) == 0U)
			return 0;
	}

	/* Reports a usable rate set. */
	return 1;
}

/* Maps a handshake message to its key information, or zero if unknown. */
static uint16_t
message_key_info(
	enum wlan_wpa2_eapol_message message)
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
	case WLAN_WPA2_EAPOL_GROUP_MESSAGE_1:
		return EAPOL_KEY_INFO_GROUP_MESSAGE_1;
	case WLAN_WPA2_EAPOL_GROUP_MESSAGE_2:
		return EAPOL_KEY_INFO_GROUP_MESSAGE_2;
	default:
		return 0U;
	}
}

/* Maps key information to its handshake message, or zero if unknown. */
static enum wlan_wpa2_eapol_message
key_info_message(
	uint16_t key_info)
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
	case EAPOL_KEY_INFO_GROUP_MESSAGE_1:
		return WLAN_WPA2_EAPOL_GROUP_MESSAGE_1;
	case EAPOL_KEY_INFO_GROUP_MESSAGE_2:
		return WLAN_WPA2_EAPOL_GROUP_MESSAGE_2;
	default:
		return 0;
	}
}

/* Checks the fields of an EAPOL-Key message against its message type. */
static int
eapol_key_fields_valid(
	const struct wlan_wpa2_eapol_key *key,
	int parsing)
{
	/* Every message needs a known type, bounded key data, and a zero IV. */
	if (key == NULL ||
	    (key->protocol_version != 1U && key->protocol_version != 2U) ||
	    message_key_info(key->message) == 0U ||
	    key->key_data_length > WLAN_WPA2_EAPOL_KEY_DATA_MAX ||
	    (key->key_data_length != 0U && key->key_data == NULL) ||
	    !all_zero(key->iv, sizeof(key->iv)))
		return 0;

	/* Only the messages that install a group key carry a sequence counter. */
	if (key->message != WLAN_WPA2_EAPOL_MESSAGE_3 &&
	    key->message != WLAN_WPA2_EAPOL_GROUP_MESSAGE_1 &&
	    !all_zero(key->rsc, sizeof(key->rsc)))
		return 0;

	/* Applies the per-message constraints; a parsed message must carry a MIC. */
	switch (key->message) {
	case WLAN_WPA2_EAPOL_MESSAGE_1:
		/* Message 1: the ANonce, no key data, no MIC. */
		if (key->key_length != EAPOL_CCMP_KEY_LENGTH)
			return 0;
		if (key->key_data_length != 0U)
			return 0;
		if (all_zero(key->nonce, sizeof(key->nonce)))
			return 0;
		if (!all_zero(key->mic, sizeof(key->mic)))
			return 0;
		return 1;
	case WLAN_WPA2_EAPOL_MESSAGE_2:
		/* Message 2: the SNonce and exactly the selected RSN element. */
		if (key->key_length != 0U)
			return 0;
		if (key->key_data_length != WLAN_WPA2_RSN_IE_LENGTH)
			return 0;
		if (all_zero(key->nonce, sizeof(key->nonce)))
			return 0;
		if (parsing && all_zero(key->mic, sizeof(key->mic)))
			return 0;
		if (wlan_wpa2_rsn_select_ccmp_psk(key->key_data,
		    key->key_data_length) != 0)
			return 0;
		if (memcmp(key->key_data, selected_rsn_ie,
		    WLAN_WPA2_RSN_IE_LENGTH) != 0)
			return 0;
		return 1;
	case WLAN_WPA2_EAPOL_MESSAGE_3:
		/* Message 3: the ANonce and encrypted key data in whole blocks. */
		if (key->key_length != EAPOL_CCMP_KEY_LENGTH)
			return 0;
		if (key->key_data_length < 24U)
			return 0;
		if ((key->key_data_length & 7U) != 0U)
			return 0;
		if (all_zero(key->nonce, sizeof(key->nonce)))
			return 0;
		if (parsing && all_zero(key->mic, sizeof(key->mic)))
			return 0;
		return 1;
	case WLAN_WPA2_EAPOL_MESSAGE_4:
		/* Message 4: no nonce and no key data. */
		if (key->key_length != 0U)
			return 0;
		if (key->key_data_length != 0U)
			return 0;
		if (!all_zero(key->nonce, sizeof(key->nonce)))
			return 0;
		if (parsing && all_zero(key->mic, sizeof(key->mic)))
			return 0;
		return 1;
	case WLAN_WPA2_EAPOL_GROUP_MESSAGE_1:
		/* Group message 1: no nonce, encrypted key data in whole blocks. */
		if (key->key_length != EAPOL_CCMP_KEY_LENGTH)
			return 0;
		if (key->key_data_length < 24U)
			return 0;
		if ((key->key_data_length & 7U) != 0U)
			return 0;
		if (!all_zero(key->nonce, sizeof(key->nonce)))
			return 0;
		if (parsing && all_zero(key->mic, sizeof(key->mic)))
			return 0;
		return 1;
	case WLAN_WPA2_EAPOL_GROUP_MESSAGE_2:
		/* Group message 2: no nonce and no key data. */
		if (key->key_length != 0U)
			return 0;
		if (key->key_data_length != 0U)
			return 0;
		if (!all_zero(key->nonce, sizeof(key->nonce)))
			return 0;
		if (parsing && all_zero(key->mic, sizeof(key->mic)))
			return 0;
		return 1;
	default:
		return 0;
	}
}

/* Tests whether the rest of a key data buffer is canonical padding. */
static int
padding_valid(
	const uint8_t *bytes,
	size_t length)
{
	size_t offset;

	/* Padding starts with the vendor-specific identifier and a zero length. */
	if (length == 0U || bytes[0] != IEEE80211_IE_VENDOR)
		return 0;
	if (length != 1U && bytes[1] != 0U)
		return 0;

	/* Everything after it must be zero. */
	if (length == 1U)
		offset = 1U;
	else
		offset = 2U;
	for (; offset < length; offset++) {
		if (bytes[offset] != 0U)
			return 0;
	}

	/* Reports valid padding. */
	return 1;
}

/* Parses decrypted key data into the GTK, requiring the RSN element on demand. */
static int
key_plaintext_parse(
	const uint8_t *plaintext,
	size_t length,
	struct wlan_wpa2_gtk *result,
	int require_rsn)
{
	struct wlan_wpa2_gtk parsed;
	const uint8_t *body;
	size_t offset;
	uint8_t identifier;
	uint8_t ie_length;
	uint8_t key_info;
	int have_rsn;
	int have_rsn_extension;
	int have_gtk;

	offset = 0U;
	have_rsn = 0;
	have_rsn_extension = 0;
	have_gtk = 0;

	/* Decrypted key data comes in whole blocks of at least two. */
	if (plaintext == NULL ||
	    result == NULL ||
	    length < 16U ||
	    (length & 7U) != 0U ||
	    length > WLAN_WPA2_EAPOL_KEY_DATA_MAX)
		return EINVAL;
	memset(&parsed, 0, sizeof(parsed));

	/* Walks the elements up to the padding. */
	while (offset < length) {
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
			/* The RSN element must be the selected profile, once. */
			if (have_rsn || wlan_wpa2_rsn_select_ccmp_psk(
			    plaintext + offset, (size_t)ie_length + 2U) != 0)
				return EINVAL;
			have_rsn = 1;
		} else if (identifier == IEEE80211_IE_RSN_EXTENSION) {
			/*
			 * WPA2/WPA3 transition-mode authenticators may carry an
			 * RSNXE in M3.  It is authenticated metadata and cannot
			 * replace the selected RSN element or GTK required by this
			 * WPA2-PSK profile.
			 */
			if (have_rsn_extension || ie_length == 0U)
				return EINVAL;
			have_rsn_extension = 1;
		} else if (identifier == IEEE80211_IE_VENDOR &&
		    ie_length >= 4U &&
		    suite_is(body, rsn_gtk_kde)) {
			/* The GTK KDE carries the key index and the key, once. */
			if (have_gtk || ie_length != 22U)
				return EINVAL;
			key_info = body[4];

			/*
			 * Key IDs 1--3 are group slots.  The Tx/reserved bits and
			 * second reserved octet are not part of this station
			 * profile.
			 */
			if ((key_info & 0xfcU) != 0U ||
			    (key_info & 3U) == 0U ||
			    body[5] != 0U)
				return EINVAL;
			parsed.key_index = key_info & 3U;
			memcpy(parsed.key, body + 6U, sizeof(parsed.key));
			have_gtk = 1;
		} else if (identifier == IEEE80211_IE_VENDOR &&
		    ie_length >= 4U &&
		    memcmp(body, rsn_gtk_kde, 3U) == 0) {
			/*
			 * Authenticated RSN key data is extensible.  A KDE with
			 * the standard RSN OUI but an unimplemented data type
			 * belongs to its defining extension and does not weaken
			 * the selected profile.
			 */
		} else {
			return EINVAL;
		}
		offset += (size_t)ie_length + 2U;
	}

	/* The RSN element is required exactly when asked, and the GTK always. */
	if (have_rsn != require_rsn || !have_gtk)
		return EINVAL;

	*result = parsed;

	/* Reports the parsed key data. */
	return 0;
}
