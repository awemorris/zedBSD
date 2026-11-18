/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef KERN_HAL_ARM64_SPACE_H
#define KERN_HAL_ARM64_SPACE_H

#include <hal/hal.h>

#define ARM64_SPACE_MAGIC 0x41363453U

struct arm64_table_page {
	hal_physaddr_t memory;
	uint64_t *parent;
	unsigned parent_index;
	struct arm64_table_page *next;
};

struct arm64_space {
	uint32_t magic;
	int space_id;

	/* Held while an operation changes or installs the space; zero when free. */
	volatile unsigned lock;

	/* The holding CPU's number plus one, or zero; catches a nested operation. */
	unsigned lock_owner;

	unsigned destroying;
	struct arm64_space *registry_next;
	hal_physaddr_t l0_memory;
	uint64_t *l0;
	struct arm64_table_page *tables;
};

void arm64_page_init(void);
void arm64_space_init(void);
uintptr_t arm64_direct_to_phys(const void *address);
void *arm64_phys_to_direct(uintptr_t address);
uintptr_t arm64_space_kernel_ttbr1(void);
void arm64_space_select_system(void);
void arm64_space_map_device_block(uint64_t physical);

#endif
