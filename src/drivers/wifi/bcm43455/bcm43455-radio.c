/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * BCM43455: encoding the radio operations' messages and reading their
 * results.
 */

#include "drivers/wifi/bcm43455/bcm43455-radio.h"
#include "drivers/wifi/bcm43455/bcm43455-bytes.h"
#include <kern/kcrt.h>
#include <uapi/errno.h>

/*
 * The channel specification's channel number.
 *
 * Values referenced: OpenBSD sys/dev/ic/bwfmreg.h revision 1.26 (lines
 * 194-234).
 */
#define CHANSPEC_CHANNEL_MASK		0x00ffU

/*
 * The 802.11n layout: the primary part, the width and the band.
 *
 * Values referenced: OpenBSD sys/dev/ic/bwfmreg.h revision 1.26 (lines
 * 194-234).
 */
#define D11N_PART_MASK			0x0300U
#define D11N_PART_LOWER			0x0100U
#define D11N_PART_UPPER			0x0200U
#define D11N_PART_NONE			0x0300U
#define D11N_WIDTH_MASK			0x0c00U
#define D11N_WIDTH_20			0x0800U
#define D11N_WIDTH_40			0x0c00U
#define D11N_BAND_5G			0x1000U
#define D11N_BAND_2G			0x2000U

/*
 * The 802.11ac layout: the primary part (three bits), the width and the band.
 *
 * Values referenced: OpenBSD sys/dev/ic/bwfmreg.h revision 1.26 (lines
 * 194-234).
 */
#define D11AC_PART_MASK			0x0700U
#define D11AC_PART_SHIFT		8U
#define D11AC_WIDTH_MASK		0x3800U
#define D11AC_WIDTH_20			0x1000U
#define D11AC_WIDTH_40			0x1800U
#define D11AC_WIDTH_80			0x2000U
#define D11AC_BAND_2G			0x0000U
#define D11AC_BAND_5G			0xc000U

/*
 * The escan request: its version, the action and the scan's id.
 *
 * Values referenced: OpenBSD sys/dev/ic/bwfmreg.h revision 1.26 (lines
 * 509-575); Infineon wifi-host-driver bd99da7b
 * WHD/COMPONENT_WIFI6/src/include/whd_wlioctl.h (lines 496-554).
 */
#define ESCAN_VERSION			0U
#define ESCAN_ACTION			4U
#define ESCAN_SYNC_ID			6U
#define ESCAN_PARAMETERS		8U

/*
 * The escan request's version for each layout.
 *
 * Values referenced: OpenBSD sys/dev/ic/bwfmreg.h revision 1.26 (lines
 * 509-575); Infineon wifi-host-driver bd99da7b
 * WHD/COMPONENT_WIFI6/src/include/whd_wlioctl.h (lines 496-554).
 */
#define ESCAN_VERSION_V0		1U
#define ESCAN_VERSION_V2		2U

/*
 * The scan parameters of layout 0, from their start.
 *
 * Values referenced: OpenBSD sys/dev/ic/bwfmreg.h revision 1.26 (lines
 * 509-575).
 */
#define V0_SSID_LENGTH			0U
#define V0_BSSID			36U
#define V0_BSS_TYPE			42U
#define V0_SCAN_TYPE			43U
#define V0_PROBES			44U
#define V0_ACTIVE_TIME			48U
#define V0_PASSIVE_TIME			52U
#define V0_HOME_TIME			56U
#define V0_CHANNELS			60U
#define V0_CHANNEL_LIST			64U

/*
 * The scan parameters of layout 2, from their start.
 *
 * Values referenced: OpenBSD sys/dev/ic/bwfmreg.h revision 1.26 (lines
 * 509-575).
 */
#define V2_VERSION			0U
#define V2_LENGTH			2U
#define V2_SSID_LENGTH			4U
#define V2_BSSID			40U
#define V2_BSS_TYPE			46U
#define V2_SCAN_TYPE			48U
#define V2_PROBES			52U
#define V2_ACTIVE_TIME			56U
#define V2_PASSIVE_TIME			60U
#define V2_HOME_TIME			64U
#define V2_CHANNELS			68U
#define V2_CHANNEL_LIST			72U

/*
 * Any kind of network, an active or a passive scan, and the firmware's own
 * timing (-1).
 *
 * Values referenced: OpenBSD sys/dev/ic/bwfmreg.h revision 1.26 (lines
 * 509-575); OpenBSD sys/dev/ic/bwfm.c revision 1.113 (bwfm_scan_v2(), lines
 * 2138-2255).
 */
#define NETWORK_KIND_ANY			2U
#define SCAN_PROBING		0U
#define SCAN_LISTENING		1U
#define FIRMWARE_DEFAULT		0xffffffffU

/*
 * The escan result's header: its length, version, the scan's id and the number
 * of networks.
 *
 * Values referenced: OpenBSD sys/dev/ic/bwfmreg.h revision 1.26 (lines
 * 577-583); Infineon wifi-host-driver bd99da7b
 * WHD/COMPONENT_WIFI6/src/include/whd_wlioctl.h (lines 556-562).
 */
