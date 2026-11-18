/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The MIPS address spaces, their page tables and the TLB.
 */

#ifndef KERN_HAL_MIPS_SPACE_H
#define KERN_HAL_MIPS_SPACE_H

#include <hal/hal.h>

/* The tag a live user space carries, so that a stale handle is caught. */
#define MIPS_SPACE_MAGIC	0x4d495053U

/*
 * The access a TLB exception was taken for: a load or an instruction
 * fetch that found an invalid pair (TLBL), a store that found one
 * (TLBS), or a store to a page whose dirty bit is clear (Mod).
 */
#define MIPS_SPACE_ACCESS_LOAD		0
#define MIPS_SPACE_ACCESS_STORE		1
#define MIPS_SPACE_ACCESS_MODIFY	2

/*
 * What the space found for a TLB exception: the page was mapped and the
 * access allowed, so the entry was brought up to date and the access is
 * retried; the page is mapped but does not allow the access; or no page
 * is mapped there.
 */
#define MIPS_SPACE_FAULT_FIXED		0
#define MIPS_SPACE_FAULT_PROTECTION	1
#define MIPS_SPACE_FAULT_MISSING	2

/*
 * One table below the root of a user space (a leaf table, or on the
 * 64-bit processor a middle one): its page, its index in the table above
 * it and the next one.  It lives from the mapping that needed it to the
 * space's destruction.
 */
struct mips_table_page {
	hal_physaddr_t memory;
	unsigned root_index;
	struct mips_table_page *next;
};

/*
 * One user address space: its root table, the tables below it and
 * the TLB's identifier for it.
 *
 * A space lives from hal_space_create() to hal_space_destroy(); it is
 * found through the registry with interrupts off.  Its ASID is valid only
 * while its generation is the TLB's current one; a space with an older
 * generation (or zero) has no entry in the TLB and gets a new ASID when
 * it is next selected.
 */
struct mips_space {
	uint32_t magic;
	unsigned destroying;
	struct mips_space *registry_next;
	hal_physaddr_t root_memory;
	uintptr_t *root;
	struct mips_table_page *tables;
	uint32_t asid;
	uint64_t generation;
};

/*
 * The counts of the TLB's exceptions since boot, for the self-test:
 * refills, invalid pairs taken for a load or a store, and stores to a
 * clean page.  They only grow.
 */
struct mips_space_counters {
	uint32_t refills;
	uint32_t invalids;
	uint32_t modifies;
};

void
mips_space_init(
	void);

int
mips_space_fault(
	uintptr_t address,
	int access);

void
mips_space_memory_stats(
	uint32_t *spaces,
	uint32_t *tables);

void
mips_space_counters(
	struct mips_space_counters *counters);

uint32_t
mips_space_set_asid_limit(
	uint32_t limit);

uint64_t
mips_space_generation(
	void);

uint32_t
mips_space_asid(
	hal_space_t handle);

#endif
