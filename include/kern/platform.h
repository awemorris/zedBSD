/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Platform initialization
 */

#ifndef KERN_KERN_PLATFORM_H
#define KERN_KERN_PLATFORM_H

#include <stddef.h>
#include <stdint.h>
#include "kern/boot.h"

struct disk;

#define KERN_PLATFORM_MAX_DEVICES	12U

size_t
kern_platform_init(
	const struct kern_boot_handoff *handoff,
	struct kern_boot_device *devices,
	size_t capacity);

void
kern_platform_refresh_devices(
	const struct kern_boot_device *devices,
	size_t count);

int
kern_platform_input_init(void);

struct disk *
kern_platform_block_device(
	const struct kern_boot_device *device);

void
kern_platform_debug_write(
	const char *text);

void
kern_platform_halt(void) __attribute__((noreturn));

void
kern_platform_reboot(void) __attribute__((noreturn));

/*
 * Turn the machine's power off.
 *
 * It returns only when the platform could not: EOPNOTSUPP when it has no
 * way to cut its power, or the error the attempt reported.  The caller
 * then halts instead.
 */
int
kern_platform_poweroff(void);

/*
 * Stop the platform's own device threads before a shutdown or reboot.
 *
 * Called by system_shutdown_prepare() once the volumes are synced and
 * before the network stops, so a device that a platform thread is still
 * bringing up is not published while the network shuts down.  Optional:
 * it is weak, and a platform without such threads does not define it.
 * Thread context.
 */
void
kern_platform_shutdown_prepare(void) __attribute__((weak));

/*
 * Look up one boot handoff object by name.
 *
 * The object stays owned by the platform. An unknown or unavailable
 * name reports NULL.
 */
void *kern_boot_handoff(const char *name);

/*
 * Write one character to the early console.
 *
 * Before the kernel console exists this is the platform's own early
 * console, the firmware's or a serial line; once the kernel console is
 * published the character is shown there instead.
 */
void kern_console_early_putc(int character);

/*
 * The screen the firmware or the boot loader left on, as the kernel reads it.
 *
 * One instance is a copy the caller owns, filled by kern_boot_framebuffer()
 * from the platform's named values; nothing in it points into the platform.
 * The pitch is the distance from one row to the next and the format the
 * pixel format, each in the unit and the code that the platform's
 * documentation of the name gives.
 */
struct kern_boot_framebuffer {
	uint64_t physical_base;
	uint64_t size;
	uint32_t width;
	uint32_t height;
	uint32_t pitch;
	uint32_t format;
};

/*
 * Read the boot framebuffer handed over under a name prefix such as
 * "pcat.framebuffer".
 *
 * Returns 0 with the framebuffer filled in, ENOENT when the platform
 * handed over none, or EINVAL for a missing argument, a prefix too long
 * for a name, or a value the record cannot hold.  A given record is
 * left cleared on error.
 */
int
kern_boot_framebuffer(
	const char *prefix,
	struct kern_boot_framebuffer *framebuffer);

#endif
