/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host check of the Mali translation-table builder
 * (src/drivers/gpu/mali/mali-pagetable.c): the bits of a page entry for
 * each access, a walk of 48 and of 32 virtual bits built by hand against
 * the builder's tables, the refusal of an overlap, of an unaligned or
 * outside run, and unmapping.  Tables live in host memory whose
 * "physical" address is an index into a pool.
 *
 * cc -std=c11 -Wall -Wextra -Werror -Isrc -Iinclude \
 *    tests/mali-pagetable-host-test.c src/drivers/gpu/mali/mali-pagetable.c -o <out> && <out>
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "drivers/gpu/mali/mali-pagetable.h"

/* The tables the pool can hand out, and the "physical" address of the first. */
#define POOL_TABLES	64U
#define POOL_BASE	0x80000000ULL

/* The pool of tables, each one page of entries. */
static uint64_t pool[POOL_TABLES][MALI_PT_ENTRIES];

/* How many tables of the pool are handed out. */
static unsigned pool_used;

/* How many checks failed. */
static int failures;

static int pool_allocate(void *context, uint64_t *physical, uint64_t **table);
static uint64_t *pool_view(void *context, uint64_t physical);
static void check(int condition, const char *what);
static void check_entry_bits(void);
static void check_walk(unsigned virtual_bits);
static void check_refusals(void);

/* The pool as the builder's memory. */
static const struct mali_pt_memory pool_memory = {
	pool_allocate,
	pool_view,
};

/* Runs every check and reports the count of failures. */
int
main(void)
{
	/* The entry bits, then the walks, then the refusals. */
	check_entry_bits();
	check_walk(48U);
	check_walk(32U);
	check_refusals();

	/* Reports the outcome. */
	if (failures != 0) {
		printf("mali-pagetable: %d failure(s)\n", failures);
		return 1;
	}

	/* Succeeded: every check passed. */
	printf("mali-pagetable: all checks passed\n");
	return 0;
}

/* Hands out the next zeroed table of the pool. */
static int
pool_allocate(
	void *context,
	uint64_t *physical,
	uint64_t **table)
{
	(void)context;

	/* Refuses once the pool is used up. */
	if (pool_used >= POOL_TABLES)
		return 1;

	/* Clears the table and names it by its place in the pool. */
	memset(pool[pool_used], 0, sizeof(pool[pool_used]));
	*physical = POOL_BASE + (uint64_t)pool_used * MALI_PT_PAGE_BYTES;
	*table = pool[pool_used];
	pool_used++;

	/* Succeeded: one more table. */
	return 0;
}

/* Finds the table of a "physical" address. */
static uint64_t *
pool_view(
	void *context,
	uint64_t physical)
{
	uint64_t index;

	(void)context;

	/* Only addresses the pool handed out. */
	if (physical < POOL_BASE)
		return NULL;
	index = (physical - POOL_BASE) / MALI_PT_PAGE_BYTES;
	if (index >= pool_used)
		return NULL;

	/* Succeeded: the table. */
	return pool[index];
}

/* Counts and reports one failed check. */
static void
check(
	int condition,
	const char *what)
{
	/* A passing check says nothing. */
	if (condition)
		return;

	printf("FAIL: %s\n", what);
	failures++;
}

/* Checks the bits of page entries against the hand-written expectation. */
static void
check_entry_bits(void)
{
	uint64_t entry;

	/* A read-only, non-executable, uncached page. */
	entry = mali_pt_page_entry(0x123456000ULL, 0U);
	check(entry == (0x123456000ULL | 0x3ULL | (1ULL << 10) | (3ULL << 8) | (1ULL << 6) | (1ULL << 7) | (3ULL << 53)),
	      "read-only page entry");

	/* A writable, executable, cached page. */
	entry = mali_pt_page_entry(0x1000ULL, MALI_PT_WRITE | MALI_PT_EXECUTE | MALI_PT_CACHED);
	check(entry == (0x1000ULL | 0x3ULL | (1ULL << 10) | (3ULL << 8) | (1ULL << 6) | (1ULL << 2)),
	      "writable executable cached page entry");

	/* Bits below the page are dropped. */
	entry = mali_pt_page_entry(0x1fffULL, MALI_PT_WRITE);
	check((entry & 0x0000fffffffff000ULL) == 0x1000ULL, "page address masked");
}

