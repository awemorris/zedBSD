/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The translation tables of one Mali address space: the AArch64
 * stage-one format of 4 KiB pages that the GPU's MMU walks.
 *
 * Only last-level page entries are made; no block entries, so a table
 * once made is never replaced and the GPU never sees a table change
 * shape under it.  Every entry is written as one aligned 64-bit store.
 */

#include <stddef.h>
#include <stdint.h>

#include <uapi/errno.h>

#include "drivers/gpu/mali/mali-pagetable.h"

/*
 * The bits of a table entry.
 *
 * Values referenced:
 * Linux v7.3 drivers/iommu/io-pgtable-arm.c (ARM_LPAE_PTE_TYPE_TABLE,
 * ARM_LPAE_PTE_TYPE_PAGE, ARM_LPAE_PTE_TYPE_MASK, ARM_LPAE_PTE_ADDR_MASK,
 * ARM_LPAE_PTE_XN, ARM_LPAE_PTE_AF, ARM_LPAE_PTE_SH_IS,
 * ARM_LPAE_PTE_AP_UNPRIV, ARM_LPAE_PTE_AP_RDONLY,
 * ARM_LPAE_PTE_ATTRINDX_SHIFT, ARM_LPAE_MAIR_ATTR_IDX_NC,
 * ARM_LPAE_MAIR_ATTR_IDX_CACHE and arm_lpae_prot_to_pte for stage one).
 */
#define PT_TYPE_MASK		0x3ULL
#define PT_TYPE_TABLE		0x3ULL
#define PT_TYPE_PAGE		0x3ULL
#define PT_ADDRESS_MASK		0x0000fffffffff000ULL
#define PT_NO_EXECUTE		(3ULL << 53)
#define PT_ACCESSED		(1ULL << 10)
#define PT_INNER_SHAREABLE	(3ULL << 8)
#define PT_UNPRIVILEGED		(1ULL << 6)
#define PT_READ_ONLY		(1ULL << 7)
#define PT_ATTRIBUTE_SHIFT	2U
#define PT_ATTRIBUTE_UNCACHED	0ULL
#define PT_ATTRIBUTE_CACHED	1ULL

/* The widest and narrowest virtual spaces the builder makes. */
#define PT_VIRTUAL_MIN		32U
#define PT_VIRTUAL_MAX		48U

/* The address bits below the last level's index, and the bits each level indexes. */
#define PT_PAGE_SHIFT		12U
#define PT_LEVEL_BITS		9U
#define PT_LAST_LEVEL		3U

static unsigned level_shift(unsigned level);
static unsigned table_index(uint64_t virtual_address, unsigned level);
static int walk(struct mali_pt *pt, uint64_t virtual_address, int create, uint64_t **last);
static int range_valid(const struct mali_pt *pt, uint64_t virtual_address, uint64_t bytes);

/*
 * Makes the root table of a space and records how deep its walk is.
 */
int
mali_pt_init(
	struct mali_pt *pt,
	unsigned virtual_bits,
	const struct mali_pt_memory *memory,
	void *context)
{
	uint64_t physical;
	uint64_t *table;
	int error;

	/* Refuses a width the walk cannot cover, or no memory to put tables in. */
	if (virtual_bits < PT_VIRTUAL_MIN || virtual_bits > PT_VIRTUAL_MAX)
		return EINVAL;
	if (memory == NULL || memory->allocate == NULL || memory->view == NULL)
		return EINVAL;

	/*
	 * Each level resolves nine bits above the page's twelve: a space of
	 * up to 39 bits starts at level 1, a wider one at level 0.
	 */
	pt->start_level = 1U;
	if (virtual_bits > 39U)
		pt->start_level = 0U;

	/* Takes the root table. */
	error = memory->allocate(context, &physical, &table);
	if (error != 0)
		return error;

	/* Records the space; the root is the only table so far. */
	pt->memory = memory;
	pt->context = context;
	pt->root_physical = physical;
	pt->root = table;
	pt->virtual_bits = virtual_bits;
	pt->tables = 1U;

	/* Succeeded: the space exists and maps nothing. */
	return 0;
}

