/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_HAL_I386_BSP_H
#define ZEDBSD_HAL_I386_BSP_H

void bsp_boot_init(const void *raw_boot_info);
const void *bsp_kernel_handoff(const void *raw_boot_info);

#endif
