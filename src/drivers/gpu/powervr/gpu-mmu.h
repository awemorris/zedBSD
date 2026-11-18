/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The page tables the PowerVR core's own MMU walks for one memory
 * context, and the context's address space.
 *
 * The core translates a 40-bit device address in three levels: a page
 * catalogue of 1024 32-bit entries, page directories of 512 64-bit
 * entries and page tables of 512 64-bit entries, each table one 4 KiB
 * page.  The driver maps 4 KiB pages only; larger pages fault on this
 * core.  Entries never ask the core to snoop the processor's caches.
 *
 * The tables live in memory the caller supplies through a small set of
 * callbacks, so the same code runs on the build host over plain memory.
 */

#ifndef KERN_DRIVERS_GPU_POWERVR_GPU_MMU_H
#define KERN_DRIVERS_GPU_POWERVR_GPU_MMU_H

#include <stddef.h>
#include <stdint.h>

/*
 * The geometry of the three levels.
 *
 * Values referenced:
 * StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
 * drivers/gpu/drm/imagination/pvr_rogue_mmu_defs.h
 * (ROGUE_MMUCTRL_VADDR_PC_INDEX_SHIFT 30, ..._PD_INDEX_SHIFT 21,
 * ..._PT_INDEX_SHIFT 12, ROGUE_MMUCTRL_ENTRIES_PC_VALUE 0x400,
 * ..._PD_VALUE 0x200, ..._PT_VALUE 0x200).
 */
#define POWERVR_GPU_PAGE_BYTES		0x1000U
#define POWERVR_GPU_ADDRESS_BITS	40U
#define POWERVR_GPU_CATALOGUE_SHIFT	30U
#define POWERVR_GPU_DIRECTORY_SHIFT	21U
#define POWERVR_GPU_TABLE_SHIFT		12U
#define POWERVR_GPU_CATALOGUE_ENTRIES	1024U
#define POWERVR_GPU_DIRECTORY_ENTRIES	512U
#define POWERVR_GPU_TABLE_ENTRIES	512U

/*
 * The fields of the three kinds of entry.
 *
 * A catalogue entry holds the directory's page number shifted up by four,
 * a directory entry the table's physical address with a 4 KiB page size,
 * and a page entry the page's physical address with its attributes.
 *
 * Values referenced:
 * StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
 * drivers/gpu/drm/imagination/pvr_rogue_mmu_defs.h
 * (ROGUE_MMUCTRL_PC_DATA_PD_BASE_SHIFT 4 and ..._ALIGNSHIFT 12,
 * ROGUE_MMUCTRL_PC_DATA_VALID_EN 1, ROGUE_MMUCTRL_PT_BASE_4KB_RANGE_CLRMSK
 * 0xFFFFFF0000000FFF, ROGUE_MMUCTRL_PD_DATA_PAGE_SIZE_4KB 0,
 * ROGUE_MMUCTRL_PD_DATA_VALID_EN 1, ROGUE_MMUCTRL_PT_DATA_PAGE_CLRMSK
 * 0xFFFFFF0000000FFF, ROGUE_MMUCTRL_PT_DATA_SLC_BYPASS_CTRL_EN 8,
 * ROGUE_MMUCTRL_PT_DATA_CC_EN 4, ROGUE_MMUCTRL_PT_DATA_READ_ONLY_EN 2,
 * ROGUE_MMUCTRL_PT_DATA_VALID_EN 1, ROGUE_MMUCTRL_PT_DATA_PM_META_PROTECT_EN
 * bit 62).
 */
#define POWERVR_GPU_CATALOGUE_SHIFT_IN	4U
#define POWERVR_GPU_CATALOGUE_VALID	0x1U
#define POWERVR_GPU_DIRECTORY_ADDRESS	0x000000fffffff000ULL
#define POWERVR_GPU_DIRECTORY_VALID	0x1ULL
#define POWERVR_GPU_PAGE_ADDRESS	0x000000fffffff000ULL
#define POWERVR_GPU_PAGE_SLC_BYPASS	0x8ULL
#define POWERVR_GPU_PAGE_SNOOP		0x4ULL
#define POWERVR_GPU_PAGE_READ_ONLY	0x2ULL
#define POWERVR_GPU_PAGE_VALID		0x1ULL
#define POWERVR_GPU_PAGE_PM_PROTECT	0x4000000000000000ULL

/* What a mapping lets the core do, passed to powervr_gpu_mmu_map(). */
#define POWERVR_GPU_MAP_READ_ONLY	0x1U
#define POWERVR_GPU_MAP_SLC_BYPASS	0x2U
/* A page only the parameter manager and the firmware reach (a free list). */
#define POWERVR_GPU_MAP_PM_PROTECT	0x4U

/*
 * One table page.
 *
 * Physical is what the core is given; view is how the host writes the
 * entries, an uncached alias in the kernel.  Opaque belongs to the
 * memory callbacks.
 */
