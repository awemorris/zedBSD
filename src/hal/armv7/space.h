/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The armv7 address spaces and the kernel's tables.
 */

#ifndef KERN_HAL_ARMV7_SPACE_H
#define KERN_HAL_ARMV7_SPACE_H

#include <hal/hal.h>

/* The tag a live user space carries, so that a stale handle is caught. */
#define ARMV7_SPACE_MAGIC	0x41524d37U

/*
 * One page of a user space's second-level tables: the shadow entries and
 * the two hardware tables of 2 MiB of the space.
 *
 * The record remembers the pair of first-level entries that point at the
 * page, so that an empty page can be unlinked.  It lives from the mapping
 * that needed the page to the unmapping that emptied it, or to the space's
 * destruction.
 */
struct armv7_table_page {
	hal_physaddr_t memory;
	uint32_t *entries;
	unsigned first_index;
	struct armv7_table_page *next;
};

/*
 * One user address space: its first-level table, which TTBR0 selects,
 * the pages of tables below it, and its address space identifier.
 *
 * The kernel's half is not in the space: TTBR1 selects the kernel's table
 * on every processor.  A space lives from hal_space_create() to
 * hal_space_destroy(); it is found through the registry before its own
 * lock is taken.
 */
struct armv7_space {
	uint32_t magic;
	int space_id;

	/* Held while an operation changes or installs the space; zero when free. */
	volatile unsigned lock;

	/* The holding processor's number plus one, or zero; catches a nested operation. */
	unsigned lock_owner;

	/* Nonzero once destruction began, so no new operation can lock it. */
	unsigned destroying;

	struct armv7_space *registry_next;
	hal_physaddr_t l1_memory;
	uint32_t *l1;
	struct armv7_table_page *tables;

	/*
	 * The identifier the space's translations are tagged with, valid
	 * while its generation is the current one; a space of an older
	 * generation gets a new identifier when it is next selected.  Kept
	 * under the identifier lock.
	 */
	uint32_t asid;
	uint32_t asid_generation;
};

void
armv7_kernel_image_init(
	uint64_t kernel_phys_start);

uint64_t
armv7_kernel_phys_start(
	void);

uint64_t
armv7_virt_to_phys(
	const void *address);

void *
armv7_phys_to_direct(
	uint64_t physical);

int
armv7_space_map_device_window(
	uint64_t physical,
	size_t size,
	unsigned memory_type,
	void **address);

void
armv7_space_init(
	void);

bool
armv7_space_checked(
	void);

int
armv7_space_resolve_fault(
	uintptr_t address,
	unsigned status,
	int write);

uint32_t
armv7_space_asid(
	hal_space_t handle);

void
armv7_space_memory_stats(
	uint32_t *spaces,
	uint32_t *tables);

#endif
