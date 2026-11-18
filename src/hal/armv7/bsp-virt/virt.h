/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The parts of the emulator's virt board the board's files share: its
 * PL011 UART and its firmware calls.
 */

#ifndef KERN_HAL_ARMV7_BSP_VIRT_VIRT_H
#define KERN_HAL_ARMV7_BSP_VIRT_VIRT_H

#include <hal/types.h>
#include "../fdt.h"

/*
 * The physical address of the board's first PL011, which the console uses
 * before the device tree is read.
 *
 * Values referenced: QEMU v10.0.11 hw/arm/virt.c, the memory map's
 * VIRT_UART0.
 */
#define VIRT_UART_PHYS		0x09000000U
#define VIRT_UART_BYTES		0x1000U

int virt_uart_init(uint64_t physical);
void virt_uart_putc(int character);
int virt_uart_getc(void);
void virt_psci_init(const struct armv7_fdt_info *info);
void virt_psci_system_reset(void);
void virt_psci_system_off(void);

#endif
