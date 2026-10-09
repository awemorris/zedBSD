/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Raspberry Pi 4 firmware property interface.
 *
 * The VideoCore firmware answers requests the ARM side posts through the
 * property channel of the BCM2835 mailbox.  The kernel uses it after boot for
 * what only the firmware can do, such as loading the USB controller's
 * firmware after its PCIe reset.
 */

#ifndef KERN_DRIVERS_RPI4_FIRMWARE_H
#define KERN_DRIVERS_RPI4_FIRMWARE_H

#include <stdint.h>

#include <drivers/generic/fdt.h>
#include <drivers/pci/pci.h>

/* Asks the firmware to load the VL805's firmware after a PCIe reset. */
#define DRV_RPI4_FIRMWARE_TAG_NOTIFY_XHCI_RESET	0x00030058U

/* The most value words one request carries. */
#define DRV_RPI4_FIRMWARE_MAX_VALUES		8U

/*
 * Finds the mailbox in the device tree and prepares the request buffer.
 * Returns an errno; a second call does nothing.
 */
int drv_rpi4_firmware_init(const struct drv_fdt *fdt);

/*
 * Sends one property tag and waits for the answer.
 *
 * values holds request_count words on entry and the answer on return;
 * capacity is how many words the tag's value buffer has.  answered reports
 * how many bytes the firmware wrote.  A notification without values uses
 * request_count=capacity=0 and may pass values=NULL; answered is required.
 * Returns EIO when the firmware refused
 * the request, ENOTSUP when it did not understand the tag and ETIMEDOUT when
 * it did not answer.
 */
int drv_rpi4_firmware_property(
	uint32_t tag,
	uint32_t *values,
	unsigned request_count,
	unsigned capacity,
	uint32_t *answered);

/*
 * Asks the firmware to load the USB controller's firmware at a PCI address.
 */
int drv_rpi4_firmware_notify_xhci_reset(const struct drv_pci_address *address);

#endif
