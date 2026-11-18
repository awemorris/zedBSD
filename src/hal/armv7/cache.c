/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The armv7 cache maintenance: the level 1 data and instruction caches,
 * by address.
 *
 * The ranges are kernel addresses of the direct map or the image, which
 * never fault.  A partial line at either end of a range to be discarded is
 * written back first, so that the bytes beside the range that share its
 * line are not lost (Procedure referenced: Linux v6.12
 * arch/arm/mm/cache-v7.S, v7_dma_inv_range).  The board's outer cache, if
 * it has one, is off.
 *
 * The Cortex-A9's instruction cache is indexed by virtual address, so a
 * line of the range fetched through another mapping (a user's, while the
 * kernel wrote the code through the direct map) may lie at another index
 * than the range's own; there the whole instruction cache is dropped.  An
 * instruction cache indexed and tagged by physical address (the
 * Cortex-A15's) is dropped by address.  Procedure referenced: Linux v6.12
 * arch/arm/kernel/setup.c, cacheid_init (any ARMv7 instruction cache but
 * a PIPT or an ASID-tagged one taken as aliasing) and arch/arm/mm/flush.c,
 * __sync_icache_dcache (the whole instruction cache then).
 *
 * The instruction cache and the branch predictor are dropped on every
 * processor of the inner shareable domain, as the multiprocessing
 * extensions of both processors allow.
 *
 * Values referenced: ARM DDI 0406C, CTR (DminLine, IminLine and L1Ip),
 * and the maintenance operations DCCMVAC, DCIMVAC, DCCIMVAC, DCCMVAU,
 * ICIMVAU, ICIALLUIS and BPIALLIS.
 */

#include <hal/hal.h>
#include "asm.h"
#include "defs.h"

/* CTR.L1Ip: the instruction cache's indexing and tagging, and the value of a PIPT one. */
#define CACHE_CTR_L1IP_SHIFT	14U
#define CACHE_CTR_L1IP_MASK	0x3U
#define CACHE_CTR_L1IP_PIPT	0x3U

static uint32_t data_line_size(void);
static uint32_t instruction_line_size(void);
static int instruction_cache_by_address(void);

/*
 * Writes the cached lines of a range back to memory.
 */
void
hal_dcache_clean_range(
	uintptr_t addr,
	size_t size)
{
	uintptr_t line;
	uintptr_t end;
	uint32_t step;

	/* An empty range needs nothing. */
	if (size == 0)
		return;

	/* Cleans each line the range touches to the point of coherency. */
	step = data_line_size();
	line = addr & ~(uintptr_t)(step - 1U);
	end = addr + size;
	while (line < end) {
		__asm__ volatile("mcr p15, 0, %0, c7, c10, 1" : : "r"(line) : "memory");
		line += step;
	}

	/* Completes the maintenance before the caller hands the memory over. */
	armv7_dsb();
}

/*
 * Discards the cached lines of a range, after writing back the partial
 * lines at its ends.
 */
void
hal_dcache_invalidate_range(
	uintptr_t addr,
	size_t size)
{
	uintptr_t line;
	uintptr_t end;
	uint32_t step;

	/* An empty range needs nothing. */
	if (size == 0)
		return;

	/* A partial first line keeps the bytes before the range. */
	step = data_line_size();
	end = addr + size;
	line = addr & ~(uintptr_t)(step - 1U);
	if (line != addr) {
		__asm__ volatile("mcr p15, 0, %0, c7, c14, 1" : : "r"(line) : "memory");
		line += step;
	}

	/* A partial last line keeps the bytes after the range. */
	if ((end & (step - 1U)) != 0 && (end & ~(uintptr_t)(step - 1U)) >= line) {
		__asm__ volatile("mcr p15, 0, %0, c7, c14, 1" : : "r"(end & ~(uintptr_t)(step - 1U)) : "memory");
		end &= ~(uintptr_t)(step - 1U);
	}

	/* Discards each whole line in between. */
	while (line < end) {
		__asm__ volatile("mcr p15, 0, %0, c7, c6, 1" : : "r"(line) : "memory");
		line += step;
	}

	/* Completes the maintenance before the caller reads the memory. */
	armv7_dsb();
}

/*
 * Writes back and discards the cached lines of a range.
 */
void
hal_dcache_clean_invalidate_range(
	uintptr_t addr,
	size_t size)
{
	uintptr_t line;
	uintptr_t end;
	uint32_t step;

	/* An empty range needs nothing. */
	if (size == 0)
		return;

	/* Cleans and discards each line the range touches. */
	step = data_line_size();
	line = addr & ~(uintptr_t)(step - 1U);
	end = addr + size;
	while (line < end) {
		__asm__ volatile("mcr p15, 0, %0, c7, c14, 1" : : "r"(line) : "memory");
		line += step;
	}

	/* Completes the maintenance. */
	armv7_dsb();
}

/*
 * Makes code written through the data side visible to instruction fetch:
 * the data cache is cleaned to the point of unification, the instruction
 * cache and the branch predictor dropped for the range.
 */
void
hal_sync_instruction_stream(
	void *addr,
	size_t size)
{
	uintptr_t line;
	uintptr_t end;
	uint32_t step;
	int by_address;

	/* An empty range needs nothing. */
	if (size == 0)
		return;

	/* Cleans each data line to the point of unification. */
	step = data_line_size();
	line = (uintptr_t)addr & ~(uintptr_t)(step - 1U);
	end = (uintptr_t)addr + size;
	while (line < end) {
		__asm__ volatile("mcr p15, 0, %0, c7, c11, 1" : : "r"(line) : "memory");
		line += step;
	}

	/* Waits for the cleans to reach the point of unification. */
	armv7_dsb();

	/* Drops each instruction line of a cache indexed by physical address, or the whole of one that is not. */
	by_address = instruction_cache_by_address();
	if (by_address) {
		step = instruction_line_size();
		line = (uintptr_t)addr & ~(uintptr_t)(step - 1U);
		while (line < end) {
			__asm__ volatile("mcr p15, 0, %0, c7, c5, 1" : : "r"(line) : "memory");
			line += step;
		}
	} else {
		__asm__ volatile("mcr p15, 0, %0, c7, c1, 0" : : "r"(0) : "memory");
	}

	/* Drops the branch predictor's entries, which may name the old instructions. */
	__asm__ volatile("mcr p15, 0, %0, c7, c1, 6" : : "r"(0) : "memory");

	/* Completes the maintenance and refetches what follows. */
	armv7_dsb();
	armv7_isb();
}

/*
 * Makes a range of instructions current.
 */
void
hal_icache_invalidate_range(
	uintptr_t addr,
	size_t size)
{
	/* The same synchronization, for the caller's range. */
	hal_sync_instruction_stream((void *)addr, size);
}

/* Reports the smallest data cache line, in bytes. */
static uint32_t
data_line_size(
	void)
{
	uint32_t type;

	/* DminLine, bits 19 to 16, is the line's words as a power of two. */
	__asm__ volatile("mrc p15, 0, %0, c0, c0, 1" : "=r"(type));

	/* Succeeded: four bytes a word. */
	return 4U << ((type >> 16) & 0xfU);
}

/* Reports the smallest instruction cache line, in bytes. */
static uint32_t
instruction_line_size(
	void)
{
	uint32_t type;

	/* IminLine, bits 3 to 0, is the line's words as a power of two. */
	__asm__ volatile("mrc p15, 0, %0, c0, c0, 1" : "=r"(type));

	/* Succeeded: four bytes a word. */
	return 4U << (type & 0xfU);
}

/* Reports whether the instruction cache is indexed and tagged by physical address. */
static int
instruction_cache_by_address(
	void)
{
	uint32_t type;
	uint32_t policy;

	/* L1Ip, bits 15 to 14, says how the instruction cache is indexed. */
	__asm__ volatile("mrc p15, 0, %0, c0, c0, 1" : "=r"(type));
	policy = (type >> CACHE_CTR_L1IP_SHIFT) & CACHE_CTR_L1IP_MASK;

	/* A PIPT cache holds a line at the index of its physical address only. */
	if (policy == CACHE_CTR_L1IP_PIPT)
		return 1;

	/* Any other may hold a line of the range at another mapping's index. */
	return 0;
}
