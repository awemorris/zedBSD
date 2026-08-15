/* Host-side MC68030 page-table mutation and rollback tests. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <hal/hal.h>
#include "src/hal/m68k/space.h"

#define CHECK(expr) do { \
	if (!(expr)) { \
		fprintf(stderr, "FAIL:%d: %s\n", __LINE__, #expr); \
		return 1; \
	} \
} while (0)

static unsigned pmem_attempts;
static unsigned pmem_live;
static unsigned fail_attempt;
static unsigned flushes;
static uintptr_t next_physical = 0x00200000U;

void *hal_malloc(size_t size) { return malloc(size); }
void hal_free(void *pointer) { free(pointer); }
void *hal_memset(void *p, int c, size_t n) { return memset(p, c, n); }
void *hal_memcpy(void *d, const void *s, size_t n) { return memcpy(d, s, n); }
void hal_fatal(const char *file, int line, const char *message)
{
	fprintf(stderr, "fatal: %s:%d: %s\n", file, line, message);
	abort();
}

int
pmem_alloc_lo(size_t size, struct pmem_desc *descriptor)
{
	void *memory;
	pmem_attempts++;
	if (pmem_attempts == fail_attempt)
		return PMEM_NOSPACE;
	memory = aligned_alloc(M68K030_PAGE_SIZE,
		(size + M68K030_PAGE_MASK) & ~M68K030_PAGE_MASK);
	if (memory == NULL)
		return PMEM_NOSPACE;
	descriptor->vaddr = memory;
	descriptor->paddr = (void *)next_physical;
	descriptor->size = (size + M68K030_PAGE_MASK) & ~M68K030_PAGE_MASK;
	next_physical += descriptor->size;
	pmem_live++;
	return PMEM_SUCCESS;
}

int
pmem_free(struct pmem_desc *descriptor)
{
	if (descriptor == NULL || descriptor->vaddr == NULL || pmem_live == 0)
		return PMEM_BADDESC;
	free(descriptor->vaddr);
	descriptor->vaddr = descriptor->paddr = NULL;
	descriptor->size = 0;
	pmem_live--;
	return PMEM_SUCCESS;
}

size_t hal_pmem_get_total_size(void) { return 0x00c00000U; }
void m68k030_load_crp(const struct m68k030_root_pointer *root) { (void)root; }
void m68k030_flush_atc(void) { flushes++; }

static struct m68k030_table_page *
table_for(struct m68k030_space *space, uintptr_t address)
{
	struct m68k030_table_page *table;
	uintptr_t base = address & ~(uintptr_t)0x003fffffU;
	for (table = space->tables; table != NULL; table = table->next)
		if (table->virtual_base == base)
			return table;
	return NULL;
}

int
main(void)
{
	struct m68k030_space *space;
	struct m68k030_table_page *table;
	uint32_t flags;
	unsigned before;

	m68k030_space_init(0x00012000U);
	CHECK(flushes == 1U);
	space = hal_mem_create_space();
	CHECK(space != NULL);
	CHECK(pmem_live == 1U);

	CHECK(hal_page_map(space, (void *)0x003ff000U, 0x00400000U,
		2U * M68K030_PAGE_SIZE, HAL_SPACE_READ | HAL_SPACE_WRITE) ==
		HAL_PMEM_SUCCESS);
	CHECK(pmem_live == 3U);
	CHECK(hal_page_query(space, (void *)0x003ff000U, &flags) == 0);
	CHECK(flags == HAL_PAGE_PRESENT);
	CHECK(hal_page_unmap(space, (void *)0x003ff000U,
		M68K030_PAGE_SIZE) == 0);
	CHECK(pmem_live == 2U);
	CHECK(hal_page_unmap(space, (void *)0x00400000U,
		M68K030_PAGE_SIZE) == 0);
	CHECK(pmem_live == 1U);

	/* Force the second L2 allocation in a two-root map to fail.  Both the
	 * first mapping and its newly-created empty table must roll back. */
	fail_attempt = pmem_attempts + 2U;
	CHECK(hal_page_map(space, (void *)0x007ff000U, 0x00600000U,
		2U * M68K030_PAGE_SIZE, HAL_SPACE_READ) == HAL_PMEM_NOSPACE);
	CHECK(pmem_live == 1U);
	CHECK(space->tables == NULL);
	fail_attempt = 0;

	CHECK(hal_page_map(space, (void *)0x01000000U, 0x00700000U,
		M68K030_PAGE_SIZE, HAL_SPACE_READ) == 0);
	table = table_for(space, 0x01000000U);
	CHECK(table != NULL);
	table->entries[m68k030_leaf_index(0x01000000U)] |=
		M68K030_DESC_USED | M68K030_PAGE_MODIFIED;
	CHECK(hal_page_query(space, (void *)0x01000000U, &flags) == 0);
	CHECK(flags == (HAL_PAGE_PRESENT | HAL_PAGE_ACCESSED |
		HAL_PAGE_DIRTY));
	CHECK(hal_page_prot(space, (void *)0x01000000U, M68K030_PAGE_SIZE,
		HAL_SPACE_READ | HAL_SPACE_NOCACHE) == 0);
	CHECK((table->entries[m68k030_leaf_index(0x01000000U)] &
		(M68K030_DESC_WRITE_PROTECT | M68K030_PAGE_CACHE_INHIBIT |
		 M68K030_DESC_USED | M68K030_PAGE_MODIFIED)) ==
		(M68K030_DESC_WRITE_PROTECT | M68K030_PAGE_CACHE_INHIBIT |
		 M68K030_DESC_USED | M68K030_PAGE_MODIFIED));
	CHECK(hal_page_clear_flags(space, (void *)0x01000000U,
		HAL_PAGE_ACCESSED | HAL_PAGE_DIRTY) == 0);
	CHECK(hal_page_query(space, (void *)0x01000000U, &flags) == 0);
	CHECK(flags == HAL_PAGE_PRESENT);

	CHECK(hal_page_map(space, (void *)0, 0, M68K030_PAGE_SIZE,
		HAL_SPACE_READ) == HAL_PMEM_BADDESC);
	CHECK(hal_page_map(space, (void *)0x7ffff000U, 0,
		2U * M68K030_PAGE_SIZE, HAL_SPACE_READ) == HAL_PMEM_BADDESC);
	CHECK(hal_page_map(space, (void *)0x02000000U, 0,
		M68K030_PAGE_SIZE, HAL_SPACE_WRITE | HAL_SPACE_EXEC) ==
		HAL_PMEM_BADDESC);

	before = flushes;
	hal_page_switch_space(space);
	CHECK(flushes == before + 1U);
	hal_page_destroy_space(space);
	CHECK(pmem_live == 0U);

	puts("m68k address-space host tests passed");
	return 0;
}
