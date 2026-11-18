/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * BCM43455: decoding the firmware's event frames.
 */

#include "drivers/wifi/bcm43455/bcm43455-event.h"
#include "drivers/wifi/bcm43455/bcm43455-bytes.h"
#include <kern/kcrt.h>
#include <uapi/errno.h>

/* The Ethernet header: two addresses and the ethertype. */
#define ETHERNET_TYPE			12U
#define ETHERNET_HEADER_LENGTH		14U

/*
 * The Broadcom vendor header that follows it.
 *
 * Values referenced: OpenBSD sys/dev/ic/bwfmreg.h revision 1.26 (struct
 * bwfm_ethhdr, lines 754-783).
 */
#define VENDOR_SUBTYPE			14U
#define VENDOR_OUI			19U
#define VENDOR_USER_SUBTYPE		22U

/*
 * The vendor header's user subtype of a firmware event.
 *
 * Value referenced: OpenBSD sys/dev/ic/bwfm.c revision 1.113 (bwfm_rx(), lines
 * 2367-2373).
 */
#define VENDOR_USER_SUBTYPE_EVENT	1U

/*
 * The event message, big-endian, after the vendor header.
 *
 * Values referenced: OpenBSD sys/dev/ic/bwfmreg.h revision 1.26 (struct
 * bwfm_event_msg, lines 754-783).
 */
#define MESSAGE_VERSION			24U
#define MESSAGE_FLAGS			26U
#define MESSAGE_TYPE			28U
#define MESSAGE_STATUS			32U
#define MESSAGE_REASON			36U
#define MESSAGE_AUTH_TYPE		40U
#define MESSAGE_DATA_LENGTH		44U
#define MESSAGE_ADDRESS			48U
#define MESSAGE_INTERFACE_INDEX		70U
#define MESSAGE_BSSCFG_INDEX		71U

/*
 * Broadcom's organizationally unique identifier, which marks its vendor header.
 *
 * Value referenced: OpenBSD sys/dev/ic/bwfm.c revision 1.113 (bwfm_rx(), lines
 * 2367-2373).
 */
static const uint8_t broadcom_oui[3] = {0x00, 0x10, 0x18};

/*
 * Decodes an Ethernet frame the firmware sent as an event.
 */
int
bcm43455_event_decode(
	const uint8_t *frame,
	size_t length,
	struct bcm43455_event *event)
{
	uint32_t data_length;
	uint16_t ethertype;
	uint16_t subtype;
	uint16_t user_subtype;
	int same;

	/* Refuses a frame without the Ethernet header. */
	if (frame == NULL || event == NULL)
		return EINVAL;
	if (length < ETHERNET_HEADER_LENGTH)
		return EPROTO;

	/* A frame of another ethertype is not an event. */
	ethertype = bcm43455_get_be16(frame + ETHERNET_TYPE);
	if (ethertype != BCM43455_ETHERTYPE_EVENT)
		return ENOENT;

	/* An event frame holds the whole vendor header and event message. */
	if (length < BCM43455_EVENT_HEADER_LENGTH)
		return EPROTO;

	/*
	 * The vendor header names Broadcom and the event user subtype.  Its own
	 * subtype (0x8001) is read but not required.
	 *
	 * Procedure referenced: OpenBSD sys/dev/ic/bwfm.c revision 1.113
	 * (bwfm_rx(), lines 2367-2373) and, for 0x8001, Infineon
	 * wifi-host-driver bd99da7b WHD/COMPONENT_WIFI6/inc/whd_events.h (line
	 * 140).
	 */
	subtype = bcm43455_get_be16(frame + VENDOR_SUBTYPE);
	event->vendor_subtype = subtype;
	same = kern_memcmp(frame + VENDOR_OUI, broadcom_oui, sizeof(broadcom_oui));
	if (same != 0)
		return EPROTO;
	user_subtype = bcm43455_get_be16(frame + VENDOR_USER_SUBTYPE);
	if (user_subtype != VENDOR_USER_SUBTYPE_EVENT)
		return EPROTO;

	/* Reads the event message. */
	event->version = bcm43455_get_be16(frame + MESSAGE_VERSION);
	event->flags = bcm43455_get_be16(frame + MESSAGE_FLAGS);
	event->type = bcm43455_get_be32(frame + MESSAGE_TYPE);
	event->status = bcm43455_get_be32(frame + MESSAGE_STATUS);
	event->reason = bcm43455_get_be32(frame + MESSAGE_REASON);
	event->authentication = bcm43455_get_be32(frame + MESSAGE_AUTH_TYPE);
	kern_memcpy(event->address, frame + MESSAGE_ADDRESS, sizeof(event->address));
	event->interface_index = frame[MESSAGE_INTERFACE_INDEX];
	event->bsscfg_index = frame[MESSAGE_BSSCFG_INDEX];

	/*
	 * The event's data follows the message and must lie inside the frame.
	 */
	data_length = bcm43455_get_be32(frame + MESSAGE_DATA_LENGTH);
	if ((size_t)data_length > length - BCM43455_EVENT_HEADER_LENGTH)
		return EPROTO;
	event->data_start = BCM43455_EVENT_HEADER_LENGTH;
	event->data_length = (size_t)data_length;

	/* Succeeded: event describes the frame. */
	return 0;
}

/*
 * Sets an event's bit in the firmware's event mask.
 */
int
bcm43455_event_mask_add(
	uint8_t mask[BCM43455_EVENT_MASK_LENGTH],
	unsigned event)
{
	/* Refuses an event the mask has no bit for. */
	if (event >= BCM43455_EVENT_MASK_LENGTH * 8U)
		return EINVAL;

	/* Event n is bit n % 8 of byte n / 8. */
	mask[event / 8U] = (uint8_t)(mask[event / 8U] | (1U << (event % 8U)));

	/* Succeeded: the firmware will report the event. */
	return 0;
}
