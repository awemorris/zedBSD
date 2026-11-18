/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The riscv64 physical memory: where RAM is, for the rest of the HAL.
 */

#ifndef KERN_HAL_RISCV64_PAGE_H
#define KERN_HAL_RISCV64_PAGE_H

#include <hal/types.h>

void
riscv64_page_init(
	void);

uint64_t
riscv64_ram_base(
	void);

uint64_t
riscv64_ram_end(
	void);

bool
riscv64_range_is_ram(
	uint64_t physical,
	uint64_t size);

bool
riscv64_range_touches_ram(
	uint64_t physical,
	uint64_t size);

void
riscv64_task_memory_stats(
	uint32_t *count,
	size_t *bytes);

#endif
