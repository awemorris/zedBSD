/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The MIPS firmware processor's page table and boot parameters.
 *
 * The host writes every entry; the processor only reads them.  The table
 * lives in memory the host writes through an uncached view, so an entry
 * is in memory as soon as its store completes.
 */

#include "drivers/gpu/powervr/mips-table.h"
#include <uapi/errno.h>

/*
 * The fields of a page table entry.
 *
 * Values referenced:
 * Linux v7.3 drivers/gpu/drm/imagination/pvr_rogue_mips.h
 * (ROGUE_MIPSFW_ENTRYLO_PFN_SHIFT 6, ROGUE_MIPSFW_ENTRYLO_PFN_MASK and
 * ..._ABOVE_32BIT, ROGUE_MIPSFW_ENTRYLO_CACHE_POLICY_SHIFT 3, the dirty,
 * valid and global bits, ROGUE_MIPSFW_UNCACHED_CACHE_POLICY 2,
 * ROGUE_MIPSFW_WRITEBACK_CACHE_POLICY 3 and
 * ROGUE_MIPSFW_WRITETHROUGH_CACHE_POLICY 1, and which of them a cached
 * page uses on a bus of 32 bits and on a wider one).
 */
#define ENTRY_FRAME_SHIFT		6U
#define ENTRY_FRAME_MASK_NARROW		0x03ffffc0U
#define ENTRY_FRAME_MASK_WIDE		0x3fffffc0U
#define ENTRY_POLICY_SHIFT		3U
#define ENTRY_WRITABLE			0x4U
#define ENTRY_VALID			0x2U
#define ENTRY_GLOBAL			0x1U
#define POLICY_UNCACHED			2U
#define POLICY_WRITE_BACK		3U
#define POLICY_WRITE_THROUGH		1U
#define NARROW_BUS_BITS			32U

/*
 * The boot loader's parameters at the start of the boot data page.
 *
 * Values referenced:
 * Linux v7.3 drivers/gpu/drm/imagination/pvr_rogue_mips.h
 * (ROGUE_MIPSFW_BOOTLDR_CONF_OFFSET 0 and struct rogue_mipsfw_boot_data:
 * the stack at 0, the registers at 8, four table pages from 16, the
 * page size's log at 48 and the page count at 52, two reserved words).
 */
#define BOOT_STACK			0U
#define BOOT_REGISTERS			8U
#define BOOT_TABLE_PAGES		16U
#define BOOT_PAGE_SIZE_LOG		48U
#define BOOT_PAGE_COUNT			52U
#define BOOT_RESERVED			56U
#define MIPS_PAGE_SIZE_LOG		12U

/*
 * The wrapper's remap windows: where the output address sits and the
 * window size each one covers.
 *
 * Values referenced:
 * Linux v7.3 drivers/gpu/drm/imagination/pvr_rogue_cr_defs.h
 * (ROGUE_CR_MIPS_ADDR_REMAP1_CONFIG2_ADDR_OUT_CLRMSK) and
 * pvr_rogue_mips.h (ROGUE_MIPSFW_BOOT_REMAP_LOG2_SEGMENT_SIZE 12).
 */
#define REMAP_OUTPUT_MASK		0x000000fffffff000ULL
#define REMAP_WINDOW_LOG		12U

static void put32(volatile uint8_t *bytes, uint32_t value);
static void put64(volatile uint8_t *bytes, uint64_t value);
static int page_held(const uint64_t *map, uint32_t page);

/*
 * Reports the processor's address of a heap offset.
 *
 * Procedure referenced:
 * Linux v7.3 drivers/gpu/drm/imagination/pvr_fw_mips.c
 * (pvr_mips_get_fw_addr_with_offset: the offset within the heap, with the
 * heap's base above it; the caching comes from the table, not the address).
 */
uint32_t
powervr_mips_address(
	uint32_t heap_offset)
{
	uint32_t address;

	/* The offset within the window, under the window's base. */
	address = (heap_offset & (POWERVR_HEAP_BYTES - 1U)) | POWERVR_HEAP_PROCESSOR_BASE;

	/* Succeeded: the processor's address. */
	return address;
}

/*
 * Builds the table entry that maps one page.
 */
