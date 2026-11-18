/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The MIPS caches.
 *
 * The primary caches are virtually indexed and physically tagged.  Their
 * shape comes from Config1: the line, the sets and the ways of each.  The
 * range operations work line by line with the Hit operations of the cache
 * instruction on the address given.  When a way is larger than a page,
 * one physical page can sit in lines of several colors (the index bits
 * above the page offset), and a Hit operation reaches only the color of
 * the address it is given; mips_cache_flush_page_colors() then empties a
 * page in every color with the Index operations.  The emulator does not
 * model caches, so there every operation is accepted and does nothing.
 *
 * A secondary cache, which Config2 describes when Config1.M is set, is
 * not kept coherent with devices: the range operations work on it too,
 * after the primary data cache.  The Ingenic XBurst cores tag their
 * instruction cache with the virtual address, so that a Hit operation on
 * the kernel's address does not reach the lines of a user's address:
 * there the instruction cache is emptied whole instead.  The cache the
 * loader leaves is emptied whole once at entry, before the board's
 * display is set up.
 *
 * A build with MIPS_FORCE_VTAG_L2_PATHS defined takes the paths of the
 * virtually tagged instruction cache and of the secondary cache on any
 * processor, so that they run on the emulator, where they do nothing.
 */

#include <hal/hal.h>
#include "asm.h"
#include "cache.h"
#include "cpu.h"
#include "io.h"

/*
 * The fields of Config1 that describe each cache.
 *
 * Values referenced: MIPS32 Architecture for Programmers Volume III
 * (Config1: IS bits 22-24, IL bits 19-21, IA bits 16-18, DS bits 13-15,
 * DL bits 10-12, DA bits 7-9).
 */
#define CACHE_I_SETS_SHIFT	22
#define CACHE_I_LINE_SHIFT	19
#define CACHE_I_WAYS_SHIFT	16
#define CACHE_D_SETS_SHIFT	13
#define CACHE_D_LINE_SHIFT	10
#define CACHE_D_WAYS_SHIFT	7
#define CACHE_FIELD_MASK	0x7U

/*
 * Config1.M, which says Config2 exists, and the fields of Config2 that
 * describe the secondary cache.
 *
 * Values referenced: MIPS32 Architecture for Programmers Volume III
 * (Config1: M bit 31; Config2: SS bits 8-11, SL bits 4-7, SA bits 0-3).
 */
#define CACHE_CONFIG1_MORE	0x80000000U
#define CACHE_S_SETS_SHIFT	8
#define CACHE_S_LINE_SHIFT	4
#define CACHE_S_WAYS_SHIFT	0
#define CACHE_S_FIELD_MASK	0xfU

/*
 * The ways of the JZ4770's secondary cache, whose Config2 reports one
 * more than it has.
 *
 * Value referenced: Linux master arch/mips/mm/sc-mips.c (mips_sc_probe:
 * the ways set to 4 for MACH_INGENIC_JZ4770).
 */
#define CACHE_INGENIC_S_WAYS	4U

/* Whether the build takes the paths of the virtual tags and the secondary cache everywhere. */
#if defined(MIPS_FORCE_VTAG_L2_PATHS)
#define CACHE_FORCED	1
#else
#define CACHE_FORCED	0
#endif

/* The smallest line the range operations step by when a cache reports none. */
#define CACHE_LINE_FALLBACK	16U

/*
 * The shape of one primary cache: its line in bytes (zero when there is
 * no such cache), the bytes of one way and the number of ways.
 */
struct cache_shape {
	uint32_t line;
	uint32_t way_bytes;
	uint32_t ways;
};

/*
 * The instruction and data caches as Config1 describes them.  Filled once
 * by mips_cache_init() before the first space is made; read-only after.
 */
static struct cache_shape cache_instruction;
static struct cache_shape cache_data;

/*
 * The secondary cache as Config2 describes it (no line when there is
 * none).  Filled once by mips_cache_init(); read-only after.
 */
