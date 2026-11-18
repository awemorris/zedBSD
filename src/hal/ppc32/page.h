/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The ppc32 physical memory: where RAM is, for the rest of the HAL.
 */

#ifndef KERN_HAL_PPC32_PAGE_H
#define KERN_HAL_PPC32_PAGE_H

#include <hal/types.h>

void
ppc32_page_init(
	uint32_t kernel_end);

uint64_t
ppc32_ram_base(
	void);

uint64_t
ppc32_ram_end(
	void);

bool
ppc32_range_is_ram(
	uint64_t physical,
	uint64_t size);

bool
ppc32_range_touches_ram(
	uint64_t physical,
	uint64_t size);

void
ppc32_task_memory_stats(
	uint32_t *count,
	size_t *bytes);

#endif