#define RESULT_SYNC_ID			8U
#define RESULT_COUNT			10U
#define RESULT_FIRST			12U

/*
 * The network description.
 *
 * Values referenced: OpenBSD sys/dev/ic/bwfmreg.h revision 1.26 (lines
 * 344-375); Infineon wifi-host-driver bd99da7b
 * WHD/COMPONENT_WIFI6/src/include/whd_wlioctl.h (line 493, the version 109).
 */
#define BSS_LENGTH			4U
#define BSS_BSSID			8U
#define BSS_BEACON_PERIOD		14U
#define BSS_CAPABILITY			16U
#define BSS_SSID_LENGTH			18U
#define BSS_SSID			19U
#define BSS_CHANSPEC			72U
#define BSS_RSSI			78U
#define BSS_CONTROL_CHANNEL		88U
#define BSS_IE_OFFSET			116U
#define BSS_IE_LENGTH			120U

/* The probe response the description is turned into. */
#define FRAME_SUBTYPE_PROBE_RESPONSE	0x50U
#define FRAME_DESTINATION		4U
#define FRAME_SOURCE			10U
#define FRAME_BSSID			16U
#define FRAME_BEACON_INTERVAL		32U
#define FRAME_CAPABILITY		34U
#define FRAME_ELEMENTS			36U
#define ELEMENT_SSID			0U
#define ELEMENT_DS_PARAMETER		3U

/* The longest SSID. */
#define SSID_MAX			32U

/*
 * The join request: the SSID, then the BSSID, its count, and the channel list.
 *
 * Values referenced: Infineon wifi-host-driver bd99da7b
 * WHD/COMPONENT_WIFI6/src/include/whd_wlioctl.h (lines 4239-4256); OpenBSD
 * sys/dev/ic/bwfmreg.h revision 1.26 (lines 585-611).
 */
#define JOIN_SSID_LENGTH		0U
#define JOIN_SSID			4U
#define JOIN_BSSID			36U
#define JOIN_CHANNELS			44U
#define JOIN_CHANNEL_LIST		48U

/*
 * The key message: the key's length, its flags (text) and the hex digits.
 *
 * Values referenced: Infineon wifi-host-driver bd99da7b
 * WHD/COMPONENT_WIFI6/src/include/whd_wlioctl.h (lines 781-785); OpenBSD
 * sys/dev/ic/bwfmreg.h revision 1.26 (lines 649-654).
 */
#define KEY_LENGTH			0U
#define KEY_FLAGS			2U
#define KEY_TEXT			4U
#define KEY_FLAG_PASSPHRASE		0x0001U

/*
 * The event statuses this driver tells apart (firmware numbering).
 *
 * Values referenced: OpenBSD sys/dev/ic/bwfmreg.h revision 1.26 (lines
 * 736-751); Infineon wifi-host-driver bd99da7b
 * WHD/COMPONENT_WIFI6/inc/whd_events.h (lines 61-77).
 */
#define STATUS_SUCCESS			0U
#define STATUS_FAIL			1U
#define STATUS_TIMEOUT			2U
#define STATUS_NO_NETWORKS		3U
#define STATUS_ABORT			4U
#define STATUS_NO_ACK			5U
#define STATUS_UNSOLICITED		6U
#define STATUS_ATTEMPT			7U
#define STATUS_PARTIAL			8U
#define STATUS_NEWSCAN			9U
#define STATUS_NEWASSOC			10U

/*
 * The link event's flag that says the link is up.
 *
 * Value referenced: Infineon wifi-host-driver bd99da7b
 * WHD/COMPONENT_WIFI6/src/whd_wifi_api.c (line 58).
 */
#define LINK_UP				0x0001U

/*
 * The supplicant's states, as the firmware numbers them.
 *
 * Values referenced: Infineon wifi-host-driver bd99da7b
 * WHD/COMPONENT_WIFI6/src/include/whd_events_int.h (lines 224-236) and
 * WHD/COMPONENT_WIFI6/src/whd_cdc_bdc.c (lines 867-870, an offset of 256 this
 * driver does not add).
 */
#define SUPPLICANT_KEYED		6U
#define SUPPLICANT_WAIT_MESSAGE_1	4U
#define SUPPLICANT_WAIT_MESSAGE_3	8U
#define SUPPLICANT_WAIT_GROUP_1		10U

/*
 * The supplicant's reasons, as the firmware numbers them.
 *
 * Values referenced: Infineon wifi-host-driver bd99da7b
 * WHD/COMPONENT_WIFI6/src/include/whd_events_int.h (lines 329-344) and
 * WHD/COMPONENT_WIFI6/src/whd_cdc_bdc.c (lines 867-870, an offset of 512 this
 * driver does not add).
 */
#define SUPPLICANT_REASON_DEAUTHENTICATED	14U
#define SUPPLICANT_REASON_TIMEOUT		15U

