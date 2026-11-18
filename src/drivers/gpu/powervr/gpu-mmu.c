/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The PowerVR core's page tables for one memory context, and the
 * context's address space.
 *
 * Nothing here touches hardware: the tables are memory the caller's
 * callbacks supply, written through their views with aligned plain
 * stores, so the code runs on the build host as well.  Telling the
 * firmware to drop what the core cached of the tables is the caller's.
 */

#include "drivers/gpu/powervr/gpu-mmu.h"
#include <uapi/errno.h>

static int take_directory(struct powervr_gpu_mmu *mmu, unsigned index, struct powervr_gpu_directory **result);
static int take_table(struct powervr_gpu_mmu *mmu, struct powervr_gpu_directory *directory, unsigned index);
static volatile uint64_t *page_slot(const struct powervr_gpu_mmu *mmu, uint64_t address, struct powervr_gpu_directory **directory, unsigned *table_index);
static void clear_table(struct powervr_gpu_table *table);

/*
 * Builds a catalogue entry naming a directory.
 *
 * Procedure referenced:
 * StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
 * drivers/gpu/drm/imagination/pvr_mmu.c
 * (pvr_page_table_l2_entry_raw_set: the page number of the directory in
 * the base field, valid, not pending).
 */
uint32_t
powervr_gpu_catalogue_entry(
	uint64_t directory)
{
	uint32_t entry;

	/* The directory's page number, moved into the base field, and valid. */
	entry = (uint32_t)((directory >> 12) << POWERVR_GPU_CATALOGUE_SHIFT_IN);
	entry |= POWERVR_GPU_CATALOGUE_VALID;

	/* Succeeded: the entry. */
	return entry;
}

/*
 * Builds a directory entry naming a table of 4 KiB pages.
 *
 * Procedure referenced:
 * StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
 * drivers/gpu/drm/imagination/pvr_mmu.c
 * (pvr_page_table_l1_entry_raw_set: the table's address, the page size,
 * valid, not pending).
 */
uint64_t
powervr_gpu_directory_entry(
	uint64_t table)
{
	uint64_t entry;

	/* The table's address; the 4 KiB page size is the field's zero. */
	entry = table & POWERVR_GPU_DIRECTORY_ADDRESS;
	entry |= POWERVR_GPU_DIRECTORY_VALID;

	/* Succeeded: the entry. */
	return entry;
}

/*
 * Builds a page entry; the core never snoops through it.
 *
 * Procedure referenced:
 * StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
 * drivers/gpu/drm/imagination/pvr_mmu.c
 * (pvr_page_table_l0_entry_raw_set and pvr_page_flags_raw_create: the
 * page's address, read-only, cache-coherent and SLC bypass flags, valid).
 */
uint64_t
powervr_gpu_page_entry(
	uint64_t physical,
	unsigned flags)
{
	uint64_t entry;

	/* The page's address, valid; the coherent bit stays clear. */
	entry = physical & POWERVR_GPU_PAGE_ADDRESS;
	entry |= POWERVR_GPU_PAGE_VALID;

	/* A page the core only reads. */
	if ((flags & POWERVR_GPU_MAP_READ_ONLY) != 0U)
		entry |= POWERVR_GPU_PAGE_READ_ONLY;

	/* A page whose accesses go around the core's system level cache. */
	if ((flags & POWERVR_GPU_MAP_SLC_BYPASS) != 0U)
		entry |= POWERVR_GPU_PAGE_SLC_BYPASS;

	/* A page the parameter manager's metadata protection covers. */
	if ((flags & POWERVR_GPU_MAP_PM_PROTECT) != 0U)
		entry |= POWERVR_GPU_PAGE_PM_PROTECT;

	/* Succeeded: the entry. */
	return entry;
}

/*
 * Sets up a context with an empty catalogue.
 */
int
powervr_gpu_mmu_init(
	struct powervr_gpu_mmu *mmu,
	const struct powervr_gpu_memory *memory)
{
	unsigned index;
	int error;

	/* Starts with no directory. */
	mmu->memory = memory;
	for (index = 0; index < POWERVR_GPU_CATALOGUE_ENTRIES; index++)
		mmu->directories[index] = NULL;

	/* Takes the catalogue page, which comes zeroed: no entry is valid. */
	error = memory->take_table(memory->context, &mmu->catalogue);
	if (error != 0) {
		mmu->catalogue.view = NULL;
		return error;
	}

	/* Succeeded: the context maps nothing. */
	return 0;
}

