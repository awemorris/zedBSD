/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Pure ZBL6 handoff version, size, and flag classification.
 */

#include <stddef.h>

#include "handoff-validation.h"
#include "bootloader/include/amd64-handoff.h"

#define ZBL6_BOOTSTRAP_LIMIT (1ULL << 30)
#define ZBL6_PAGE_SIZE 4096U

static int low_array_valid(uint64_t base, uint32_t count, uint32_t stride);
static int allocation_covers(const struct zbl6_memory_handoff *memory,
    const struct zbl6_boot_allocation *allocations, uint64_t base,
    uint64_t size, uint32_t owner);

/* Bounds pointers before the consumer dereferences firmware-owned arrays. */
int
zbl6_memory_envelope_valid(const struct zbl6_memory_handoff *memory, uint32_t source)
{
	if (memory == NULL ||
	    (source != ZBL6_MEMORY_SOURCE_BIOS_E820 && source != ZBL6_MEMORY_SOURCE_UEFI) ||
	    memory->source != source || memory->flags != ZBL6_MEMORY_MAP_COMPLETE ||
	    memory->range_count == 0 || memory->range_count > ZBL6_MAX_MEMORY_RANGES ||
	    memory->range_entry_size != ZBL6_MEMORY_RANGE_V6_SIZE ||
	    memory->allocation_count == 0 || memory->allocation_count > ZBL6_MAX_BOOT_ALLOCATIONS ||
	    memory->allocation_entry_size != ZBL6_BOOT_ALLOCATION_SIZE)
		return 0;
	if (!low_array_valid(memory->ranges, memory->range_count, memory->range_entry_size) ||
	    !low_array_valid(memory->allocations, memory->allocation_count, memory->allocation_entry_size))
		return 0;
	if (memory->bootstrap_cr3 == 0 || memory->bootstrap_cr3 >= ZBL6_BOOTSTRAP_LIMIT ||
	    (memory->bootstrap_cr3 & (ZBL6_PAGE_SIZE - 1U)) != 0 ||
	    memory->kernel_phys_start != 0x200000U ||
	    memory->kernel_phys_end <= memory->kernel_phys_start ||
	    memory->kernel_phys_end > 0x1200000U)
		return 0;
	return 1;
}

/* The bootstrap still maps one GiB; high RAM descriptors are values only. */
static int
low_array_valid(uint64_t base, uint32_t count, uint32_t stride)
{
	return base != 0 && (base & 7U) == 0 && base < ZBL6_BOOTSTRAP_LIMIT &&
	    (uint64_t)count * stride <= ZBL6_BOOTSTRAP_LIMIT - base;
}

/* Validates immutable copies without following physical pointers on the host. */
int
zbl6_memory_contents_valid(const struct zbl6_memory_handoff *memory,
    const struct zbl6_memory_range_v6 *ranges,
    const struct zbl6_boot_allocation *allocations)
{
	const struct zbl6_memory_range_v6 *range;
	const struct zbl6_boot_allocation *allocation;
	uint64_t previous_end;
	uint32_t i;

	if (memory == NULL || ranges == NULL || allocations == NULL ||
	    !zbl6_memory_envelope_valid(memory, memory->source))
		return 0;
	previous_end = 0;
	for (i = 0; i < memory->range_count; i++) {
		range = &ranges[i];
		if (range->size == 0 || range->size > UINT64_MAX - range->base ||
		    ((range->base | range->size) & (ZBL6_PAGE_SIZE - 1U)) != 0 ||
		    range->base < previous_end || range->type < ZBL6_MEMORY_USABLE ||
		    range->type > ZBL6_MEMORY_BOOT_RECLAIM ||
		    (range->flags & ~ZBL6_RANGE_MIXED_ATTRIBUTES) != 0 ||
		    (range->flags != 0 && (range->type != ZBL6_MEMORY_RESERVED || range->attributes != 0)))
			return 0;
		previous_end = range->base + range->size;
	}
	for (i = 0; i < memory->allocation_count; i++) {
		allocation = &allocations[i];
		if (allocation->size == 0 || allocation->size > UINT64_MAX - allocation->base ||
		    ((allocation->base | allocation->size) & (ZBL6_PAGE_SIZE - 1U)) != 0 ||
		    allocation->owner < ZBL6_BOOT_OWNER_KERNEL || allocation->owner > ZBL6_BOOT_OWNER_STACK ||
		    allocation->lifetime < ZBL6_BOOT_KEEP || allocation->lifetime > ZBL6_BOOT_AFTER_INIT ||
		    (allocation->owner == ZBL6_BOOT_OWNER_KERNEL && allocation->lifetime != ZBL6_BOOT_KEEP) ||
		    (allocation->owner == ZBL6_BOOT_OWNER_BOOTSTRAP && allocation->lifetime == ZBL6_BOOT_AFTER_HANDOFF))
			return 0;
	}
	return allocation_covers(memory, allocations, memory->kernel_phys_start,
	    memory->kernel_phys_end - memory->kernel_phys_start, ZBL6_BOOT_OWNER_KERNEL) &&
	    allocation_covers(memory, allocations, memory->bootstrap_cr3, ZBL6_PAGE_SIZE, ZBL6_BOOT_OWNER_BOOTSTRAP) &&
	    allocation_covers(memory, allocations, memory->ranges,
	    (uint64_t)memory->range_count * memory->range_entry_size, 0) &&
	    allocation_covers(memory, allocations, memory->allocations,
	    (uint64_t)memory->allocation_count * memory->allocation_entry_size, 0);
}

