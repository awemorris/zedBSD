/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * WLAN beacon and probe response parsing.
 *
 * A management frame becomes a BSS record: BSSID, signal, capability,
 * SSID, channel, and a security summary derived from the RSN and WPA
 * information elements.  The summary marks cipher suites the station
 * profile cannot use, so that the caller can refuse them early.
 */

#include "kern/net/wlan.h"

#include <errno.h>
#include <string.h>

#define WLAN_FRAME_FIXED_LENGTH 36U
#define WLAN_IE_SSID 0U
#define WLAN_IE_DS_PARAMETER 3U
#define WLAN_IE_RSN 48U
#define WLAN_IE_VENDOR 221U

#define WLAN_CAPABILITY_PRIVACY 0x0010U

enum cipher_kind {
	CIPHER_UNSUPPORTED = 0,
	CIPHER_TKIP,
	CIPHER_CCMP
};

static uint16_t read_le16(const uint8_t *bytes);
static int suite_oui_is(const uint8_t *suite, uint8_t a, uint8_t b, uint8_t c);
static enum cipher_kind parse_cipher(const uint8_t *suite, int rsn);
static void parse_akm(const uint8_t *suite, int rsn, uint32_t *security);
static int parse_security_body(const uint8_t *body, size_t length, int rsn, uint32_t *security);
static uint32_t channel_frequency(uint8_t channel);

/*
 * Parses a beacon or probe response into a BSS record.
 *
 * The frame must carry an SSID; the channel comes from the DS parameter
 * element or, failing that, from the channel the frame was received on.
 * Duplicate elements and a channel without a known frequency are errors.
 */
int
wlan_frame_parse_bss(
	const uint8_t *frame,
	size_t length,
	int32_t rssi_dbm,
	uint8_t channel_hint,
	struct wlan_bss_record *result)
{
	struct wlan_bss_record parsed;
	const uint8_t *body;
	size_t offset;
	unsigned bssid_nonzero;
	unsigned bssid_index;
	uint8_t identifier;
	uint8_t ie_length;
	int have_ssid;
	int have_channel;
	int have_rsn;
	int have_wpa;
	int error;

	have_ssid = 0;
	have_channel = 0;
	have_rsn = 0;
	have_wpa = 0;
	bssid_nonzero = 0U;

	/* Rejects a missing operand or a frame shorter than the fixed part. */
	if (frame == NULL || result == NULL || length < WLAN_FRAME_FIXED_LENGTH)
		return EINVAL;

	/* Accepts only an unfragmented beacon or probe response. */
	if ((frame[0] & 0xfcU) != 0x80U && (frame[0] & 0xfcU) != 0x50U)
		return EINVAL;
	if ((frame[0] & 0x03U) != 0U)
		return EINVAL;

	/* The BSSID must be a non-zero individual address. */
	if ((frame[16] & 0x01U) != 0U)
		return EINVAL;
	for (bssid_index = 0U; bssid_index < 6U; bssid_index++)
		bssid_nonzero |= frame[16U + bssid_index];
	if (bssid_nonzero == 0U)
		return EINVAL;

	/* Takes the fixed fields. */
	memset(&parsed, 0, sizeof(parsed));
	memcpy(parsed.bssid, frame + 16U, sizeof(parsed.bssid));
	parsed.rssi_dbm = rssi_dbm;
	parsed.beacon_interval_tu = read_le16(frame + 32U);
	parsed.capability = read_le16(frame + 34U);
	if ((parsed.capability & WLAN_CAPABILITY_PRIVACY) != 0U)
		parsed.security |= WLAN_SECURITY_PRIVACY;

