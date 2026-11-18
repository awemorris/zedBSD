/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Raspberry Pi 4 (BCM2711): its name in the HAL's messages and handoff
 * objects, and the peripherals the HAL reaches at fixed addresses.
 */

#ifndef KERN_HAL_ARM64_BSP_RPI4_BOARD_H
#define KERN_HAL_ARM64_BSP_RPI4_BOARD_H

/*
 * The board's name in the HAL's boot messages, and the prefix of the
 * handoff objects the kernel reads ("rpi4.fdt").
 */
#define ARM64_BOARD_TAG "RPI4"
#define ARM64_BOARD_HANDOFF "rpi4"

/*
 * The peripherals the HAL reaches before or without the device tree, at
 * the firmware's default low-peripheral addresses: the GPIO block, the
 * PL011 on GPIO 14 and 15, and the power manager whose watchdog resets
 * the board.
 */
#define RPI4_GPIO_BASE 0xfe200000ULL
#define RPI4_PL011_BASE 0xfe201000ULL
#define RPI4_PM_BASE 0xfe100000ULL

/*
 * Where the VideoCore sees the ARM's memory through the mailbox: from
 * 0xc000_0000, its uncached alias.
 */
#define RPI_BOARD_MAILBOX_BUS_ALIAS 0xc0000000U

#endif
