/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <stddef.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <zedbsd/wlan.h>

#define IOC_SIZE(command) (((command) >> 16) & 0x1fffUL)
#define IOC_GROUP(command) (((command) >> 8) & 0xffUL)
#define IOC_NUMBER(command) ((command) & 0xffUL)

_Static_assert(sizeof(struct wlan_ioctl_header) == 24U,
    "WLAN ioctl prefix layout");
_Static_assert(offsetof(struct wlan_ioctl_header, version) == 16U,
    "WLAN ioctl version offset");
_Static_assert(offsetof(struct wlan_ioctl_header, size) == 20U,
    "WLAN ioctl size offset");

_Static_assert(sizeof(struct wlan_bss_record) == 72U,
    "WLAN BSS layout");
_Static_assert(offsetof(struct wlan_bss_record, ssid_length) == 32U,
    "WLAN BSS SSID length offset");
_Static_assert(offsetof(struct wlan_bss_record, bssid) == 33U,
    "WLAN BSS BSSID offset");
_Static_assert(offsetof(struct wlan_bss_record, rssi_dbm) == 40U,
    "WLAN BSS RSSI offset");
_Static_assert(offsetof(struct wlan_bss_record, security) == 60U,
    "WLAN BSS security offset");

_Static_assert(sizeof(struct wlan_scan_request) == 64U,
    "WLAN scan-control layout");
_Static_assert(offsetof(struct wlan_scan_request, generation) == 32U,
    "WLAN scan-control generation offset");
_Static_assert(sizeof(struct wlan_scan_status_request) == 88U,
    "WLAN scan-status layout");
_Static_assert(offsetof(struct wlan_scan_status_request,
    scan_generation) == 32U, "WLAN active scan generation offset");
_Static_assert(sizeof(struct wlan_bss_request) == 128U,
    "WLAN BSS request layout");
_Static_assert(offsetof(struct wlan_bss_request, bss) == 40U,
    "WLAN BSS result offset");
_Static_assert(sizeof(struct wlan_connect_request) == 168U,
    "WLAN connect layout");
_Static_assert(offsetof(struct wlan_connect_request, passphrase) == 56U,
    "WLAN passphrase offset");
_Static_assert(offsetof(struct wlan_connect_request, generation) == 128U,
    "WLAN connect generation offset");
_Static_assert(sizeof(struct wlan_disconnect_request) == 64U,
    "WLAN disconnect layout");
_Static_assert(sizeof(struct wlan_status_request) == 136U,
    "WLAN status layout");
_Static_assert(offsetof(struct wlan_status_request, bssid) == 104U,
    "WLAN status BSSID offset");

#define ASSERT_IOCTL(command, number, type) \
	_Static_assert(IOC_SIZE(command) == sizeof(type), "encoded size"); \
	_Static_assert(IOC_SIZE(command) != 0U, "nonzero encoded size"); \
	_Static_assert(IOC_GROUP(command) == (unsigned long)'W', "ioctl group"); \
	_Static_assert(IOC_NUMBER(command) == (number), "ioctl number"); \
	_Static_assert(((command) & ZEDBSD_IOC_INOUT) == ZEDBSD_IOC_INOUT, \
	    "ioctl direction")

ASSERT_IOCTL(SIOCSWLANSCAN, 1U, struct wlan_scan_request);
ASSERT_IOCTL(SIOCGWLANSCAN, 2U, struct wlan_scan_status_request);
ASSERT_IOCTL(SIOCGWLANBSS, 3U, struct wlan_bss_request);
ASSERT_IOCTL(SIOCSWLANCONNECT, 4U, struct wlan_connect_request);
ASSERT_IOCTL(SIOCSWLANDISCONNECT, 5U, struct wlan_disconnect_request);
ASSERT_IOCTL(SIOCGWLANSTATUS, 6U, struct wlan_status_request);

int
main(void)
{
	return 0;
}
