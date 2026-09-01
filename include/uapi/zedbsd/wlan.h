/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_UAPI_WLAN_H
#define ZEDBSD_UAPI_WLAN_H

#include <stdint.h>
#include <sys/ioctl.h>
#include <zedbsd/netif.h>

#define WLAN_ABI_VERSION 1U

#define WLAN_SSID_MAX 32U
#define WLAN_PASSPHRASE_MIN 8U
#define WLAN_PASSPHRASE_MAX 63U
#define WLAN_PASSPHRASE_STORAGE 64U
#define WLAN_BSS_MAX 64U

#define WLAN_SCAN_START 1U
#define WLAN_SCAN_STOP  2U

#define WLAN_SCAN_IDLE      0U
#define WLAN_SCAN_RUNNING   1U
#define WLAN_SCAN_COMPLETE  2U
#define WLAN_SCAN_CANCELLED 3U
#define WLAN_SCAN_FAILED    4U

#define WLAN_STATE_DOWN           0U
#define WLAN_STATE_IDLE           1U
#define WLAN_STATE_SCANNING       2U
#define WLAN_STATE_AUTHENTICATING 3U
#define WLAN_STATE_ASSOCIATING    4U
#define WLAN_STATE_FOUR_WAY       5U
#define WLAN_STATE_CONNECTED      6U
#define WLAN_STATE_DISCONNECTING  7U
#define WLAN_STATE_FAILED         8U
#define WLAN_STATE_REMOVED        9U

/* Normalized security description.  These are properties, not a bitmask of
 * suites which the station promises to implement. */
#define WLAN_SECURITY_PRIVACY      0x00000001U
#define WLAN_SECURITY_WPA1         0x00000002U
#define WLAN_SECURITY_WPA2         0x00000004U
#define WLAN_SECURITY_TKIP         0x00000008U
#define WLAN_SECURITY_CCMP         0x00000010U
#define WLAN_SECURITY_PSK          0x00000020U
#define WLAN_SECURITY_IEEE8021X    0x00000040U
#define WLAN_SECURITY_SAE          0x00000080U
#define WLAN_SECURITY_PMF_CAPABLE  0x00000100U
#define WLAN_SECURITY_PMF_REQUIRED 0x00000200U
#define WLAN_SECURITY_UNSUPPORTED_SUITE 0x00000400U

struct wlan_ioctl_header {
	char ifr_name[IFNAMSIZ];
	uint32_t version;
	uint32_t size;
};

struct wlan_bss_record {
	uint8_t ssid[WLAN_SSID_MAX];
	uint8_t ssid_length;
	uint8_t bssid[6];
	uint8_t channel;
	int32_t rssi_dbm;
	uint32_t center_frequency_mhz;
	uint32_t beacon_interval_tu;
	uint32_t age_ms;
	uint32_t capability;
	uint32_t security;
	uint32_t reserved[2];
};

struct wlan_scan_request {
	char ifr_name[IFNAMSIZ];
	uint32_t version;
	uint32_t size;
	uint32_t action;
	uint32_t flags;
	uint64_t generation;
	uint32_t state;
	int32_t terminal_error;
	uint32_t reserved[4];
};

struct wlan_scan_status_request {
	char ifr_name[IFNAMSIZ];
	uint32_t version;
	uint32_t size;
	/* generation/result_count/truncated describe one immutable snapshot.
	 * scan_generation/state/deadline describe the current scan operation. */
	uint64_t generation;
	uint64_t scan_generation;
	uint64_t cache_sequence;
	uint64_t deadline_ticks;
	uint32_t state;
	int32_t terminal_error;
	uint32_t result_count;
	uint32_t truncated;
	uint32_t reserved[4];
};

struct wlan_bss_request {
	char ifr_name[IFNAMSIZ];
	uint32_t version;
	uint32_t size;
	uint64_t generation;
	uint32_t index;
	uint32_t reserved0;
	struct wlan_bss_record bss;
	uint32_t reserved[4];
};

struct wlan_connect_request {
	char ifr_name[IFNAMSIZ];
	uint32_t version;
	uint32_t size;
	uint8_t ssid[WLAN_SSID_MAX];
	uint8_t passphrase[WLAN_PASSPHRASE_STORAGE];
	uint32_t ssid_length;
	uint32_t passphrase_length;
	uint64_t generation;
	uint32_t state;
	int32_t terminal_error;
	uint32_t reserved[6];
};

struct wlan_disconnect_request {
	char ifr_name[IFNAMSIZ];
	uint32_t version;
	uint32_t size;
	uint64_t generation;
	uint32_t flags;
	uint32_t state;
	int32_t terminal_error;
	uint32_t reserved[5];
};

struct wlan_status_request {
	char ifr_name[IFNAMSIZ];
	uint32_t version;
	uint32_t size;
	uint64_t operation_generation;
	uint64_t scan_generation;
	uint64_t snapshot_generation;
	uint64_t deadline_ticks;
	uint64_t cache_sequence;
	uint32_t state;
	uint32_t scan_state;
	uint32_t administrative_up;
	uint32_t authenticated;
	uint32_t associated;
	uint32_t key_installed;
	uint32_t controlled_port;
	uint32_t retry_count;
	int32_t terminal_error;
	int32_t rssi_dbm;
	uint8_t bssid[6];
	uint8_t channel;
	uint8_t reserved0;
	uint32_t center_frequency_mhz;
	uint32_t security;
	uint32_t reserved[4];
};

#define ZEDBSD_WLAN_IOCTL_GROUP 'W'
#define SIOCSWLANSCAN \
	_IOWR(ZEDBSD_WLAN_IOCTL_GROUP, 1, struct wlan_scan_request)
#define SIOCGWLANSCAN \
	_IOWR(ZEDBSD_WLAN_IOCTL_GROUP, 2, struct wlan_scan_status_request)
#define SIOCGWLANBSS \
	_IOWR(ZEDBSD_WLAN_IOCTL_GROUP, 3, struct wlan_bss_request)
#define SIOCSWLANCONNECT \
	_IOWR(ZEDBSD_WLAN_IOCTL_GROUP, 4, struct wlan_connect_request)
#define SIOCSWLANDISCONNECT \
	_IOWR(ZEDBSD_WLAN_IOCTL_GROUP, 5, struct wlan_disconnect_request)
#define SIOCGWLANSTATUS \
	_IOWR(ZEDBSD_WLAN_IOCTL_GROUP, 6, struct wlan_status_request)

#endif
