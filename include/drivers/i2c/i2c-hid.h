/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The HID over I2C driver (ws159-p003): finds the ACPI PNP0C50 devices and
 * publishes their touch pads and touch screens as input devices.
 */

#ifndef DRIVERS_I2C_I2C_HID_H
#define DRIVERS_I2C_I2C_HID_H

int
drv_i2c_hid_probe(void);

#endif