/* One reservation must cover each bootstrap object for its entire lifetime. */
static int
allocation_covers(const struct zbl6_memory_handoff *memory,
    const struct zbl6_boot_allocation *allocations, uint64_t base,
    uint64_t size, uint32_t owner)
{
	const struct zbl6_boot_allocation *allocation;
	uint32_t i;

	for (i = 0; i < memory->allocation_count; i++) {
		allocation = &allocations[i];
		if ((owner == 0 || allocation->owner == owner) &&
		    allocation->base <= base && base - allocation->base < allocation->size &&
		    size <= allocation->size - (base - allocation->base))
			return 1;
	}
	return 0;
}

/*
 * Classifies a decoded ZBL6 handoff envelope.
 */
enum zbl6_handoff_form
zbl6_handoff_classify(
	uint16_t version,
	uint16_t size,
	uint32_t flags)
{
	uint32_t required;
	uint32_t allowed;

	/* Starts with the services required by every legacy UEFI form. */
	required = ZBL6_HANDOFF_FLAG_UEFI |
	    ZBL6_HANDOFF_FLAG_MEMORY_MAP |
	    ZBL6_HANDOFF_FLAG_ACPI_RSDP;

	/* Accepts the original BIOS form only at its minimum size. */
	if (version == ZBL6_HANDOFF_VERSION) {
		/* Accepts an envelope large enough for the legacy BIOS fields. */
		if (size >= ZBL6_HANDOFF_SIZE)
			return ZBL6_HANDOFF_FORM_LEGACY_BIOS;

		/* Rejects a truncated original BIOS envelope. */
		return ZBL6_HANDOFF_FORM_INVALID;
	}

	/* Requires the original UEFI service flags for version two. */
	if (version == ZBL6_HANDOFF_V2_VERSION) {
		/* Accepts a complete envelope carrying every required service. */
		if (size >= ZBL6_HANDOFF_V2_SIZE &&
		    (flags & required) == required)
			return ZBL6_HANDOFF_FORM_LEGACY_UEFI;

		/* Rejects an incomplete version-two UEFI envelope. */
		return ZBL6_HANDOFF_FORM_INVALID;
	}

	/* Adds the framebuffer contract introduced by version three. */
	if (version == ZBL6_HANDOFF_V3_VERSION) {
		required |= ZBL6_HANDOFF_FLAG_FRAMEBUFFER;

		/* Accepts a complete envelope carrying every required service. */
		if (size >= ZBL6_HANDOFF_V3_SIZE &&
		    (flags & required) == required)
			return ZBL6_HANDOFF_FORM_LEGACY_UEFI;

		/* Rejects an incomplete version-three UEFI envelope. */
		return ZBL6_HANDOFF_FORM_INVALID;
	}

	/* Adds the boot-volume identity contract introduced by version four. */
	if (version == ZBL6_HANDOFF_V4_VERSION) {
		required |= ZBL6_HANDOFF_FLAG_FRAMEBUFFER |
		    ZBL6_HANDOFF_FLAG_BOOT_UUID;

		/* Accepts a complete envelope carrying every required service. */
		if (size >= ZBL6_HANDOFF_V4_SIZE &&
		    (flags & required) == required)
			return ZBL6_HANDOFF_FORM_LEGACY_UEFI;

		/* Rejects an incomplete version-four UEFI envelope. */
		return ZBL6_HANDOFF_FORM_INVALID;
	}

	/* V7 is UEFI-only and retains the complete V6 ownership envelope. */
	if (version == ZBL6_HANDOFF_V7_VERSION) {
		required |= ZBL6_HANDOFF_FLAG_FRAMEBUFFER |
		    ZBL6_HANDOFF_FLAG_BOOT_UUID | ZBL6_HANDOFF_FLAG_BOOT_PARAMETERS |
		    ZBL6_HANDOFF_FLAG_BOOT_ALLOCATIONS;
		if (size == ZBL6_HANDOFF_V7_UEFI_SIZE && flags == required)
			return ZBL6_HANDOFF_FORM_V7_UEFI;
		return ZBL6_HANDOFF_FORM_INVALID;
	}

	/* Version six retains both prefixes and requires explicit ownership. */
	if (version == ZBL6_HANDOFF_V6_VERSION) {
		if (size == ZBL6_HANDOFF_V6_BIOS_SIZE) {
			required = ZBL6_HANDOFF_FLAG_MEMORY_MAP |
			    ZBL6_HANDOFF_FLAG_BOOT_PARAMETERS |
			    ZBL6_HANDOFF_FLAG_BOOT_ALLOCATIONS;
			allowed = required | ZBL6_HANDOFF_FLAG_FRAMEBUFFER |
			    ZBL6_HANDOFF_FLAG_BOOT_UUID;
			if ((flags & required) == required && (flags & ~allowed) == 0U)
				return ZBL6_HANDOFF_FORM_V6_BIOS;
		} else if (size == ZBL6_HANDOFF_V6_UEFI_SIZE) {
			required |= ZBL6_HANDOFF_FLAG_FRAMEBUFFER |
			    ZBL6_HANDOFF_FLAG_BOOT_UUID |
			    ZBL6_HANDOFF_FLAG_BOOT_PARAMETERS |
			    ZBL6_HANDOFF_FLAG_BOOT_ALLOCATIONS;
			if (flags == required)
				return ZBL6_HANDOFF_FORM_V6_UEFI;
		}
		return ZBL6_HANDOFF_FORM_INVALID;
	}

	/* Rejects versions outside the supported legacy and version-five set. */
	if (version != ZBL6_HANDOFF_V5_VERSION)
		return ZBL6_HANDOFF_FORM_INVALID;

	/* Recognizes the compact version-five BIOS envelope exactly. */
	if (size == ZBL6_HANDOFF_V5_BIOS_SIZE) {
		allowed = ZBL6_HANDOFF_FLAG_FRAMEBUFFER |
		    ZBL6_HANDOFF_FLAG_BOOT_UUID |
		    ZBL6_HANDOFF_FLAG_BOOT_PARAMETERS;

		/* Accepts only the mandatory parameter flag and allowed options. */
		if ((flags & ZBL6_HANDOFF_FLAG_BOOT_PARAMETERS) != 0U &&
		    (flags & ~allowed) == 0U)
			return ZBL6_HANDOFF_FORM_V5_BIOS;

		/* Rejects a compact BIOS envelope with invalid flags. */
		return ZBL6_HANDOFF_FORM_INVALID;
	}

	/* Recognizes the exact version-five UEFI envelope and flag set. */
	required |= ZBL6_HANDOFF_FLAG_FRAMEBUFFER |
	    ZBL6_HANDOFF_FLAG_BOOT_UUID |
	    ZBL6_HANDOFF_FLAG_BOOT_PARAMETERS;
	if (size == ZBL6_HANDOFF_V5_UEFI_SIZE && flags == required)
		return ZBL6_HANDOFF_FORM_V5_UEFI;

	/* Rejects every other version-five representation. */
	return ZBL6_HANDOFF_FORM_INVALID;
}