	/* Walks the information elements. */
	offset = WLAN_FRAME_FIXED_LENGTH;
	while (offset < length) {
		if (length - offset < 2U)
			return EINVAL;
		identifier = frame[offset];
		ie_length = frame[offset + 1U];
		offset += 2U;
		if ((size_t)ie_length > length - offset)
			return EINVAL;
		body = frame + offset;

		/* Takes the SSID, channel, RSN, and WPA elements once each. */
		if (identifier == WLAN_IE_SSID) {
			if (have_ssid || ie_length > WLAN_SSID_MAX)
				return EINVAL;
			have_ssid = 1;
			parsed.ssid_length = ie_length;
			memcpy(parsed.ssid, body, ie_length);
		} else if (identifier == WLAN_IE_DS_PARAMETER) {
			if (have_channel || ie_length != 1U)
				return EINVAL;
			have_channel = 1;
			parsed.channel = body[0];
		} else if (identifier == WLAN_IE_RSN) {
			if (have_rsn)
				return EINVAL;
			have_rsn = 1;
			parsed.security |= WLAN_SECURITY_WPA2;
			error = parse_security_body(body, ie_length, 1,
			    &parsed.security);
			if (error != 0)
				return error;
		} else if (identifier == WLAN_IE_VENDOR &&
		    ie_length >= 4U &&
		    body[0] == 0x00U &&
		    body[1] == 0x50U &&
		    body[2] == 0xf2U &&
		    body[3] == 0x01U) {
			if (have_wpa)
				return EINVAL;
			have_wpa = 1;
			parsed.security |= WLAN_SECURITY_WPA1;
			error = parse_security_body(body + 4U,
			    (size_t)ie_length - 4U, 0, &parsed.security);
			if (error != 0)
				return error;
		}
		offset += ie_length;
	}

	/* Requires an SSID and a channel with a known frequency. */
	if (!have_ssid)
		return EINVAL;
	if (!have_channel)
		parsed.channel = channel_hint;
	parsed.center_frequency_mhz = channel_frequency(parsed.channel);
	if (parsed.center_frequency_mhz == 0U)
		return EINVAL;

	*result = parsed;

	/* Reports the parsed record. */
	return 0;
}

/* Reads a little-endian 16-bit field. */
static uint16_t
read_le16(
	const uint8_t *bytes)
{
	return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
}

/* Tests whether a suite selector carries an OUI. */
static int
suite_oui_is(
	const uint8_t *suite,
	uint8_t a,
	uint8_t b,
	uint8_t c)
{
	/* All three OUI bytes must match. */
	if (suite[0] != a)
		return 0;
	if (suite[1] != b)
		return 0;
	if (suite[2] != c)
		return 0;

	/* Reports a matching OUI. */
	return 1;
}

/* Classifies a cipher suite selector of an RSN or WPA element. */
static enum cipher_kind
parse_cipher(
	const uint8_t *suite,
	int rsn)
{
	uint8_t oui_b;
	uint8_t oui_c;

	/* RSN uses the IEEE OUI, WPA the Microsoft one. */
	if (rsn) {
		oui_b = 0x0fU;
		oui_c = 0xacU;
	} else {
		oui_b = 0x50U;
		oui_c = 0xf2U;
	}
	if (!suite_oui_is(suite, 0x00U, oui_b, oui_c))
		return CIPHER_UNSUPPORTED;

	/* Recognizes TKIP and CCMP. */
	if (suite[3] == 2U)
		return CIPHER_TKIP;
	if (suite[3] == 4U)
		return CIPHER_CCMP;

	/* Reports any other cipher as unsupported. */
	return CIPHER_UNSUPPORTED;
}

/* Records the key management a suite selector names. */
static void
parse_akm(
	const uint8_t *suite,
	int rsn,
	uint32_t *security)
{
	uint8_t oui_b;
	uint8_t oui_c;

	/* RSN uses the IEEE OUI, WPA the Microsoft one. */
	if (rsn) {
		oui_b = 0x0fU;
		oui_c = 0xacU;
	} else {
		oui_b = 0x50U;
		oui_c = 0xf2U;
	}
	if (!suite_oui_is(suite, 0x00U, oui_b, oui_c))
		return;

	/* Recognizes 802.1X, PSK, and for RSN also SAE. */
	if (suite[3] == 1U)
		*security |= WLAN_SECURITY_IEEE8021X;
	else if (suite[3] == 2U)
		*security |= WLAN_SECURITY_PSK;
	else if (rsn && suite[3] == 8U)
		*security |= WLAN_SECURITY_SAE;
}

