/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The ODROID-C5 (Amlogic S905X5M): its name in the HAL's messages and
 * handoff objects, and the peripherals the HAL reaches at fixed addresses.
 */

#ifndef KERN_HAL_ARM64_BSP_S905X5M_BOARD_H
#define KERN_HAL_ARM64_BSP_S905X5M_BOARD_H

/*
 * The board's name in the HAL's boot messages, and the prefix of the
 * handoff objects the kernel reads ("odroidc5.fdt").
 */
#define ARM64_BOARD_TAG "ODROID-C5"
#define ARM64_BOARD_HANDOFF "odroidc5"

/*
 * The peripherals the HAL reaches before it reads the device tree: the
 * debug UART (uart_B, the board's UART connector, which the bootloader
 * has already started at its console rate).  It sits in the gigabyte from
 * 0xc000_0000 that locore.S maps as device memory.
 *
 * Values referenced: Hardkernel Linux odroids7d-6.12.y
 * common_drivers arch/arm64/boot/dts/amlogic/meson-s7d.dtsi (apb4's
 * ranges and uart_B's reg).
 */
#define S905X5M_DEBUG_UART_BASE 0xfe07a000ULL

#endif
