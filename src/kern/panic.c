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
#include "kern/klog.h"
#include "kern/text-display.h"
#include <stddef.h>
#include <kern/kcrt.h>

/*
 * Writes bytes from the kernel's stdio subset to the HAL console.
 */
size_t
__stdio_console_write(
	const char *bytes,
	size_t length)
{
	size_t index;

	/* Writes the bytes unmodified through the character path. */
	for (index = 0; index < length; index++)
		hal_putc((unsigned char)bytes[index]);

	/* Reports every byte as written. */
	return length;
}

/*
 * Writes a terminated string through the console character path.
 */
static void
panic_puts(
	const char *string)
{
	/* Emits every byte in order. */
	while (*string != '\0')
		hal_putc((unsigned char)*string++);
}

/*
 * Reports a fatal kernel error and halts the machine.
 */
__attribute__((noreturn)) void
__libc_panic(
	const char *message)
{
	char record[256];
	const char *reason;

	/* Nothing on this CPU interrupts the stop from here on. */
	(void)hal_irq_disable();

	/* Names a panic that came without a message. */
	reason = message;
	if (reason == NULL)
		reason = "unknown";

	/* Keeps the reason in the ring, where a debugger or a later dmesg reads it (BUG-158). */
	(void)kern_snprintf(record, sizeof(record), "kernel panic: %s\n", reason);
	kern_log_record_fatal(record);

	/* The console is shown for the panic, over a quiet boot or a graphics mode. */
	kern_text_reveal_fatal();

	/* Prints the panic message, substituting a marker for a missing one. */
	panic_puts("kernel panic: ");
	panic_puts(reason);
	panic_puts("\n");

	/* Halts permanently with interrupts disabled. */
	(void)hal_irq_disable();
	for (;;)
		hal_halt();
}

/*
 * Reports a fatal kernel condition and stops the machine.
 */
void
kern_fatal(
	const char *file,
	int line,
	const char *message)
{
	char record[256];
	const char *site;
	const char *reason;

	/* Nothing on this CPU interrupts the stop from here on. */
	(void)hal_irq_disable();

	/* Names a stop that came without a site or a message. */
	site = file;
	if (site == NULL)
		site = "?";
	reason = message;
	if (reason == NULL)
		reason = "unknown";

	/* Keeps the site and the reason in the ring (BUG-158). */
	(void)kern_snprintf(record, sizeof(record), "fatal: %s:%d: %s\n",
			    site, line, reason);
	kern_log_record_fatal(record);

	/* The console is shown for the stop, over a quiet boot or a graphics mode. */
	kern_text_reveal_fatal();

	/* The HAL owns the stop; it records the site and never returns. */
	hal_fatal(file, line, message);
}
