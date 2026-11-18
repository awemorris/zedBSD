/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The riscv64 HAL's reading of the UEFI loader's handoff: which layout it
 * has, and what the HAL keeps from the second layout's fields.
 */

#ifndef KERN_HAL_RISCV64_LOADER_H
#define KERN_HAL_RISCV64_LOADER_H

#include <hal/types.h>
#include "fdt.h"
#include "bootloader/include/riscv64-handoff.h"

/* The most ranges of RAM the firmware keeps that the HAL holds. */
#define RISCV64_LOADER_MAX_RESERVED	16U

/* What reading the loader's handoff found. */
enum riscv64_loader_result {
	RISCV64_LOADER_OK = 0,
	RISCV64_LOADER_OLD_VERSION,
	RISCV64_LOADER_TOO_SHORT,
	RISCV64_LOADER_BAD_COUNT,
	RISCV64_LOADER_BAD_RANGE,
	RISCV64_LOADER_OVERFLOW,
	RISCV64_LOADER_TOO_MANY
};

/* What became of the screen the loader handed over. */
enum riscv64_loader_screen {
	RISCV64_LOADER_SCREEN_NONE = 0,
	RISCV64_LOADER_SCREEN_TAKEN,
	RISCV64_LOADER_SCREEN_REFUSED
};

/*
 * The screen the firmware left on, as the loader handed it over: the
 * physical address and size of its memory, its size in pixels, the bytes
 * from one row to the next and a KERN_BOOT_FRAMEBUFFER_* format.
 */
struct riscv64_framebuffer {
	uint64_t phys;
	uint64_t size;
	uint32_t width;
	uint32_t height;
	uint32_t pitch;
	uint32_t format;
};

/*
 * What the HAL keeps from a handoff of the second layout.
 *
 * One instance is filled by riscv64_loader_take() and kept by the boot
 * record; a handoff of the first layout leaves it empty.  The ranges are
 * the firmware's RAM that the device tree does not already reserve,
 * joined where they touch.
 */
struct riscv64_loader_extras {
	uint32_t layout;
	enum riscv64_loader_screen screen;
	struct riscv64_framebuffer framebuffer;
	uint32_t reserved_given;
	unsigned reserved_count;
	struct riscv64_fdt_range reserved[RISCV64_LOADER_MAX_RESERVED];
};

int
riscv64_loader_check(
	const struct zbl6_riscv64_handoff *handoff);

int
riscv64_loader_take(
	const struct zbl6_riscv64_handoff *handoff,
	const struct riscv64_fdt_info *info,
	struct riscv64_loader_extras *extras);

#endif
