/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "../bsp.h"

void bsp_boot_init(const void *raw_boot_info) { (void)raw_boot_info; }
const void *bsp_kernel_handoff(const void *raw_boot_info)
{
	return raw_boot_info;
}
