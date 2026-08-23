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
bool hal_irq_disable(void) { return true; }
void hal_irq_enable(void) {}
unsigned hal_cpu_count(void) { return 1U; }
void hal_fatal(const char *file, int line, const char *message)
{
	fprintf(stderr, "fatal: %s:%d: %s\n", file, line, message);
	abort();
}

int
hal_pmem_alloc(const struct hal_pmem_request *request,
	       struct hal_pmem *descriptor)
{
	void *memory;
	size_t size;
	if (request == NULL || descriptor == NULL ||
	    request->type != HAL_PMEM_TYPE_RAM ||
	    request->paddr != HAL_PMEM_PADDR_ANY)
		return HAL_ERR_INVALID;
	size = request->size;
	pmem_attempts++;
	if (pmem_attempts == fail_attempt)
		return HAL_ERR_NOMEM;
	memory = aligned_alloc(M68K030_PAGE_SIZE,
		(size + M68K030_PAGE_MASK) & ~M68K030_PAGE_MASK);
	if (memory == NULL)
		return HAL_ERR_NOMEM;
	descriptor->vaddr = memory;
	descriptor->paddr = next_physical;
	descriptor->size = (size + M68K030_PAGE_MASK) & ~M68K030_PAGE_MASK;
	descriptor->type = HAL_PMEM_TYPE_RAM;
	descriptor->attr = 0;
	next_physical += descriptor->size;
	pmem_live++;
	return HAL_OK;
}

int
hal_pmem_free(struct hal_pmem *descriptor)
{
	if (descriptor == NULL || descriptor->vaddr == NULL || pmem_live == 0)
		return HAL_ERR_INVALID;
	free(descriptor->vaddr);
	memset(descriptor, 0, sizeof(*descriptor));
	pmem_live--;
	return HAL_OK;
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
	uint32_t descriptor_before;
	uint32_t flags;
	unsigned before;

	m68k030_space_init(0x00012000U);
	CHECK(flushes == 1U);
	space = hal_mem_create_space();
	CHECK(space != NULL);
	CHECK(pmem_live == 1U);

	CHECK(hal_page_map(space, (void *)0x003ff000U, 0x00400000U,
		2U * M68K030_PAGE_SIZE, HAL_SPACE_READ | HAL_SPACE_WRITE) ==
		HAL_OK);
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
		2U * M68K030_PAGE_SIZE, HAL_SPACE_READ) == HAL_ERR_NOMEM);
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
	/* The second page is absent.  Validation of the entire range must finish
	 * before the first descriptor is changed. */
	descriptor_before = table->entries[m68k030_leaf_index(0x01000000U)];
	flags = 0x5a5a5a5aU;
	CHECK(hal_page_prot_query(space, (void *)0x01000000U,
		2U * M68K030_PAGE_SIZE, HAL_SPACE_READ | HAL_SPACE_NOCACHE,
		&flags) == HAL_ERR_INVALID);
	CHECK(table->entries[m68k030_leaf_index(0x01000000U)] ==
		descriptor_before);
	CHECK(flags == 0x5a5a5a5aU);
	flags = 0;
	CHECK(hal_page_prot_query(space, (void *)0x01000000U,
		M68K030_PAGE_SIZE, HAL_SPACE_READ | HAL_SPACE_NOCACHE,
		&flags) == 0);
	CHECK(flags == (HAL_PAGE_PRESENT | HAL_PAGE_ACCESSED |
		HAL_PAGE_DIRTY));
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
		HAL_SPACE_READ) == HAL_ERR_INVALID);
	CHECK(hal_page_map(space, (void *)0x7ffff000U, 0,
		2U * M68K030_PAGE_SIZE, HAL_SPACE_READ) == HAL_ERR_INVALID);
	CHECK(hal_page_map(space, (void *)0x02000000U, 0,
		M68K030_PAGE_SIZE, HAL_SPACE_WRITE | HAL_SPACE_EXEC) ==
		HAL_ERR_INVALID);

	before = flushes;
	hal_page_switch_space(space);
	CHECK(flushes == before + 1U);
	/* Address-space ownership belongs to the generic kernel.  It must stop
	 * selecting a user space before asking the HAL to destroy it. */
	hal_page_switch_space(HAL_SPACE_SYS);
	CHECK(flushes == before + 2U);
	hal_page_destroy_space(space);
	CHECK(pmem_live == 0U);

	puts("m68k address-space host tests passed");
	return 0;
}
