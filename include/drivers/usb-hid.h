/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_DRIVERS_USB_HID_H
#define ZEDBSD_DRIVERS_USB_HID_H

int drv_usb_hid_driver_register(void);
void drv_usb_hid_input_ready(void);

#endif