/*
 * The 802.11 reason an access point gives when the four-way handshake timed
 * out.
 */
#define REASON_FOUR_WAY_TIMEOUT		15U

static int control_channel_d11n(uint16_t chanspec, uint32_t *channel);
static int control_channel_d11ac(uint16_t chanspec, uint32_t *channel);
static void classify_join(uint32_t status, struct bcm43455_connect_outcome *outcome);
static void classify_attempt(uint32_t status, struct bcm43455_connect_outcome *outcome);
static void classify_handshake(uint32_t status, uint32_t reason, struct bcm43455_connect_outcome *outcome);
static void classify_leave(const struct bcm43455_event *event, unsigned associated, unsigned authorized, struct bcm43455_connect_outcome *outcome);
static int ie_walk(const uint8_t *elements, size_t length, uint8_t *frame, size_t capacity, size_t *used);
static char hex_digit(unsigned value);

/*
 * Makes the channel specification of a 20 MHz channel.
 */
uint16_t
bcm43455_chanspec_from_channel(
	unsigned io_type,
	uint32_t channel)
{
	uint16_t chanspec;
	int band_5g;

	/* Tells the bands apart: 1 to 14 are 2.4 GHz, 36 to 181 are 5 GHz. */
	if (channel >= 1U && channel <= 14U) {
		band_5g = 0;
	} else if (channel >= 36U && channel <= 181U) {
		band_5g = 1;
	} else {
		/* Any other number is not a channel of the chip's bands. */
		return 0;
	}

	/*
	 * The 802.11n layout marks a 20 MHz channel as having no primary part.
	 */
	chanspec = (uint16_t)(channel & CHANSPEC_CHANNEL_MASK);
	if (io_type == BCM43455_CHANSPEC_LAYOUT_N) {
		chanspec |= D11N_PART_NONE;
		chanspec |= D11N_WIDTH_20;
		if (band_5g)
			chanspec |= D11N_BAND_5G;
		else
			chanspec |= D11N_BAND_2G;

		/* Succeeded: the 802.11n form. */
		return chanspec;
	}

	/*
	 * The 802.11ac layout's primary part of a 20 MHz channel is the lowest,
	 * zero.
	 */
	chanspec |= D11AC_WIDTH_20;
	if (band_5g)
		chanspec |= D11AC_BAND_5G;
	else
		chanspec |= D11AC_BAND_2G;

	/* Succeeded: the 802.11ac form. */
	return chanspec;
}

/*
 * Reads the primary channel of a channel specification.
 *
 * Procedure referenced: OpenBSD sys/dev/ic/bwfm.c revision 1.113
 * (bwfm_spec2chan_d11n() and bwfm_spec2chan_d11ac(), lines 1903-1983).
 */
int
bcm43455_chanspec_control_channel(
	unsigned io_type,
	uint16_t chanspec,
	uint32_t *channel)
{
	int error;

	/* Refuses nowhere to put the channel. */
	if (channel == NULL)
		return EINVAL;

	/* Reads it in the layout the firmware uses. */
	if (io_type == BCM43455_CHANSPEC_LAYOUT_N)
		error = control_channel_d11n(chanspec, channel);
	else
		error = control_channel_d11ac(chanspec, channel);
	if (error != 0)
		return error;

	/* Succeeded: channel is the primary one. */
	return 0;
}

/*
 * Writes an escan request for one channel, or an abort.
 *
 * Procedure referenced: OpenBSD sys/dev/ic/bwfm.c revision 1.113
 * (bwfm_scan_v2(), lines 2138-2255); Infineon wifi-host-driver bd99da7b
 * WHD/COMPONENT_WIFI6/src/whd_wifi_api.c (the abort, lines 3394-3419).
 */
