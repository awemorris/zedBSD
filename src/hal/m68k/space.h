/* MC68030 user address spaces, private to the m68k HAL. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#ifndef ZEDBSD_HAL_M68K_SPACE_H
#define ZEDBSD_HAL_M68K_SPACE_H

#include <hal/hal.h>
#include "mmu030.h"

#define M68K030_SPACE_MAGIC 0x4d335350U
#define M68K030_DIRECT_BASE 0x80000000U
#define M68K030_USER_LIMIT  0x80000000U
#define M68K030_DIRECT_SIZE 0x01000000U

struct m68k030_table_page {
	uintptr_t virtual_base;
	struct hal_pmem memory;
	uint32_t *entries;
	struct m68k030_table_page *next;
};

struct m68k030_space {
	uint32_t magic;
	uint32_t space_id;
	struct hal_pmem root_memory;
	uint32_t *root;
	struct m68k030_root_pointer root_pointer;
	struct m68k030_table_page *tables;
};

static inline void *
m68k030_phys_to_direct(uintptr_t physical)
{
	return (void *)(M68K030_DIRECT_BASE + physical);
}

static inline uintptr_t
m68k030_direct_to_phys(const void *address)
{
	return (uintptr_t)address - M68K030_DIRECT_BASE;
}

void m68k030_space_init(uintptr_t empty_root_physical);
void m68k030_page_init(void);

#endif
