/*
 * USB Attached SCSI descriptor capabilities.
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */
#ifndef ZEDBSD_DRIVERS_USB_UAS_H
#define ZEDBSD_DRIVERS_USB_UAS_H

#include <stddef.h>
#include <stdint.h>

enum drv_usb_uas_profile {
	DRV_USB_UAS_HIGH_SPEED,
	DRV_USB_UAS_SUPER_SPEED
};

struct drv_usb_uas_pipe {
	uint8_t address;
	uint8_t maximum_burst;
	uint8_t stream_exponent;
	uint16_t maximum_packet;
};

struct drv_usb_uas_capabilities {
	/* Pipe Usage IDs 1 through 4: command, status, data-in, data-out. */
	struct drv_usb_uas_pipe pipes[4];
	uint8_t interface_number;
	uint8_t alternate_setting;
	uint8_t minimum_stream_exponent;
};

/*
 * Decodes one alternate from a complete configuration. A valid capability
 * description does not imply that the host implements its stream transport.
 * Failure clears the output; input and output must not overlap.
 */
int drv_usb_uas_decode_configuration(const void *raw, size_t length,
    unsigned interface_number, unsigned alternate_setting,
    enum drv_usb_uas_profile profile, struct drv_usb_uas_capabilities *result);

#endif