/*
 * Returns every table of a context; nothing may still use it.
 */
void
powervr_gpu_mmu_fini(
	struct powervr_gpu_mmu *mmu)
{
	const struct powervr_gpu_memory *memory;
	struct powervr_gpu_directory *directory;
	unsigned index;
	unsigned table;

	/* A context that was never set up holds nothing. */
	memory = mmu->memory;
	if (mmu->catalogue.view == NULL)
		return;

	/* Returns each directory's tables, then the directory and its bookkeeping. */
	for (index = 0; index < POWERVR_GPU_CATALOGUE_ENTRIES; index++) {
		directory = mmu->directories[index];
		if (directory == NULL)
			continue;

		/* Returns the tables under the directory. */
		for (table = 0; table < POWERVR_GPU_DIRECTORY_ENTRIES; table++) {
			if (directory->tables[table].view != NULL)
				memory->give_table(memory->context, &directory->tables[table]);
		}

		memory->give_table(memory->context, &directory->table);
		memory->give_memory(memory->context, directory);
		mmu->directories[index] = NULL;
	}

	/* Returns the catalogue last. */
	memory->give_table(memory->context, &mmu->catalogue);
	mmu->catalogue.view = NULL;
}

/*
 * Reports the physical address of a context's catalogue.
 */
uint64_t
powervr_gpu_mmu_root(
	const struct powervr_gpu_mmu *mmu)
{
	/* Succeeded: what the firmware's memory context names. */
	return mmu->catalogue.physical;
}

/*
 * Maps a physically contiguous run at a device address.
 */
int
powervr_gpu_mmu_map(
	struct powervr_gpu_mmu *mmu,
	uint64_t address,
	uint64_t physical,
	uint64_t bytes,
	unsigned flags)
{
	struct powervr_gpu_directory *directory;
	volatile uint64_t *slot;
	uint64_t limit;
	uint64_t offset;
	unsigned catalogue_index;
	unsigned table_index;
	int error;

	/* Refuses a range that is empty, not whole pages, or outside the space. */
	limit = 1ULL << POWERVR_GPU_ADDRESS_BITS;
	if (bytes == 0U)
		return EINVAL;
	if (((address | physical | bytes) & (POWERVR_GPU_PAGE_BYTES - 1U)) != 0U)
		return EINVAL;
	if (address >= limit || bytes > limit - address)
		return EINVAL;
	if (physical > POWERVR_GPU_PAGE_ADDRESS || bytes - 1U > POWERVR_GPU_PAGE_ADDRESS - physical)
		return EINVAL;

	/* Refuses the range when any page of it is mapped, before changing anything. */
	for (offset = 0; offset < bytes; offset += POWERVR_GPU_PAGE_BYTES) {
		slot = page_slot(mmu, address + offset, &directory, &table_index);
		if (slot != NULL && (*slot & POWERVR_GPU_PAGE_VALID) != 0U)
			return EEXIST;
	}

	/* Writes each page's entry, making the directory and table it needs first. */
	for (offset = 0; offset < bytes; offset += POWERVR_GPU_PAGE_BYTES) {
		catalogue_index = (unsigned)((address + offset) >> POWERVR_GPU_CATALOGUE_SHIFT);
		error = take_directory(mmu, catalogue_index, &directory);
		if (error != 0)
			goto undo;

		/* The table under the directory. */
		table_index = (unsigned)(((address + offset) >> POWERVR_GPU_DIRECTORY_SHIFT) & (POWERVR_GPU_DIRECTORY_ENTRIES - 1U));
		error = take_table(mmu, directory, table_index);
		if (error != 0)
			goto undo;

		/* The page's entry. */
		slot = page_slot(mmu, address + offset, &directory, &table_index);
		*slot = powervr_gpu_page_entry(physical + offset, flags);
		directory->used[table_index]++;
	}

	/* Succeeded: every page of the range is mapped. */
	return 0;

undo:
	/* Removes the entries written so far; the tables made for them stay. */
	powervr_gpu_mmu_unmap(mmu, address, offset);
	return error;
}

/*
 * Removes the page entries of a range mapped earlier.
 */
void
powervr_gpu_mmu_unmap(
	struct powervr_gpu_mmu *mmu,
	uint64_t address,
	uint64_t bytes)
{
	struct powervr_gpu_directory *directory;
	volatile uint64_t *slot;
	uint64_t offset;
	unsigned table_index;

	/* Clears each valid entry of the range and counts it off its table. */
	for (offset = 0; offset < bytes; offset += POWERVR_GPU_PAGE_BYTES) {
		slot = page_slot(mmu, address + offset, &directory, &table_index);
		if (slot == NULL || (*slot & POWERVR_GPU_PAGE_VALID) == 0U)
			continue;

		*slot = 0;
		directory->used[table_index]--;
	}
}

