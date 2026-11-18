/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The m68040 physical memory: where RAM is, for the rest of the HAL.
 */

#ifndef KERN_HAL_M68040_PAGE_H
#define KERN_HAL_M68040_PAGE_H

#include <hal/types.h>

void m68040_page_init(uint32_t kernel_end);
uint64_t m68040_ram_base(void);
uint64_t m68040_ram_end(void);
bool m68040_range_is_ram(uint64_t physical, uint64_t size);
bool m68040_range_touches_ram(uint64_t physical, uint64_t size);
void m68040_task_memory_stats(uint32_t *count, size_t *bytes);

#endif
