/*
 * PCI xHCI control-transfer and enumeration arithmetic
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */
#ifndef ZEDBSD_DRIVERS_PCI_XHCI_CONTROL_H
#define ZEDBSD_DRIVERS_PCI_XHCI_CONTROL_H

#include <drivers/usb.h>

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

#define DRV_XHCI_PORTSC_CCS	0x00000001U
#define DRV_XHCI_PORTSC_PED	0x00000002U
#define DRV_XHCI_PORTSC_PR	0x00000010U
#define DRV_XHCI_PORTSC_PLS_MASK	0x000001e0U
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
 * total service-interval payload.  Other endpoint kinds retain the legacy
 * encoding while their wider periodic rules remain outside this contract.
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
	unsigned periodic;
	unsigned interrupt;

	if (context == NULL || type < 1U || type > 7U)
		return 0;
	periodic = type == 1U || type == 3U || type == 5U || type == 7U;
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
		if (payload == 0 || payload > capacity || payload > 16384U)
			return 0;
		interval = descriptor_interval - 1U;
		encoded.word0 = (uint32_t)interval << 16;
		encoded.word1 = drv_xhci_endpoint_context_word1(type, packet,
		    maximum_burst);
		encoded.word4 = ((uint32_t)payload << 16) | payload;
		*context = encoded;
		return 1;
	}
	if (periodic) {
		if (speed >= DRV_USB_SPEED_HIGH) {
			interval = descriptor_interval != 0 ?
			    descriptor_interval - 1U : 0;
			if (interval > 15U)
				interval = 15U;
		} else {
			microframes = (descriptor_interval != 0 ?
			    descriptor_interval : 1U) * 8U;
			for (interval = 0;
			    (1U << interval) < microframes && interval < 15U;
			    interval++)
				;
		}
	}
	encoded.word0 = (uint32_t)(interval & 0xffU) << 16;
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
