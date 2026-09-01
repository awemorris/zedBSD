/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_KERN_NET_WLAN_WPA2_CODEC_H
#define ZEDBSD_KERN_NET_WLAN_WPA2_CODEC_H

#include <stddef.h>
#include <stdint.h>

#define WLAN_WPA2_MAC_LENGTH 6U
#define WLAN_WPA2_NONCE_LENGTH 32U
#define WLAN_WPA2_KEY_IV_LENGTH 16U
#define WLAN_WPA2_KEY_RSC_LENGTH 8U
#define WLAN_WPA2_KEY_MIC_LENGTH 16U
#define WLAN_WPA2_GTK_LENGTH 16U
#define WLAN_WPA2_RSN_IE_LENGTH 22U
#define WLAN_WPA2_RATE_MAX 12U
#define WLAN_WPA2_EAPOL_KEY_DATA_MAX 512U

enum wlan_wpa2_eapol_message {
	WLAN_WPA2_EAPOL_MESSAGE_1 = 1,
	WLAN_WPA2_EAPOL_MESSAGE_2 = 2,
	WLAN_WPA2_EAPOL_MESSAGE_3 = 3,
	WLAN_WPA2_EAPOL_MESSAGE_4 = 4
};

/* key_data points into the caller-owned parsed frame, or into caller-owned
 * build input.  The codec never retains it. */
struct wlan_wpa2_eapol_key {
	enum wlan_wpa2_eapol_message message;
	uint8_t protocol_version;
	uint16_t key_length;
	uint64_t replay_counter;
	uint8_t nonce[WLAN_WPA2_NONCE_LENGTH];
	uint8_t iv[WLAN_WPA2_KEY_IV_LENGTH];
	uint8_t rsc[WLAN_WPA2_KEY_RSC_LENGTH];
	uint8_t mic[WLAN_WPA2_KEY_MIC_LENGTH];
	const uint8_t *key_data;
	size_t key_data_length;
};

struct wlan_wpa2_assoc_response {
	uint16_t capability;
	uint16_t status;
	uint16_t aid;
};

struct wlan_wpa2_gtk {
	uint8_t key_index;
	uint8_t key[WLAN_WPA2_GTK_LENGTH];
};

/* The RSN helpers consume and produce a complete element, including ID and
 * length.  Selection accepts extra pairwise and AKM suites when a complete
 * CCMP+PSK choice remains, while the builder emits only that selected subset.
 * SAE-only and every unsupported mandatory capability remain rejected. */
int wlan_wpa2_rsn_select_ccmp_psk(const uint8_t *element, size_t length);
int wlan_wpa2_rsn_build_ccmp_psk(uint8_t *output, size_t capacity,
	size_t *result_length);

int wlan_wpa2_auth_request_build(uint8_t *output, size_t capacity,
	const uint8_t station[WLAN_WPA2_MAC_LENGTH],
	const uint8_t bssid[WLAN_WPA2_MAC_LENGTH], uint16_t sequence_number,
	size_t *result_length);
int wlan_wpa2_auth_response_parse(const uint8_t *frame, size_t length,
	const uint8_t station[WLAN_WPA2_MAC_LENGTH],
	const uint8_t bssid[WLAN_WPA2_MAC_LENGTH], uint16_t *status);

int wlan_wpa2_assoc_request_build(uint8_t *output, size_t capacity,
	const uint8_t station[WLAN_WPA2_MAC_LENGTH],
	const uint8_t bssid[WLAN_WPA2_MAC_LENGTH], uint16_t sequence_number,
	uint16_t capability, uint16_t listen_interval, const uint8_t *ssid,
	size_t ssid_length, const uint8_t *rates, size_t rate_count,
	size_t *result_length);
int wlan_wpa2_assoc_response_parse(const uint8_t *frame, size_t length,
	const uint8_t station[WLAN_WPA2_MAC_LENGTH],
	const uint8_t bssid[WLAN_WPA2_MAC_LENGTH],
	struct wlan_wpa2_assoc_response *result);

/* EAPOL helpers consume and produce an EAPOL payload beginning with the
 * 802.1X protocol-version octet (the Ethernet header/EtherType is external).
 * Parse accepts only the four exact WPA2-PSK/CCMP key-info profiles. */
int wlan_wpa2_eapol_key_parse(const uint8_t *frame, size_t length,
	struct wlan_wpa2_eapol_key *result);
int wlan_wpa2_eapol_key_build(uint8_t *output, size_t capacity,
	const struct wlan_wpa2_eapol_key *key, size_t *result_length);

/* Message-3 key data is passed here only after authenticated RFC 3394
 * unwrapping.  The first profile accepts exactly one compatible RSN element,
 * one CCMP GTK KDE, and canonical AES-wrap padding. */
int wlan_wpa2_m3_plaintext_parse(const uint8_t *plaintext, size_t length,
	struct wlan_wpa2_gtk *result);
int wlan_wpa2_m3_plaintext_build(uint8_t *output, size_t capacity,
	uint8_t key_index, const uint8_t gtk[WLAN_WPA2_GTK_LENGTH],
	size_t *result_length);

#endif
