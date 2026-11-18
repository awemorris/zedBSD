/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The USB transport of the Bluetooth controllers (ws143-p002, usb-bt.c):
 * an interface of class E0/01/01 published through the HCI class
 * (drivers/generic/bt-hci.h) as /dev/bluetoothN.
 */

#ifndef KERN_DRIVERS_USB_BT_H
#define KERN_DRIVERS_USB_BT_H

/* The interface class, subclass and protocol of a Bluetooth controller's HCI. */
#define USB_BT_INTERFACE_CLASS		0xe0U
#define USB_BT_INTERFACE_SUBCLASS	0x01U
#define USB_BT_INTERFACE_PROTOCOL	0x01U

int drv_usb_bt_driver_register(void);

#endif
