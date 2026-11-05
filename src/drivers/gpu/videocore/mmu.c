/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The preparation of V3D 4.2's single-level page table.
 *
 * These routines edit only caller-owned memory.  They allocate nothing and
 * do not access the GPU.  The owner serializes edits, keeps jobs away from
 * changing mappings, and cleans the table and flushes the hardware TLB
 * before publishing a mapping or reusing an unmapped physical buffer.
 */

#include <stdbool.h>
#include <stdint.h>

#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/bcm2711-private.h"

/* The PTE bit that makes its physical page accessible. */
#define PAGE_ENTRY_PRESENT 0x10000000U

/* The PTE bit that permits writes to its physical page. */
#define PAGE_ENTRY_WRITABLE 0x20000000U

/* The PTE bits for larger pages, which these 4 KiB routines never split. */
#define PAGE_ENTRY_LARGE 0xc0000000U

/* The exclusive end of the GPU's 32-bit virtual address space. */
#define PAGE_VIRTUAL_END ((uint64_t)1 << 32)

/* The exclusive end of the physical address range encoded by a 24-bit PFN. */
#define PAGE_PHYSICAL_END ((uint64_t)1 << 36)

static bool virtual_span_valid(uint32_t address, uint64_t bytes);

/*
 * Installs writable 4 KiB mappings for one contiguous physical buffer.
 *
 * table has BCM2711_V3D_PAGE_ENTRIES entries and initially has zero in each
 * unused slot.  All input and occupancy checks precede every table write,
 * so a refusal preserves both older mappings and the requested free slots.
 * Returns EINVAL for an unrepresentable span and EBUSY for occupied slots.
 */
int
bcm2711_v3d_pages_map(
	uint32_t *table,
	uint32_t address,
	uint64_t physical,
	uint64_t bytes)
{
	bool valid;
	uint32_t first;
	uint32_t count;
	uint32_t index;
	uint32_t physical_page;
	uint32_t entry;

	/* Bounds the GPU span before computing any table index. */
	valid = virtual_span_valid(address, bytes);
	if (!valid)
		return EINVAL;

	/* Refuses physical addresses that lose low bits when converted to PFNs. */
	if ((physical & (BCM2711_V3D_PAGE_BYTES - 1U)) != 0)
		return EINVAL;

	/* Bounds the physical start before computing its remaining range. */
	if (physical >= PAGE_PHYSICAL_END)
		return EINVAL;

	/* Refuses a span that would truncate a physical PFN or wrap its end. */
	if (bytes > PAGE_PHYSICAL_END - physical)
		return EINVAL;

	/* Checks the entire reservation before replacing any entry. */
	first = address / BCM2711_V3D_PAGE_BYTES;
	count = (uint32_t)(bytes / BCM2711_V3D_PAGE_BYTES);
	for (index = 0; index < count; index++) {
		/* Preserves mappings owned by another buffer, including invalid residues. */
		if (table[first + index] != 0)
			return EBUSY;
	}

	/* Encodes consecutive 4 KiB pages without setting larger-page flags. */
	physical_page = (uint32_t)(physical / BCM2711_V3D_PAGE_BYTES);
	for (index = 0; index < count; index++) {
		/* Gives each virtual slot its physical PFN and access bits. */
		entry = physical_page + index;
		entry |= PAGE_ENTRY_PRESENT;
		entry |= PAGE_ENTRY_WRITABLE;
		table[first + index] = entry;
	}

	/* Succeeded: the owner can now clean the table and flush the hardware TLB. */
	return 0;
}

/*
 * Removes one fully mapped 4 KiB span from the software page table.
 *
 * The owner identifies the buffer and serializes its removal against jobs.
 * Returns EINVAL for an invalid span or larger-page entry, and ENOENT if
 * any requested entry is absent.  Refusals leave the entire table unchanged.
 * Physical storage remains owned until the caller's hardware TLB flush ends.
 */
int
bcm2711_v3d_pages_unmap(
	uint32_t *table,
	uint32_t address,
	uint64_t bytes)
{
	bool valid;
	uint32_t first;
	uint32_t count;
	uint32_t index;
	uint32_t entry;

	/* Bounds the GPU span before inspecting any mapping. */
	valid = virtual_span_valid(address, bytes);
	if (!valid)
		return EINVAL;

	/* Rejects holes and larger pages before removing the first entry. */
	first = address / BCM2711_V3D_PAGE_BYTES;
	count = (uint32_t)(bytes / BCM2711_V3D_PAGE_BYTES);
	for (index = 0; index < count; index++) {
		/* Refuses a span with a missing valid page before changing earlier slots. */
		entry = table[first + index];
		if ((entry & PAGE_ENTRY_PRESENT) == 0)
			return ENOENT;

		/* Avoids partially dismantling an unsupported larger-page mapping. */
		if ((entry & PAGE_ENTRY_LARGE) != 0)
			return EINVAL;
	}

	/* Returns only the requested slots to the software allocator. */
	for (index = 0; index < count; index++)
		table[first + index] = 0;

	/* Succeeded: storage can be reused after the caller's hardware TLB flush. */
	return 0;
}

/* Accepts aligned nonempty GPU spans while preserving the reserved zero page. */
static bool
virtual_span_valid(
	uint32_t address,
	uint64_t bytes)
{
	/* Keeps address zero unavailable to hardware that treats it as disabled. */
	if (address < BCM2711_V3D_PAGE_BYTES)
		return false;

	/* Refuses a virtual address whose low bits cannot name a table entry. */
	if ((address & (BCM2711_V3D_PAGE_BYTES - 1U)) != 0)
		return false;

	/* Requires at least one complete 4 KiB page. */
	if (bytes == 0)
		return false;

	/* Refuses a partial last page rather than silently extending the buffer. */
	if ((bytes & (BCM2711_V3D_PAGE_BYTES - 1U)) != 0)
		return false;

	/* Bounds the length without computing a potentially overflowing end. */
	if (bytes > PAGE_VIRTUAL_END - address)
		return false;

	/* Succeeded: both ends describe slots within the nonzero GPU VA range. */
	return true;
}