int
bcm43455_escan_encode(
	uint8_t *buffer,
	size_t capacity,
	unsigned layout,
	unsigned action,
	uint16_t sync_id,
	uint16_t chanspec,
	int passive,
	size_t *length)
{
	uint8_t *parameters;
	size_t request_length;
	uint32_t channels;
	uint32_t scan_type;

	/* Refuses an unknown layout or action, or nowhere to write. */
	if (buffer == NULL || length == NULL)
		return EINVAL;
	if (layout != BCM43455_SCAN_LAYOUT_V0 && layout != BCM43455_SCAN_LAYOUT_V2)
		return EINVAL;
	if (action != BCM43455_ESCAN_START && action != BCM43455_ESCAN_ABORT)
		return EINVAL;

	/* A start lists its one channel; an abort lists none. */
	channels = 0;
	if (action == BCM43455_ESCAN_START)
		channels = 1;

	/*
	 * The request is the header, the parameters and the channel list,
	 * rounded to four bytes.
	 */
	if (layout == BCM43455_SCAN_LAYOUT_V0)
		request_length = ESCAN_PARAMETERS + V0_CHANNEL_LIST + 2U * channels;
	else
		request_length = ESCAN_PARAMETERS + V2_CHANNEL_LIST + 2U * channels;
	request_length = (request_length + 3U) & ~(size_t)3U;
	if (request_length > capacity)
		return ENOSPC;
	kern_memset(buffer, 0, request_length);

	/*
	 * Writes the header: the layout's version, the action and the scan's
	 * id.
	 */
	if (layout == BCM43455_SCAN_LAYOUT_V0)
		bcm43455_put_le32(buffer + ESCAN_VERSION, ESCAN_VERSION_V0);
	else
		bcm43455_put_le32(buffer + ESCAN_VERSION, ESCAN_VERSION_V2);
	bcm43455_put_le16(buffer + ESCAN_ACTION, (uint16_t)action);
	bcm43455_put_le16(buffer + ESCAN_SYNC_ID, sync_id);

	/* An active scan probes; a passive one only listens. */
	scan_type = SCAN_PROBING;
	if (passive)
		scan_type = SCAN_LISTENING;

	/*
	 * Writes the parameters: no SSID, the broadcast BSSID, any kind of
	 * network, the firmware's own timing, and the one channel.
	 */
	parameters = buffer + ESCAN_PARAMETERS;
	if (layout == BCM43455_SCAN_LAYOUT_V0) {
		kern_memset(parameters + V0_BSSID, 0xff, 6U);
		parameters[V0_BSS_TYPE] = (uint8_t)NETWORK_KIND_ANY;
		parameters[V0_SCAN_TYPE] = (uint8_t)scan_type;
		bcm43455_put_le32(parameters + V0_PROBES, FIRMWARE_DEFAULT);
		bcm43455_put_le32(parameters + V0_ACTIVE_TIME, FIRMWARE_DEFAULT);
		bcm43455_put_le32(parameters + V0_PASSIVE_TIME, FIRMWARE_DEFAULT);
		bcm43455_put_le32(parameters + V0_HOME_TIME, FIRMWARE_DEFAULT);
		bcm43455_put_le32(parameters + V0_CHANNELS, channels);
		if (channels != 0)
			bcm43455_put_le16(parameters + V0_CHANNEL_LIST, chanspec);
	} else {
		/*
		 * Layout 2 also carries its version and the request's whole
		 * length.
		 */
		bcm43455_put_le16(parameters + V2_VERSION, (uint16_t)ESCAN_VERSION_V2);
		bcm43455_put_le16(parameters + V2_LENGTH, (uint16_t)request_length);
		kern_memset(parameters + V2_BSSID, 0xff, 6U);
		parameters[V2_BSS_TYPE] = (uint8_t)NETWORK_KIND_ANY;
		bcm43455_put_le32(parameters + V2_SCAN_TYPE, scan_type);
		bcm43455_put_le32(parameters + V2_PROBES, FIRMWARE_DEFAULT);
		bcm43455_put_le32(parameters + V2_ACTIVE_TIME, FIRMWARE_DEFAULT);
		bcm43455_put_le32(parameters + V2_PASSIVE_TIME, FIRMWARE_DEFAULT);
		bcm43455_put_le32(parameters + V2_HOME_TIME, FIRMWARE_DEFAULT);
		bcm43455_put_le32(parameters + V2_CHANNELS, channels);
		if (channels != 0)
			bcm43455_put_le16(parameters + V2_CHANNEL_LIST, chanspec);
	}

	/* Succeeded: buffer holds the request. */
	*length = request_length;
	return 0;
}

/*
 * Decodes the header of an escan result event's data.
 *
 * Procedure referenced: OpenBSD sys/dev/ic/bwfm.c revision 1.113 (lines
 * 2611-2665).
 */
int
bcm43455_escan_result_decode(
	const uint8_t *data,
	size_t length,
	uint16_t *sync_id,
	unsigned *count,
	size_t *first)
{
	/* Refuses data shorter than the header. */
	if (data == NULL ||
	    sync_id == NULL ||
	    count == NULL ||
	    first == NULL)
		return EINVAL;
	if (length < RESULT_FIRST)
		return EPROTO;

	/* Reads the scan's id and how many networks follow. */
	*sync_id = bcm43455_get_le16(data + RESULT_SYNC_ID);
	*count = bcm43455_get_le16(data + RESULT_COUNT);
	*first = RESULT_FIRST;

	/* Succeeded: the descriptions start at first. */
	return 0;
}

/*
 * Measures the network description at offset of an escan result's data.
 *
 * Procedure referenced: OpenBSD sys/dev/ic/bwfm.c revision 1.113 (lines
 * 2611-2665).
 */
int
bcm43455_bss_length(
	const uint8_t *data,
	size_t length,
	size_t offset,
	size_t *bss_length)
{
	uint32_t described;

	/* Refuses a description whose fixed part does not fit the data. */
	if (data == NULL || bss_length == NULL)
		return EINVAL;
	if (offset > length || length - offset < BCM43455_BSS_INFO_FIXED)
		return EPROTO;

	/* The description says how long it is, its elements included. */
	described = bcm43455_get_le32(data + offset + BSS_LENGTH);
	if (described < BCM43455_BSS_INFO_FIXED)
		return EPROTO;
	if ((size_t)described > length - offset)
		return EPROTO;

	/* Succeeded: the description takes bss_length bytes. */
	*bss_length = (size_t)described;
	return 0;
}

