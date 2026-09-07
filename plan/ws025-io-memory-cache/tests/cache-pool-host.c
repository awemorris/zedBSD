/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <hal/hal.h>
#include <kern/io-pool.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "io-pool-hal-host.h"
#include "cache-device-lock-host.h"

int main(int argc, char **argv)
{
	struct cache_memory_stats stats;
	struct io_pool_stats pool;
	void *buffer;
	size_t capacity;

	if (argc == 2) fail_at = (unsigned)strtoul(argv[1], NULL, 10);
	cache_memory_init();
	assert(cache_memory_set_target(0) == 0);
	io_pool_init();
	io_pool_get_stats(&pool);
	cache_memory_get_stats(&stats);
	assert(stats.pending_bytes == 0 && stats.resident_bytes == live_bytes);
	assert(stats.usage[CACHE_MEMORY_IO_POOL].resident_bytes == pool.resident_bytes);
	assert(pool.resident_bytes == live_bytes);
	buffer = io_pool_borrow(65536, &capacity);
	if (buffer != NULL) {
		assert(capacity >= 65536);
		memset(buffer, 0x51, capacity);
		io_pool_release(buffer);
	}
	cache_memory_get_stats(&stats);
	assert(stats.resident_bytes == live_bytes && stats.pending_bytes == 0);
	if (live_bytes != 0) assert(cache_memory_set_target(0) == EBUSY);
	puts("pool shared accounting PASS: backing charged once, borrow stable, allocation failure");
	return 0;
}
