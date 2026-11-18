/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The firmware memory map normalizer shared by the BIOS and UEFI loaders.
 */

#include "memory-map.h"

static enum zbl_memory_result get_range(const void *context, uint32_t index, zbl_memory_decoder decode, struct zbl6_memory_range_v6 *range);
static unsigned range_priority(uint32_t type);
static int range_attributes_equal(const struct zbl6_memory_range_v6 *left, const struct zbl6_memory_range_v6 *right);

/*
 * Normalizes immutable firmware ranges without allocating scratch storage.
 *
 * A bounded boundary sweep keeps real-mode stack use small. Usable pages
 * round inward; protected ranges round outward and win BIOS overlaps.
 * Strict firmware maps can instead reject every overlapping interval.
 * Output is unpublished on error: count remains zero.
 */
enum zbl_memory_result
zbl_memory_normalize(
	const void *context,
	uint32_t input_count,
	zbl_memory_decoder decode,
	unsigned options,
	struct zbl6_memory_range_v6 *ranges,
	uint32_t capacity,
	uint32_t *count)
{
	struct zbl6_memory_range_v6 range;
	struct zbl6_memory_range_v6 selected;
	struct zbl6_memory_range_v6 *last;
	uint64_t cursor;
	uint64_t next;
	uint64_t end;
	uint64_t highest;
	uint32_t index;
	uint32_t used;
	unsigned priority;
	unsigned selected_priority;
	unsigned covering;
	int selected_valid;
	int selected_conflict;
	int equal;
	enum zbl_memory_result result;

	/* Refuses a call with nowhere to publish the count. */
	if (count == 0)
		return ZBL_MEMORY_INVALID;

	/* Clears the publication field before anything can fail. */
	*count = 0;

	/* Refuses missing buffers, an unbounded input and unknown options. */
	if (context == 0 ||
	    decode == 0 ||
	    ranges == 0 ||
	    capacity == 0 ||
	    input_count > ZBL_MEMORY_MAX_INPUTS ||
	    (options & ~ZBL_MEMORY_REJECT_OVERLAP) != 0)
		return ZBL_MEMORY_INVALID;

	/* Validates every input and finds the span it covers before the first output write. */
	cursor = UINT64_MAX;
	highest = 0;
	for (index = 0; index < input_count; index++) {
		/* Reads the range with its page geometry applied. */
		result = get_range(context, index, decode, &range);
		if (result != ZBL_MEMORY_OK)
			return result;

		/* An empty range covers nothing. */
		if (range.size == 0)
			continue;

		/* The sweep starts at the lowest base. */
		if (range.base < cursor)
			cursor = range.base;

		/* The sweep ends at the highest end. */
		end = range.base + range.size;
		if (end > highest)
			highest = end;
	}

	/* Refuses a map that describes no memory at all. */
	if (highest == 0)
		return ZBL_MEMORY_EMPTY;

	/* Sweeps all distinct boundaries without treating address holes as RAM. */
	used = 0;
	while (cursor < highest) {
		/* Starts the interval at the cursor with nothing selected yet. */
		next = highest;
		selected_valid = 0;
		selected_conflict = 0;
		selected_priority = 0;
		covering = 0;

		/* Finds the next boundary and the range that describes the interval. */
		for (index = 0; index < input_count; index++) {
			/* Reads the range with its page geometry applied. */
			result = get_range(context, index, decode, &range);
			if (result != ZBL_MEMORY_OK)
				return result;

			/* An empty range has no boundary and covers nothing. */
			if (range.size == 0)
				continue;

			/* A base beyond the cursor may be the next boundary. */
			end = range.base + range.size;
			if (range.base > cursor && range.base < next)
				next = range.base;

			/* An end beyond the cursor may be the next boundary. */
			if (end > cursor && end < next)
				next = end;

			/* A range that does not cover the cursor plays no part in the interval. */
			if (range.base > cursor || end <= cursor)
				continue;

			/* Refuses overlap when firmware promises a disjoint map. */
			covering++;
			if ((options & ZBL_MEMORY_REJECT_OVERLAP) != 0 && covering > 1)
				return ZBL_MEMORY_OVERLAP;

			/* Ranks the covering range against the one selected so far. */
			priority = range_priority(range.type);

			/* A first or higher-priority range becomes the selection. */
			if (!selected_valid || priority > selected_priority) {
				selected = range;
				selected_priority = priority;
				selected_valid = 1;
				selected_conflict = 0;
			} else if (priority == selected_priority) {
				/* A range of the same priority with other attributes conflicts. */
				equal = range_attributes_equal(&selected, &range);
				if (!equal)
					selected_conflict = 1;
			}
		}

		/* Refuses a sweep that would not advance. */
		if (next <= cursor)
			return ZBL_MEMORY_INVALID;

		/* Publishes only described intervals, coalescing equal neighbours. */
		if (selected_valid) {
			/* Resolves conflicts after selecting the actual highest priority. */
			if (selected_conflict) {
				selected.type = ZBL6_MEMORY_RESERVED;
				selected.flags = ZBL6_RANGE_MIXED_ATTRIBUTES;
				selected.attributes = 0;
			}

			/* Decides whether the interval extends the range published last. */
			last = 0;
			equal = 0;
			if (used != 0) {
				last = &ranges[used - 1U];
				if (last->base + last->size == cursor)
					equal = range_attributes_equal(last, &selected);
			}

			/* Extends the last range, or publishes the interval as a new one. */
			if (equal) {
				last->size = next - last->base;
			} else {
				/* Refuses a map with more intervals than the caller can hold. */
				if (used == capacity)
					return ZBL_MEMORY_CAPACITY;

				/* Stores the interval as the next published range. */
				selected.base = cursor;
				selected.size = next - cursor;
				ranges[used] = selected;
				used++;
			}
		}

		/* The next interval starts at the boundary just found. */
		cursor = next;
	}

	/* Makes the complete normalized array visible to the caller. */
	*count = used;

	/* Reports a map whose every interval was dropped as empty. */
	if (used == 0)
		return ZBL_MEMORY_EMPTY;

	/* Succeeded: the caller's array holds the normalized map. */
	return ZBL_MEMORY_OK;
}