/*
 * Turns one network description into a probe response.
 *
 * Procedure referenced: OpenBSD sys/dev/ic/bwfm.c revision 1.113
 * (bwfm_scan_node(), lines 2717-2763); the SSID and DS parameter elements are
 * made here.
 */
int
bcm43455_bss_to_frame(
	const uint8_t *bss,
	size_t length,
	unsigned io_type,
	uint8_t *frame,
	size_t capacity,
	size_t *frame_length,
	uint32_t *channel,
	int32_t *rssi)
{
	uint32_t described;
	uint32_t ie_offset;
	uint32_t ie_length;
	uint32_t primary;
	uint32_t reported;
	uint16_t chanspec;
	uint8_t ssid_length;
	size_t used;
	size_t copied;
	int error;

	/*
	 * Refuses a description shorter than its fixed part, or nowhere to
	 * write.
	 */
	if (bss == NULL ||
	    frame == NULL ||
	    frame_length == NULL ||
	    channel == NULL ||
	    rssi == NULL)
		return EINVAL;
	if (length < BCM43455_BSS_INFO_FIXED)
		return EPROTO;
	described = bcm43455_get_le32(bss + BSS_LENGTH);
	if (described < BCM43455_BSS_INFO_FIXED || (size_t)described > length)
		return EPROTO;

	/* The elements lie inside the description. */
	ie_offset = bcm43455_get_le16(bss + BSS_IE_OFFSET);
	ie_length = bcm43455_get_le32(bss + BSS_IE_LENGTH);
	if (ie_offset > described || ie_length > described - ie_offset)
		return EPROTO;

	/* The SSID is at most 32 bytes. */
	ssid_length = bss[BSS_SSID_LENGTH];
	if (ssid_length > SSID_MAX)
		return EPROTO;

	/*
	 * The primary channel comes from the channel specification; the
	 * description's own primary channel, when the firmware filled it in,
	 * is the one taken if they differ.
	 */
	chanspec = bcm43455_get_le16(bss + BSS_CHANSPEC);
	error = bcm43455_chanspec_control_channel(io_type, chanspec, &primary);
	if (error != 0)
		return EPROTO;
	reported = bss[BSS_CONTROL_CHANNEL];
	if (reported != 0 && reported != primary)
		primary = reported;

	/* The frame's fixed part and both made elements fit. */
	used = FRAME_ELEMENTS + 2U + (size_t)ssid_length + 3U;
	if (used > capacity)
		return EMSGSIZE;
	kern_memset(frame, 0, FRAME_ELEMENTS);

	/*
	 * A probe response from the BSSID to everyone, with the beacon interval
	 * and capabilities.
	 */
	frame[0] = (uint8_t)FRAME_SUBTYPE_PROBE_RESPONSE;
	kern_memset(frame + FRAME_DESTINATION, 0xff, 6U);
	kern_memcpy(frame + FRAME_SOURCE, bss + BSS_BSSID, 6U);
	kern_memcpy(frame + FRAME_BSSID, bss + BSS_BSSID, 6U);
	kern_memcpy(frame + FRAME_BEACON_INTERVAL, bss + BSS_BEACON_PERIOD, 2U);
	kern_memcpy(frame + FRAME_CAPABILITY, bss + BSS_CAPABILITY, 2U);

	/* The SSID element, from the description. */
	frame[FRAME_ELEMENTS] = (uint8_t)ELEMENT_SSID;
	frame[FRAME_ELEMENTS + 1U] = ssid_length;
	kern_memcpy(frame + FRAME_ELEMENTS + 2U, bss + BSS_SSID, ssid_length);

	/* The DS parameter element, naming the primary channel. */
	copied = FRAME_ELEMENTS + 2U + (size_t)ssid_length;
	frame[copied] = (uint8_t)ELEMENT_DS_PARAMETER;
	frame[copied + 1U] = 1U;
	frame[copied + 2U] = (uint8_t)primary;

	/* The firmware's elements but its SSID and DS parameter ones. */
	copied = 0;
	error = ie_walk(bss + ie_offset, ie_length, frame + used, capacity - used, &copied);
	if (error != 0)
		return error;

	/* Succeeded: frame is a probe response the core's parser reads. */
	*frame_length = used + copied;
	*channel = primary;
	*rssi = (int32_t)(int16_t)bcm43455_get_le16(bss + BSS_RSSI);
	return 0;
}

/*
 * Writes a join request.
 *
 * Procedure referenced: Infineon wifi-host-driver bd99da7b
 * WHD/COMPONENT_WIFI6/src/whd_wifi_api.c (lines 2343-2386).
 */
