/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#define WS025_DMA_RANGE_EMBEDDED
#include "dma-range-host.c"
#include <stdio.h>
#include "cache-device-lock-host.h"

void hal_fatal(const char *file, int line, const char *message)
{ fprintf(stderr, "%s:%d %s\n", file, line, message); abort(); }
void hal_pmem_get_stats(struct hal_pmem_stats *stats)
{
	memset(stats, 0, sizeof(*stats));
	stats->physical_total = 4U * 1024U * 1024U;
	stats->physical_free = (pools[0].free_pages + pools[1].free_pages) * 4096U;
}
#ifndef DMA_CACHE_MAIN
#define DMA_CACHE_MAIN main
#endif
int DMA_CACHE_MAIN(void)
{
	struct cache_memory_stats stats;
	struct drv_dma_constraints constraints = {64, 65536, 65536, 1};
	struct drv_dma_device *device;
	struct drv_dma_buffer buffer;

	cache_memory_init();
	assert(dma_range_run() == 0);
	cache_memory_get_stats(&stats);
	assert(stats.resident_bytes == 0 && stats.pending_bytes == 0);
	assert(cache_memory_set_target(4096) == 0);
	assert(drv_dma_device_create(&constraints, &device) == 0);
	assert(drv_dma_alloc_coherent(device, 5000, 4096, &buffer) == 0);
	cache_memory_get_stats(&stats);
	assert(stats.resident_bytes == 8192 && stats.pending_bytes == 0);
	assert(stats.usage[CACHE_MEMORY_DMA].resident_bytes == 8192);
	assert(cache_memory_reserve(CACHE_MEMORY_FILE_DATA, 4096, 1) == ENOMEM);
	assert(cache_memory_set_target(0) == EBUSY);

	/* Failed physical retirement preserves ownership and a retryable descriptor. */
	reject_free = 1;
	drv_dma_free_coherent(device, &buffer);
	assert(buffer.private_data[0] != 0 && buffer.address != NULL);
	cache_memory_get_stats(&stats);
	assert(stats.resident_bytes == 8192 && stats.pending_bytes == 0);
	assert(drv_dma_device_destroy(device) == EBUSY);
	reject_free = 0;
	drv_dma_free_coherent(device, &buffer);
	assert(buffer.private_data[0] == 0 && buffer.address == NULL);
	assert(drv_dma_device_destroy(device) == 0);
	cache_memory_get_stats(&stats);
	assert(stats.resident_bytes == 0 && stats.pending_bytes == 0);
	assert(cache_memory_set_target(0) == 0);
	puts("DMA shared accounting PASS: rounded mandatory bytes, low/high constraints, failed retirement retry");
	return 0;
}
