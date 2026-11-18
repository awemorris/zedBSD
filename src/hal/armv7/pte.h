/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The armv7 translation table entries, built from what they stand for.
 *
 * A user page has two entries: the shadow entry, the HAL's own record of
 * the page's logical state (present, writable, executable, accessed,
 * dirty, its memory type), and the hardware small-page descriptor, which
 * is always made from the shadow.  The hardware does not set the access
 * flag or mark a page dirty on this processor, so the descriptor makes the
 * first access to a page fault (its access flag is clear until the page is
 * accessed) and the first store fault (it is read-only until the page is
 * dirty), and the HAL updates the shadow and the descriptor on those
 * faults (Procedure referenced: Linux v6.12 arch/arm/mm/proc-v7-2level.S,
 * cpu_v7_set_pte_ext, which keeps the Linux entry beside the hardware one
 * and makes the second from the first).
 *
 * Everything here is arithmetic on the entries, so the host test checks it
 * as it is.
 */

#ifndef KERN_HAL_ARMV7_PTE_H
#define KERN_HAL_ARMV7_PTE_H

#include <hal/types.h>
#include "defs.h"

/*
 * What the HAL does about a fault on a user page, by the page's shadow
 * entry: resolve it itself and run the access again, or hand it to the
 * kernel.
 */
enum armv7_pte_resolution {
	ARMV7_PTE_RESOLVED = 0,
	ARMV7_PTE_TO_KERNEL
};

/*
 * Builds a user page's shadow entry: the page's frame, present, user,
 * writable and executable as the mapping allows, not yet accessed nor
 * dirty, and of the memory type given.
 */
static __inline uint32_t
armv7_pte_shadow(
	uint64_t physical,
	int writable,
	int executable,
	unsigned memory_type)
{
	uint32_t entry;

	/* The frame, present and the user's. */
	entry = ((uint32_t)physical & ARMV7_PTE_FRAME_MASK) | ARMV7_PTE_PRESENT | ARMV7_PTE_USER;

	/* The access the mapping allows; a present page is always readable. */
	if (writable)
		entry |= ARMV7_PTE_WRITE;
	if (executable)
		entry |= ARMV7_PTE_EXEC;

	/* Succeeded: the entry, with its memory type. */
	return entry | (((uint32_t)memory_type << ARMV7_PTE_ATTR_SHIFT) & ARMV7_PTE_ATTR_MASK);
}

/*
 * Builds the hardware small-page descriptor of a shadow entry.
 *
 * A page that is not present has a zero descriptor, which faults.  The
 * descriptor is the user's (AP[1]) and not global; its access flag is the
 * shadow's accessed bit; it is writable (AP[2] clear) only when the page
 * is both logically writable and already dirty, and executable only when
 * the page is executable.  Normal memory is write-back write-allocate and
 * shareable, uncached memory normal non-cacheable, and device memory
 * shareable device.
 */
static __inline uint32_t
armv7_pte_hardware(
	uint32_t shadow)
{
	uint32_t descriptor;
	uint32_t memory_type;

	/* A missing page faults on every access. */
	if ((shadow & ARMV7_PTE_PRESENT) == 0)
		return 0;

	/* The frame, a small page, not global. */
	descriptor = (shadow & ARMV7_L2_BASE_MASK) | ARMV7_L2_SMALL | ARMV7_L2_NG;

	/* The user's page, or the kernel's alone. */
	if ((shadow & ARMV7_PTE_USER) != 0)
		descriptor |= ARMV7_L2_AP1;

	/* An access flag clear until the page has been accessed. */
	if ((shadow & ARMV7_PTE_ACCESSED) != 0)
		descriptor |= ARMV7_L2_AF;

	/* Read-only until a logically writable page has been written. */
	if ((shadow & ARMV7_PTE_WRITE) == 0 || (shadow & ARMV7_PTE_DIRTY) == 0)
		descriptor |= ARMV7_L2_AP2;

	/* Never executed unless the page is executable. */
	if ((shadow & ARMV7_PTE_EXEC) == 0)
		descriptor |= ARMV7_L2_XN;

	/* The memory type, by TEX, C and B with TEX remap off. */
	memory_type = (shadow & ARMV7_PTE_ATTR_MASK) >> ARMV7_PTE_ATTR_SHIFT;
	if (memory_type == ARMV7_PTE_ATTR_DEVICE) {
		descriptor |= ARMV7_L2_B;
	} else if (memory_type == ARMV7_PTE_ATTR_NOCACHE) {
		descriptor |= ARMV7_L2_TEX0 | ARMV7_L2_S;
	} else {
		descriptor |= ARMV7_L2_TEX0 | ARMV7_L2_C | ARMV7_L2_B | ARMV7_L2_S;
	}

	/* Succeeded: the descriptor the table walk reads. */
	return descriptor;
}

/*
 * Builds a kernel small-page descriptor: normal memory, global, already
 * accessed, the kernel's alone, read-only or read-write, executable or
 * not.
 */
static __inline uint32_t
armv7_pte_kernel_page(
	uint64_t physical,
	int writable,
	int executable)
{
	uint32_t descriptor;

	/* The frame, normal write-back shareable memory, already accessed. */
	descriptor = ((uint32_t)physical & ARMV7_L2_BASE_MASK) | ARMV7_L2_SMALL;
	descriptor |= ARMV7_L2_TEX0 | ARMV7_L2_C | ARMV7_L2_B | ARMV7_L2_S | ARMV7_L2_AF;

	/* Read-only unless writable; never both writable and executable. */
	if (!writable)
		descriptor |= ARMV7_L2_AP2;
	if (!executable)
		descriptor |= ARMV7_L2_XN;

	/* Succeeded: the descriptor. */
	return descriptor;
}

