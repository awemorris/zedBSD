/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The riscv64 loader's list of the RAM the firmware keeps.
 *
 * The handoff's array holds the ranges joined where they touch, so a run
 * of neighbouring firmware ranges takes one slot.  A range that finds no
 * slot is not dropped silently: the handoff's flags say the list is short,
 * and the HAL refuses to boot on it.
 */

#include "riscv64-reserved.h"

/* The bytes of one page of the firmware's memory map. */
#define RESERVED_PAGE_SIZE	4096ULL

static void add_range(struct zbl6_riscv64_handoff *handoff, uint64_t base, uint64_t end);

/*
 * Reports whether the firmware still uses memory of a map type after boot
 * services end: reserved memory, the runtime services' code and data,
 * memory that is unusable, ACPI tables and storage, PAL code and
 * persistent memory.  The kernel must not allocate it.
 *
 * Values referenced: UEFI Specification 2.10, section 7.2
 * (EFI_MEMORY_TYPE, and the usage of each type after
 * ExitBootServices()).
 */
int
zbl_riscv64_firmware_keeps(
	UINT32 type)
{
	/* Picks the types the firmware keeps. */
	switch (type) {
	case EfiReservedMemoryType:
	case EfiRuntimeServicesCode:
	case EfiRuntimeServicesData:
	case EfiUnusableMemory:
	case EfiACPIReclaimMemory:
	case EfiACPIMemoryNVS:
	case EfiPalCode:
	case EfiPersistentMemory:
		return 1;
	default:
		break;
	}

	/* Every other type is the kernel's once boot services end, or not RAM. */
	return 0;
}

/*
 * Adds one range to the handoff's list of the firmware's RAM, joined with
 * every listed range it touches.
 *
 * A range that wraps the address space is cut at its top.  When no slot is
 * left the handoff's flags say ZBL6_RISCV64_RESERVED_OVERFLOW.
 */
void
zbl_riscv64_reserved_add(
	struct zbl6_riscv64_handoff *handoff,
	uint64_t base,
	uint64_t size)
{
	uint64_t end;

	/* An empty range adds nothing. */
	if (size == 0)
		return;

	/* Cuts a range whose end does not fit at the top of the address space. */
	end = ~0ULL;
	if (size <= ~0ULL - base)
		end = base + size;

	/* Lists the range, joined with the ones it touches. */
	add_range(handoff, base, end);
}

/*
 * Fills the handoff's list from the firmware's memory map, after the
 * ranges already listed (the screen's).
 *
 * The map is map_size bytes of descriptors descriptor_size bytes apart; a
 * descriptor size smaller than the descriptor is not read at all.
 */
void
zbl_riscv64_reserved_collect(
	struct zbl6_riscv64_handoff *handoff,
	const void *map,
	UINTN map_size,
	UINTN descriptor_size)
{
	const EFI_MEMORY_DESCRIPTOR *descriptor;
	const uint8_t *bytes;
	uint64_t size;
	UINTN offset;
	int keeps;

	/* Refuses a map whose descriptors are shorter than the ones it describes. */
	if (map == 0 || descriptor_size < sizeof(EFI_MEMORY_DESCRIPTOR))
		return;

	/* Lists the RAM of each descriptor the firmware keeps. */
	bytes = map;
	for (offset = 0; map_size - offset >= descriptor_size; offset += descriptor_size) {
		/* Skips memory the kernel may use, or that is not RAM. */
		descriptor = (const EFI_MEMORY_DESCRIPTOR *)(const void *)(bytes + offset);
		keeps = zbl_riscv64_firmware_keeps(descriptor->Type);
		if (!keeps)
			continue;

		/* Cuts a page count whose bytes do not fit, which the add cuts at the top. */
		size = ~0ULL;
		if (descriptor->NumberOfPages <= ~0ULL / RESERVED_PAGE_SIZE)
			size = descriptor->NumberOfPages * RESERVED_PAGE_SIZE;

		/* Lists the descriptor's range. */
		zbl_riscv64_reserved_add(handoff, descriptor->PhysicalStart, size);
	}
}

/*
 * Lists the range from base to end, absorbing every listed range it
 * touches so the listed ranges never touch each other.
 */
static void
add_range(
	struct zbl6_riscv64_handoff *handoff,
	uint64_t base,
	uint64_t end)
{
	uint64_t listed_base;
	uint64_t listed_end;
	uint32_t index;
	uint32_t last;

	/* Absorbs each listed range the new one touches, taking the last slot's range into its place. */
	index = 0;
	while (index < handoff->reserved_count) {
		/* Takes the bounds of the listed range in this slot. */
		listed_base = handoff->reserved[index].phys;
		listed_end = listed_base + handoff->reserved[index].size;

		/* Steps over a listed range that lies apart from the new one. */
		if (listed_base > end || listed_end < base) {
			index++;
			continue;
		}

		/* Widens the new range over the listed one. */
		if (listed_base < base)
			base = listed_base;
		if (listed_end > end)
			end = listed_end;

		/*
		 * Frees the slot by moving the last slot's range into it; the moved
		 * range is looked at next, in this same slot.
		 */
		last = handoff->reserved_count - 1U;
		handoff->reserved[index].phys = handoff->reserved[last].phys;
		handoff->reserved[index].size = handoff->reserved[last].size;
		handoff->reserved_count = last;
	}

	/* Says the list is short when no slot is left. */
	if (handoff->reserved_count >= ZBL6_RISCV64_RESERVED_MAX) {
		handoff->reserved_flags |= ZBL6_RISCV64_RESERVED_OVERFLOW;
		return;
	}

	/* Lists the range in the next slot. */
	handoff->reserved[handoff->reserved_count].phys = base;
	handoff->reserved[handoff->reserved_count].size = end - base;
	handoff->reserved_count++;
}
