/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The firmware heap of a PowerVR core whose firmware runs on a MIPS
 * processor, and the page table that processor walks.
 *
 * The heap is one 16 MiB window of the processor's address space.  Every
 * 4 KiB page of it has one 32-bit entry in a single-level table the host
 * fills; the table's four pages are named to the firmware at boot.  The
 * top of the heap holds the three blocks the firmware finds at fixed
 * places (its connection control, the per-OS and the system
 * initialisation records); the code goes at the bottom and the data at
 * the place the firmware file names.
 *
 * Nothing here touches hardware, so it runs on the build host as well.
 */

#ifndef KERN_DRIVERS_GPU_POWERVR_MIPS_TABLE_H
#define KERN_DRIVERS_GPU_POWERVR_MIPS_TABLE_H

#include <stdint.h>

/*
 * The heap's geometry.
 *
 * Values referenced:
 * Linux v7.3 drivers/gpu/drm/imagination/pvr_fw_mips.c
 * (ROGUE_FW_HEAP_MIPS_BASE 0xC0000000, ROGUE_FW_HEAP_MIPS_SHIFT 24,
 * ROGUE_FW_HEAP_MIPS_RESERVED_SIZE 1 MiB); pvr_fw.c
 * (PVR_ROGUE_FW_CONFIG_HEAP_GRANULARITY 64 KiB, three of them, and the
 * order of the connection control, the per-OS and the system records in
 * them); pvr_rogue_mips.h (ROGUE_MIPSFW_LOG2_PAGE_SIZE_4K,
 * ROGUE_MIPSFW_MAX_NUM_PAGETABLE_PAGES 4).
 */
#define POWERVR_HEAP_PROCESSOR_BASE	0xc0000000U
#define POWERVR_HEAP_BYTES		0x1000000U
#define POWERVR_HEAP_RESERVED_BYTES	0x100000U
#define POWERVR_HEAP_CONFIG_STEP	0x10000U
#define POWERVR_HEAP_CONFIG_BYTES	(3U * POWERVR_HEAP_CONFIG_STEP)
#define POWERVR_HEAP_CONFIG_OFFSET	(POWERVR_HEAP_BYTES - POWERVR_HEAP_CONFIG_BYTES)
#define POWERVR_HEAP_CONNECTION_OFFSET	(POWERVR_HEAP_CONFIG_OFFSET)
#define POWERVR_HEAP_OS_INIT_OFFSET	(POWERVR_HEAP_CONFIG_OFFSET + POWERVR_HEAP_CONFIG_STEP)
#define POWERVR_HEAP_SYSTEM_INIT_OFFSET	(POWERVR_HEAP_CONFIG_OFFSET + 2U * POWERVR_HEAP_CONFIG_STEP)
#define POWERVR_HEAP_GENERAL_BYTES	(POWERVR_HEAP_BYTES - POWERVR_HEAP_CONFIG_BYTES - POWERVR_HEAP_RESERVED_BYTES)
#define POWERVR_MIPS_PAGE_BYTES		0x1000U
#define POWERVR_MIPS_TABLE_ENTRIES	(POWERVR_HEAP_BYTES / POWERVR_MIPS_PAGE_BYTES)
#define POWERVR_MIPS_TABLE_BYTES	(POWERVR_MIPS_TABLE_ENTRIES * 4U)
#define POWERVR_MIPS_TABLE_PAGES	(POWERVR_MIPS_TABLE_BYTES / POWERVR_MIPS_PAGE_BYTES)

/*
 * The pages of the heap's general part, one bit each in a map of 64-bit
 * words: set while a block holds the page.
 */
#define POWERVR_HEAP_GENERAL_PAGES	(POWERVR_HEAP_GENERAL_BYTES / POWERVR_MIPS_PAGE_BYTES)
#define POWERVR_HEAP_MAP_WORDS		((POWERVR_HEAP_GENERAL_PAGES + 63U) / 64U)

/* How a heap page is cached by the firmware's processor. */
enum powervr_mips_caching {
	POWERVR_MIPS_CACHED = 0,
	POWERVR_MIPS_UNCACHED
};

/* Reports the processor's address of a heap offset. */
uint32_t powervr_mips_address(uint32_t heap_offset);

/*
 * Builds the table entry that maps one page.
 *
 * The page is readable and writable.  Bus_bits is the width of the core's
 * physical bus, which decides both how many address bits the entry holds
 * and the policy a cached page uses.
 */
uint32_t powervr_mips_entry(uint64_t physical, enum powervr_mips_caching caching, unsigned bus_bits);

/*
 * Maps a physically contiguous run at a heap offset.
 *
 * Offset and size must be whole pages and inside the heap, and the run
 * must be reachable on a bus of bus_bits.  Returns EINVAL otherwise; no
 * entry is changed then.
 */
int powervr_mips_map(volatile uint32_t *table, uint32_t heap_offset, uint64_t physical, uint32_t size, enum powervr_mips_caching caching, unsigned bus_bits);

/* Removes the entries of a heap range mapped earlier. */
void powervr_mips_unmap(volatile uint32_t *table, uint32_t heap_offset, uint32_t size);

/*
 * Takes the first run of free pages of the general part long enough for
 * a block, and reports its first page; ENOMEM when none is.
 */
int powervr_heap_pages_take(uint64_t map[POWERVR_HEAP_MAP_WORDS], uint32_t pages, uint32_t *first);

/*
 * Takes a given run of pages of the general part; EEXIST when a page of
 * it is held, and nothing is taken then.
 */
int powervr_heap_pages_claim(uint64_t map[POWERVR_HEAP_MAP_WORDS], uint32_t first, uint32_t pages);

/* Gives back a run of pages taken earlier. */
void powervr_heap_pages_give(uint64_t map[POWERVR_HEAP_MAP_WORDS], uint32_t first, uint32_t pages);

/*
 * Fills the boot loader's parameters at the start of the boot data page:
 * where the stack and the core's registers are, and the table's pages.
 */
void powervr_mips_boot_parameters(volatile uint8_t *boot_data, uint64_t stack, uint64_t registers, const uint64_t table_pages[POWERVR_MIPS_TABLE_PAGES]);

/*
 * Builds the value that points one of the wrapper's 4 KiB remap windows
 * at a physical page.
 */
uint64_t powervr_mips_remap_target(uint64_t physical);

#endif
