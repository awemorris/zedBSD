/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include <hal/hal.h>
#include "../bsp.h"

void bsp_boot_init(const void *raw_boot_info) { (void)raw_boot_info; }

void *
hal_get_arch_handoff(const char *name)
{
	(void)name;
	return NULL;
}

const void *bsp_kernel_handoff(const void *raw_boot_info)
{
	return raw_boot_info;
}
