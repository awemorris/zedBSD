/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The translation tables of one Mali address space.
 *
 * The GPU's MMU walks AArch64 stage-one tables of 4 KiB pages.  This
 * builder writes them; it knows nothing of the GPU's registers or of
 * where the tables live, which the caller's memory operations decide, so
 * the same code runs in the kernel and in a host test.
 */

#ifndef KERN_DRIVERS_GPU_MALI_PAGETABLE_H
#define KERN_DRIVERS_GPU_MALI_PAGETABLE_H

#include <stddef.h>
#include <stdint.h>

/* The page the tables map, and the entries of one table. */
#define MALI_PT_PAGE_BYTES	4096ULL
#define MALI_PT_ENTRIES		512U

/* What a mapping lets the GPU do: write it, run code from it, cache it. */
#define MALI_PT_WRITE		1U
#define MALI_PT_EXECUTE		2U
#define MALI_PT_CACHED		4U

/*
 * Where the caller keeps the tables.
 *
 * Allocate gives one zeroed table of MALI_PT_PAGE_BYTES, aligned to its
 * size, with its physical address (what the GPU walks) and the CPU's view
 * of it (what the builder writes); it returns zero or an error number.
 * View finds the CPU's view of a table the builder made earlier from its
 * physical address, or NULL.  Both are called only from the builder's
 * own calls, never at the same time for one table set.
 */
struct mali_pt_memory {
	int (*allocate)(void *context, uint64_t *physical, uint64_t **table);
	uint64_t *(*view)(void *context, uint64_t physical);
};

/*
 * One set of translation tables.
 *
 * The root is made by mali_pt_init() and lives until the caller frees
 * every table it handed out; the builder never frees a table, so an
 * address the GPU might still walk stays valid until the space is gone.
 * Start level is the level of the root, decided by the virtual width.
 */
struct mali_pt {
	const struct mali_pt_memory *memory;
	void *context;
	uint64_t root_physical;
	uint64_t *root;
	unsigned virtual_bits;
	unsigned start_level;
	uint64_t tables;
};

/*
 * Makes the root table of a space of virtual_bits bits (between 32 and
 * 48).
 */
int mali_pt_init(struct mali_pt *pt, unsigned virtual_bits, const struct mali_pt_memory *memory, void *context);

/*
 * Maps a run of whole pages at a virtual address onto a run of physical
 * pages, with the access a set of MALI_PT_ flags names.
 */
int mali_pt_map(struct mali_pt *pt, uint64_t virtual_address, uint64_t physical_address, uint64_t bytes, unsigned flags);

/*
 * Removes the pages of a run from the space; the tables stay.
 */
int mali_pt_unmap(struct mali_pt *pt, uint64_t virtual_address, uint64_t bytes);

/*
 * Reads the last-level entry of one virtual address, zero when nothing
 * maps it.
 */
int mali_pt_lookup(const struct mali_pt *pt, uint64_t virtual_address, uint64_t *entry);

/*
 * Builds the last-level entry that maps one page with a set of MALI_PT_
 * flags.
 */
uint64_t mali_pt_page_entry(uint64_t physical_address, unsigned flags);

#endif