/* Reads one range and applies page geometry without narrowing its address. */
static enum zbl_memory_result
get_range(
	const void *context,
	uint32_t index,
	zbl_memory_decoder decode,
	struct zbl6_memory_range_v6 *range)
{
	uint64_t end;
	uint64_t mask;
	enum zbl_memory_result result;

	/* Decodes the firmware's description of the range. */
	result = decode(context, index, range);
	if (result != ZBL_MEMORY_OK)
		return result;

	/* An empty range needs no page geometry. */
	if (range->size == 0)
		return ZBL_MEMORY_OK;

	/* Refuses a range whose end does not fit in an address. */
	if (range->size > UINT64_MAX - range->base)
		return ZBL_MEMORY_OVERFLOW;

	/* Treats a type this loader does not know as reserved. */
	if (range->type < ZBL6_MEMORY_USABLE ||
	    range->type > ZBL6_MEMORY_BOOT_RECLAIM)
		range->type = ZBL6_MEMORY_RESERVED;

	/* Computes the range's end and the mask of an offset within a page. */
	end = range->base + range->size;
	mask = ZBL_MEMORY_PAGE_SIZE - 1U;

	/* Allocatable RAM must contain complete pages; protection covers edges. */
	if (range->type == ZBL6_MEMORY_USABLE ||
	    range->type == ZBL6_MEMORY_BOOT_RECLAIM) {
		/* RAM that starts in the last page holds no complete page. */
		if (range->base > UINT64_MAX - mask) {
			range->size = 0;
			return ZBL_MEMORY_OK;
		}

		/* Rounds the RAM inward to whole pages. */
		range->base = (range->base + mask) & ~mask;
		end &= ~mask;
	} else {
		/* Refuses a protected range that cannot be rounded up. */
		if (end > UINT64_MAX - mask)
			return ZBL_MEMORY_OVERFLOW;

		/* Rounds the protected range outward to whole pages. */
		range->base &= ~mask;
		end = (end + mask) & ~mask;
	}

	/* RAM smaller than a page rounds to nothing. */
	range->size = 0;
	if (end > range->base)
		range->size = end - range->base;

	/* Succeeded: the range is page aligned. */
	return ZBL_MEMORY_OK;
}

/* Gives reservations precedence over memory which could become allocatable. */
static unsigned
range_priority(
	uint32_t type)
{
	/* Ranks the type; anything reserved or unknown ranks highest. */
	switch (type) {
	case ZBL6_MEMORY_USABLE:
		return 1;
	case ZBL6_MEMORY_BOOT_RECLAIM:
		return 2;
	case ZBL6_MEMORY_ACPI_RECLAIM:
		return 3;
	case ZBL6_MEMORY_ACPI_NVS:
		return 4;
	case ZBL6_MEMORY_MMIO:
		return 5;
	default:
		return 6;
	}
}

/* Tests whether adjacent ranges may share one normalized description. */
static int
range_attributes_equal(
	const struct zbl6_memory_range_v6 *left,
	const struct zbl6_memory_range_v6 *right)
{
	/* Ranges of different types are described apart. */
	if (left->type != right->type)
		return 0;

	/* Ranges with different flags are described apart. */
	if (left->flags != right->flags)
		return 0;

	/* Ranges with different firmware attributes are described apart. */
	if (left->attributes != right->attributes)
		return 0;

	/* Succeeded: one description covers both. */
	return 1;
}
