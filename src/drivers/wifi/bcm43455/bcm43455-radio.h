/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * BCM43455: the messages of the radio operations.
 *
 * A scan, a join and the firmware supplicant's key are firmware commands
 * and variables with binary payloads; scan results come back as event
 * data holding the firmware's description of each network, and the
 * connection's progress as events.  The functions here only encode,
 * decode and classify, so a host test reaches every rule without the
 * chip.
 */

#ifndef KERN_DRIVERS_BCM43455_RADIO_H
#define KERN_DRIVERS_BCM43455_RADIO_H

#include <stddef.h>
#include <stdint.h>

#include "drivers/wifi/bcm43455/bcm43455-event.h"

/*
 * How the firmware numbers channel specifications: the 802.11n layout, or the
 * 802.11ac one.
 *
 * Values referenced: OpenBSD sys/dev/ic/bwfmreg.h revision 1.26 (line 247);
 * OpenBSD sys/dev/ic/bwfm.c revision 1.113 (lines 256-260).
 */
#define BCM43455_CHANSPEC_LAYOUT_N		1U
#define BCM43455_CHANSPEC_LAYOUT_AC		2U

/*
 * The firmware commands of a connection.
 *
 * Values referenced: Infineon wifi-host-driver bd99da7b
 * WHD/COMPONENT_WIFI6/src/include/whd_wlioctl.h (lines 1304-1544); OpenBSD
 * sys/dev/ic/bwfmreg.h revision 1.26 (lines 247-312).
 */
#define BCM43455_COMMAND_IO_VERSION		1U
#define BCM43455_COMMAND_INFRASTRUCTURE		20U
#define BCM43455_COMMAND_AUTHENTICATION		22U
#define BCM43455_COMMAND_JOIN			26U
#define BCM43455_COMMAND_LEAVE			52U
#define BCM43455_COMMAND_CIPHERS		134U
#define BCM43455_COMMAND_KEY_MANAGEMENT		165U
#define BCM43455_COMMAND_SUPPLICANT_KEY		268U

/*
 * The values a WPA2-PSK join with AES sets: open authentication, the AES
 * cipher, WPA2 with a pre-shared key.
 *
 * Values referenced: Infineon wifi-host-driver bd99da7b
 * WHD/COMPONENT_WIFI6/src/include/whd_wlioctl.h (lines 831 and 4273) and
 * WHD/COMPONENT_WIFI6/inc/whd_types.h (line 65); OpenBSD sys/dev/ic/bwfmreg.h
 * revision 1.26 (lines 167-191).
 */
#define BCM43455_AUTHENTICATION_OPEN	0U
#define BCM43455_CIPHER_AES		0x04U
#define BCM43455_KEY_MANAGEMENT_WPA2_PSK	0x80U

/*
 * The escan actions.
 *
 * Values referenced: Infineon wifi-host-driver bd99da7b
 * WHD/COMPONENT_WIFI6/src/include/whd_wlioctl.h (lines 496-554); OpenBSD
 * sys/dev/ic/bwfmreg.h revision 1.26 (lines 509-575).
 */
#define BCM43455_ESCAN_START		1U
#define BCM43455_ESCAN_ABORT		3U

/*
 * The two layouts of the scan parameters ("scan_ver" 0 or absent, and 2).
 *
 * Values referenced: OpenBSD sys/dev/ic/bwfm.c revision 1.113 (lines 277-278).
 */
#define BCM43455_SCAN_LAYOUT_V0		0U
#define BCM43455_SCAN_LAYOUT_V2		2U

/* The most bytes an escan request takes: the larger layout and one channel. */
#define BCM43455_ESCAN_REQUEST_MAX	84U

/* The bytes of a join request: the SSID, the BSSID and one channel. */
#define BCM43455_JOIN_REQUEST_LENGTH	52U

/*
 * The bytes of the supplicant's key message: its length, its flags and the key
 * as hex text.
 *
 * Values referenced: Infineon wifi-host-driver bd99da7b
 * WHD/COMPONENT_WIFI6/src/include/whd_wlioctl.h (lines 781-785).
 */
#define BCM43455_KEY_MESSAGE_LENGTH	70U

/* The PMK the core derives. */
#define BCM43455_PMK_LENGTH		32U

/*
 * The fixed part of the firmware's description of one network.
 *
 * Values referenced: OpenBSD sys/dev/ic/bwfmreg.h revision 1.26 (lines
 * 344-375).
 */
#define BCM43455_BSS_INFO_FIXED		128U

/* The longest probe response a scan result is turned into. */
#define BCM43455_SCAN_FRAME_MAX		1400U

/*
 * What an escan result event says about the scan it belongs to.
 *
 * RESULTS carries networks and more follow, COMPLETE ends the scan,
 * ABORTED is the end of a scan the host stopped, BUSY means another scan
 * or a join took the radio, FAILED any other end.
 */
