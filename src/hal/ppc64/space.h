/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The 64-bit PowerPC address spaces.
 */

#ifndef KERN_HAL_PPC64_SPACE_H
#define KERN_HAL_PPC64_SPACE_H

#include <hal/hal.h>

#define PPC64_SPACE_MAGIC	0x50363453U

/* One leaf table of a space's own record: its page and the next one. */
struct ppc64_table_page {
	hal_physaddr_t memory;
	struct ppc64_table_page *next;
};

struct ppc64_space {
	uint32_t magic;
	uint32_t space_id;
	unsigned destroying;
	struct ppc64_space *registry_next;
	hal_physaddr_t root_memory;
	uint64_t *root;
	struct ppc64_table_page *tables;
};

struct ppc64_board_info;

void
ppc64_space_init(
	const struct ppc64_board_info *board);

uint64_t
ppc64_space_ram_limit(
	void);

int
ppc64_space_reload(
	uintptr_t address,
	int store,
	int execute);

void
ppc64_space_memory_stats(
	uint32_t *spaces,
	uint32_t *tables);

#endif