uint32_t
powervr_mips_entry(
	uint64_t physical,
	enum powervr_mips_caching caching,
	unsigned bus_bits)
{
	uint32_t frame_mask;
	uint32_t policy;
	uint32_t entry;

	/* A bus wider than 32 bits takes more frame bits and writes through when cached. */
	if (bus_bits > NARROW_BUS_BITS) {
		frame_mask = ENTRY_FRAME_MASK_WIDE;
		policy = POLICY_WRITE_THROUGH;
	} else {
		frame_mask = ENTRY_FRAME_MASK_NARROW;
		policy = POLICY_WRITE_BACK;
	}

	/* A page the host shares with the firmware is not cached by the processor. */
	if (caching == POWERVR_MIPS_UNCACHED)
		policy = POLICY_UNCACHED;

	/* The frame number, the policy, and a readable, writable, valid global page. */
	entry = (uint32_t)(((physical >> MIPS_PAGE_SIZE_LOG) << ENTRY_FRAME_SHIFT) & frame_mask);
	entry |= policy << ENTRY_POLICY_SHIFT;
	entry |= ENTRY_WRITABLE | ENTRY_VALID | ENTRY_GLOBAL;

	/* Succeeded: the entry. */
	return entry;
}

/*
 * Maps a physically contiguous run at a heap offset.
 */
int
powervr_mips_map(
	volatile uint32_t *table,
	uint32_t heap_offset,
	uint64_t physical,
	uint32_t size,
	enum powervr_mips_caching caching,
	unsigned bus_bits)
{
	uint32_t first;
	uint32_t count;
	uint32_t index;

	/* Refuses partial pages and an empty run. */
	if (size == 0U || (size % POWERVR_MIPS_PAGE_BYTES) != 0U)
		return EINVAL;
	if ((heap_offset % POWERVR_MIPS_PAGE_BYTES) != 0U || (physical % POWERVR_MIPS_PAGE_BYTES) != 0U)
		return EINVAL;

	/* Refuses a range past the heap. */
	if (heap_offset >= POWERVR_HEAP_BYTES || size > POWERVR_HEAP_BYTES - heap_offset)
		return EINVAL;

	/* Refuses a run the core's bus cannot reach. */
	if (bus_bits < 64U && physical + size > (1ULL << bus_bits))
		return EINVAL;

	/* Writes one entry per page, each page following the last. */
	first = heap_offset / POWERVR_MIPS_PAGE_BYTES;
	count = size / POWERVR_MIPS_PAGE_BYTES;
	for (index = 0; index < count; index++)
		table[first + index] = powervr_mips_entry(physical + (uint64_t)index * POWERVR_MIPS_PAGE_BYTES, caching, bus_bits);

	/* Succeeded: the range is visible to the processor. */
	return 0;
}

/*
 * Removes the entries of a heap range mapped earlier.
 */
void
powervr_mips_unmap(
	volatile uint32_t *table,
	uint32_t heap_offset,
	uint32_t size)
{
	uint32_t first;
	uint32_t count;
	uint32_t index;

	/* A range past the heap was never mapped. */
	if (heap_offset >= POWERVR_HEAP_BYTES || size > POWERVR_HEAP_BYTES - heap_offset)
		return;

	/* Clears each entry the range covers; a zero entry is not valid. */
	first = heap_offset / POWERVR_MIPS_PAGE_BYTES;
	count = (size + POWERVR_MIPS_PAGE_BYTES - 1U) / POWERVR_MIPS_PAGE_BYTES;
	for (index = 0; index < count; index++)
		table[first + index] = 0;
}

/*
 * Fills the boot loader's parameters at the start of the boot data page.
 *
 * Procedure referenced:
 * Linux v7.3 drivers/gpu/drm/imagination/pvr_fw_mips.c
 * (pvr_mips_fw_process: the stack's and the registers' physical
 * addresses, each table page's physical address, 4 KiB pages, four of
 * them).
 */
void
powervr_mips_boot_parameters(
	volatile uint8_t *boot_data,
	uint64_t stack,
	uint64_t registers,
	const uint64_t table_pages[POWERVR_MIPS_TABLE_PAGES])
{
	uint32_t page;

	/* Where the stack and the core's registers are. */
	put64(boot_data + BOOT_STACK, stack);
	put64(boot_data + BOOT_REGISTERS, registers);

	/* Each of the table's pages, in order. */
	for (page = 0; page < POWERVR_MIPS_TABLE_PAGES; page++)
		put64(boot_data + BOOT_TABLE_PAGES + page * 8U, table_pages[page]);

	/* The table's page size and page count, then the reserved words. */
	put32(boot_data + BOOT_PAGE_SIZE_LOG, MIPS_PAGE_SIZE_LOG);
	put32(boot_data + BOOT_PAGE_COUNT, POWERVR_MIPS_TABLE_PAGES);
	put32(boot_data + BOOT_RESERVED, 0);
	put32(boot_data + BOOT_RESERVED + 4U, 0);
}