static struct cache_shape cache_secondary;

/*
 * Nonzero when the instruction cache is tagged with the virtual address,
 * or the build takes that path everywhere.  Set by mips_cache_init().
 */
static int cache_virtual_tags;

/*
 * Nonzero when either cache's way is larger than a page, so that a
 * physical page has more than one color.  Set by mips_cache_init().
 */
static int cache_alias;

/*
 * The pages emptied in every color since boot, for the self-test.  It
 * only grows.
 */
static uint32_t cache_color_flushes;

static void read_shape(uint32_t config1, unsigned sets_shift, unsigned line_shift, unsigned ways_shift, struct cache_shape *shape);
static void read_secondary(uint32_t config1, uint32_t processor_id, struct cache_shape *shape);
static int is_ingenic(uint32_t processor_id);
static bool secondary_in_use(void);
static uint32_t step_of(const struct cache_shape *shape);
static void instruction_hazard(void);

/*
 * Reads the shape of the caches and whether pages have colors.
 */
void
mips_cache_init(
	void)
{
	uint32_t config1;
	uint32_t processor_id;

	/* Both primary caches from Config1, the secondary one from Config2. */
	config1 = mips_read_config1();
	processor_id = mips_read_processor_id();
	read_shape(config1, CACHE_I_SETS_SHIFT, CACHE_I_LINE_SHIFT, CACHE_I_WAYS_SHIFT, &cache_instruction);
	read_shape(config1, CACHE_D_SETS_SHIFT, CACHE_D_LINE_SHIFT, CACHE_D_WAYS_SHIFT, &cache_data);
	read_secondary(config1, processor_id, &cache_secondary);

	/* The Ingenic cores tag the instruction cache with the virtual address. */
	cache_virtual_tags = CACHE_FORCED;
	if (is_ingenic(processor_id))
		cache_virtual_tags = 1;

	/* A way larger than a page gives one physical page several colors. */
	cache_alias = 0;
	if (cache_instruction.way_bytes > MIPS_PAGE_SIZE || cache_data.way_bytes > MIPS_PAGE_SIZE)
		cache_alias = 1;

	/* Reports the shape of both caches and whether pages have colors. */
	hal_printf(MIPS_HAL_NAME " CACHE I %ux%ux%u D %ux%ux%u alias=%u\n",
		   cache_instruction.line,
		   cache_instruction.way_bytes,
		   cache_instruction.ways,
		   cache_data.line,
		   cache_data.way_bytes,
		   cache_data.ways,
		   (unsigned)cache_alias);

	/* The secondary cache and the virtual tags, only where they are. */
	if (cache_secondary.line != 0) {
		hal_printf(MIPS_HAL_NAME " CACHE L2 %ux%ux%u\n",
			   cache_secondary.line,
			   cache_secondary.way_bytes,
			   cache_secondary.ways);
	}
	if (cache_virtual_tags)
		hal_printf(MIPS_HAL_NAME " CACHE I VIRTUAL TAGS forced=%u\n", (unsigned)CACHE_FORCED);
}

/*
 * Writes back and empties every cache the loader may have left lines
 * in: the primary data cache, then the secondary cache, then the
 * primary instruction cache, each whole with the Index operations.
 *
 * Called once from mips_cmain() before the board's display is set up.
 * The shapes are read here from Config1 and Config2, and no global is
 * used, since mips_cache_init() comes later.
 */
