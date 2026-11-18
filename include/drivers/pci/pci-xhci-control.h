/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * PCI xHCI control-transfer and enumeration arithmetic
 */

#ifndef KERN_DRIVERS_PCI_XHCI_CONTROL_H
#define KERN_DRIVERS_PCI_XHCI_CONTROL_H

#include <drivers/usb/usb.h>

#include <stdint.h>

/* ring_push() supplies the Cycle bit; these builders supply all other words. */
#define DRV_XHCI_TRB_CHAIN	0x00000010U
#define DRV_XHCI_TRB_ISP	0x00000004U
#define DRV_XHCI_TRB_IOC	0x00000020U
#define DRV_XHCI_TRB_IDT	0x00000040U
#define DRV_XHCI_TRB_TYPE(n)	((uint32_t)(n) << 10)
#define DRV_XHCI_TRB_DIR_IN	0x00010000U
#define DRV_XHCI_TRB_TD_SIZE_MASK	0x003e0000U
#define DRV_XHCI_TRB_TRANSFER_LENGTH_MASK	0x0001ffffU
#define DRV_XHCI_CONTROL_DATA_MAX_LENGTH	0x00010000U

#define DRV_XHCI_CONTROL_AVERAGE_TRB_LENGTH	8U
#define DRV_XHCI_SUPERSPEED_INTERRUPT_MAX_ESIT_PAYLOAD	3072U

#define DRV_XHCI_PORTSC_CCS	0x00000001U
#define DRV_XHCI_PORTSC_PED	0x00000002U
#define DRV_XHCI_PORTSC_PR	0x00000010U
#define DRV_XHCI_PORTSC_PLS_MASK	0x000001e0U
#define DRV_XHCI_PORTSC_CSC	0x00020000U
#define DRV_XHCI_PORTSC_PRC	0x00200000U

enum drv_xhci_control_data {
	DRV_XHCI_CONTROL_NO_DATA,
	DRV_XHCI_CONTROL_DATA_OUT,
	DRV_XHCI_CONTROL_DATA_IN
};

enum drv_xhci_port_reset_decision {
	DRV_XHCI_PORT_RESET_WAIT,
	DRV_XHCI_PORT_RESET_SUCCESS,
	DRV_XHCI_PORT_RESET_DISCONNECTED,
	DRV_XHCI_PORT_RESET_INVALID
};

enum drv_xhci_endpoint_reset_admission {
	DRV_XHCI_ENDPOINT_RESET_ACQUIRE,
	DRV_XHCI_ENDPOINT_RESET_WAIT_PUBLICATION,
	DRV_XHCI_ENDPOINT_RESET_BUSY
};

struct drv_xhci_trb_words {
	uint32_t parameter_low;
	uint32_t parameter_high;
	uint32_t status;
	uint32_t control;
};

struct drv_xhci_ep0_context_words {
	uint32_t words[5];
};

struct drv_xhci_endpoint_context_words {
	uint32_t word0;
	uint32_t word1;
	uint32_t word4;
};

static inline unsigned
drv_xhci_port_speed_id(uint32_t portsc)
{
	return (portsc >> 10) & 15U;
}

/* A STALL completion temporarily owns endpoint recovery while it hands the
 * hardware result to the USB core.  The reset worker may wait for that one
 * owner because the core halt latch is already published before the HCD URB
 * reference is released.  It must reject an active TD or every other recovery
 * owner instead of accidentally joining an unrelated command transaction. */
static inline enum drv_xhci_endpoint_reset_admission
drv_xhci_endpoint_reset_admit(unsigned active, unsigned recovering,
	unsigned stall_publishing)
{
	if (active || (stall_publishing && !recovering))
		return DRV_XHCI_ENDPOINT_RESET_BUSY;
	if (!recovering)
		return DRV_XHCI_ENDPOINT_RESET_ACQUIRE;
	return stall_publishing ? DRV_XHCI_ENDPOINT_RESET_WAIT_PUBLICATION :
	    DRV_XHCI_ENDPOINT_RESET_BUSY;
}

static inline uint32_t
drv_xhci_endpoint_context_word1(unsigned type, unsigned packet,
	unsigned maximum_burst)
{
	return (3U << 1) | ((type & 7U) << 3) |
	    ((maximum_burst & 0xffU) << 8) | ((packet & 0xffffU) << 16);
}