int
bcm43455_join_encode(
	uint8_t *buffer,
	size_t capacity,
	const uint8_t *ssid,
	size_t ssid_length,
	const uint8_t bssid[6],
	uint16_t chanspec,
	size_t *length)
{
	/* Refuses an SSID that does not fit, or no room for the request. */
	if (buffer == NULL ||
	    bssid == NULL ||
	    length == NULL)
		return EINVAL;
	if (ssid == NULL && ssid_length != 0)
		return EINVAL;
	if (ssid_length > SSID_MAX)
		return EINVAL;
	if (capacity < BCM43455_JOIN_REQUEST_LENGTH)
		return ENOSPC;
	kern_memset(buffer, 0, BCM43455_JOIN_REQUEST_LENGTH);

	/* The network's name. */
	bcm43455_put_le32(buffer + JOIN_SSID_LENGTH, (uint32_t)ssid_length);
	if (ssid_length != 0)
		kern_memcpy(buffer + JOIN_SSID, ssid, ssid_length);

	/* The access point to join, and the one channel it is on. */
	kern_memcpy(buffer + JOIN_BSSID, bssid, 6U);
	bcm43455_put_le32(buffer + JOIN_CHANNELS, 1U);
	bcm43455_put_le16(buffer + JOIN_CHANNEL_LIST, chanspec);

	/* Succeeded: buffer holds the request. */
	*length = BCM43455_JOIN_REQUEST_LENGTH;
	return 0;
}

/*
 * Writes the supplicant's key message for a PMK.
 *
 * Procedure referenced: Infineon wifi-host-driver bd99da7b
 * WHD/COMPONENT_WIFI6/src/whd_wifi_api.c (whd_wifi_sae_password(), lines
 * 1140-1176).
 */
void
bcm43455_key_encode(
	uint8_t message[BCM43455_KEY_MESSAGE_LENGTH],
	const uint8_t pmk[BCM43455_PMK_LENGTH])
{
	unsigned i;

	/* Starts from zeros, which also ends the text. */
	kern_memset(message, 0, BCM43455_KEY_MESSAGE_LENGTH);

	/*
	 * Sixty-four hex digits, marked as text so the firmware reads them as
	 * the PMK.
	 */
	bcm43455_put_le16(message + KEY_LENGTH, (uint16_t)(2U * BCM43455_PMK_LENGTH));
	bcm43455_put_le16(message + KEY_FLAGS, (uint16_t)KEY_FLAG_PASSPHRASE);

	/* Writes each byte as two lower-case hex digits. */
	for (i = 0; i < BCM43455_PMK_LENGTH; i++) {
		message[KEY_TEXT + 2U * i] = (uint8_t)hex_digit(pmk[i] >> 4);
		message[KEY_TEXT + 2U * i + 1U] = (uint8_t)hex_digit(pmk[i] & 0x0fU);
	}
}

/*
 * Reads what an escan result event's status says about its scan.
 */
enum bcm43455_scan_status
bcm43455_scan_classify(
	uint32_t status)
{
	/* Tells the statuses apart. */
	switch (status) {
	case STATUS_PARTIAL:
		/* Networks, and more to come. */
		return BCM43455_SCAN_STATUS_RESULTS;

	case STATUS_SUCCESS:
		/* The channel is done. */
		return BCM43455_SCAN_STATUS_COMPLETE;

	case STATUS_ABORT:
		/* A scan that was stopped. */
		return BCM43455_SCAN_STATUS_ABORTED;

	case STATUS_NEWSCAN:
	case STATUS_NEWASSOC:
		/* Another scan or a join took the radio. */
		return BCM43455_SCAN_STATUS_BUSY;

	default:
		/* Any other end is a failure. */
		return BCM43455_SCAN_STATUS_FAILED;
	}
}

/*
 * Reads what an event means to the connection being made.
 *
 * The table:
 *   join result success, or the link coming up       ASSOCIATED
 *   supplicant keyed                                  AUTHORIZED
 *   join result: no network, timeout, no ack         ETIMEDOUT
 *   join result, authentication or association
 *   refused                                           ECONNREFUSED
 *   authentication or association timeout, no ack    ETIMEDOUT
 *   supplicant timed out waiting for message 1 or
 *   the group key                                     ETIMEDOUT
 *   supplicant failed waiting for message 3 (after
 *   message 2 went out: the key was wrong)            EACCES
 *   access point's deauthentication with reason 15
 *   (four-way timeout) before the key was set         EACCES
 *   any other supplicant failure                      EPROTO
 *   deauthentication, disassociation or link down
 *   after joining                                     ECONNRESET
 */
void
bcm43455_connect_classify(
	const struct bcm43455_event *event,
	unsigned associated,
	unsigned authorized,
	struct bcm43455_connect_outcome *outcome)
{
	/* Starts from an event that changes nothing. */
	outcome->step = BCM43455_CONNECT_NONE;
	outcome->error = 0;