/*
 * Maps a run of whole pages onto a run of physical pages.
 *
 * Tables that the run needs and that do not exist yet are made on the
 * way.  A page already mapped is refused, before anything of the run is
 * written, so a failed call leaves every earlier mapping as it was; the
 * pages before a failure to make a table stay mapped, and the caller
 * unmaps the run to undo them.
 */
int
mali_pt_map(
	struct mali_pt *pt,
	uint64_t virtual_address,
	uint64_t physical_address,
	uint64_t bytes,
	unsigned flags)
{
	uint64_t *last;
	uint64_t offset;
	uint64_t existing;
	int error;

	/* Refuses a run that is not whole pages inside the space. */
	error = range_valid(pt, virtual_address, bytes);
	if (error != 0)
		return error;
	if ((physical_address & (MALI_PT_PAGE_BYTES - 1U)) != 0U)
		return EINVAL;
	if ((physical_address & ~PT_ADDRESS_MASK) != 0U)
		return EINVAL;

	/* Refuses a run that would overwrite a page already mapped. */
	for (offset = 0U; offset < bytes; offset += MALI_PT_PAGE_BYTES) {
		error = mali_pt_lookup(pt, virtual_address + offset, &existing);
		if (error != 0)
			return error;
		if (existing != 0U)
			return EEXIST;
	}

	/* Writes one last-level entry for every page of the run. */
	for (offset = 0U; offset < bytes; offset += MALI_PT_PAGE_BYTES) {
		/* Finds, or makes, the last-level table of the page. */
		error = walk(pt, virtual_address + offset, 1, &last);
		if (error != 0)
			return error;

		/* One aligned store, so the GPU sees the old entry or the new one. */
		*last = mali_pt_page_entry(physical_address + offset, flags);
	}

	/* Succeeded: every page of the run is mapped. */
	return 0;
}

/*
 * Removes the pages of a run; a page the run names but nothing maps is
 * skipped, and tables are kept.
 */
int
mali_pt_unmap(
	struct mali_pt *pt,
	uint64_t virtual_address,
	uint64_t bytes)
{
	uint64_t *last;
	uint64_t offset;
	int error;

	/* Refuses a run that is not whole pages inside the space. */
	error = range_valid(pt, virtual_address, bytes);
	if (error != 0)
		return error;

	/* Clears the last-level entry of every page that has one. */
	for (offset = 0U; offset < bytes; offset += MALI_PT_PAGE_BYTES) {
		/* A page whose tables were never made maps nothing. */
		error = walk(pt, virtual_address + offset, 0, &last);
		if (error == ENOENT)
			continue;
		if (error != 0)
			return error;

		/* One aligned store, so the GPU sees the old entry or none. */
		*last = 0U;
	}

	/* Succeeded: no page of the run is mapped. */
	return 0;
}

/*
 * Reads the last-level entry of one virtual address.
 */
int
mali_pt_lookup(
	const struct mali_pt *pt,
	uint64_t virtual_address,
	uint64_t *entry)
{
	uint64_t *last;
	int error;

	/* Refuses an address outside the space. */
	*entry = 0U;
	error = range_valid(pt, virtual_address & ~(MALI_PT_PAGE_BYTES - 1U), MALI_PT_PAGE_BYTES);
	if (error != 0)
		return error;

	/* A missing table means the address maps nothing. */
	error = walk((struct mali_pt *)pt, virtual_address, 0, &last);
	if (error == ENOENT)
		return 0;
	if (error != 0)
		return error;

	/* Succeeded: the entry, zero when the page is not mapped. */
	*entry = *last;
	return 0;
}

/*
 * Builds the last-level entry of one page.
 *
 * The entry is always accessed (the GPU would fault on its first use
 * otherwise), inner shareable and reachable without privilege; a page
 * without MALI_PT_WRITE is read-only, without MALI_PT_EXECUTE is never
 * run, and without MALI_PT_CACHED takes the uncached attribute slot.
 */
