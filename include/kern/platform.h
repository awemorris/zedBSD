/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef BOOTS_KERN_PLATFORM_H
#define BOOTS_KERN_PLATFORM_H

#include <stddef.h>
#include <stdint.h>
#include "kern/boot.h"

struct disk;
struct boots_filesystem;

#define KERN_PLATFORM_MAX_DEVICES 12U

size_t kern_platform_init(const struct boots_handoff *handoff,
			  struct boots_device *devices, size_t capacity);
void kern_platform_refresh_devices(const struct boots_device *devices,
				   size_t count);
struct disk *kern_platform_block_device(
	const struct boots_device *device);
int kern_platform_boot_linux(struct boots_filesystem *filesystem,
			     const char *path, const char *arguments,
			     const struct boots_device *devices, unsigned count,
			     int boot_device);
int kern_platform_graphics_init(uint64_t (*milliseconds)(void *),
				int (*key_state)(void *, int),
				void (*drain)(void *));
void kern_platform_restore_text(void);
void kern_platform_debug_write(const char *text);
void kern_platform_halt(void) __attribute__((noreturn));
void kern_platform_reboot(void) __attribute__((noreturn));

#endif
