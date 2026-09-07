/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_KERN_CACHE_MEMORY_H
#define ZEDBSD_KERN_CACHE_MEMORY_H

#include <stddef.h>
#include <stdint.h>
#include <zedbsd/cache-memory.h>

struct cache_worker_buffer {
	void *data;
	size_t capacity;
	void *metadata;
	size_t metadata_capacity;
};

int cache_worker_init(void);
int cache_worker_borrow(struct cache_worker_buffer *buffer);
void cache_worker_release(struct cache_worker_buffer *buffer);

void cache_memory_policy(uint64_t managed, uint64_t *target, uint64_t *reserve);
void cache_memory_init(void);
int cache_memory_reserve(enum cache_memory_kind kind, size_t bytes, int optional);
void cache_memory_commit(enum cache_memory_kind kind, size_t bytes);
void cache_memory_cancel(enum cache_memory_kind kind, size_t bytes);
void cache_memory_release(enum cache_memory_kind kind, size_t bytes);
size_t cache_memory_reclaim(size_t target);
int cache_memory_set_target(uint64_t target);
void cache_memory_get_stats(struct cache_memory_stats *stats);

#endif