	/* Tells the events apart. */
	switch (event->type) {
	case BCM43455_EVENT_JOIN_RESULT:
		/* The join's own result; a joined link ignores a repeat. */
		if (!associated)
			classify_join(event->status, outcome);
		break;

	case BCM43455_EVENT_AUTHENTICATION:
	case BCM43455_EVENT_ASSOCIATION:
		/* Only a failure before the join completed ends the attempt. */
		if (!associated)
			classify_attempt(event->status, outcome);
		break;

	case BCM43455_EVENT_LINK:
		/*
		 * The link coming up is the join; going down after it is the
		 * end.
		 */
		if ((event->flags & LINK_UP) != 0) {
			if (!associated)
				outcome->step = BCM43455_CONNECT_ASSOCIATED;
		} else if (associated) {
			outcome->step = BCM43455_CONNECT_LOST;
			outcome->error = ECONNRESET;
		}

		break;

	case BCM43455_EVENT_SUPPLICANT:
		/*
		 * The supplicant only speaks once the link is up, and before
		 * the key is set.
		 */
		if (associated && !authorized)
			classify_handshake(event->status, event->reason, outcome);
		break;

	case BCM43455_EVENT_DEAUTHENTICATED:
	case BCM43455_EVENT_PEER_DEAUTHENTICATED:
	case BCM43455_EVENT_PEER_DISASSOCIATED:
		/* Leaving a joined link ends the connection. */
		classify_leave(event, associated, authorized, outcome);
		break;

	default:
		/* Any other event says nothing about the connection. */
		break;
	}
}

/* Reads the primary channel of an 802.11n channel specification. */
static int
control_channel_d11n(
	uint16_t chanspec,
	uint32_t *channel)
{
	uint32_t number;
	uint16_t width;
	uint16_t part;

	/* Takes the channel number, the width and the primary part apart. */
	number = chanspec & CHANSPEC_CHANNEL_MASK;
	width = chanspec & D11N_WIDTH_MASK;
	part = chanspec & D11N_PART_MASK;

	/* A 20 MHz channel is its own primary. */
	if (width == D11N_WIDTH_20) {
		*channel = number;
		return 0;
	}

	/*
	 * Refuses a width this driver does not know, and a 40 MHz channel
	 * without a primary part.
	 */
	if (width != D11N_WIDTH_40)
		return EINVAL;
	if (part != D11N_PART_LOWER && part != D11N_PART_UPPER)
		return EINVAL;
	if (number < 3U)
		return EINVAL;

	/*
	 * A 40 MHz channel names its centre; the primary is 10 MHz (two
	 * numbers) below or above.
	 */
	if (part == D11N_PART_LOWER)
		*channel = number - 2U;
	else
		*channel = number + 2U;

	/* Succeeded: channel is the primary one. */
	return 0;
}

/* Reads the primary channel of an 802.11ac channel specification. */
static int
control_channel_d11ac(
	uint16_t chanspec,
	uint32_t *channel)
{
	uint32_t number;
	uint32_t part;
	uint16_t width;

	/* Takes the channel number, the width and the primary part apart. */
	number = chanspec & CHANSPEC_CHANNEL_MASK;
	part = (uint32_t)(chanspec & D11AC_PART_MASK) >> D11AC_PART_SHIFT;
	width = chanspec & D11AC_WIDTH_MASK;

	/* A 20 MHz channel is its own primary. */
	if (width == D11AC_WIDTH_20) {
		*channel = number;
		return 0;
	}

	/*
	 * Refuses a width this driver does not know (160 MHz, 80+80, 5 or 10
	 * MHz).
	 */
	if (width != D11AC_WIDTH_40 && width != D11AC_WIDTH_80)
		return EINVAL;

	/*
	 * A 40 MHz channel's two parts lie 2 numbers below and above its
	 * centre; an 80 MHz channel's four parts lie 6 and 2 below and 2 and
	 * 6 above.
	 */
	if (width == D11AC_WIDTH_40) {
		if (part > 1U || number < 3U)
			return EINVAL;
		*channel = number - 2U + 4U * part;
	} else {
		if (part > 3U || number < 7U)
			return EINVAL;
		*channel = number - 6U + 4U * part;
	}

	/* Succeeded: channel is the primary one. */
	return 0;
}

/* Reads a join result's status. */
static void
classify_join(
	uint32_t status,
	struct bcm43455_connect_outcome *outcome)
{
	/* Tells the statuses apart. */
	switch (status) {
	case STATUS_SUCCESS:
		/* The firmware joined the access point. */
		outcome->step = BCM43455_CONNECT_ASSOCIATED;
		break;

	case STATUS_NO_NETWORKS:
	case STATUS_TIMEOUT:
	case STATUS_NO_ACK:
		/*
		 * The access point was not found or did not answer, as a
		 * SoftMAC join that hears nothing.
		 */
		outcome->step = BCM43455_CONNECT_LOST;
		outcome->error = ETIMEDOUT;
		break;

	case STATUS_UNSOLICITED:
	case STATUS_ATTEMPT:
		/* Not the join's end. */
		break;

	default:
		/* The access point refused the join. */
		outcome->step = BCM43455_CONNECT_LOST;
		outcome->error = ECONNREFUSED;
		break;
	}
}