void
mips_cache_flush_all_early(
	void)
{
	struct cache_shape instruction;
	struct cache_shape data;
	struct cache_shape secondary;
	uintptr_t address;
	uint32_t config1;
	uint32_t processor_id;
	uint32_t offset;
	uint32_t end;

	/* The three caches' shapes. */
	config1 = mips_read_config1();
	processor_id = mips_read_processor_id();
	read_shape(config1, CACHE_I_SETS_SHIFT, CACHE_I_LINE_SHIFT, CACHE_I_WAYS_SHIFT, &instruction);
	read_shape(config1, CACHE_D_SETS_SHIFT, CACHE_D_LINE_SHIFT, CACHE_D_WAYS_SHIFT, &data);
	read_secondary(config1, processor_id, &secondary);

	/*
	 * The primary data cache: every line of every way, written back
	 * and dropped.  The index of a line is its offset in KSEG0, the way
	 * the bits above one way.
	 *
	 * Value referenced: MIPS32 Architecture for Programmers Volume III
	 * (the cache instruction's Index_Writeback_Inv_D, 0x01).
	 */
	end = data.way_bytes * data.ways;
	for (offset = 0; data.line != 0 && offset < end; offset += data.line) {
		address = mips_segment_pointer(MIPS_KSEG0_BASE + offset);
		__asm__ volatile("cache 0x01, 0(%0)" : : "r"(address) : "memory");
	}
	__asm__ volatile("sync" : : : "memory");

	/*
	 * The secondary cache, after the lines the primary one wrote into
	 * it.
	 *
	 * Value referenced: MIPS32 Architecture for Programmers Volume III
	 * (the cache instruction's Index_Writeback_Inv_S, 0x03).
	 */
	end = secondary.way_bytes * secondary.ways;
	for (offset = 0; secondary.line != 0 && offset < end; offset += secondary.line) {
		address = mips_segment_pointer(MIPS_KSEG0_BASE + offset);
		__asm__ volatile("cache 0x03, 0(%0)" : : "r"(address) : "memory");
	}
	__asm__ volatile("sync" : : : "memory");

	/*
	 * The primary instruction cache, dropped.
	 *
	 * Value referenced: MIPS32 Architecture for Programmers Volume III
	 * (the cache instruction's Index_Invalidate_I, 0x00).
	 */
	end = instruction.way_bytes * instruction.ways;
	for (offset = 0; instruction.line != 0 && offset < end; offset += instruction.line) {
		address = mips_segment_pointer(MIPS_KSEG0_BASE + offset);
		__asm__ volatile("cache 0x00, 0(%0)" : : "r"(address) : "memory");
	}

	/* Orders the operations and drops instructions fetched before them. */
	__asm__ volatile("sync" : : : "memory");
	instruction_hazard();
}

/*
 * Reports whether the instruction cache is tagged with the virtual
 * address, so that only emptying it whole reaches a user's lines.
 */
bool
mips_cache_virtual_tags(
	void)
{
	/* The tags are virtual on the Ingenic cores, or the build says so. */
	if (cache_virtual_tags)
		return true;

	/* The tags are physical. */
	return false;
}

/*
 * Drops every line of the instruction cache.
 */
void
mips_cache_invalidate_instructions(
	void)
{
	uintptr_t address;
	uint32_t offset;
	uint32_t end;

	/*
	 * Every line of every way.
	 *
	 * Value referenced: MIPS32 Architecture for Programmers Volume III
	 * (the cache instruction's Index_Invalidate_I, 0x00).
	 */
	end = cache_instruction.way_bytes * cache_instruction.ways;
	for (offset = 0; cache_instruction.line != 0 && offset < end; offset += cache_instruction.line) {
		address = mips_segment_pointer(MIPS_KSEG0_BASE + offset);
		__asm__ volatile("cache 0x00, 0(%0)" : : "r"(address) : "memory");
	}

	/* Orders the operations and drops instructions fetched before them. */
	__asm__ volatile("sync" : : : "memory");
	instruction_hazard();
}

/*
 * Reports whether a physical page can sit in lines of several colors.
 */
bool
mips_cache_aliases(
	void)
{
	/* Pages have colors when a way is larger than a page. */
	if (cache_alias)
		return true;

	/* A page has one color. */
	return false;
}

/*
 * Writes back and drops a physical page in every color of the data
 * cache, and of the instruction cache when asked.
 *
 * Nothing is done when pages have one color only: the Hit operations on
 * the kernel's address then reach every line the page can be in.
 */
