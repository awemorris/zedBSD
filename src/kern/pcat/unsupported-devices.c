/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/boot-device.h"
#include <hal/hal.h>

int boot_device_register(void) { return 0; }
int kern_boot_pending(void) { return 0; }
void kern_boot_execute_pending(void)
{
	HAL_FATAL("PC/AT boot device unavailable");
	for (;;) ;
}
