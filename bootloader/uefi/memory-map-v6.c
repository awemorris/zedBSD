/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The UEFI memory-map normalization into the v6 handoff's ranges.
 */

#include "memory-map.h"

/* The UEFI attribute of memory the firmware's runtime services keep using. */
#define ZBL_EFI_MEMORY_RUNTIME (1ULL << 63)

/* The UEFI attribute of persistent memory. */
#define ZBL_EFI_MEMORY_NV 0x8000ULL

/* The firmware's descriptor array as the normalizer's decoder walks it. */
struct memory_map_context {
	const uint8_t *raw;
	UINTN stride;
};

static enum zbl_memory_result decode_uefi(const void *context, uint32_t index, struct zbl6_memory_range_v6 *range);

/*
 * Preserves full-width UEFI attributes and deferred boot-services ownership.
 * This pure operation is safe after ExitBootServices and allocates nothing.
 */
enum zbl_memory_result
zbl_uefi_normalize_memory_map_v6(
	const void *raw_map,
	UINTN map_size,
	UINTN descriptor_size,
	struct zbl6_memory_range_v6 *ranges,
	uint32_t capacity,
	uint32_t *range_count)
{
	struct memory_map_context context;
	enum zbl_memory_result result;

	/* Refuses a call with nowhere to publish the count. */
	if (range_count == 0)
		return ZBL_MEMORY_INVALID;

	/* Clears the publication field before anything else can fail. */
	*range_count = 0;

	/* Refuses a descriptor size or map the decoder cannot walk aligned. */
	if (descriptor_size < sizeof(EFI_MEMORY_DESCRIPTOR) ||
	    descriptor_size % 8U != 0 ||
	    map_size % descriptor_size != 0 ||
	    map_size / descriptor_size > ZBL_MEMORY_MAX_INPUTS ||
	    raw_map == 0 ||
	    ((uintptr_t)raw_map & 7U) != 0)
		return ZBL_MEMORY_INVALID;

	/* Refuses a map whose end does not fit in an address. */
	if (map_size > UINTPTR_MAX - (uintptr_t)raw_map)
		return ZBL_MEMORY_OVERFLOW;

	/* Normalizes the descriptors, refusing overlap as UEFI promises a disjoint map. */
	context.raw = raw_map;
	context.stride = descriptor_size;
	result = zbl_memory_normalize(&context,
				      (uint32_t)(map_size / descriptor_size),
				      decode_uefi,
				      ZBL_MEMORY_REJECT_OVERLAP,
				      ranges,
				      capacity,
				      range_count);
	if (result != ZBL_MEMORY_OK)
		return result;

	/* Succeeded: the ranges hold the normalized map. */
	return ZBL_MEMORY_OK;
}

/* Decodes one aligned descriptor while keeping runtime and persistent data owned. */
static enum zbl_memory_result
decode_uefi(
	const void *context,
	uint32_t index,
	struct zbl6_memory_range_v6 *range)
{
	const struct memory_map_context *map;
	const EFI_MEMORY_DESCRIPTOR *descriptor;

	/* Refuses a descriptor that does not start on a page. */
	map = context;
	descriptor = (const void *)(map->raw + (UINTN)index * map->stride);
	if ((descriptor->PhysicalStart & (ZBL_MEMORY_PAGE_SIZE - 1U)) != 0)
		return ZBL_MEMORY_INVALID;

	/* Refuses a page count whose byte size does not fit. */
	if (descriptor->NumberOfPages > UINT64_MAX / ZBL_MEMORY_PAGE_SIZE)
		return ZBL_MEMORY_OVERFLOW;

	/* Copies the descriptor's range and attributes. */
	range->base = descriptor->PhysicalStart;
	range->size = descriptor->NumberOfPages * ZBL_MEMORY_PAGE_SIZE;
	range->flags = 0;
	range->attributes = descriptor->Attribute;

	/* Translates the UEFI type; the loader's and boot services' memory is reclaimed after the handoff. */
	switch (descriptor->Type) {
	case EfiConventionalMemory:
		range->type = ZBL6_MEMORY_USABLE;
		break;
	case EfiLoaderCode:
	case EfiLoaderData:
	case EfiBootServicesCode:
	case EfiBootServicesData:
		range->type = ZBL6_MEMORY_BOOT_RECLAIM;
		break;
	case EfiACPIReclaimMemory:
		range->type = ZBL6_MEMORY_ACPI_RECLAIM;
		break;
	case EfiACPIMemoryNVS:
		range->type = ZBL6_MEMORY_ACPI_NVS;
		break;
	case EfiMemoryMappedIO:
	case EfiMemoryMappedIOPortSpace:
		range->type = ZBL6_MEMORY_MMIO;
		break;
	default:
		range->type = ZBL6_MEMORY_RESERVED;
		break;
	}

	/* Runtime-services and persistent memory stays owned whatever its type. */
	if ((descriptor->Attribute & (ZBL_EFI_MEMORY_RUNTIME | ZBL_EFI_MEMORY_NV)) != 0)
		range->type = ZBL6_MEMORY_RESERVED;

	/* Succeeded: the range describes the descriptor. */
	return ZBL_MEMORY_OK;
}