void
mips_cache_flush_page_colors(
	hal_physaddr_t page,
	bool instructions)
{
	uintptr_t address;
	uint32_t way;
	uint32_t offset;
	uint32_t step;

	UNUSED_PARAMETER(page);

	/* A page of one color needs no index operation. */
	if (!cache_alias)
		return;

	/* Counts the flush for the self-test. */
	cache_color_flushes++;

	/*
	 * Every line of every way of the data cache whose index the page
	 * can take: the page's offset in any of the way's page-sized
	 * colors.  The index operations write back and drop whatever line
	 * is there, which includes the page's own.
	 *
	 * Value referenced: MIPS32 Architecture for Programmers Volume III
	 * (the cache instruction's Index_Writeback_Inv_D, 0x01).
	 */
	step = step_of(&cache_data);
	for (way = 0; way < cache_data.ways; way++) {
		for (offset = 0; offset < cache_data.way_bytes; offset += step) {
			address = mips_segment_pointer(MIPS_KSEG0_BASE + way * cache_data.way_bytes + offset);
			__asm__ volatile("cache 0x01, 0(%0)" : : "r"(address) : "memory");
		}
	}

	/*
	 * The same for the instruction cache, when the page is to run.
	 *
	 * Value referenced: MIPS32 Architecture for Programmers Volume III
	 * (the cache instruction's Index_Invalidate_I, 0x00).
	 */
	if (instructions) {
		step = step_of(&cache_instruction);
		for (way = 0; way < cache_instruction.ways; way++) {
			for (offset = 0; offset < cache_instruction.way_bytes; offset += step) {
				address = mips_segment_pointer(MIPS_KSEG0_BASE + way * cache_instruction.way_bytes + offset);
				__asm__ volatile("cache 0x00, 0(%0)" : : "r"(address) : "memory");
			}
		}
	}

	/* Orders the write-backs before what follows, and drops prefetched instructions. */
	__asm__ volatile("sync" : : : "memory");
	instruction_hazard();
}

/*
 * Reports how many pages were emptied in every color since boot.
 */
uint32_t
mips_cache_color_flushes(
	void)
{
	/* Succeeded: the counter. */
	return cache_color_flushes;
}

/*
 * Writes back a range of the data cache.
 */
void
hal_dcache_clean_range(
	uintptr_t address,
	size_t size)
{
	uintptr_t line;
	uintptr_t end;
	uint32_t step;

	/*
	 * Works on each line of the range.
	 *
	 * Value referenced: MIPS32 Architecture for Programmers Volume III
	 * (the cache instruction's Hit_Writeback_D, 0x19).
	 */
	step = step_of(&cache_data);
	end = address + size;
	for (line = address & ~(uintptr_t)(step - 1U); line < end; line += step)
		__asm__ volatile("cache 0x19, 0(%0)" : : "r"(line) : "memory");

	/*
	 * The same in the secondary cache, after the primary one.
	 *
	 * Value referenced: MIPS32 Architecture for Programmers Volume III
	 * (the cache instruction's Hit_Writeback_S, 0x1b).
	 */
	if (secondary_in_use()) {
		step = step_of(&cache_secondary);
		for (line = address & ~(uintptr_t)(step - 1U); line < end; line += step)
			__asm__ volatile("cache 0x1b, 0(%0)" : : "r"(line) : "memory");
	}

	/* Orders the operations before what follows. */
	__asm__ volatile("sync" : : : "memory");
}

/*
 * Drops a range of the data cache without writing it back.
 */