enum bcm43455_scan_status {
	BCM43455_SCAN_STATUS_RESULTS = 1,
	BCM43455_SCAN_STATUS_COMPLETE,
	BCM43455_SCAN_STATUS_ABORTED,
	BCM43455_SCAN_STATUS_BUSY,
	BCM43455_SCAN_STATUS_FAILED
};

/*
 * What one event means to the connection being made.
 *
 * NONE changes nothing; ASSOCIATED says the firmware joined the access
 * point; AUTHORIZED says its supplicant finished the handshakes; LOST
 * ends the connection, with error saying why as the WLAN core expects it.
 */
enum bcm43455_connect_step {
	BCM43455_CONNECT_NONE = 0,
	BCM43455_CONNECT_ASSOCIATED,
	BCM43455_CONNECT_AUTHORIZED,
	BCM43455_CONNECT_LOST
};

/*
 * One event's meaning to the connection.
 */
struct bcm43455_connect_outcome {
	enum bcm43455_connect_step step;
	int error;
};

/*
 * Makes the channel specification of a 20 MHz channel.
 *
 * Returns 0 for a channel number the firmware's bands do not have.
 */
uint16_t bcm43455_chanspec_from_channel(unsigned io_type, uint32_t channel);

/*
 * Reads the primary (control) channel of a channel specification.
 *
 * A 40 or 80 MHz specification names its centre channel and which 20 MHz
 * part is the primary one.  Returns EINVAL for a width or a part this
 * driver does not know.
 */
int bcm43455_chanspec_control_channel(unsigned io_type, uint16_t chanspec, uint32_t *channel);

/*
 * Writes an escan request for one channel, or an abort.
 *
 * layout is BCM43455_SCAN_LAYOUT_V0 or _V2, action BCM43455_ESCAN_START
 * or _ABORT; a start scans chanspec, actively unless passive is set, for
 * any network.  length receives the request's length.  Returns EINVAL for
 * an unknown layout or action and ENOSPC when it does not fit capacity.
 */
int bcm43455_escan_encode(uint8_t *buffer, size_t capacity, unsigned layout, unsigned action, uint16_t sync_id, uint16_t chanspec, int passive, size_t *length);

/*
 * Decodes the header of an escan result event's data.
 *
 * sync_id names the scan, count the networks described, first where the
 * first description starts.  Returns EPROTO for data too short.
 */
int bcm43455_escan_result_decode(const uint8_t *data, size_t length, uint16_t *sync_id, unsigned *count, size_t *first);

/*
 * Measures the network description at offset of an escan result's data.
 *
 * Returns EPROTO when it is shorter than its fixed part or runs past the
 * data.
 */
int bcm43455_bss_length(const uint8_t *data, size_t length, size_t offset, size_t *bss_length);

/*
 * Turns one network description into the probe response the WLAN core's
 * parser reads.
 *
 * The frame carries the BSSID, beacon interval and capabilities, an SSID
 * and a DS parameter element made from the description, then the
 * firmware's own elements but its SSID and DS parameter elements.
 * channel receives the primary channel, rssi the signal in dBm.  Returns
 * EPROTO for a malformed description and EMSGSIZE for one too long for
 * capacity.
 */
int bcm43455_bss_to_frame(const uint8_t *bss, size_t length, unsigned io_type, uint8_t *frame, size_t capacity, size_t *frame_length, uint32_t *channel, int32_t *rssi);

/*
 * Writes a join request: an SSID, the BSSID to join and its channel.
 *
 * Returns EINVAL for an SSID longer than 32 bytes and ENOSPC when it does
 * not fit capacity.
 */
int bcm43455_join_encode(uint8_t *buffer, size_t capacity, const uint8_t *ssid, size_t ssid_length, const uint8_t bssid[6], uint16_t chanspec, size_t *length);

/*
 * Writes the supplicant's key message for a PMK: 64 hex digits marked as
 * text, as the firmware takes a PMK.
 */
void bcm43455_key_encode(uint8_t message[BCM43455_KEY_MESSAGE_LENGTH], const uint8_t pmk[BCM43455_PMK_LENGTH]);

/*
 * Reads what an escan result event's status says about its scan.
 */
enum bcm43455_scan_status bcm43455_scan_classify(uint32_t status);

/*
 * Reads what an event means to the connection being made.
 *
 * associated and authorized say how far the connection has come; the
 * table of failures and the errors they become is the one networkd
 * relies on: EACCES only with evidence that the key was wrong.
 */
void bcm43455_connect_classify(const struct bcm43455_event *event, unsigned associated, unsigned authorized, struct bcm43455_connect_outcome *outcome);

#endif
