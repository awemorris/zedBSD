/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The binding of the 68040 Macintosh (the Quadra 630 family, and the
 * emulator's Quadra 800).
 *
 * The machine publishes no boot device yet: the kernel starts and finds
 * no root.  Its console is the HAL's early console, which writes to the
 * screen the ROM set up; halting, rebooting and powering off go through
 * the HAL.
 */

#include <hal/hal.h>
#include <kern/boot.h>
#include <kern/platform.h>

/* Marks a parameter that the kernel's platform interface passes but this machine does not need. */
#define UNUSED_PARAMETER(parameter)	((void)(parameter))

/*
 * Publishes the boot devices of the machine: none yet.
 *
 * Returns the number of devices published.
 */
size_t
kern_platform_init(
	const struct kern_boot_handoff *handoff,
	struct kern_boot_device *devices,
	size_t capacity)
{
	UNUSED_PARAMETER(handoff);
	UNUSED_PARAMETER(devices);
	UNUSED_PARAMETER(capacity);

	/* Succeeded: the machine has no boot device to publish yet. */
	return 0;
}

/*
 * Finishes device discovery once interrupts are enabled; nothing to do.
 */
void
kern_platform_refresh_devices(
	const struct kern_boot_device *devices,
	size_t count)
{
	UNUSED_PARAMETER(devices);
	UNUSED_PARAMETER(count);
}

/*
 * Starts the machine's input devices; it has none the kernel drives yet.
 */
int
kern_platform_input_init(
	void)
{
	/* Succeeded: nothing to start. */
	return 0;
}

/*
 * Resolves a published boot device to its disk; the machine publishes none.
 */
struct disk *
kern_platform_block_device(
	const struct kern_boot_device *device)
{
	UNUSED_PARAMETER(device);

	/* No device has a disk. */
	return NULL;
}

/*
 * Writes a debug string to the early console.
 */
void
kern_platform_debug_write(
	const char *text)
{
	/* Ignores a missing string. */
	if (text == NULL)
		return;

	/* Emits every byte through the HAL. */
	while (*text != '\0') {
		hal_putc((unsigned char)*text);
		text++;
	}
}

/*
 * Halts the machine permanently.
 */
void
kern_platform_halt(
	void)
{
	/* Halts with interrupts disabled. */
	(void)hal_irq_disable();
	for (;;)
		hal_halt();
}

/*
 * Turns the power off through the firmware, halting if that returns.
 */
int
kern_platform_poweroff(
	void)
{
	/* The HAL's power-off does not return when the firmware can turn the power off. */
	hal_poweroff();
	for (;;)
		hal_halt();
}

/*
 * Reboots the machine through the firmware.
 */
void
kern_platform_reboot(
	void)
{
	/* Halts in case the reset request returns. */
	hal_reset();
	for (;;)
		hal_halt();
}
