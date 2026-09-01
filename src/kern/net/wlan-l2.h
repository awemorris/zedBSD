/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#ifndef ZEDBSD_KERN_NET_WLAN_L2_H
#define ZEDBSD_KERN_NET_WLAN_L2_H

#include <stddef.h>
#include <stdint.h>

#define WLAN_L2_ETHERNET_HEADER_SIZE 14U
#define WLAN_L2_DATA_HEADER_SIZE     24U
#define WLAN_L2_CCMP_HEADER_SIZE      8U
#define WLAN_L2_CCMP_MIC_SIZE         8U
#define WLAN_L2_LLC_SNAP_SIZE         8U
#define WLAN_L2_ETHERNET_MAX       1514U
#define WLAN_L2_MPDU_MAX \
	(WLAN_L2_DATA_HEADER_SIZE + WLAN_L2_CCMP_HEADER_SIZE + \
	WLAN_L2_LLC_SNAP_SIZE + WLAN_L2_ETHERNET_MAX - \
	WLAN_L2_ETHERNET_HEADER_SIZE)

struct wlan_l2_rx_security {
	uint64_t key_generation;
	uint64_t packet_number;
	uint8_t decrypted;
	uint8_t cipher_ccmp;
	uint8_t key_index;
	uint8_t reserved[5];
};

struct wlan_l2_rx_state {
	uint64_t key_generation;
	uint64_t pairwise_packet_number;
	uint64_t group_packet_number[4];
};

int wlan_l2_build_data(const uint8_t station[6], const uint8_t bssid[6],
	const uint8_t *ethernet, size_t ethernet_length, int protected_frame,
	uint8_t key_index, uint64_t packet_number, uint8_t *mpdu,
	size_t capacity, size_t *mpdu_length);
int wlan_l2_parse_data(const uint8_t station[6], const uint8_t bssid[6],
	const uint8_t *mpdu, size_t mpdu_length,
	const struct wlan_l2_rx_security *security,
	struct wlan_l2_rx_state *state, uint8_t *ethernet, size_t capacity,
	size_t *ethernet_length);

#endif
