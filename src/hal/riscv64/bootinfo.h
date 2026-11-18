/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * What the riscv64 HAL learned about the machine at boot, and what it
 * hands to the kernel.
 */

#ifndef KERN_HAL_RISCV64_BOOTINFO_H
#define KERN_HAL_RISCV64_BOOTINFO_H

#include <hal/types.h>
#include "fdt.h"
#include "loader.h"

/*
 * A physical range handed to the kernel by name: the device tree or the
 * initial ramdisk.
 */
struct riscv64_handoff_range {
	uint64_t phys;
	uint64_t size;
};

void
riscv64_boot_record(
	uint64_t hart_id,
	uintptr_t fdt_phys,
	const struct riscv64_fdt_info *info);

void
riscv64_boot_set_command_line(
	const char *text,
	size_t available);

int
riscv64_boot_take_loader(
	const struct zbl6_riscv64_handoff *handoff);

const struct riscv64_framebuffer *
riscv64_boot_framebuffer(
	void);

const struct riscv64_fdt_range *
riscv64_boot_firmware_reserved(
	unsigned *count);

const struct riscv64_fdt_info *
riscv64_boot_fdt(
	void);

uintptr_t
riscv64_boot_fdt_phys(
	void);

uint64_t
riscv64_boot_hart(
	void);

const char *
riscv64_boot_command_line(
	void);

bool
riscv64_boot_parameter_given(
	const char *word);

unsigned
riscv64_boot_parameter_number(
	const char *key);

bool
riscv64_boot_name_is(
	const char *name,
	const char *wanted);

const void *
riscv64_kernel_handoff(
	void);

#endif
