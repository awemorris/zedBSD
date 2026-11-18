/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Raspberry Pi 4 onboard Bluetooth: the BCM43455's HCI on the PL011 UART
 * (ws215-p003).
 *
 * The chip's Bluetooth part talks HCI in the H4 framing on UART0, the
 * PL011, routed to GPIO 30 to 33 with hardware flow control; its power
 * line is BT_ON on the firmware's GPIO expander.  The driver publishes the
 * controller through the Bluetooth HCI class (/dev/bluetoothN); the daemon
 * loads the chip's firmware and changes the line's speed.
 */

#ifndef KERN_DRIVERS_RPI4_BT_H
#define KERN_DRIVERS_RPI4_BT_H

#include <stdint.h>

/*
 * Looks for the onboard Bluetooth and starts its thread.
 *
 * fdt_phys is the device tree's physical address.  Logs the firmware's
 * core, V3D and ARM clocks first on a board.  Returns ENODEV for QEMU's
 * raspi4b or a tree whose Bluetooth node is missing or disabled (the
 * disable-bt overlay), EBUSY when the PL011 is the serial console, and
 * EALREADY for a second call.  The thread powers the controller and
 * publishes it.  Thread context only.
 */
int drv_rpi4_bt_probe(uint64_t fdt_phys);

#endif