/*
 * Build the controller-owned endpoint context fields which do not depend on
 * the transfer-ring address.  The SuperSpeed interrupt case is intentionally
 * strict because xHCI cannot truthfully schedule it without its companion's
 * total service-interval payload.  Isochronous and slower interrupt
 * endpoints carry their interval's payload too; control and bulk have none.
 */
static inline int
drv_xhci_endpoint_context_encode(enum drv_usb_speed speed, unsigned type,
	uint16_t maximum_packet_size, uint8_t descriptor_interval,
	const struct drv_usb_superspeed_endpoint_companion_descriptor *companion,
	struct drv_xhci_endpoint_context_words *context)
{
	struct drv_xhci_endpoint_context_words encoded;
	unsigned interval = 0;
	unsigned maximum_burst = 0;
	unsigned microframes;
	unsigned packet = maximum_packet_size & 0x7ffU;
	unsigned interval_payload;
	unsigned interrupt;

	if (context == NULL || type < 1U || type > 7U)
		return 0;
	interrupt = type == 3U || type == 7U;
	if (speed >= DRV_USB_SPEED_SUPER && companion != NULL)
		maximum_burst = companion->maximum_burst;
	if (speed == DRV_USB_SPEED_SUPER && interrupt) {
		unsigned capacity;
		unsigned payload;

		if (companion == NULL || maximum_burst > 15U ||
		    companion->attributes != 0 || packet == 0 ||
		    (maximum_packet_size & 0xf800U) != 0 || packet > 1024U ||
		    descriptor_interval == 0 || descriptor_interval > 16U)
			return 0;
		payload = companion->bytes_per_interval;
		capacity = packet * (maximum_burst + 1U);
		if (payload == 0 || payload > capacity ||
		    payload > DRV_XHCI_SUPERSPEED_INTERRUPT_MAX_ESIT_PAYLOAD)
			return 0;
		interval = descriptor_interval - 1U;
		encoded.word0 = (uint32_t)interval << 16;
		encoded.word1 = drv_xhci_endpoint_context_word1(type, packet,
		    maximum_burst);
		encoded.word4 = ((uint32_t)payload << 16) | payload;
		*context = encoded;
		return 1;
	}
	/*
	 * Isochronous: full speed and high speed.  No error retries (CErr
	 * 0); the interval is 2^(bInterval - 1) frames at full speed
	 * (bInterval + 2 as microframes' exponent) and 2^(bInterval - 1)
	 * microframes at high speed.  A high-speed endpoint may be high
	 * bandwidth: the extra transactions a microframe in wMaxPacketSize
	 * bits 12:11 (USB 2.0 9.6.6; 3 is reserved) go to Max Burst Size,
	 * and an interval carries Max Packet Size x (Max Burst Size + 1)
	 * bytes (Value(s) referenced: xHCI 1.2 4.14.2.1.1, 6.2.3 and
	 * 6.2.3.8).  Full speed has no extra transactions; SuperSpeed is
	 * refused.
	 */
	if (type == 1U || type == 5U) {
		/* SuperSpeed and anything slower than full speed are refused. */
		if (speed >= DRV_USB_SPEED_SUPER || speed < DRV_USB_SPEED_FULL)
			return 0;

		/* The extra transactions a microframe, which wMaxPacketSize bits 12:11 give. */
		maximum_burst = (maximum_packet_size >> 11) & 3U;

		/* Refuses reserved bits, an empty packet and the reserved burst value. */
		if ((maximum_packet_size & 0xe000U) != 0 ||
		    packet == 0 ||
		    maximum_burst == 3U)
			return 0;

		/* Full speed has no extra transactions. */
		if (speed == DRV_USB_SPEED_FULL && maximum_burst != 0)
			return 0;

		/* A high-bandwidth endpoint's packet is at most 1024 bytes. */
		if (speed == DRV_USB_SPEED_HIGH &&
		    maximum_burst != 0 &&
		    packet > 1024U)
			return 0;

		/* Refuses an interval the descriptor cannot express. */
		if (descriptor_interval == 0 || descriptor_interval > 16U)
			return 0;

		/* The interval's exponent, in frames at full speed and in microframes at high speed. */
		interval = descriptor_interval - 1U;
		if (speed == DRV_USB_SPEED_FULL)
			interval = descriptor_interval + 2U;

		/* Refuses an exponent the context cannot hold. */
		if (interval > 15U)
			return 0;

		/* Encodes the interval, the burst and the bytes an interval carries. */
		interval_payload = packet * (maximum_burst + 1U);
		encoded.word0 = (uint32_t)interval << 16;
		encoded.word1 = ((type & 7U) << 3) | (maximum_burst << 8) |
		    ((packet & 0xffffU) << 16);
		encoded.word4 = ((uint32_t)interval_payload << 16) | interval_payload;
		*context = encoded;

		/* Succeeded: the isochronous context is encoded. */
		return 1;
	}
	/*
	 * Interrupt below SuperSpeed.  The interval is 2^Interval x 125 us:
	 * at high speed 2^(bInterval - 1) microframes (bInterval 1..16); at
	 * full and low speed bInterval frames rounded down to a power of two,
	 * so the endpoint is served at least as often as it asked, and kept to
	 * 3..10 (1 to 128 ms).  A high-speed endpoint may take extra
	 * transactions a microframe (wMaxPacketSize bits 12:11, the reserved
	 * 3 read as 2), which go to Max Burst Size.  Max ESIT Payload is the
	 * bytes an interval carries, Max Packet Size x (Max Burst Size + 1),
	 * and an interrupt TD is one interval's data, so it is also the
	 * Average TRB Length (Value(s) referenced: xHCI 1.2 6.2.3.4, 6.2.3.6
	 * and 6.2.3.8; the same rounding and limits as Linux's
	 * xhci_get_endpoint_interval() and xhci_get_max_esit_payload()).
	 */
	if (interrupt) {
		/* The extra transactions a high-speed microframe. */
		if (speed == DRV_USB_SPEED_HIGH) {
			maximum_burst = (maximum_packet_size >> 11) & 3U;
			if (maximum_burst == 3U)
				maximum_burst = 2U;
		}

		/* The interval's exponent: in microframes from high speed up, in frames below. */
		if (speed >= DRV_USB_SPEED_HIGH) {
			interval = 0;
			if (descriptor_interval != 0)
				interval = descriptor_interval - 1U;
			if (interval > 15U)
				interval = 15U;
		} else {
			microframes = 8U;
			if (descriptor_interval != 0)
				microframes = (unsigned)descriptor_interval * 8U;
			interval = 0;
			while ((2U << interval) <= microframes)
				interval++;
			if (interval < 3U)
				interval = 3U;
			if (interval > 10U)
				interval = 10U;
		}

		/* Encodes the interval, the burst and the bytes an interval carries. */
		interval_payload = packet * (maximum_burst + 1U);
		encoded.word0 = (uint32_t)interval << 16;
		encoded.word1 = drv_xhci_endpoint_context_word1(type, packet,
		    maximum_burst);
		encoded.word4 = ((uint32_t)interval_payload << 16) | interval_payload;
		*context = encoded;

		/* Succeeded: the interrupt context is encoded. */
		return 1;
	}

