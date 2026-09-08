/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Kernel panic and the console sink used by the kernel's libc subset.
 */

#include "hal/hal.h"
#include <stddef.h>

/*
 * Writes bytes from the kernel's stdio subset to the HAL console.
 */
size_t
__stdio_console_write(
	const char *bytes,
	size_t length)
{
	/* Writes the bytes unmodified. */
	hal_cons_write_n(bytes, (unsigned)length);

	/* Reports every byte as written. */
	return length;
}

/*
 * Reports a fatal kernel error and halts the machine.
 */
__attribute__((noreturn)) void
__libc_panic(
	const char *message)
{
	/* Prints the panic message, substituting a marker for a missing one. */
	hal_cons_write("kernel panic: ");
	hal_cons_write(message != NULL ? message : "unknown");
	hal_cons_write("\n");
	hal_cons_update_cursor();

	/* Halts permanently with interrupts disabled. */
	(void)hal_irq_disable();
	for (;;)
		hal_halt();
}