/* Maps a few runs into a space and walks them by hand. */
static void
check_walk(
	unsigned virtual_bits)
{
	struct mali_pt pt;
	uint64_t *table;
	uint64_t entry;
	uint64_t va;
	unsigned level;
	unsigned shift;
	int error;

	/* A fresh pool and space. */
	pool_used = 0U;
	error = mali_pt_init(&pt, virtual_bits, &pool_memory, NULL);
	check(error == 0, "init");
	if (error != 0)
		return;
	check(pt.start_level == (virtual_bits > 39U ? 0U : 1U), "start level");

	/* Three pages at the start of the shared region, writable. */
	error = mali_pt_map(&pt, 0x04000000ULL, 0x40000000ULL, 3U * MALI_PT_PAGE_BYTES, MALI_PT_WRITE);
	check(error == 0, "map three pages");

	/* One executable page near the top of a 32-bit space. */
	error = mali_pt_map(&pt, 0xfffff000ULL, 0x50000000ULL, MALI_PT_PAGE_BYTES, MALI_PT_EXECUTE);
	check(error == 0, "map the top page");

	/* Walks the second page by hand from the root. */
	va = 0x04001000ULL;
	table = pt.root;
	for (level = pt.start_level; level < 3U; level++) {
		shift = 12U + (3U - level) * 9U;
		entry = table[(va >> shift) & 511U];
		check((entry & 3ULL) == 3ULL, "table entry on the way");
		table = pool_view(NULL, entry & 0x0000fffffffff000ULL);
		check(table != NULL, "table found");
		if (table == NULL)
			return;
	}

	entry = table[(va >> 12) & 511U];
	check((entry & 0x0000fffffffff000ULL) == 0x40001000ULL, "second page's address");
	check((entry & (1ULL << 7)) == 0U, "second page writable");

	/* The builder's lookup agrees, and an unmapped page reads zero. */
	error = mali_pt_lookup(&pt, 0x04002000ULL, &entry);
	check(error == 0 && (entry & 0x0000fffffffff000ULL) == 0x40002000ULL, "lookup third page");
	error = mali_pt_lookup(&pt, 0x04003000ULL, &entry);
	check(error == 0 && entry == 0U, "lookup unmapped page");
	error = mali_pt_lookup(&pt, 0xfffff000ULL, &entry);
	check(error == 0 && (entry & (3ULL << 53)) == 0U, "top page executable");

	/* Unmapping clears the entries and keeps the others. */
	error = mali_pt_unmap(&pt, 0x04000000ULL, 2U * MALI_PT_PAGE_BYTES);
	check(error == 0, "unmap two pages");
	error = mali_pt_lookup(&pt, 0x04001000ULL, &entry);
	check(error == 0 && entry == 0U, "unmapped page reads zero");
	error = mali_pt_lookup(&pt, 0x04002000ULL, &entry);
	check(error == 0 && entry != 0U, "kept page still mapped");

	/* Four tables for 48 bits (L0..L3 plus the top page's L2 and L3), three for 32 (L1..L3 plus L2, L3). */
	if (virtual_bits > 39U) {
		check(pt.tables == 6U, "tables made for 48 bits");
	} else {
		check(pt.tables == 5U, "tables made for 32 bits");
	}
}

/* Checks the runs the builder must refuse. */
static void
check_refusals(void)
{
	struct mali_pt pt;
	int error;

	/* A fresh pool and a 32-bit space. */
	pool_used = 0U;
	error = mali_pt_init(&pt, 32U, &pool_memory, NULL);
	check(error == 0, "init for refusals");
	if (error != 0)
		return;

	/* Widths outside what the walk covers. */
	check(mali_pt_init(&pt, 31U, &pool_memory, NULL) != 0, "width 31 refused");
	check(mali_pt_init(&pt, 49U, &pool_memory, NULL) != 0, "width 49 refused");
	pool_used = 0U;
	error = mali_pt_init(&pt, 32U, &pool_memory, NULL);
	check(error == 0, "init again");

	/* Unaligned addresses and sizes, and an empty run. */
	check(mali_pt_map(&pt, 0x1001ULL, 0x2000ULL, MALI_PT_PAGE_BYTES, 0U) != 0, "unaligned virtual refused");
	check(mali_pt_map(&pt, 0x1000ULL, 0x2001ULL, MALI_PT_PAGE_BYTES, 0U) != 0, "unaligned physical refused");
	check(mali_pt_map(&pt, 0x1000ULL, 0x2000ULL, 100U, 0U) != 0, "partial page refused");
	check(mali_pt_map(&pt, 0x1000ULL, 0x2000ULL, 0U, 0U) != 0, "empty run refused");

	/* A run past the end of the space. */
	check(mali_pt_map(&pt, 0xfffff000ULL, 0x2000ULL, 2U * MALI_PT_PAGE_BYTES, 0U) != 0, "run past the end refused");
	check(mali_pt_map(&pt, 0x100000000ULL, 0x2000ULL, MALI_PT_PAGE_BYTES, 0U) != 0, "address past the end refused");

	/* An overlap with a page already mapped, and the earlier mapping kept. */
	check(mali_pt_map(&pt, 0x10000ULL, 0x20000ULL, MALI_PT_PAGE_BYTES, 0U) == 0, "first mapping");
	check(mali_pt_map(&pt, 0xf000ULL, 0x30000ULL, 2U * MALI_PT_PAGE_BYTES, 0U) != 0, "overlap refused");
}