/*
 * Reads back the page entry of a device address, zero when none.
 */
uint64_t
powervr_gpu_mmu_lookup(
	const struct powervr_gpu_mmu *mmu,
	uint64_t address)
{
	struct powervr_gpu_directory *directory;
	volatile uint64_t *slot;
	unsigned table_index;

	/* An address with no table has no entry. */
	slot = page_slot(mmu, address, &directory, &table_index);
	if (slot == NULL)
		return 0;

	/* Succeeded: the entry as written. */
	return *slot;
}

/*
 * Sets up an empty address space over a window.
 */
void
powervr_gpu_space_init(
	struct powervr_gpu_space *space,
	uint64_t base,
	uint64_t size)
{
	/* No range is taken yet. */
	space->base = base;
	space->size = size;
	space->count = 0;
}

/*
 * Takes a range of whole pages; ENOSPC when no gap fits (the device's
 * address space is run out).
 *
 * The first gap that holds the range and its guard page wins.
 */
int
powervr_gpu_space_take(
	struct powervr_gpu_space *space,
	uint64_t bytes,
	uint64_t *address)
{
	uint64_t candidate;
	uint64_t end;
	uint64_t need;
	unsigned index;
	unsigned move;

	/* Refuses an empty request, one not of whole pages, and a full table. */
	if (bytes == 0U || (bytes & (POWERVR_GPU_PAGE_BYTES - 1U)) != 0U)
		return EINVAL;
	if (space->count == POWERVR_GPU_SPACE_RANGES)
		return ENOSPC;
	if (bytes > space->size - POWERVR_GPU_PAGE_BYTES)
		return ENOSPC;
	need = bytes + POWERVR_GPU_PAGE_BYTES;

	/* Walks the gaps in address order: before each taken range, then after the last. */
	candidate = space->base;
	for (index = 0; index <= space->count; index++) {
		/* The gap ends at the next range, or at the window's end. */
		if (index < space->count) {
			end = space->ranges[index].start;
		} else {
			end = space->base + space->size;
		}

		/* Stops at the first gap the range and its guard fit. */
		if (end >= candidate && end - candidate >= need)
			break;

		/* Moves past the range that closed the gap, and its guard. */
		if (index < space->count)
			candidate = space->ranges[index].start + space->ranges[index].size + POWERVR_GPU_PAGE_BYTES;
	}

	/* No gap fits. */
	if (index > space->count)
		return ENOSPC;

	/* Opens a place in the sorted table for the new range. */
	for (move = space->count; move > index; move--)
		space->ranges[move] = space->ranges[move - 1U];
	space->ranges[index].start = candidate;
	space->ranges[index].size = bytes;
	space->count++;

	/* Succeeded: the range's address. */
	*address = candidate;
	return 0;
}

/*
 * Gives back the range that starts at an address; EINVAL when none does.
 */
int
powervr_gpu_space_give(
	struct powervr_gpu_space *space,
	uint64_t address)
{
	unsigned index;

	/* Finds the range that starts at the address. */
	for (index = 0; index < space->count; index++) {
		if (space->ranges[index].start == address)
			break;
	}

	/* No range starts there. */
	if (index == space->count)
		return EINVAL;

	/* Closes the gap in the sorted table. */
	for (; index + 1U < space->count; index++)
		space->ranges[index] = space->ranges[index + 1U];
	space->count--;

	/* Succeeded: the range is free again. */
	return 0;
}

/* Finds or makes the directory under a catalogue entry. */
static int
take_directory(
	struct powervr_gpu_mmu *mmu,
	unsigned index,
	struct powervr_gpu_directory **result)
{
	const struct powervr_gpu_memory *memory;
	struct powervr_gpu_directory *directory;
	volatile uint32_t *catalogue;
	int error;

	/* A directory made earlier is reused. */
	directory = mmu->directories[index];
	if (directory != NULL) {
		*result = directory;
		return 0;
	}

	/* The host's bookkeeping for the new directory. */
	memory = mmu->memory;
	directory = memory->take_memory(memory->context, sizeof(*directory));
	if (directory == NULL)
		return ENOMEM;

