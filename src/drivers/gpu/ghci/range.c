/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The allocator of GPU addresses: first fit over a table of the runs in
 * use, kept in address order.  A device has one allocator for each kind
 * of memory it hands out, and its owner serializes every call.
 */

#include <stddef.h>
#include <stdint.h>

#include <uapi/errno.h>

#include "ghci-words.h"

/*
 * Starts an allocator over [base, limit) with the caller's table.
 */
void
ghci_range_init(
	struct ghci_range_map *map,
	uint64_t base,
	uint64_t limit,
	struct ghci_range_extent *extents,
	unsigned capacity)
{
	/* The range and its empty table. */
	map->base = base;
	map->limit = limit;
	map->extents = extents;
	map->count = 0U;
	map->capacity = capacity;
}

/*
 * Takes the first aligned run of addresses that fits.
 */
int
ghci_range_alloc(
	struct ghci_range_map *map,
	uint32_t bytes,
	uint32_t alignment,
	uint32_t *address)
{
	uint64_t candidate;
	uint64_t gap_end;
	uint64_t mask;
	unsigned index;
	unsigned slot;

	/* Refuses an empty run. */
	if (bytes == 0U)
		return EINVAL;

	/* Refuses an alignment that is not a power of two. */
	if (alignment == 0U)
		return EINVAL;
	if ((alignment & (alignment - 1U)) != 0U)
		return EINVAL;

	/* Refuses a request when the table cannot record another run. */
	if (map->count >= map->capacity)
		return ENOSPC;

	/*
	 * Tries each gap in address order: before the first run, between two
	 * runs, after the last one.  slot is the index the new run takes.
	 */
	mask = (uint64_t)alignment - 1U;
	candidate = map->base;
	for (slot = 0U; slot <= map->count; slot++) {
		/* The gap ends at the next run, or at the range's limit. */
		gap_end = map->limit;
		if (slot < map->count)
			gap_end = map->extents[slot].address;

		/* Aligns the start of the gap; the first gap that holds the run takes it. */
		candidate = (candidate + mask) & ~mask;
		if (candidate + bytes <= gap_end)
			break;

		/* The next gap starts after this run. */
		if (slot < map->count)
			candidate = (uint64_t)map->extents[slot].address + map->extents[slot].bytes;
	}

	/* Reports a range without a gap large enough. */
	if (slot > map->count)
		return ENOMEM;

	/* Opens the slot by moving the later runs up by one. */
	for (index = map->count; index > slot; index--)
		map->extents[index] = map->extents[index - 1U];

	/* Records the new run. */
	map->extents[slot].address = (uint32_t)candidate;
	map->extents[slot].bytes = bytes;
	map->count++;

	/* Succeeded: the run is the caller's. */
	*address = (uint32_t)candidate;
	return 0;
}

/*
 * Gives back the run that starts at address.
 */
int
ghci_range_free(
	struct ghci_range_map *map,
	uint32_t address)
{
	unsigned index;
	unsigned found;

	/* Finds the run, which is named by its first address. */
	found = map->count;
	for (index = 0U; index < map->count; index++) {
		if (map->extents[index].address == address) {
			found = index;
			break;
		}
	}

	/* Reports an address that starts no run. */
	if (found == map->count)
		return ENOENT;

	/* Closes the hole by moving the later runs down by one. */
	for (index = found; index + 1U < map->count; index++)
		map->extents[index] = map->extents[index + 1U];
	map->count--;

	/* Succeeded: the run is free. */
	return 0;
}

/*
 * Reports the bytes of the range that are still free.
 */
uint64_t
ghci_range_free_bytes(
	const struct ghci_range_map *map)
{
	uint64_t used;
	unsigned index;

	/* Adds up the runs in use. */
	used = 0U;
	for (index = 0U; index < map->count; index++)
		used += map->extents[index].bytes;

	/* Reports the rest of the range. */
	return (map->limit - map->base) - used;
}

/*
 * Reports whether a run of addresses lies wholly inside one run in use.
 */
int
ghci_range_holds(
	const struct ghci_range_map *map,
	uint32_t address,
	uint32_t bytes)
{
	uint64_t start;
	uint64_t end;
	uint64_t run_end;
	unsigned index;

	/* The run asked about, which may end at 2^32. */
	start = address;
	end = start + bytes;

	/* Looks for the run in use that starts at or before the address. */
	for (index = 0U; index < map->count; index++) {
		/* Runs that start after the address cannot hold it, and the table is in order. */
		if (map->extents[index].address > address)
			break;

		/* The run in use holds the run asked about when it reaches its end. */
		run_end = (uint64_t)map->extents[index].address + map->extents[index].bytes;
		if (end <= run_end && start < run_end)
			return 1;
	}

	/* Succeeded: no single run in use holds it. */
	return 0;
}