void
hal_dcache_invalidate_range(
	uintptr_t address,
	size_t size)
{
	uintptr_t line;
	uintptr_t end;
	uint32_t step;

	/*
	 * Works on each line of the range.
	 *
	 * Value referenced: MIPS32 Architecture for Programmers Volume III
	 * (the cache instruction's Hit_Invalidate_D, 0x11).
	 */
	step = step_of(&cache_data);
	end = address + size;
	for (line = address & ~(uintptr_t)(step - 1U); line < end; line += step)
		__asm__ volatile("cache 0x11, 0(%0)" : : "r"(line) : "memory");

	/*
	 * The same in the secondary cache, after the primary one.
	 *
	 * Value referenced: MIPS32 Architecture for Programmers Volume III
	 * (the cache instruction's Hit_Invalidate_S, 0x13).
	 */
	if (secondary_in_use()) {
		step = step_of(&cache_secondary);
		for (line = address & ~(uintptr_t)(step - 1U); line < end; line += step)
			__asm__ volatile("cache 0x13, 0(%0)" : : "r"(line) : "memory");
	}

	/* Orders the operations before what follows. */
	__asm__ volatile("sync" : : : "memory");
}

/*
 * Writes back and drops a range of the data cache.
 */
void
hal_dcache_clean_invalidate_range(
	uintptr_t address,
	size_t size)
{
	uintptr_t line;
	uintptr_t end;
	uint32_t step;

	/*
	 * Works on each line of the range.
	 *
	 * Value referenced: MIPS32 Architecture for Programmers Volume III
	 * (the cache instruction's Hit_Writeback_Inv_D, 0x15).
	 */
	step = step_of(&cache_data);
	end = address + size;
	for (line = address & ~(uintptr_t)(step - 1U); line < end; line += step)
		__asm__ volatile("cache 0x15, 0(%0)" : : "r"(line) : "memory");

	/*
	 * The same in the secondary cache, after the primary one.
	 *
	 * Value referenced: MIPS32 Architecture for Programmers Volume III
	 * (the cache instruction's Hit_Writeback_Inv_S, 0x17).
	 */
	if (secondary_in_use()) {
		step = step_of(&cache_secondary);
		for (line = address & ~(uintptr_t)(step - 1U); line < end; line += step)
			__asm__ volatile("cache 0x17, 0(%0)" : : "r"(line) : "memory");
	}

	/* Orders the operations before what follows. */
	__asm__ volatile("sync" : : : "memory");
}

/*
 * Makes code written through the data side visible to instruction fetch.
 */
void
hal_sync_instruction_stream(
	void *addr,
	size_t size)
{
	uintptr_t line;
	uintptr_t end;
	uint32_t step;

	/* Writes the range back from the data cache. */
	hal_dcache_clean_range((uintptr_t)addr, size);

	/*
	 * Virtual tags: the range's lines may be under a user's address the
	 * kernel's address does not reach, so the whole cache is dropped.
	 */
	if (cache_virtual_tags) {
		mips_cache_invalidate_instructions();
		return;
	}

	/*
	 * Drops the range's lines from the instruction cache.
	 *
	 * Value referenced: MIPS32 Architecture for Programmers Volume III
	 * (the cache instruction's Hit_Invalidate_I, 0x10).
	 */
	step = step_of(&cache_instruction);
	end = (uintptr_t)addr + size;
	for (line = (uintptr_t)addr & ~(uintptr_t)(step - 1U); line < end; line += step)
		__asm__ volatile("cache 0x10, 0(%0)" : : "r"(line) : "memory");

	/* Orders the operations and drops instructions fetched before them. */
	__asm__ volatile("sync" : : : "memory");
	instruction_hazard();
}

/*
 * Makes a range of instructions current.
 */
void
hal_icache_invalidate_range(
	uintptr_t addr,
	size_t size)
{
	/* The same synchronization. */
	hal_sync_instruction_stream((void *)addr, size);
}

/* Reads one cache's line, way size and ways from its three fields of Config1. */
static void
read_shape(
	uint32_t config1,
	unsigned sets_shift,
	unsigned line_shift,
	unsigned ways_shift,
	struct cache_shape *shape)
{
	uint32_t sets;
	uint32_t line_field;