/*
 * Builds the value that points one of the wrapper's 4 KiB remap windows
 * at a physical page.
 */
uint64_t
powervr_mips_remap_target(
	uint64_t physical)
{
	uint64_t target;

	/* The page's address bits, with the window's size in the low bits. */
	target = (physical & REMAP_OUTPUT_MASK) | REMAP_WINDOW_LOG;

	/* Succeeded: the register value. */
	return target;
}

/*
 * Takes the first run of free pages of the general part long enough for
 * a block, and reports its first page.
 */
int
powervr_heap_pages_take(
	uint64_t map[POWERVR_HEAP_MAP_WORDS],
	uint32_t pages,
	uint32_t *first)
{
	uint32_t start;
	uint32_t length;
	uint32_t page;
	int held;

	/* Refuses an empty run and one longer than the general part. */
	if (pages == 0U || pages > POWERVR_HEAP_GENERAL_PAGES)
		return EINVAL;

	/* Walks the pages, growing a run of free ones and restarting it after each held page. */
	start = 0;
	length = 0;
	for (page = 0; page < POWERVR_HEAP_GENERAL_PAGES; page++) {
		held = page_held(map, page);
		if (held) {
			start = page + 1U;
			length = 0;
			continue;
		}

		/* Stops once the run is long enough. */
		length++;
		if (length == pages)
			break;
	}

	/* No run is long enough. */
	if (length != pages)
		return ENOMEM;

	/* Marks the run held. */
	for (page = start; page < start + pages; page++)
		map[page / 64U] |= 1ULL << (page % 64U);

	/* Succeeded: the run's first page. */
	*first = start;
	return 0;
}

/*
 * Takes a given run of pages of the general part.
 */
int
powervr_heap_pages_claim(
	uint64_t map[POWERVR_HEAP_MAP_WORDS],
	uint32_t first,
	uint32_t pages)
{
	uint32_t page;
	int held;

	/* Refuses a run outside the general part. */
	if (first >= POWERVR_HEAP_GENERAL_PAGES || pages > POWERVR_HEAP_GENERAL_PAGES - first)
		return EINVAL;

	/* Refuses the run when any page of it is held, before marking anything. */
	for (page = first; page < first + pages; page++) {
		held = page_held(map, page);
		if (held)
			return EEXIST;
	}

	/* Marks the run held. */
	for (page = first; page < first + pages; page++)
		map[page / 64U] |= 1ULL << (page % 64U);

	/* Succeeded: the run is the caller's. */
	return 0;
}

/*
 * Gives back a run of pages taken earlier.
 */
void
powervr_heap_pages_give(
	uint64_t map[POWERVR_HEAP_MAP_WORDS],
	uint32_t first,
	uint32_t pages)
{
	uint32_t page;

	/* Clears the run's bits; pages past the general part were never tracked. */
	for (page = first; page < first + pages && page < POWERVR_HEAP_GENERAL_PAGES; page++)
		map[page / 64U] &= ~(1ULL << (page % 64U));
}

/* Reports whether a page of the general part is held. */
static int
page_held(
	const uint64_t *map,
	uint32_t page)
{
	/* The page's bit. */
	if ((map[page / 64U] & (1ULL << (page % 64U))) != 0U)
		return 1;

	/* The page is free. */
	return 0;
}

/* Stores a little-endian 32-bit field a byte at a time. */
static void
put32(
	volatile uint8_t *bytes,
	uint32_t value)
{
	/* Lowest byte first. */
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
	bytes[2] = (uint8_t)(value >> 16);
	bytes[3] = (uint8_t)(value >> 24);
}

/* Stores a little-endian 64-bit field. */
static void
put64(
	volatile uint8_t *bytes,
	uint64_t value)
{
	/* The low half first. */
	put32(bytes, (uint32_t)value);
	put32(bytes + 4, (uint32_t)(value >> 32));
}