/*
 * Reads an authentication's or an association's status before the join
 * completed.
 */
static void
classify_attempt(
	uint32_t status,
	struct bcm43455_connect_outcome *outcome)
{
	/* Tells the statuses apart. */
	switch (status) {
	case STATUS_SUCCESS:
	case STATUS_UNSOLICITED:
	case STATUS_ATTEMPT:
		/* A step that went through, or a frame not of this attempt. */
		break;

	case STATUS_TIMEOUT:
	case STATUS_NO_ACK:
		/* The access point did not answer. */
		outcome->step = BCM43455_CONNECT_LOST;
		outcome->error = ETIMEDOUT;
		break;

	default:
		/* The access point refused the station. */
		outcome->step = BCM43455_CONNECT_LOST;
		outcome->error = ECONNREFUSED;
		break;
	}
}

/*
 * Reads a supplicant event before the key was set.
 *
 * Procedure referenced: Infineon wifi-host-driver bd99da7b
 * WHD/COMPONENT_WIFI6/src/whd_wifi_api.c (whd_wifi_join_events_handler(), lines
 * 1360-1400); the errors are this driver's own, after the WLAN core's SoftMAC
 * rules.
 */
static void
classify_handshake(
	uint32_t status,
	uint32_t reason,
	struct bcm43455_connect_outcome *outcome)
{
	/* The handshakes finished: the controlled port may open. */
	if (status == SUPPLICANT_KEYED) {
		outcome->step = BCM43455_CONNECT_AUTHORIZED;
		return;
	}

	/* Any other supplicant event ends the attempt. */
	outcome->step = BCM43455_CONNECT_LOST;

	/*
	 * Message 2 went out and message 3 never came: the access point could
	 * not check message 2's MIC, so the key was wrong.  This is the only
	 * supplicant failure that is evidence of a wrong key.
	 */
	if (status == SUPPLICANT_WAIT_MESSAGE_3) {
		outcome->error = EACCES;
		return;
	}

	/*
	 * Message 1 or the group key did not come: the access point was slow or
	 * far.
	 */
	if (reason == SUPPLICANT_REASON_TIMEOUT) {
		if (status == SUPPLICANT_WAIT_MESSAGE_1 || status == SUPPLICANT_WAIT_GROUP_1) {
			outcome->error = ETIMEDOUT;
			return;
		}
	}

	/*
	 * Any other failure (an element or cipher mismatch, a group key not
	 * decrypted, a deauthentication) is a protocol error.
	 */
	outcome->error = EPROTO;
}

/* Reads a deauthentication or disassociation. */
static void
classify_leave(
	const struct bcm43455_event *event,
	unsigned associated,
	unsigned authorized,
	struct bcm43455_connect_outcome *outcome)
{
	/*
	 * Before the join completed, the join's own result says how it ended.
	 */
	if (!associated)
		return;

	/* The connection ends. */
	outcome->step = BCM43455_CONNECT_LOST;
	outcome->error = ECONNRESET;

	/*
	 * An access point that gave up on the four-way handshake refused the
	 * key; the firmware's own deauthentication is no such evidence.
	 */
	if (!authorized &&
	    event->type != BCM43455_EVENT_DEAUTHENTICATED &&
	    event->reason == REASON_FOUR_WAY_TIMEOUT)
		outcome->error = EACCES;
}

/*
 * Copies the firmware's information elements behind the made ones,
 * leaving out its SSID and DS parameter elements.  Returns EPROTO for an
 * element that runs past the list and EMSGSIZE when the frame is full.
 */
static int
ie_walk(
	const uint8_t *elements,
	size_t length,
	uint8_t *frame,
	size_t capacity,
	size_t *used)
{
	size_t offset;
	size_t element_length;
	size_t copied;
	uint8_t identifier;

	/* Walks the elements one at a time. */
	offset = 0;
	copied = 0;
	while (offset < length) {
		/* An element is an identifier, a length and that many bytes. */
		if (length - offset < 2U)
			return EPROTO;
		identifier = elements[offset];
		element_length = elements[offset + 1U];
		if (element_length > length - offset - 2U)
			return EPROTO;

		/*
		 * The SSID and the channel were made from the description
		 * already.
		 */
		if (identifier != ELEMENT_SSID && identifier != ELEMENT_DS_PARAMETER) {
			if (2U + element_length > capacity - copied)
				return EMSGSIZE;
			kern_memcpy(frame + copied, elements + offset, 2U + element_length);
			copied += 2U + element_length;
		}

		/* Moves on to the next element. */
		offset += 2U + element_length;
	}

	/* Succeeded: used bytes were copied. */
	*used = copied;
	return 0;
}

/* The lower-case hex digit of a value from 0 to 15. */
static char
hex_digit(
	unsigned value)
{
	/* The digits come first, then the letters. */
	if (value < 10U)
		return (char)('0' + value);

	/* Reports a letter. */
	return (char)('a' + value - 10U);
}
