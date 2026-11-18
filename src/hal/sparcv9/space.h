/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The SPARC V9 HAL's address spaces.
 *
 * The UltraSPARC-I/II MMU has no page tables of its own: each TLB miss is
 * resolved by software.  An address space is a list of its 8 KiB mappings,
 * looked up on a miss and written into the TLB under the space's context.
 */

#ifndef KERN_HAL_SPARCV9_SPACE_H
#define KERN_HAL_SPARCV9_SPACE_H

#include <hal/hal.h>

/* The magic of a live address space, cleared when it is destroyed. */
#define SPARCV9_SPACE_MAGIC	0x53395639U

/*
 * One 8 KiB page of an address space and where it lies in physical memory.
 *
 * Owned by the space's list; created by hal_space_map() and freed when the
 * page is unmapped or the space is destroyed.
 */
struct sparcv9_mapping {
	uintptr_t virtual_address;
	uintptr_t physical_address;
	uint32_t attributes;
	uint32_t flags;
	struct sparcv9_mapping *next;
};

/*
 * One user address space: the context its TLB entries are tagged with and
 * its mappings.
 *
 * Created by hal_space_create() and kept in the registry until
 * hal_space_destroy(); busy is set while an operation works on it.
 */
struct sparcv9_space {
	uint32_t magic;
	uint32_t context;
	unsigned busy;
	unsigned destroying;
	struct sparcv9_space *registry_next;
	struct sparcv9_mapping *mappings;
};

/*
 * Starts the MMU with no user space selected.
 */
void
sparcv9_space_init(
	void);

/*
 * Resolves a TLB miss from the current user space or the device window.
 *
 * Returns 1 when the entry was written, or 0 when no mapping allows the
 * access.
 */
int
sparcv9_resolve_miss(
	uintptr_t address,
	int instruction,
	int write);

/*
 * Writes the TLB entry of a user address before the first user access,
 * as a miss would.
 *
 * Returns 1 when the entry was written, or 0 when no mapping allows the
 * access.
 */
int
sparcv9_prime_mapping(
	uintptr_t address,
	int instruction,
	int write);

/*
 * Reports the number of address spaces and of mapping records.
 */
void
sparcv9_space_memory_stats(
	uint32_t *space_count,
	uint32_t *mapping_count);

#endif