/*
 * Validates and classifies a raw ZBL6 handoff envelope.
 */
enum zbl6_handoff_form
zbl6_handoff_classify_raw(
	const void *raw_handoff)
{
	const struct zbl6_handoff *bios;
	const struct zbl6_handoff_v2 *uefi;
	enum zbl6_handoff_form form;
	uint32_t flags;

	/* Interprets the shared prefix through both defined envelope layouts. */
	bios = raw_handoff;
	uefi = raw_handoff;

	/* Requires a present handoff with the protocol magic. */
	if (bios == NULL || bios->magic != ZBL6_HANDOFF_MAGIC)
		return ZBL6_HANDOFF_FORM_INVALID;

	/* Reads flags at the offset selected by the encoded envelope form. */
	if (bios->version == ZBL6_HANDOFF_VERSION ||
	    (bios->version == ZBL6_HANDOFF_V5_VERSION &&
	    bios->size == ZBL6_HANDOFF_V5_BIOS_SIZE) ||
	    (bios->version == ZBL6_HANDOFF_V6_VERSION &&
	    bios->size == ZBL6_HANDOFF_V6_BIOS_SIZE))
		flags = bios->flags;
	else
		flags = uefi->flags;

	/* Classifies the decoded version, size, and selected flags. */
	form = zbl6_handoff_classify(bios->version, bios->size, flags);

	/* Returns the decoded handoff form. */
	return form;
}