/*
 * Builds the first-level entry that points at a second-level table, in
 * domain 0.
 */
static __inline uint32_t
armv7_pte_table_pointer(
	uint64_t table_physical)
{
	/* The table's base above bit 10 and the table type. */
	return ((uint32_t)table_physical & ARMV7_L1_TABLE_BASE_MASK) | ARMV7_L1_TYPE_TABLE;
}

/*
 * Builds the value of TTBR0 or TTBR1 for a first-level table: its base and
 * the walk attributes.
 */
static __inline uint32_t
armv7_pte_ttbr(
	uint64_t table_physical)
{
	/* The table's base with the cacheable, shareable walk. */
	return (uint32_t)table_physical | ARMV7_TTBR_FLAGS;
}

/*
 * Reports the index of a user address's shadow entry in its page of
 * tables, which covers 2 MiB; its hardware entry is the same index in the
 * hardware half of the page.
 */
static __inline unsigned
armv7_pte_index(
	uintptr_t address)
{
	/* 512 small pages per page of tables. */
	return (unsigned)((address >> ARMV7_PAGE_SHIFT) & (2U * ARMV7_L2_ENTRIES - 1U));
}

/*
 * Decides a fault on a present or missing user page from its shadow entry
 * and the fault's status, and updates the shadow when the HAL resolves it.
 *
 * An access-flag fault on a present page marks it accessed (and dirty, for
 * a store to a writable page).  A permission fault of a store to a
 * logically writable page marks it dirty.  Every other fault, a store to a
 * read-only page among them, is the kernel's.
 */
static __inline enum armv7_pte_resolution
armv7_pte_resolve(
	uint32_t *shadow,
	unsigned status,
	int write)
{
	/* A page that is not there is the kernel's to bring in. */
	if ((*shadow & ARMV7_PTE_PRESENT) == 0)
		return ARMV7_PTE_TO_KERNEL;

	/* The first access to a page: the page is now accessed. */
	if (status == ARMV7_FS_AF_PAGE) {
		*shadow |= ARMV7_PTE_ACCESSED;

		/* A store to a writable page also makes it dirty, saving the second fault. */
		if (write && (*shadow & ARMV7_PTE_WRITE) != 0)
			*shadow |= ARMV7_PTE_DIRTY;
		return ARMV7_PTE_RESOLVED;
	}

	/* Only a permission fault can be a page that is not yet dirty. */
	if (status != ARMV7_FS_PERMISSION_PAGE)
		return ARMV7_PTE_TO_KERNEL;

	/* Only a store can be one. */
	if (!write)
		return ARMV7_PTE_TO_KERNEL;

	/* A store to a page the mapping keeps read-only is the kernel's. */
	if ((*shadow & ARMV7_PTE_WRITE) == 0)
		return ARMV7_PTE_TO_KERNEL;

	/* The first store to a writable page: the page is now dirty, and accessed. */
	*shadow |= ARMV7_PTE_DIRTY | ARMV7_PTE_ACCESSED;
	return ARMV7_PTE_RESOLVED;
}

/*
 * Changes a present page's logical access, keeping its frame, its memory
 * type and what it recorded, and reports what it recorded: present, and
 * accessed and dirty when it was.
 */
static __inline uint32_t
armv7_pte_change_access(
	uint32_t *shadow,
	int writable,
	int executable)
{
	uint32_t observed;

	/* What the page recorded under its old access. */
	observed = HAL_SPACE_PAGE_PRESENT;
	if ((*shadow & ARMV7_PTE_ACCESSED) != 0)
		observed |= HAL_SPACE_PAGE_ACCESSED;
	if ((*shadow & ARMV7_PTE_DIRTY) != 0)
		observed |= HAL_SPACE_PAGE_DIRTY;

	/* The new access; the hardware entry made from it is read-only if not writable. */
	*shadow &= ~(uint32_t)(ARMV7_PTE_WRITE | ARMV7_PTE_EXEC);
	if (writable)
		*shadow |= ARMV7_PTE_WRITE;
	if (executable)
		*shadow |= ARMV7_PTE_EXEC;

	/* Succeeded: what the page recorded. */
	return observed;
}

/*
 * Translates a physical address of RAM to its direct-map address, given
 * where the direct map starts and how much RAM there is.  Reports -1 for
 * an address outside that RAM or past what the direct map holds.
 */
static __inline int
armv7_pte_direct_from_physical(
	uint64_t physical,
	uint64_t physical_base,
	uint64_t ram_bytes,
	uintptr_t *address)
{
	uint64_t offset;

	/* Below the direct map's first byte. */
	if (physical < physical_base)
		return -1;

	/* Past the RAM, or past the direct map's 1 GiB. */
	offset = physical - physical_base;
	if (offset >= ram_bytes)
		return -1;
	if (offset >= ARMV7_DIRECT_BYTES)
		return -1;

	/* Succeeded: the direct map holds the address at the same offset. */
	*address = (uintptr_t)(ARMV7_DIRECT_BASE + offset);
	return 0;
}

#endif
