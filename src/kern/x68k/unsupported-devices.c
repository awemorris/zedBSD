/* X68000 platform operations not available during native bring-up. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include <hal/hal.h>
#include <kern/boot-device.h>

int boot_device_register(void) { return 0; }
int kern_boot_pending(void) { return 0; }
void kern_boot_execute_pending(void)
{
	HAL_FATAL("X68k chain boot unavailable");
	for (;;)
		;
}
