/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * What the armv7 HAL learned about the machine at boot, and what it hands
 * to the kernel.
 */

#ifndef KERN_HAL_ARMV7_BOOTINFO_H
#define KERN_HAL_ARMV7_BOOTINFO_H

#include <hal/types.h>
#include "fdt.h"

/*
 * A physical range handed to the kernel by name: the device tree or the
 * initial ramdisk.
 */
struct armv7_handoff_range {
	uint64_t phys;
	uint64_t size;
};

void
armv7_boot_record(
	uintptr_t fdt_phys,
	const struct armv7_fdt_info *info);

const struct armv7_fdt_info *
armv7_boot_fdt(
	void);

uintptr_t
armv7_boot_fdt_phys(
	void);

const char *
armv7_boot_command_line(
	void);

int
armv7_boot_has_parameter(
	const char *name);

const void *
armv7_kernel_handoff(
	void);

#endif
