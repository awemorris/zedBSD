/* MC68030 conservative whole-cache HAL operations. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include <hal/hal.h>
#include "mmu030.h"

void
hal_mb(void)
{
	hal_compiler_barrier();
}

void hal_rmb(void) { hal_mb(); }
void hal_wmb(void) { hal_mb(); }
void hal_io_mb(void) { hal_mb(); }
void hal_io_rmb(void) { hal_mb(); }
void hal_io_wmb(void) { hal_mb(); }

void
hal_icache_invalidate_range(uintptr_t address, size_t size)
{
	(void)address;
	(void)size;
	hal_compiler_barrier();
	m68k030_icache_clear_all();
	hal_compiler_barrier();
}

void
hal_dcache_clean_range(uintptr_t address, size_t size)
{
	/* The 68030 data cache is write-through; there are no dirty cache lines
	 * to push.  The barriers retain the API's publication ordering. */
	(void)address;
	(void)size;
	hal_compiler_barrier();
}

void
hal_dcache_invalidate_range(uintptr_t address, size_t size)
{
	(void)address;
	(void)size;
	hal_compiler_barrier();
	m68k030_dcache_clear_all();
	hal_compiler_barrier();
}

void
hal_dcache_clean_invalidate_range(uintptr_t address, size_t size)
{
	hal_dcache_clean_range(address, size);
	hal_dcache_invalidate_range(address, size);
}

void
hal_sync_instruction_stream(void *address, size_t size)
{
	hal_dcache_clean_range((uintptr_t)address, size);
	hal_icache_invalidate_range((uintptr_t)address, size);
}
