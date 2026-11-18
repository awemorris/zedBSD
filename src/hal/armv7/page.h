/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The armv7 physical memory: where RAM is, for the rest of the HAL.
 */

#ifndef KERN_HAL_ARMV7_PAGE_H
#define KERN_HAL_ARMV7_PAGE_H

#include <hal/types.h>

void
armv7_page_init(
	void);

uint64_t
armv7_ram_base(
	void);

uint64_t
armv7_ram_end(
	void);

bool
armv7_range_is_ram(
	uint64_t physical,
	uint64_t size);

bool
armv7_range_touches_ram(
	uint64_t physical,
	uint64_t size);

void
armv7_task_memory_stats(
	uint32_t *count,
	size_t *bytes);

#endif
