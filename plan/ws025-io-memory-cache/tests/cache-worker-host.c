/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <hal/hal.h>
#include <kern/cache-memory.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include "cache-device-lock-host.h"

static size_t live_bytes;
static int reject_allocation = 1;
static unsigned char *backing;
void hal_fatal(const char *file, int line, const char *message)
{ fprintf(stderr, "%s:%d %s\n", file, line, message); abort(); }
void hal_memory_get_stats(struct hal_memory_stats *stats)
{
	memset(stats, 0, sizeof(*stats));
	stats->physical_total = 8U * 1024U * 1024U;
	stats->physical_free = stats->physical_total - live_bytes;
}
int hal_pmem_alloc(const struct hal_pmem_request *request, struct hal_pmem *memory)
{
	if (reject_allocation) return HAL_ERR_NOMEM;
	assert(request->size == 69632 && request->alignment == 4096);
	memory->size = 131072; /* Charge actual backing rather than requested bytes. */
	memory->vaddr = calloc(1, memory->size);
	assert(memory->vaddr);
	backing = memory->vaddr;
	live_bytes += memory->size;
	return HAL_OK;
}
int hal_pmem_free(struct hal_pmem *memory)
{ live_bytes -= memory->size; free(memory->vaddr); memset(memory, 0, sizeof(*memory)); return HAL_OK; }
static void *borrower(void *argument)
{
	struct cache_worker_buffer buffer;
	unsigned round, index;
	unsigned char byte = (unsigned char)(uintptr_t)argument;
	for (round = 0; round < 1000; round++) {
		if (cache_worker_borrow(&buffer) != 0) { sched_yield(); continue; }
		memset(buffer.data, byte, buffer.capacity);
		memset(buffer.metadata, byte, buffer.metadata_capacity);
		sched_yield();
		for (index = 0; index < 69632; index++) assert(backing[index] == byte);
		cache_worker_release(&buffer);
	}
	return NULL;
}
int main(void)
{
	struct cache_memory_stats stats;
	struct cache_worker_buffer buffer, other;
	pthread_t threads[4];
	unsigned i;
	cache_memory_init();
	assert(cache_worker_borrow(&buffer) == EAGAIN);
	assert(cache_worker_init() == ENOMEM);
	cache_memory_get_stats(&stats);
	assert(stats.resident_bytes == 0 && stats.pending_bytes == 0);
	assert(cache_memory_set_target(0) == 0);
	reject_allocation = 0;
	assert(cache_worker_init() == 0);
	assert(cache_worker_init() == EBUSY);
	cache_memory_get_stats(&stats);
	assert(stats.usage[CACHE_MEMORY_WORKER].resident_bytes == live_bytes);
	assert(stats.resident_bytes == 131072 && stats.pending_bytes == 0);
	assert(cache_worker_borrow(&buffer) == 0);
	assert(cache_worker_borrow(&other) == EAGAIN);
	assert(buffer.capacity == 65536 && buffer.metadata_capacity == 4096);
	cache_worker_release(&buffer);
	for (i = 0; i < 4; i++) assert(pthread_create(&threads[i], NULL, borrower, (void *)(uintptr_t)(i + 1)) == 0);
	for (i = 0; i < 4; i++) assert(pthread_join(threads[i], NULL) == 0);
	assert(cache_memory_set_target(0) == EBUSY);
	puts("worker reserve PASS: absent/failure states, actual charge, exclusive nonblocking borrow");
	return 0;
}

size_t hal_page_get_page_size(int level) { (void)level;return 4096; }