	/* The directory's page, zeroed: none of its entries is valid. */
	error = memory->take_table(memory->context, &directory->table);
	if (error != 0) {
		memory->give_memory(memory->context, directory);
		return error;
	}

	/* Names the directory in the catalogue once its page is clear. */
	catalogue = (volatile uint32_t *)mmu->catalogue.view;
	catalogue[index] = powervr_gpu_catalogue_entry(directory->table.physical);
	mmu->directories[index] = directory;

	/* Succeeded: the directory. */
	*result = directory;
	return 0;
}

/* Makes the table under a directory entry when it is not there yet. */
static int
take_table(
	struct powervr_gpu_mmu *mmu,
	struct powervr_gpu_directory *directory,
	unsigned index)
{
	const struct powervr_gpu_memory *memory;
	volatile uint64_t *entries;
	int error;

	/* A table made earlier is reused. */
	if (directory->tables[index].view != NULL)
		return 0;

	/* The table's page, zeroed: none of its pages is mapped. */
	memory = mmu->memory;
	error = memory->take_table(memory->context, &directory->tables[index]);
	if (error != 0) {
		directory->tables[index].view = NULL;
		return error;
	}
	clear_table(&directory->tables[index]);

	/* Names the table in the directory once its page is clear. */
	entries = (volatile uint64_t *)directory->table.view;
	entries[index] = powervr_gpu_directory_entry(directory->tables[index].physical);
	directory->used[index] = 0;

	/* Succeeded: the table is there. */
	return 0;
}

/*
 * Finds the page entry of a device address, or NULL when its directory or
 * table does not exist.
 */
static volatile uint64_t *
page_slot(
	const struct powervr_gpu_mmu *mmu,
	uint64_t address,
	struct powervr_gpu_directory **directory,
	unsigned *table_index)
{
	struct powervr_gpu_directory *found;
	volatile uint64_t *entries;
	unsigned catalogue_index;
	unsigned page_index;

	/* An address outside the space, or under no directory, has no entry. */
	*directory = NULL;
	if ((address >> POWERVR_GPU_ADDRESS_BITS) != 0U)
		return NULL;
	catalogue_index = (unsigned)(address >> POWERVR_GPU_CATALOGUE_SHIFT);
	found = mmu->directories[catalogue_index];
	if (found == NULL)
		return NULL;

	/* An address under no table has no entry. */
	*table_index = (unsigned)((address >> POWERVR_GPU_DIRECTORY_SHIFT) & (POWERVR_GPU_DIRECTORY_ENTRIES - 1U));
	if (found->tables[*table_index].view == NULL)
		return NULL;

	/* Succeeded: the entry's place in its table. */
	page_index = (unsigned)((address >> POWERVR_GPU_TABLE_SHIFT) & (POWERVR_GPU_TABLE_ENTRIES - 1U));
	entries = (volatile uint64_t *)found->tables[*table_index].view;
	*directory = found;
	return &entries[page_index];
}

/* Clears a table page a word at a time, whatever the callback left in it. */
static void
clear_table(
	struct powervr_gpu_table *table)
{
	volatile uint64_t *entries;
	unsigned index;

	/* Every entry invalid. */
	entries = (volatile uint64_t *)table->view;
	for (index = 0; index < POWERVR_GPU_TABLE_ENTRIES; index++)
		entries[index] = 0;
}

/* The mapping flags for a buffer: a host-visible or shareable one bypasses the SLC. */
unsigned
powervr_gpu_buffer_flags(
	int mappable,
	int shareable)
{
	/* The processor and the display read memory, not the core's cache. */
	if (mappable || shareable)
		return POWERVR_GPU_MAP_SLC_BYPASS;

	/* A private buffer keeps the cache. */
	return 0;
}

/* Checks a contiguous run against an address limit and a base alignment. */
int
powervr_gpu_run_fits(
	uint64_t paddr,
	uint64_t size,
	uint64_t limit,
	uint64_t alignment)
{
	/* Refuses an empty run and an alignment that is not a power of two. */
	if (size == 0U || paddr + size < paddr)
		return EINVAL;
	if (alignment > 1U && (alignment & (alignment - 1U)) != 0U)
		return EINVAL;

	/* The last byte must be at or below the limit. */
	if (limit != 0U && paddr + size - 1U > limit)
		return ENOTSUP;

	/* The base must be on the alignment. */
	if (alignment > 1U && (paddr & (alignment - 1U)) != 0U)
		return ENOTSUP;

	/* Succeeded: the run fits. */
	return 0;
}
