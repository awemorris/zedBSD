/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#include "src/hal/amd64/pmem-range.h"

#define PAGE AMD64_PMEM_PAGE
static struct amd64_pmem_extent extent;
static unsigned char owned[8193];
static uint64_t addresses[512], sizes[512];
static uint32_t random_state = 1234567;
static pthread_mutex_t pool_lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t random_number(void)
{
	random_state ^= random_state << 13;
	random_state ^= random_state >> 17;
	random_state ^= random_state << 5;
	return random_state;
}

static void audit(void)
{
	uint64_t i, free_pages = 0, reserved_pages = 0, allocated_pages = 0;
	for (i = 0; i < extent.pages; i++) {
		unsigned used = (extent.used[i / 64] >> (i % 64)) & 1;
		unsigned reserved = (extent.reserved[i / 64] >> (i % 64)) & 1;
		assert(used == (owned[i] != 0));
		assert(reserved == (owned[i] == 2));
		free_pages += owned[i] == 0;
		reserved_pages += owned[i] == 2;
		allocated_pages += owned[i] == 1;
	}
	assert(extent.free_pages == free_pages && extent.reserved_pages == reserved_pages);
	assert(extent.allocated_pages == allocated_pages);
	assert(free_pages + reserved_pages + allocated_pages == extent.pages);
	for (i = 0; i < extent.words; i++)
		assert(((extent.summary[i / 64] >> (i % 64)) & 1) == (extent.used[i] != UINT64_MAX));
}

static void *worker(void *argument)
{
	uint64_t physical, size, first;
	unsigned iteration;
	uint64_t requested = ((uintptr_t)argument + 1) * PAGE;
	for (iteration = 0; iteration < 500; iteration++) {
		pthread_mutex_lock(&pool_lock);
		assert(amd64_pmem_extent_alloc(&extent, requested, PAGE, 0, UINT64_MAX, 0, &physical, &size) == AMD64_PMEM_OK);
		first = (physical - extent.base) / PAGE;
		for (uint64_t i = first; i < first + size / PAGE; i++) assert(owned[i] == 0);
		memset(owned + first, 1, (size_t)(size / PAGE));
		audit();
		pthread_mutex_unlock(&pool_lock);
		sched_yield();
		pthread_mutex_lock(&pool_lock);
		assert(amd64_pmem_extent_free(&extent, physical, size) == AMD64_PMEM_OK);
		memset(owned + first, 0, (size_t)(size / PAGE));
		audit();
		pthread_mutex_unlock(&pool_lock);
	}
	return NULL;
}