struct powervr_gpu_table {
	uint64_t physical;
	volatile uint8_t *view;
	void *opaque;
};

/*
 * Where tables and the host's bookkeeping come from.
 *
 * Take_table returns a zeroed table page reachable by the core, or an
 * error; give_table returns one.  Take_memory returns zeroed host memory
 * for bookkeeping, or NULL; give_memory returns it.
 */
struct powervr_gpu_memory {
	int (*take_table)(void *context, struct powervr_gpu_table *table);
	void (*give_table)(void *context, struct powervr_gpu_table *table);
	void *(*take_memory)(void *context, size_t size);
	void (*give_memory)(void *context, void *memory);
	void *context;
};

/*
 * One directory and the tables under it.
 *
 * Used counts the valid page entries of each table, so a table with none
 * left can be told apart.  Tables are kept until the context goes: the
 * core may still hold a freed table's entries in its caches until the
 * firmware invalidates them, and nothing here waits for that.
 */
struct powervr_gpu_directory {
	struct powervr_gpu_table table;
	struct powervr_gpu_table tables[POWERVR_GPU_DIRECTORY_ENTRIES];
	uint16_t used[POWERVR_GPU_DIRECTORY_ENTRIES];
};

/* One memory context's tables. */
struct powervr_gpu_mmu {
	const struct powervr_gpu_memory *memory;
	struct powervr_gpu_table catalogue;
	struct powervr_gpu_directory *directories[POWERVR_GPU_CATALOGUE_ENTRIES];
};

/*
 * The address space one context hands out: a window of device addresses
 * and the ranges taken in it, kept sorted.  Every range is followed by
 * one unmapped page, so an overrun faults instead of reaching the next.
 */
#define POWERVR_GPU_SPACE_RANGES	512U

struct powervr_gpu_range {
	uint64_t start;
	uint64_t size;
};

struct powervr_gpu_space {
	uint64_t base;
	uint64_t size;
	unsigned count;
	struct powervr_gpu_range ranges[POWERVR_GPU_SPACE_RANGES];
};

/* Builds a catalogue entry naming a directory. */
uint32_t powervr_gpu_catalogue_entry(uint64_t directory);

/* Builds a directory entry naming a table of 4 KiB pages. */
uint64_t powervr_gpu_directory_entry(uint64_t table);

/* Builds a page entry; the core never snoops through it. */
uint64_t powervr_gpu_page_entry(uint64_t physical, unsigned flags);

/* Sets up a context with an empty catalogue. */
int powervr_gpu_mmu_init(struct powervr_gpu_mmu *mmu, const struct powervr_gpu_memory *memory);

/* Returns every table of a context; nothing may still use it. */
void powervr_gpu_mmu_fini(struct powervr_gpu_mmu *mmu);

/* Reports the physical address of a context's catalogue. */
uint64_t powervr_gpu_mmu_root(const struct powervr_gpu_mmu *mmu);

/*
 * Maps a physically contiguous run at a device address.
 *
 * Address, physical and bytes must be whole pages and the range inside
 * the 40-bit space.  EEXIST means a page of the range is mapped already;
 * on any failure nothing of the range stays mapped.
 */
int powervr_gpu_mmu_map(struct powervr_gpu_mmu *mmu, uint64_t address, uint64_t physical, uint64_t bytes, unsigned flags);

/* Removes the page entries of a range mapped earlier. */
void powervr_gpu_mmu_unmap(struct powervr_gpu_mmu *mmu, uint64_t address, uint64_t bytes);

/* Reads back the page entry of a device address, zero when none. */
uint64_t powervr_gpu_mmu_lookup(const struct powervr_gpu_mmu *mmu, uint64_t address);

/* Sets up an empty address space over a window. */
void powervr_gpu_space_init(struct powervr_gpu_space *space, uint64_t base, uint64_t size);

/* Takes a range of whole pages; ENOSPC when no gap fits. */
int powervr_gpu_space_take(struct powervr_gpu_space *space, uint64_t bytes, uint64_t *address);

/* Gives back the range that starts at an address; EINVAL when none does. */
int powervr_gpu_space_give(struct powervr_gpu_space *space, uint64_t address);

/*
 * The mapping flags for a buffer: one the processor can see, or one another
 * device (the display) may read, goes around the core's system level cache.
 */
unsigned powervr_gpu_buffer_flags(int mappable, int shareable);

/*
 * Checks that a contiguous run lies at or below an inclusive address limit
 * (zero means none) and starts on an alignment (zero or one means any, else
 * a power of two).  Zero when it does, ENOTSUP when it does not, EINVAL for
 * an empty run or a bad alignment.
 */
int powervr_gpu_run_fits(uint64_t paddr, uint64_t size, uint64_t limit, uint64_t alignment);

#endif
