/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The riscv32 address spaces and the kernel's tables.
 */

#ifndef KERN_HAL_RISCV32_SPACE_H
#define KERN_HAL_RISCV32_SPACE_H

#include <hal/hal.h>

/* The tag a live user space carries, so that a stale handle is caught. */
#define RISCV32_SPACE_MAGIC	0x52563634U

/*
 * One page-table page of a user space below its root.
 *
 * The record remembers the entry that points at the page, so that an empty
 * page can be unlinked.  It lives from the mapping that needed the page to
 * the unmapping that emptied it, or to the space's destruction.
 */
struct riscv32_table_page {
	hal_physaddr_t memory;
	uint32_t *parent;
	unsigned parent_index;
	struct riscv32_table_page *next;
};

/*
 * One user address space: its root table and the pages below it.
 *
 * The root's upper half is a copy of the kernel's, made when the space is
 * created; the kernel never changes its half after boot, so the copy stays
 * right.  A space lives from hal_space_create() to hal_space_destroy(); it
 * is found through the registry before its own lock is taken.
 */
struct riscv32_space {
	uint32_t magic;
	int space_id;

	/* Held while an operation changes or installs the space; zero when free. */
	volatile unsigned lock;

	/* The holding hart's number plus one, or zero; catches a nested operation. */
	unsigned lock_owner;

	/* Nonzero once destruction began, so no new operation can lock it. */
	unsigned destroying;

	struct riscv32_space *registry_next;
	hal_physaddr_t root_memory;
	uint32_t *root;
	struct riscv32_table_page *tables;
};

void
riscv32_kernel_image_init(
	uint64_t kernel_phys_start);

uint64_t
riscv32_kernel_phys_start(
	void);

uint64_t
riscv32_virt_to_phys(
	const void *address);

void *
riscv32_phys_to_direct(
	uint64_t physical);

uint64_t
riscv32_boot_satp(
	void);

void
riscv32_space_init(
	void);

bool
riscv32_space_checked(
	void);

void
riscv32_space_select_system(
	void);

void
riscv32_space_memory_stats(
	uint32_t *spaces,
	uint32_t *tables);

void
riscv32_tlb_flush_everywhere(
	void);

#endif
