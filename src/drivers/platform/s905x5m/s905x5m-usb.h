/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The S905X5M's USB host: its xHCI controller with the USB 2 PHY and the
 * glue in front of it, behind which the ODROID-C5's hub has the four
 * ports.
 */

#ifndef KERN_DRIVERS_PLATFORM_S905X5M_USB_H
#define KERN_DRIVERS_PLATFORM_S905X5M_USB_H

#include <stdint.h>

/*
 * Brings up the USB host the device tree at fdt_phys names and hands the
 * controller to the xHCI driver.  Returns 0 on success or a negative
 * errno; the boot goes on without USB on failure.
 */
int drv_s905x5m_usb_init(uint64_t fdt_phys);

/* Has the controller find the devices on its ports, once interrupts are on. */
void drv_s905x5m_usb_refresh(void);

#endif