	/*
	 * The line is two bytes shifted by the field (zero: no cache), the
	 * sets 64 shifted by theirs, the ways one more than theirs.
	 */
	line_field = (config1 >> line_shift) & CACHE_FIELD_MASK;
	sets = 64U << ((config1 >> sets_shift) & CACHE_FIELD_MASK);
	shape->ways = ((config1 >> ways_shift) & CACHE_FIELD_MASK) + 1U;
	shape->line = 0;
	shape->way_bytes = 0;
	if (line_field != 0) {
		shape->line = 2U << line_field;
		shape->way_bytes = sets * shape->line;
	}
}

/*
 * Reads the secondary cache's line, way size and ways from Config2, or
 * no line when Config1 says there is no Config2 or Config2 no cache.
 * The JZ4770's ways are corrected.
 */
static void
read_secondary(
	uint32_t config1,
	uint32_t processor_id,
	struct cache_shape *shape)
{
	uint32_t config2;
	uint32_t sets;
	uint32_t line_field;

	/* No Config2: no secondary cache. */
	shape->line = 0;
	shape->way_bytes = 0;
	shape->ways = 0;
	if ((config1 & CACHE_CONFIG1_MORE) == 0)
		return;

	/*
	 * The line is two bytes shifted by its field (zero: no cache), the
	 * sets 64 shifted by theirs, the ways one more than theirs.
	 */
	config2 = mips_read_config2();
	line_field = (config2 >> CACHE_S_LINE_SHIFT) & CACHE_S_FIELD_MASK;
	if (line_field == 0)
		return;
	sets = 64U << ((config2 >> CACHE_S_SETS_SHIFT) & CACHE_S_FIELD_MASK);
	shape->line = 2U << line_field;
	shape->way_bytes = sets * shape->line;
	shape->ways = ((config2 >> CACHE_S_WAYS_SHIFT) & CACHE_S_FIELD_MASK) + 1U;

	/* The Ingenic cores report one way too many. */
	if (is_ingenic(processor_id))
		shape->ways = CACHE_INGENIC_S_WAYS;
}

/* Reports whether PRId's company field is the Ingenic XBurst cores'. */
static int
is_ingenic(
	uint32_t processor_id)
{
	uint32_t company;

	/* The company field. */
	company = (processor_id >> MIPS_PRID_COMPANY_SHIFT) & MIPS_PRID_COMPANY_MASK;
	if (company == MIPS_PRID_COMPANY_INGENIC_D1)
		return 1;

	/* Another company. */
	return 0;
}

/* Reports whether the range operations work on the secondary cache: it exists, or the build says so. */
static bool
secondary_in_use(
	void)
{
	/* A secondary cache, or the forced path. */
	if (cache_secondary.line != 0 || CACHE_FORCED)
		return true;

	/* No secondary cache. */
	return false;
}

/* Reports the step of a cache's range operations: its line, or a small fallback. */
static uint32_t
step_of(
	const struct cache_shape *shape)
{
	/* A cache that reports no line is stepped through at the smallest line. */
	if (shape->line == 0)
		return CACHE_LINE_FALLBACK;

	/* Succeeded: the cache's line. */
	return shape->line;
}

/* Clears the instruction hazard: a jump with the hazard barrier to the next instruction. */
static void
instruction_hazard(
	void)
{
	uintptr_t target;

	/*
	 * Jumps to the label that follows with jr.hb.  The label's address
	 * is built from its halves, which both widths sign-extend alike.
	 */
	__asm__ volatile(".set push\n\t"
			 ".set noreorder\n\t"
			 "lui %0, %%hi(.Lhazard%=)\n\t"
			 "addiu %0, %0, %%lo(.Lhazard%=)\n\t"
			 "jr.hb %0\n\t"
			 "nop\n"
			 ".Lhazard%=:\n\t"
			 ".set pop"
			 : "=r"(target)
			 :
			 : "memory");
}
