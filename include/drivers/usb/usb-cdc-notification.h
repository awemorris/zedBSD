/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The notifications of a USB CDC network function (ECM and NCM): the
 * stream of its interrupt endpoint cut into whole notifications.
 *
 * A function sends NetworkConnection (8 bytes) and ConnectionSpeedChange
 * (16 bytes) on its interrupt endpoint.  One transfer may carry the end of
 * one and the start of the next, and an endpoint of 8-byte packets splits
 * the speed's notification over two transfers (CDC 1.2 section 6.3,
 * CDC ECM 1.2 section 6.3, CDC NCM 1.0 section 7).  The reader joins the
 * transfers and hands out each notification once it is whole.
 */

#ifndef KERN_DRIVERS_USB_CDC_NOTIFICATION_H
#define KERN_DRIVERS_USB_CDC_NOTIFICATION_H

#include <stddef.h>
#include <stdint.h>

/* The request type of every notification: class, interface, device to host. */
#define DRV_USB_CDC_NOTIFICATION_REQUEST_TYPE	0xa1U

/* The notifications a network function sends. */
#define DRV_USB_CDC_NETWORK_CONNECTION		0x00U
#define DRV_USB_CDC_RESPONSE_AVAILABLE		0x01U
#define DRV_USB_CDC_CONNECTION_SPEED_CHANGE	0x2aU

/* A notification's header, and the longest notification the reader keeps. */
#define DRV_USB_CDC_NOTIFICATION_HEADER		8U
#define DRV_USB_CDC_NOTIFICATION_MAX		16U

/* The longest transfer a driver asks of the interrupt endpoint. */
#define DRV_USB_CDC_NOTIFICATION_TRANSFER	16U

/*
 * The bytes of the interrupt endpoint not yet handed out: the start of a
 * notification whose rest is still to come.  One per adapter, emptied
 * when the adapter opens and after a transfer that failed.
 */
struct drv_usb_cdc_notification_reader {
	size_t length;
	uint8_t bytes[2U * DRV_USB_CDC_NOTIFICATION_MAX];
};

/*
 * One whole notification: its code, its value (for NetworkConnection,
 * 1 connected and 0 not), the interface it names, and for
 * ConnectionSpeedChange the two directions' speeds in bits a second.
 */
struct drv_usb_cdc_notification {
	uint8_t code;
	uint16_t value;
	uint16_t interface_number;
	uint16_t data_length;
	uint32_t downstream_bps;
	uint32_t upstream_bps;
};

void drv_usb_cdc_notification_reset(struct drv_usb_cdc_notification_reader *reader);
void drv_usb_cdc_notification_add(struct drv_usb_cdc_notification_reader *reader, const uint8_t *bytes, size_t length);
int drv_usb_cdc_notification_next(struct drv_usb_cdc_notification_reader *reader, struct drv_usb_cdc_notification *notification);
size_t drv_usb_cdc_notification_transfer_length(uint16_t max_packet_size);

#endif