/*
 * Validates UEFI partition metadata for a classified handoff version.
 */
int
zbl6_uefi_partition_handoff_valid(
	uint16_t version,
	uint8_t scheme,
	uint8_t root_partition_index,
	uint8_t loader_partition_index,
	uint32_t flags)
{
	/* Requires every accepted envelope to identify UEFI boot. */
	if ((flags & ZBL6_HANDOFF_FLAG_UEFI) == 0U)
		return 0;

	/* Applies the historical MBR ordinal contract to versions two to four. */
	if (version >= ZBL6_HANDOFF_V2_VERSION &&
	    version <= ZBL6_HANDOFF_V4_VERSION) {
		/* Requires the historical MBR partition scheme. */
		if (scheme != ZBL6_PARTITION_SCHEME_MBR)
			return 0;

		/* Requires a one-based primary root partition. */
		if (root_partition_index < 1U || root_partition_index > 4U)
			return 0;

		/* Requires the historical loader partition ordinal. */
		if (loader_partition_index != 2U)
			return 0;

		/* Reports valid historical UEFI partition metadata. */
		return 1;
	}

	/* Requires version five and its authoritative boot-volume UUID. */
	if ((version != ZBL6_HANDOFF_V5_VERSION &&
	    version != ZBL6_HANDOFF_V6_VERSION &&
	    version != ZBL6_HANDOFF_V7_VERSION) ||
	    (flags & ZBL6_HANDOFF_FLAG_BOOT_UUID) == 0U)
		return 0;

	/*
	 * Version five carries only the selected FAT's actual partition-table
	 * style.  Its UUID, not either historically overloaded ordinal, is
	 * authoritative.
	 */
	if (scheme == ZBL6_PARTITION_SCHEME_MBR ||
	    scheme == ZBL6_PARTITION_SCHEME_GPT) {
		/* Requires unspecified ordinals and the authoritative UUID flag. */
		if (root_partition_index != ZBL6_PARTITION_INDEX_UNKNOWN ||
		    loader_partition_index != ZBL6_PARTITION_INDEX_UNKNOWN ||
		    (flags & ZBL6_HANDOFF_FLAG_BOOT_UUID) == 0U)
			return 0;

		/* Reports valid version-five partition metadata. */
		return 1;
	}

	/* Rejects every unsupported partition-table style. */
	return 0;
}