uint64_t
mali_pt_page_entry(
	uint64_t physical_address,
	unsigned flags)
{
	uint64_t entry;

	/* The page, valid, with the bits every mapping has. */
	entry = (physical_address & PT_ADDRESS_MASK) | PT_TYPE_PAGE;
	entry |= PT_ACCESSED | PT_INNER_SHAREABLE | PT_UNPRIVILEGED;

	/* Read-only unless the mapping is writable. */
	if ((flags & MALI_PT_WRITE) == 0U)
		entry |= PT_READ_ONLY;

	/* Never executed unless the mapping holds code. */
	if ((flags & MALI_PT_EXECUTE) == 0U)
		entry |= PT_NO_EXECUTE;

	/* The attribute slot: write-back when cached, otherwise non-cacheable. */
	if ((flags & MALI_PT_CACHED) != 0U) {
		entry |= PT_ATTRIBUTE_CACHED << PT_ATTRIBUTE_SHIFT;
	} else {
		entry |= PT_ATTRIBUTE_UNCACHED << PT_ATTRIBUTE_SHIFT;
	}

	/* Succeeded: the entry maps the page. */
	return entry;
}

/* Reports the lowest address bit a level's index covers. */
static unsigned
level_shift(
	unsigned level)
{
	/* Level 3 indexes bits 12 to 20, each level above it nine bits higher. */
	return PT_PAGE_SHIFT + (PT_LAST_LEVEL - level) * PT_LEVEL_BITS;
}

/* Reports the index of an address in the table of one level. */
static unsigned
table_index(
	uint64_t virtual_address,
	unsigned level)
{
	unsigned shift;

	/* Nine bits at the level's place. */
	shift = level_shift(level);

	/* Succeeded: the entry of the table the address goes through. */
	return (unsigned)((virtual_address >> shift) & (MALI_PT_ENTRIES - 1U));
}

/*
 * Walks from the root to the last-level entry of an address, making the
 * missing tables when create is set; ENOENT when a table is missing and
 * create is not.
 */
static int
walk(
	struct mali_pt *pt,
	uint64_t virtual_address,
	int create,
	uint64_t **last)
{
	uint64_t *table;
	uint64_t *next;
	uint64_t entry;
	uint64_t physical;
	unsigned level;
	unsigned index;
	int error;

	/* Descends one table per level, from the root's level to the last's parent. */
	table = pt->root;
	for (level = pt->start_level; level < PT_LAST_LEVEL; level++) {
		index = table_index(virtual_address, level);
		entry = table[index];

		/* An existing table entry names the next level's table. */
		if ((entry & PT_TYPE_MASK) == PT_TYPE_TABLE) {
			next = pt->memory->view(pt->context, entry & PT_ADDRESS_MASK);
			if (next == NULL)
				return EFAULT;
			table = next;
			continue;
		}

		/* Anything else than an empty entry is not a table this builder made. */
		if (entry != 0U)
			return EFAULT;

		/* A missing table is not made when only looking. */
		if (!create)
			return ENOENT;

		/* Makes the table, then links it, so the GPU never walks a table that is not yet empty. */
		error = pt->memory->allocate(pt->context, &physical, &next);
		if (error != 0)
			return error;
		if ((physical & ~PT_ADDRESS_MASK) != 0U)
			return EFAULT;

		/* One aligned store links the new, empty table. */
		table[index] = physical | PT_TYPE_TABLE;
		pt->tables++;
		table = next;
	}

	/* Succeeded: the entry of the last level. */
	*last = &table[table_index(virtual_address, PT_LAST_LEVEL)];
	return 0;
}

/* Reports whether a run is whole pages inside the space. */
static int
range_valid(
	const struct mali_pt *pt,
	uint64_t virtual_address,
	uint64_t bytes)
{
	uint64_t limit;

	/* Whole pages, at least one. */
	if (bytes == 0U)
		return EINVAL;
	if ((virtual_address & (MALI_PT_PAGE_BYTES - 1U)) != 0U)
		return EINVAL;
	if ((bytes & (MALI_PT_PAGE_BYTES - 1U)) != 0U)
		return EINVAL;

	/* Inside the space, without wrapping past its end. */
	limit = 1ULL << pt->virtual_bits;
	if (virtual_address >= limit)
		return EINVAL;
	if (bytes > limit - virtual_address)
		return EINVAL;

	/* Succeeded: the run can be mapped. */
	return 0;
}
