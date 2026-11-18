/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * What the riscv32 HAL learned about the machine at boot, and what it
 * hands to the kernel.
 */

#ifndef KERN_HAL_RISCV32_BOOTINFO_H
#define KERN_HAL_RISCV32_BOOTINFO_H

#include <hal/types.h>
#include "fdt.h"

/*
 * A physical range handed to the kernel by name: the device tree or the
 * initial ramdisk.
 */
struct riscv32_handoff_range {
	uint64_t phys;
	uint64_t size;
};

void
riscv32_boot_record(
	uint64_t hart_id,
	uintptr_t fdt_phys,
	const struct riscv32_fdt_info *info);

const struct riscv32_fdt_info *
riscv32_boot_fdt(
	void);

uintptr_t
riscv32_boot_fdt_phys(
	void);

uint64_t
riscv32_boot_hart(
	void);

const char *
riscv32_boot_command_line(
	void);

unsigned
riscv32_boot_parameter_number(
	const char *key);

const void *
riscv32_kernel_handoff(
	void);

#endif
