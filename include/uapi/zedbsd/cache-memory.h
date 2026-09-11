/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef KERN_UAPI_CACHE_MEMORY_H
#define KERN_UAPI_CACHE_MEMORY_H

#include <stdint.h>

#define CACHE_MEMORY_VERSION 1U

enum cache_memory_kind {
	CACHE_MEMORY_FILE_DATA,
	CACHE_MEMORY_FILE_META,
	CACHE_MEMORY_BUF_DATA,
	CACHE_MEMORY_BUF_META,
	CACHE_MEMORY_IO_POOL,
	CACHE_MEMORY_DMA,
	CACHE_MEMORY_WORKER,
	CACHE_MEMORY_KINDS
};

struct cache_memory_usage {
	uint64_t resident_bytes;
	uint64_t pending_bytes;
};

struct cache_memory_stats {
	uint32_t version;
	uint32_t count;
	uint64_t managed_bytes;
	uint64_t free_bytes;
	uint64_t reserve_bytes;
	uint64_t target_bytes;
	uint64_t pending_target_bytes;
	uint64_t resident_bytes;
	uint64_t pending_bytes;
	uint64_t refusals;
	uint64_t reclaimed_bytes;
	uint32_t initialized;
	uint32_t resizing;
	struct cache_memory_usage usage[CACHE_MEMORY_KINDS];
};

#endif
