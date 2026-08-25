/* UEFI memory-map normalization for the amd64 loader handoff. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "memory-map.h"

#define ZBL_UEFI_PAGE_SIZE 4096ULL

static uint32_t
handoff_memory_type(UINT32 uefi_type)
{
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

static void
remove_range(struct zbl6_memory_range *ranges, uint32_t *count, uint32_t index)
{
	uint32_t cursor;

	for (cursor = index + 1U; cursor < *count; cursor++)
		ranges[cursor - 1U] = ranges[cursor];
	(*count)--;
}

static enum zbl_uefi_map_result
insert_range(struct zbl6_memory_range *ranges, uint32_t capacity,
	     uint32_t *count, uint64_t base, uint64_t size, uint32_t type)
{
	uint64_t end = base + size;
	uint32_t position = 0;
	uint32_t cursor;

	while (position < *count && ranges[position].base < base)
		position++;
	if (position != 0) {
		struct zbl6_memory_range *left = &ranges[position - 1U];
		uint64_t left_end = left->base + left->size;

		if (base < left_end)
			return ZBL_UEFI_MAP_OVERLAP;
		if (base == left_end && left->type == type) {
			left->size = end - left->base;
			if (position < *count &&
			    ranges[position].base < left->base + left->size)
				return ZBL_UEFI_MAP_OVERLAP;
			if (position < *count &&
			    ranges[position].base == left->base + left->size &&
			    ranges[position].type == type) {
				left->size += ranges[position].size;
				remove_range(ranges, count, position);
			}
			return ZBL_UEFI_MAP_OK;
		}
	}
	if (position < *count) {
		struct zbl6_memory_range *right = &ranges[position];

		if (end > right->base)
			return ZBL_UEFI_MAP_OVERLAP;
		if (end == right->base && right->type == type) {
			right->size += right->base - base;
			right->base = base;
			return ZBL_UEFI_MAP_OK;
		}
	}
	if (*count == capacity)
		return ZBL_UEFI_MAP_CAPACITY;
	for (cursor = *count; cursor > position; cursor--)
		ranges[cursor] = ranges[cursor - 1U];
	ranges[position].base = base;
	ranges[position].size = size;
	ranges[position].type = type;
	ranges[position].flags = 0;
	(*count)++;
	return ZBL_UEFI_MAP_OK;
}

enum zbl_uefi_map_result
zbl_uefi_normalize_memory_map(const void *raw_map, UINTN map_size,
			      UINTN descriptor_size,
			      struct zbl6_memory_range *ranges,
			      uint32_t range_capacity, uint32_t *range_count)
{
	const uint8_t *cursor = raw_map;
	UINTN offset;
	uint32_t count = 0;
	enum zbl_uefi_map_result result;

	if (raw_map == 0 || ranges == 0 || range_count == 0 ||
	    range_capacity == 0) {
		return ZBL_UEFI_MAP_INVALID_ARGUMENT;
	}
	*range_count = 0;
	if (descriptor_size < sizeof(EFI_MEMORY_DESCRIPTOR) ||
	    map_size % descriptor_size != 0)
		return ZBL_UEFI_MAP_INVALID_DESCRIPTOR_SIZE;
	for (offset = 0; offset < map_size; offset += descriptor_size) {
		const EFI_MEMORY_DESCRIPTOR *descriptor =
		    (const void *)(cursor + offset);
		uint64_t base = descriptor->PhysicalStart;
		uint64_t size;

		if (descriptor->NumberOfPages == 0)
			continue;
		if (descriptor->NumberOfPages > UINT64_MAX / ZBL_UEFI_PAGE_SIZE)
			return ZBL_UEFI_MAP_PAGE_OVERFLOW;
		size = descriptor->NumberOfPages * ZBL_UEFI_PAGE_SIZE;
		if (base > UINT64_MAX - size)
			return ZBL_UEFI_MAP_RANGE_OVERFLOW;
		result =
		    insert_range(ranges, range_capacity, &count, base, size,
				 handoff_memory_type(descriptor->Type));
		if (result != ZBL_UEFI_MAP_OK)
			return result;
	}
	if (count == 0)
		return ZBL_UEFI_MAP_EMPTY;
	*range_count = count;
	return ZBL_UEFI_MAP_OK;
}

const char *
zbl_uefi_map_result_name(enum zbl_uefi_map_result result)
{
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
