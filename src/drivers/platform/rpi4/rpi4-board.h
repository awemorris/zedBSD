/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Raspberry Pi 4 board identity: a real board or QEMU's raspi4b.
 */

#ifndef KERN_DRIVERS_RPI4_BOARD_H
#define KERN_DRIVERS_RPI4_BOARD_H

#include <stdbool.h>

#include <drivers/generic/fdt.h>

/*
 * Reports whether the device tree was handed over by QEMU's raspi4b rather
 * than by a board's firmware.  A driver of a block QEMU does not emulate
 * (PCIe, PWM1) leaves it alone when this is true, because the first access
 * to its registers faults there.
 */
bool drv_rpi4_board_emulated(const struct drv_fdt *fdt);

#endif