int main(void)
{
	uint64_t bytes, base = 0x100003000ULL, physical, allocated, first, i, j, n;
	uint64_t minimum, maximum, alignment, boundary, request;
	void *metadata;
	uint64_t worst_scan;
	pthread_t threads[4];
	enum amd64_pmem_result result;

	bytes = amd64_pmem_metadata_size(8193);
	metadata = malloc((size_t)bytes);
	assert(metadata != NULL);
	assert(amd64_pmem_extent_init(&extent, base, 8193ULL * PAGE, metadata, bytes - 1) == AMD64_PMEM_INVALID);
	assert(amd64_pmem_extent_init(&extent, base, 8193ULL * PAGE, metadata, bytes) == AMD64_PMEM_OK);
	assert(amd64_pmem_reserve(&extent, base + 60 * PAGE, 70 * PAGE) == AMD64_PMEM_OK);
	memset(owned + 60, 2, 70);
	assert(amd64_pmem_reserve(&extent, base + 60 * PAGE, 70 * PAGE) == AMD64_PMEM_OK);
	audit();
	assert(amd64_pmem_extent_alloc(&extent, PAGE, PAGE, 0, UINT32_MAX, 0, &physical, &allocated) == AMD64_PMEM_NOMEM);
	assert(amd64_pmem_extent_alloc(&extent, PAGE, PAGE, base + 1, base + PAGE - 1, 0, &physical, &allocated) == AMD64_PMEM_NOMEM);
	assert(amd64_pmem_extent_alloc(&extent, PAGE + 1, PAGE, base, UINT64_MAX, PAGE, &physical, &allocated) == AMD64_PMEM_NOMEM);
	assert(amd64_pmem_extent_alloc(&extent, UINT64_MAX, PAGE, 0, UINT64_MAX, 0, &physical, &allocated) == AMD64_PMEM_INVALID);
	assert(amd64_pmem_extent_alloc(&extent, PAGE, 3, 0, UINT64_MAX, 0, &physical, &allocated) == AMD64_PMEM_INVALID);

	for (n = 0; n < 5000; n++) {
		i = random_number() % 512;
		if (sizes[i] != 0) {
			first = (addresses[i] - base) / PAGE;
			assert(amd64_pmem_reserve(&extent, addresses[i], sizes[i]) == AMD64_PMEM_STATE);
			if (sizes[i] > PAGE)
				assert(amd64_pmem_extent_free(&extent, addresses[i], sizes[i] - PAGE) == AMD64_PMEM_STATE);
			assert(amd64_pmem_extent_free(&extent, addresses[i], sizes[i]) == AMD64_PMEM_OK);
			assert(amd64_pmem_extent_free(&extent, addresses[i], sizes[i]) == AMD64_PMEM_STATE);
			memset(owned + first, 0, (size_t)(sizes[i] / PAGE));
			sizes[i] = 0;
		} else {
			request = 1 + random_number() % (32 * PAGE);
			alignment = (uint64_t)PAGE << (random_number() % 6);
			boundary = random_number() & 1 ? 64 * PAGE : 0;
			minimum = base + (random_number() % 32) * PAGE;
			maximum = base + (8193 - random_number() % 32) * PAGE - 1;
			result = amd64_pmem_extent_alloc(&extent, request, alignment, minimum, maximum, boundary, &physical, &allocated);
			assert(result == AMD64_PMEM_OK || result == AMD64_PMEM_NOMEM);
			if (result == AMD64_PMEM_OK) {
				assert(physical >= minimum && allocated - 1 <= maximum - physical);
				assert((physical & (alignment - 1)) == 0);
				assert(boundary == 0 || allocated <= boundary - physical % boundary);
				assert(allocated == (request + PAGE - 1) / PAGE * PAGE);
				first = (physical - base) / PAGE;
				for (j = first; j < first + allocated / PAGE; j++) assert(owned[j] == 0);
				memset(owned + first, 1, (size_t)(allocated / PAGE));
				addresses[i] = physical; sizes[i] = allocated;
			} else {
				/* An independent page oracle must also find no legal run. */
				uint64_t candidate, need = (request + PAGE - 1) / PAGE;
				for (candidate = 0; candidate + need <= extent.pages; candidate++) {
					uint64_t address = base + candidate * PAGE;
					if (address < minimum || address > maximum || need * PAGE - 1 > maximum - address ||
					    address % alignment != 0 || (boundary && need * PAGE > boundary - address % boundary)) continue;
					for (j = 0; j < need && owned[candidate + j] == 0; j++) {}
					assert(j != need);
				}
			}
		}
		audit();
	}
	for (i = 0; i < 512; i++) if (sizes[i]) {
		assert(amd64_pmem_extent_free(&extent, addresses[i], sizes[i]) == AMD64_PMEM_OK);
		memset(owned + (addresses[i] - base) / PAGE, 0, (size_t)(sizes[i] / PAGE));
	}
	audit();
	assert(extent.free_pages == 8193 - 70);
	assert(amd64_pmem_extent_free(&extent, base + 60 * PAGE, PAGE) == AMD64_PMEM_STATE);
	assert(amd64_pmem_extent_free(&extent, base - PAGE, PAGE) == AMD64_PMEM_INVALID);
	for (i = 0; i < 4; i++) assert(pthread_create(&threads[i], NULL, worker, (void *)(uintptr_t)i) == 0);
	for (i = 0; i < 4; i++) assert(pthread_join(threads[i], NULL) == 0);
	audit();
	worst_scan = extent.max_scan_words;
	assert(amd64_pmem_extent_init(&extent, 0xffffe000ULL, 4 * PAGE, metadata, bytes) == AMD64_PMEM_OK);
	assert(amd64_pmem_extent_alloc(&extent, 2 * PAGE, PAGE, 0, UINT32_MAX, 0, &addresses[0], &sizes[0]) == AMD64_PMEM_OK);
	assert(addresses[0] == 0xffffe000ULL);
	assert(amd64_pmem_extent_alloc(&extent, PAGE, PAGE, 0, UINT32_MAX, 0, &physical, &allocated) == AMD64_PMEM_NOMEM);
	assert(amd64_pmem_extent_alloc(&extent, 2 * PAGE, PAGE, 0, UINT64_MAX, 0, &addresses[1], &sizes[1]) == AMD64_PMEM_OK);
	assert(addresses[1] == 0x100000000ULL);
	assert(amd64_pmem_extent_free(&extent, addresses[0], 4 * PAGE) == AMD64_PMEM_STATE);
	assert(amd64_pmem_extent_free(&extent, addresses[0], sizes[0]) == AMD64_PMEM_OK);
	assert(amd64_pmem_extent_free(&extent, addresses[1], sizes[1]) == AMD64_PMEM_OK);
	free(metadata);
	printf("PASS: 5000 randomized high-RAM ownership operations, DMA limits and summary accounting; max_scan=%llu\n",
	    (unsigned long long)worst_scan);
	return 0;
}
