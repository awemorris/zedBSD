/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * USB CDC Ethernet Control Model (ECM)
 */

#ifndef KERN_DRIVERS_USB_CDC_ECM_H
#define KERN_DRIVERS_USB_CDC_ECM_H

/* Register the independent CDC ECM communication-interface driver. */
int
drv_usb_cdc_ecm_driver_register(void);

#endif
