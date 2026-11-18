/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Raspberry Pi 5's USB ports: RP1's two DWC3 controllers in host mode,
 * each an xHCI.
 *
 * The controllers are not PCI functions of their own, so each is shown to
 * the PCI core as a function of a bus of its own making, whose BAR is the
 * controller's registers; the xHCI driver attaches to it as to any other.
 */

#ifndef KERN_DRIVERS_PLATFORM_RPI5_RP1_USB_H
#define KERN_DRIVERS_PLATFORM_RPI5_RP1_USB_H

/*
 * Starts the USB core and RP1's controllers.  drv_rp1_init() must have
 * succeeded.  Returns 0, or ENODEV when RP1 has no USB controller.
 */
int drv_rp1_usb_init(void);

/*
 * Finishes discovery once interrupts are enabled: starts serving the
 * controllers' interrupts and has them look at their root ports.
 */
void drv_rp1_usb_refresh(void);

#endif