/* Summarizes the ciphers, key management, and capabilities of an element. */
static int
parse_security_body(
	const uint8_t *body,
	size_t length,
	int rsn,
	uint32_t *security)
{
	enum cipher_kind cipher;
	size_t offset;
	uint16_t count;
	uint16_t capabilities;
	unsigned index;
	int pairwise_supported;

	offset = 0;
	capabilities = 0;
	pairwise_supported = 0;

	/* Accepts only version 1. */
	if (length < 2U || read_le16(body) != 1U)
		return EINVAL;
	offset = 2U;

	/* Classifies the group cipher. */
	if (length - offset < 4U)
		return EINVAL;
	cipher = parse_cipher(body + offset, rsn);
	if (cipher == CIPHER_TKIP)
		*security |= WLAN_SECURITY_TKIP;

	/*
	 * The frozen station profile requires CCMP as the group cipher.  An
	 * otherwise valid CCMP pairwise choice cannot repair this field.
	 */
	if (cipher != CIPHER_CCMP)
		*security |= WLAN_SECURITY_UNSUPPORTED_SUITE;
	offset += 4U;

	/* Classifies every pairwise cipher. */
	if (length - offset < 2U)
		return EINVAL;
	count = read_le16(body + offset);
	offset += 2U;
	if (count == 0U || (size_t)count > (length - offset) / 4U)
		return EINVAL;
	for (index = 0; index < count; index++) {
		cipher = parse_cipher(body + offset, rsn);
		if (cipher == CIPHER_TKIP) {
			*security |= WLAN_SECURITY_TKIP;
		} else if (cipher == CIPHER_CCMP) {
			*security |= WLAN_SECURITY_CCMP;
			pairwise_supported = 1;
		}
		offset += 4U;
	}

	/*
	 * Additional pairwise suites are advertisements, not mandatory
	 * choices.  Preserve known properties above, but reject only when no
	 * CCMP choice exists.  This lets the association codec select a
	 * CCMP-only RSN IE.
	 */
	if (!pairwise_supported)
		*security |= WLAN_SECURITY_UNSUPPORTED_SUITE;

	/* Records every key management suite. */
	if (length - offset < 2U)
		return EINVAL;
	count = read_le16(body + offset);
	offset += 2U;
	if (count == 0U || (size_t)count > (length - offset) / 4U)
		return EINVAL;
	for (index = 0; index < count; index++) {
		parse_akm(body + offset, rsn, security);
		offset += 4U;
	}

	/* The capabilities, PMKID list, and group management cipher are optional. */
	if (offset == length)
		return 0;
	if (length - offset < 2U)
		return EINVAL;
	capabilities = read_le16(body + offset);
	offset += 2U;
	if (rsn && (capabilities & 0x0080U) != 0U)
		*security |= WLAN_SECURITY_PMF_CAPABLE;
	if (rsn && (capabilities & 0x0040U) != 0U)
		*security |= WLAN_SECURITY_PMF_REQUIRED;
	if (offset == length)
		return 0;
	if (!rsn || length - offset < 2U)
		return EINVAL;
	count = read_le16(body + offset);
	offset += 2U;
	if ((size_t)count > (length - offset) / 16U)
		return EINVAL;
	offset += (size_t)count * 16U;
	if (offset == length)
		return 0;
	if (length - offset != 4U)
		return EINVAL;

	/* Reports a well-formed element. */
	return 0;
}

/* Converts a channel number to its center frequency, or zero if unknown. */
static uint32_t
channel_frequency(
	uint8_t channel)
{
	/* The 2.4 GHz band has channels 1-13 spaced 5 MHz and channel 14. */
	if (channel >= 1U && channel <= 13U)
		return 2407U + 5U * (uint32_t)channel;
	if (channel == 14U)
		return 2484U;

	/* The 5 GHz band has channels 36-144 and 149-181 spaced 20 MHz. */
	if ((channel >= 36U && channel <= 144U &&
	    (channel - 36U) % 4U == 0U) ||
	    (channel >= 149U && channel <= 181U &&
	    (channel - 149U) % 4U == 0U))
		return 5000U + 5U * (uint32_t)channel;

	/* Reports an unknown channel. */
	return 0U;
}
