/* Kernel-owned X68000 loader handoff. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include <hal/hal.h>
#include "bsp.h"

static struct x68k_boot_handoff kernel_handoff;

void
x68k_boot_init(const struct x68k_boot_handoff *handoff)
{
	if (!x68k_boot_handoff_valid(handoff))
		HAL_FATAL("invalid X68k boot handoff");
	hal_memcpy(&kernel_handoff, handoff, sizeof(kernel_handoff));
}

const struct x68k_boot_handoff *
x68k_boot_handoff(void)
{
	return &kernel_handoff;
}

void *
hal_get_arch_handoff(const char *name)
{
	(void)name;
	return NULL;
}
