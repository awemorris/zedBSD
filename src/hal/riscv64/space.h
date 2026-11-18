/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The riscv64 address spaces and the kernel's tables.
 */

#ifndef KERN_HAL_RISCV64_SPACE_H
#define KERN_HAL_RISCV64_SPACE_H

#include <hal/hal.h>

/* The tag a live user space carries, so that a stale handle is caught. */
#define RISCV64_SPACE_MAGIC	0x52563634U

/*
 * One page-table page of a user space below its root.
 *
 * The record remembers the entry that points at the page, so that an empty
 * page can be unlinked.  It lives from the mapping that needed the page to
 * the unmapping that emptied it, or to the space's destruction.
 */
struct riscv64_table_page {
	hal_physaddr_t memory;
	uint64_t *parent;
	unsigned parent_index;
	struct riscv64_table_page *next;
};

/*
 * One user address space: its root table and the pages below it.
 *
 * The root's upper half is a copy of the kernel's, made when the space is
 * created; the kernel never changes its half after boot, so the copy stays
 * right.  A space lives from hal_space_create() to hal_space_destroy(); it
 * is found through the registry before its own lock is taken.
 */
struct riscv64_space {
	uint32_t magic;
	int space_id;

	/* Held while an operation changes or installs the space; zero when free. */
	volatile unsigned lock;

	/* The holding hart's number plus one, or zero; catches a nested operation. */
	unsigned lock_owner;

	/* Nonzero once destruction began, so no new operation can lock it. */
	unsigned destroying;

	struct riscv64_space *registry_next;
	hal_physaddr_t root_memory;
	uint64_t *root;
	struct riscv64_table_page *tables;
};

void
riscv64_kernel_image_init(
	uint64_t kernel_phys_start);

uint64_t
riscv64_kernel_phys_start(
	void);

uint64_t
riscv64_virt_to_phys(
	const void *address);

void *
riscv64_phys_to_direct(
	uint64_t physical);

uint64_t
riscv64_boot_satp(
	void);

void
riscv64_space_init(
	void);

bool
riscv64_space_checked(
	void);

bool
riscv64_space_in_direct_map(
	uint64_t physical,
	uint64_t size);

void
riscv64_space_select_system(
	void);

void
riscv64_space_memory_stats(
	uint32_t *spaces,
	uint32_t *tables);

void
riscv64_tlb_flush_everywhere(
	void);

int
riscv64_space_map_alias(
	uint64_t physical,
	uint64_t size);

#endif
