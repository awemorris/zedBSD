/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "hal/hal.h"
#include <stddef.h>

size_t
__stdio_console_write(const char *bytes, size_t length)
{
	hal_cons_write_n(bytes, (unsigned)length);
	return length;
}

__attribute__((noreturn)) void
__libc_panic(const char *message)
{
	hal_cons_write("kernel panic: ");
	hal_cons_write(message != NULL ? message : "unknown");
	hal_cons_write("\n");
	hal_cons_update_cursor();
	(void)hal_irq_disable();
	for (;;)
		hal_halt();
}
