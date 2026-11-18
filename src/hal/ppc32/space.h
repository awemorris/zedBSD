/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The 32-bit PowerPC address spaces.
 */

#ifndef KERN_HAL_PPC32_SPACE_H
#define KERN_HAL_PPC32_SPACE_H

#include <hal/hal.h>

#define PPC32_SPACE_MAGIC	0x50333253U

/* One leaf table of a space's own record: its page and the next one. */
struct ppc32_table_page {
	hal_physaddr_t memory;
	unsigned root_index;
	struct ppc32_table_page *next;
};

struct ppc32_space {
	uint32_t magic;
	uint32_t space_id;
	unsigned destroying;
	struct ppc32_space *registry_next;
	hal_physaddr_t root_memory;
	uint32_t *root;
	struct ppc32_table_page *tables;
	uint32_t segments[4];
};

void
ppc32_space_init(
	uint64_t ram_end,
	const uint32_t *windows,
	unsigned window_count);

uint64_t
ppc32_space_ram_end(
	void);

int
ppc32_space_reload(
	uintptr_t address,
	int store,
	int execute);

void
ppc32_space_memory_stats(
	uint32_t *spaces,
	uint32_t *tables);

#endif
