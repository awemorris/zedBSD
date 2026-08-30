/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Platform initialization
 */

#ifndef ZEDBSD_KERN_PLATFORM_H
#define ZEDBSD_KERN_PLATFORM_H

#include <stddef.h>
#include <stdint.h>
#include "kern/boot.h"

struct disk;

#define KERN_PLATFORM_MAX_DEVICES	12U

size_t
kern_platform_init(
	const struct boot_handoff *handoff,
	struct boot_device *devices,
	size_t capacity);

void
kern_platform_refresh_devices(
	const struct boot_device *devices,
	size_t count);

int
kern_platform_input_init(void);

struct disk *
kern_platform_block_device(
	const struct boot_device *device);

void
kern_platform_debug_write(
	const char *text);

void
kern_platform_halt(void) __attribute__((noreturn));

void
kern_platform_reboot(void) __attribute__((noreturn));

#endif