	/* Control and bulk: not periodic, no interval and no ESIT payload. */
	encoded.word0 = 0;
	encoded.word1 = drv_xhci_endpoint_context_word1(type, packet,
	    maximum_burst);
	encoded.word4 = type == 4U ?
	    DRV_XHCI_CONTROL_AVERAGE_TRB_LENGTH : packet;
	*context = encoded;
	return 1;
}

static inline int
drv_xhci_control_setup_words(uint64_t setup,
	enum drv_xhci_control_data data, struct drv_xhci_trb_words *words)
{
	uint32_t trt;

	if (words == NULL)
		return 0;
	switch (data) {
	case DRV_XHCI_CONTROL_NO_DATA:
		trt = 0;
		break;
	case DRV_XHCI_CONTROL_DATA_OUT:
		trt = 2U;
		break;
	case DRV_XHCI_CONTROL_DATA_IN:
		trt = 3U;
		break;
	default:
		return 0;
	}
	words->parameter_low = (uint32_t)setup;
	words->parameter_high = (uint32_t)(setup >> 32);
	words->status = 8U;
	words->control = DRV_XHCI_TRB_TYPE(2) | DRV_XHCI_TRB_IDT |
	    (trt << 16);
	return 1;
}

static inline int
drv_xhci_control_data_words(uint64_t address, uint32_t length,
	enum drv_xhci_control_data data, struct drv_xhci_trb_words *words)
{
	if (words == NULL || length == 0 ||
	    length > DRV_XHCI_CONTROL_DATA_MAX_LENGTH ||
	    (data != DRV_XHCI_CONTROL_DATA_OUT &&
	     data != DRV_XHCI_CONTROL_DATA_IN))
		return 0;
	words->parameter_low = (uint32_t)address;
	words->parameter_high = (uint32_t)(address >> 32);
	words->status = length;
	words->control = DRV_XHCI_TRB_TYPE(3) |
	    (data == DRV_XHCI_CONTROL_DATA_IN ?
		DRV_XHCI_TRB_DIR_IN | DRV_XHCI_TRB_ISP : 0U);
	return 1;
}

