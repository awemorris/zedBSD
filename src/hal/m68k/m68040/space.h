/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The MC68040 address spaces.
 */

#ifndef KERN_HAL_M68K_M68040_SPACE_H
#define KERN_HAL_M68K_M68040_SPACE_H

#include <hal/hal.h>

#define M68040_SPACE_MAGIC	0x4d343053U

/*
 * One table of a user space: its page, the entry of its parent that
 * points at it, and the next table of the space.  Every table takes a page
 * of its own, so its alignment is the page's.
 */
struct m68040_table_page {
	hal_physaddr_t memory;
	uint32_t *parent;
	unsigned parent_index;
	struct m68040_table_page *next;
};

struct m68040_space {
	uint32_t magic;
	int space_id;
	unsigned destroying;
	struct m68040_space *registry_next;
	hal_physaddr_t root_memory;
	uint32_t *root;
	struct m68040_table_page *tables;
};

void m68040_space_init(void);
void m68040_space_memory_stats(uint32_t *spaces, uint32_t *tables);

#endif
