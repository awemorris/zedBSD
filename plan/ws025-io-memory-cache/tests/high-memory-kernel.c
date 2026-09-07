/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
/* Link-only observer: production allocator, page mapper and guest COW run unchanged. */
#include <hal/hal.h>

static uint64_t observed_pages[1024];
static unsigned observed_count;

void __real_amd64_boot_memory_release(void);
int __real_hal_pmem_free(struct hal_pmem *);
int __real_hal_page_map(hal_space_t, void *, hal_physaddr_t, size_t, uint32_t);
int __real_hal_page_unmap(hal_space_t, void *, size_t);
static void probe(uint64_t minimum, uint64_t maximum);

void
__wrap_amd64_boot_memory_release(void)
{
	__real_amd64_boot_memory_release();
	probe(UINT64_C(0x40000000), UINT32_MAX);
	probe(UINT64_C(0x100000000), UINT64_MAX);
}

int
__wrap_hal_page_map(hal_space_t space, void *address, hal_physaddr_t physical,
    size_t size, uint32_t attr)
{
	int result;
	unsigned index;

	result = __real_hal_page_map(space, address, physical, size, attr);
	if ((uintptr_t)address >= UINT64_C(0x40000000) &&
	    (uintptr_t)address < UINT64_C(0x40010000)) {
		if (result == HAL_OK) {
			for (index = 0; index < observed_count; index++) {
				if (observed_pages[index] == physical)
					break;
			}
			if (index == observed_count) {
				if (observed_count == 1024)
					HAL_FATAL("HIGH observer capacity exceeded");
				observed_pages[observed_count++] = physical;
			}
		}
		hal_printf("HIGH MAP space=%llu va=%llu pa=%llu size=%llu attr=%u result=%d\n",
		    (unsigned long long)(uintptr_t)space, (unsigned long long)(uintptr_t)address, (unsigned long long)physical,
		    (unsigned long long)size, attr, result);
	}
	return result;
}

int
__wrap_hal_page_unmap(hal_space_t space, void *address, size_t size)
{
	int result;

	result = __real_hal_page_unmap(space, address, size);
	if ((uintptr_t)address >= UINT64_C(0x40000000) &&
	    (uintptr_t)address < UINT64_C(0x40010000))
		hal_printf("HIGH UNMAP space=%llu va=%llu size=%llu result=%d\n",
		    (unsigned long long)(uintptr_t)space, (unsigned long long)(uintptr_t)address, (unsigned long long)size, result);
	return result;
}

static void
probe(uint64_t minimum, uint64_t maximum)
{
	struct hal_pmem_request request = {0};
	struct hal_pmem memory = {0};
	struct hal_memory_stats before;
	struct hal_memory_stats after;
	volatile uint64_t *words;
	uint64_t physical;
	size_t index;
	int result;

	request.type = HAL_PMEM_TYPE_RAM;
	request.paddr = HAL_PMEM_PADDR_ANY;
	request.size = 32768;
	request.alignment = 65536;
	hal_memory_get_stats(&before);
	result = hal_pmem_alloc_range(&request, minimum, maximum, 65536, &memory);
	if (result == HAL_ERR_NOMEM) {
		hal_printf("HIGH PROBE unavailable min=%llu\n", (unsigned long long)minimum);
		return;
	}
	if (result != HAL_OK || memory.paddr < minimum || memory.paddr > maximum ||
	    memory.size != 32768 || (memory.paddr & 65535U) != 0)
		HAL_FATAL("HIGH PROBE allocation failed");
	physical = memory.paddr;
	words = memory.vaddr;
	for (index = 0; index < memory.size / sizeof(*words); index++)
		words[index] = physical ^ (UINT64_C(0xfedcba9876543210) + index);
	for (index = 0; index < memory.size / sizeof(*words); index++) {
		if (words[index] != (physical ^ (UINT64_C(0xfedcba9876543210) + index)))
			HAL_FATAL("HIGH PROBE content mismatch");
	}
	if (hal_pmem_free(&memory) != HAL_OK)
		HAL_FATAL("HIGH PROBE free failed");
	hal_memory_get_stats(&after);
	if (before.physical_allocated != after.physical_allocated ||
	    before.physical_free != after.physical_free ||
	    before.physical_reserved != after.physical_reserved)
		HAL_FATAL("HIGH PROBE accounting mismatch");
	hal_printf("HIGH PROBE PASS min=%llu pa=%llu bytes=32768 accounting=restored\n",
	    (unsigned long long)minimum, (unsigned long long)physical);
}

/* Records retirement of the observed user backing, not unrelated heap objects. */
int
__wrap_hal_pmem_free(struct hal_pmem *memory)
{
	uint64_t physical;
	uint64_t size;
	unsigned index;
	int result;

	physical = memory == NULL ? 0 : memory->paddr;
	size = memory == NULL ? 0 : memory->size;
	result = __real_hal_pmem_free(memory);
	if (result != HAL_OK)
		return result;
	for (index = 0; index < observed_count; index++) {
		if (observed_pages[index] >= physical && observed_pages[index] - physical < size)
			hal_printf("HIGH FREE pa=%llu\n", (unsigned long long)observed_pages[index]);
	}
	return result;
}
