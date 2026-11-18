/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The UEFI memory-map normalization into the earlier range form.
 */

#include "memory-map.h"

/* The size of a UEFI page. */
#define ZBL_UEFI_PAGE_SIZE 4096ULL

static uint32_t handoff_memory_type(UINT32 uefi_type);
static void remove_range(struct zbl6_memory_range *ranges, uint32_t *count, uint32_t index);
static enum zbl_uefi_map_result insert_range(struct zbl6_memory_range *ranges, uint32_t capacity, uint32_t *count, uint64_t base, uint64_t size, uint32_t type);

/*
 * Normalizes a UEFI memory map into sorted, coalesced ranges.
 *
 * Overlapping descriptors are refused.  The count is published only when the
 * whole map was taken.
 */
enum zbl_uefi_map_result
zbl_uefi_normalize_memory_map(
	const void *raw_map,
	UINTN map_size,
	UINTN descriptor_size,
	struct zbl6_memory_range *ranges,
	uint32_t range_capacity,
	uint32_t *range_count)
{
	const EFI_MEMORY_DESCRIPTOR *descriptor;
	const uint8_t *cursor;
	UINTN offset;
	uint64_t base;
	uint64_t size;
	uint32_t count;
	uint32_t type;
	enum zbl_uefi_map_result result;

	/* Views the map as bytes. */
	cursor = raw_map;
	count = 0;

	/* Refuses missing buffers and an output with no room. */
	if (raw_map == 0 ||
	    ranges == 0 ||
	    range_count == 0 ||
	    range_capacity == 0)
		return ZBL_UEFI_MAP_INVALID_ARGUMENT;

	/* Clears the publication field before anything else can fail. */
	*range_count = 0;

	/* Refuses a descriptor size smaller than a descriptor or not dividing the map. */
	if (descriptor_size < sizeof(EFI_MEMORY_DESCRIPTOR) || map_size % descriptor_size != 0)
		return ZBL_UEFI_MAP_INVALID_DESCRIPTOR_SIZE;

	/* Inserts each descriptor's range into the sorted list. */
	for (offset = 0; offset < map_size; offset += descriptor_size) {
		/* An empty descriptor describes nothing. */
		descriptor = (const void *)(cursor + offset);
		base = descriptor->PhysicalStart;
		if (descriptor->NumberOfPages == 0)
			continue;

		/* Refuses a page count whose byte size does not fit. */
		if (descriptor->NumberOfPages > UINT64_MAX / ZBL_UEFI_PAGE_SIZE)
			return ZBL_UEFI_MAP_PAGE_OVERFLOW;

		/* Refuses a range whose end does not fit. */
		size = descriptor->NumberOfPages * ZBL_UEFI_PAGE_SIZE;
		if (base > UINT64_MAX - size)
			return ZBL_UEFI_MAP_RANGE_OVERFLOW;

		/* Inserts the range under its handoff type. */
		type = handoff_memory_type(descriptor->Type);
		result = insert_range(ranges, range_capacity, &count, base, size, type);
		if (result != ZBL_UEFI_MAP_OK)
			return result;
	}

	/* Refuses a map that describes nothing. */
	if (count == 0)
		return ZBL_UEFI_MAP_EMPTY;

	/* Publishes the count. */
	*range_count = count;

	/* Succeeded: the ranges hold the normalized map. */
	return ZBL_UEFI_MAP_OK;
}

/* Names a normalization result for a loader message. */
const char *
zbl_uefi_map_result_name(
	enum zbl_uefi_map_result result)
{
	/* Names each result; anything else is unknown. */
	switch (result) {
	case ZBL_UEFI_MAP_OK:
		return "ok";
	case ZBL_UEFI_MAP_INVALID_ARGUMENT:
		return "invalid argument";
	case ZBL_UEFI_MAP_INVALID_DESCRIPTOR_SIZE:
		return "descriptor size";
	case ZBL_UEFI_MAP_PAGE_OVERFLOW:
		return "page overflow";
	case ZBL_UEFI_MAP_RANGE_OVERFLOW:
		return "range overflow";
	case ZBL_UEFI_MAP_OVERLAP:
		return "overlap";
	case ZBL_UEFI_MAP_CAPACITY:
		return "range capacity";
	case ZBL_UEFI_MAP_EMPTY:
		return "empty map";
	default:
		return "unknown";
	}
}

