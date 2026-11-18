/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The i386 board bootstrap contract.
 */

#ifndef KERN_HAL_I386_BSP_H
#define KERN_HAL_I386_BSP_H

void bsp_boot_init(const void *raw_boot_info);

/*
 * Opens the PC-98 gate on memory above 16 MB.
 *
 * The board reports that memory in its work area whether or not the
 * gate is open, so this must run before the physical page map counts
 * it. Boards without the gate do not define this.
 */
void hal_pc98_enable_high_memory(void);
const void *bsp_kernel_handoff(const void *raw_boot_info);

#endif