/* An IN Control TD uses Setup, Data, Status.  With ISP on Data, completion
 * code 13 reports the real short length but Status IOC remains the terminal
 * event for the TD. */
static inline int
drv_xhci_control_short_data_event(int input, unsigned trb_offset,
	unsigned trb_count, unsigned completion_code)
{
	return input && trb_count == 3U && trb_offset == 1U &&
	    completion_code == 13U;
}

static inline int
drv_xhci_control_status_words(enum drv_xhci_control_data data,
	struct drv_xhci_trb_words *words)
{
	uint32_t direction;

	if (words == NULL)
		return 0;
	switch (data) {
	case DRV_XHCI_CONTROL_NO_DATA:
	case DRV_XHCI_CONTROL_DATA_OUT:
		direction = DRV_XHCI_TRB_DIR_IN;
		break;
	case DRV_XHCI_CONTROL_DATA_IN:
		direction = 0;
		break;
	default:
		return 0;
	}
	words->parameter_low = 0;
	words->parameter_high = 0;
	words->status = 0;
	words->control = DRV_XHCI_TRB_TYPE(4) | DRV_XHCI_TRB_IOC |
	    direction;
	return 1;
}

/*
 * USB 2.x descriptors encode bMaxPacketSize0 in bytes.  USB 3.x encodes
 * it as an exponent; the only valid value for EP0 is 9 (512 bytes).
 */
static inline int
drv_xhci_ep0_max_packet_size(enum drv_usb_speed speed, uint8_t encoded,
	uint16_t *packet_size)
{
	uint16_t decoded = 0;

	if (packet_size == NULL)
		return 0;
	switch (speed) {
	case DRV_USB_SPEED_LOW:
		if (encoded == 8U)
			decoded = 8U;
		break;
	case DRV_USB_SPEED_FULL:
		if (encoded == 8U || encoded == 16U || encoded == 32U ||
		    encoded == 64U)
			decoded = encoded;
		break;
	case DRV_USB_SPEED_HIGH:
		if (encoded == 64U)
			decoded = 64U;
		break;
	case DRV_USB_SPEED_SUPER:
	case DRV_USB_SPEED_SUPER_PLUS:
		if (encoded == 9U)
			decoded = 512U;
		break;
	default:
		break;
	}
	*packet_size = decoded;
	return decoded != 0;
}

static inline int
drv_xhci_ep0_context(uint16_t packet_size, uint64_t dequeue,
	struct drv_xhci_ep0_context_words *context)
{
	if (context == NULL || packet_size == 0 || packet_size > 1024U ||
	    (dequeue & UINT64_C(0xe)) != 0)
		return 0;
	context->words[0] = 0;
	context->words[1] = (3U << 1) | (4U << 3) |
	    ((uint32_t)packet_size << 16);
	context->words[2] = (uint32_t)dequeue;
	context->words[3] = (uint32_t)(dequeue >> 32);
	context->words[4] = DRV_XHCI_CONTROL_AVERAGE_TRB_LENGTH;
	return 1;
}

static inline enum drv_xhci_port_reset_decision
drv_xhci_port_reset_status(uint32_t portsc)
{
	if (portsc == UINT32_MAX)
		return DRV_XHCI_PORT_RESET_INVALID;
	/* A latched connection-status edge may represent a fast detach/reinsert
	 * even when CCS is set again.  Never accept reset completion for the old
	 * device generation across that edge. */
	if ((portsc & DRV_XHCI_PORTSC_CSC) != 0)
		return DRV_XHCI_PORT_RESET_DISCONNECTED;
	if ((portsc & DRV_XHCI_PORTSC_CCS) == 0)
		return DRV_XHCI_PORT_RESET_DISCONNECTED;
	if ((portsc & DRV_XHCI_PORTSC_PR) != 0 ||
	    (portsc & DRV_XHCI_PORTSC_PED) == 0 ||
	    (portsc & DRV_XHCI_PORTSC_PRC) == 0 ||
	    (portsc & DRV_XHCI_PORTSC_PLS_MASK) != 0)
		return DRV_XHCI_PORT_RESET_WAIT;
	return DRV_XHCI_PORT_RESET_SUCCESS;
}

#endif