/* Translates a UEFI memory type to the handoff's. */
static uint32_t
handoff_memory_type(
	UINT32 uefi_type)
{
	/* Translates the type; anything the kernel may not use is reserved. */
	switch (uefi_type) {
	case EfiConventionalMemory:
		return ZBL6_MEMORY_USABLE;
	case EfiACPIReclaimMemory:
		return ZBL6_MEMORY_ACPI_RECLAIM;
	case EfiACPIMemoryNVS:
		return ZBL6_MEMORY_ACPI_NVS;
	case EfiMemoryMappedIO:
	case EfiMemoryMappedIOPortSpace:
		return ZBL6_MEMORY_MMIO;
	default:
		return ZBL6_MEMORY_RESERVED;
	}
}

/* Removes one range from the list, moving the later ones down. */
static void
remove_range(
	struct zbl6_memory_range *ranges,
	uint32_t *count,
	uint32_t index)
{
	uint32_t cursor;

	/* Moves every later range down by one. */
	for (cursor = index + 1U; cursor < *count; cursor++)
		ranges[cursor - 1U] = ranges[cursor];

	/* The list is one range shorter. */
	(*count)--;
}

/* Inserts a range into the sorted list, merging it with equal-typed neighbours. */
static enum zbl_uefi_map_result
insert_range(
	struct zbl6_memory_range *ranges,
	uint32_t capacity,
	uint32_t *count,
	uint64_t base,
	uint64_t size,
	uint32_t type)
{
	struct zbl6_memory_range *left;
	struct zbl6_memory_range *right;
	uint64_t end;
	uint64_t left_end;
	uint32_t position;
	uint32_t cursor;

	/* Finds the first range that does not start below the new one. */
	end = base + size;
	position = 0;
	while (position < *count && ranges[position].base < base)
		position++;

	/* Compares the range with its left neighbour. */
	if (position != 0) {
		/* Refuses a range that starts inside its left neighbour. */
		left = &ranges[position - 1U];
		left_end = left->base + left->size;
		if (base < left_end)
			return ZBL_UEFI_MAP_OVERLAP;

		/* A range that continues its left neighbour's type extends it. */
		if (base == left_end && left->type == type) {
			left->size = end - left->base;

			/* Refuses an extension that runs into the right neighbour. */
			if (position < *count && ranges[position].base < left->base + left->size)
				return ZBL_UEFI_MAP_OVERLAP;

			/* Absorbs a right neighbour of the same type that now touches it. */
			if (position < *count &&
			    ranges[position].base == left->base + left->size &&
			    ranges[position].type == type) {
				left->size += ranges[position].size;
				remove_range(ranges, count, position);
			}

			/* Succeeded: the left neighbour holds the range. */
			return ZBL_UEFI_MAP_OK;
		}
	}

	/* Compares the range with its right neighbour. */
	if (position < *count) {
		/* Refuses a range that runs into its right neighbour. */
		right = &ranges[position];
		if (end > right->base)
			return ZBL_UEFI_MAP_OVERLAP;

		/* A range that ends where its right neighbour of the same type starts extends it down. */
		if (end == right->base && right->type == type) {
			right->size += right->base - base;
			right->base = base;
			return ZBL_UEFI_MAP_OK;
		}
	}

	/* Refuses a new range with the list full. */
	if (*count == capacity)
		return ZBL_UEFI_MAP_CAPACITY;

	/* Moves the later ranges up to open the position. */
	for (cursor = *count; cursor > position; cursor--)
		ranges[cursor] = ranges[cursor - 1U];

	/* Stores the range at the position. */
	ranges[position].base = base;
	ranges[position].size = size;
	ranges[position].type = type;
	ranges[position].flags = 0;
	(*count)++;

	/* Succeeded: the list holds the range. */
	return ZBL_UEFI_MAP_OK;
}
