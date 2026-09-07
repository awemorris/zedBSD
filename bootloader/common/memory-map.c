/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

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
	enum zbl_memory_result result;

	/* Validates the bounded input and clears the publication field first. */
	if (count == 0)
		return ZBL_MEMORY_INVALID;
	*count = 0;
	if (context == 0 || decode == 0 || ranges == 0 || capacity == 0 ||
	    input_count > ZBL_MEMORY_MAX_INPUTS ||
	    (options & ~ZBL_MEMORY_REJECT_OVERLAP) != 0)
		return ZBL_MEMORY_INVALID;

	/* Validates every input before the first output write. */
	cursor = UINT64_MAX;
	highest = 0;
	for (index = 0; index < input_count; index++) {
		result = get_range(context, index, decode, &range);
		if (result != ZBL_MEMORY_OK)
			return result;
		if (range.size == 0)
			continue;
		if (range.base < cursor)
			cursor = range.base;
		end = range.base + range.size;
		if (end > highest)
			highest = end;
	}
	if (highest == 0)
		return ZBL_MEMORY_EMPTY;

	/* Sweeps all distinct boundaries without treating address holes as RAM. */
	used = 0;
	while (cursor < highest) {
		next = highest;
		selected_valid = 0;
		selected_conflict = 0;
		selected_priority = 0;
		covering = 0;
		for (index = 0; index < input_count; index++) {
			result = get_range(context, index, decode, &range);
			if (result != ZBL_MEMORY_OK)
				return result;
			if (range.size == 0)
				continue;
			end = range.base + range.size;
			if (range.base > cursor && range.base < next)
				next = range.base;
			if (end > cursor && end < next)
				next = end;
			if (range.base > cursor || end <= cursor)
				continue;

			/* Refuses overlap when firmware promises a disjoint map. */
			covering++;
			if ((options & ZBL_MEMORY_REJECT_OVERLAP) != 0 && covering > 1)
				return ZBL_MEMORY_OVERLAP;
			priority = range_priority(range.type);
			if (!selected_valid || priority > selected_priority) {
				selected = range;
				selected_priority = priority;
				selected_valid = 1;
				selected_conflict = 0;
			} else if (priority == selected_priority &&
			    !range_attributes_equal(&selected, &range)) {
				selected_conflict = 1;
			}
		}

		/* Publishes only described intervals, coalescing equal neighbours. */
		if (next <= cursor)
			return ZBL_MEMORY_INVALID;
		if (selected_valid) {
			/* Resolves conflicts after selecting the actual highest priority. */
			if (selected_conflict) {
				selected.type = ZBL6_MEMORY_RESERVED;
				selected.flags = ZBL6_RANGE_MIXED_ATTRIBUTES;
				selected.attributes = 0;
			}
			last = used == 0 ? 0 : &ranges[used - 1U];
			if (last != 0 && last->base + last->size == cursor &&
			    range_attributes_equal(last, &selected)) {
				last->size = next - last->base;
			} else {
				if (used == capacity)
					return ZBL_MEMORY_CAPACITY;
				selected.base = cursor;
				selected.size = next - cursor;
				ranges[used++] = selected;
			}
		}
		cursor = next;
	}

	/* Makes the complete normalized array visible to the caller. */
	*count = used;
	return used == 0 ? ZBL_MEMORY_EMPTY : ZBL_MEMORY_OK;
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

	result = decode(context, index, range);
	if (result != ZBL_MEMORY_OK || range->size == 0)
		return result;
	if (range->size > UINT64_MAX - range->base)
		return ZBL_MEMORY_OVERFLOW;
	if (range->type < ZBL6_MEMORY_USABLE ||
	    range->type > ZBL6_MEMORY_BOOT_RECLAIM)
		range->type = ZBL6_MEMORY_RESERVED;
	end = range->base + range->size;
	mask = ZBL_MEMORY_PAGE_SIZE - 1U;

	/* Allocatable RAM must contain complete pages; protection covers edges. */
	if (range->type == ZBL6_MEMORY_USABLE ||
	    range->type == ZBL6_MEMORY_BOOT_RECLAIM) {
		if (range->base > UINT64_MAX - mask) {
			range->size = 0;
			return ZBL_MEMORY_OK;
		}
		range->base = (range->base + mask) & ~mask;
		end &= ~mask;
	} else {
		if (end > UINT64_MAX - mask)
			return ZBL_MEMORY_OVERFLOW;
		range->base &= ~mask;
		end = (end + mask) & ~mask;
	}
	range->size = end > range->base ? end - range->base : 0;
	return ZBL_MEMORY_OK;
}

/* Gives reservations precedence over memory which could become allocatable. */
static unsigned
range_priority(
	uint32_t type)
{
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
	return left->type == right->type && left->flags == right->flags &&
	    left->attributes == right->attributes;
}
