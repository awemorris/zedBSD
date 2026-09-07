/* -*- mode: c; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Dedicated nonblocking scratch reserved before delayed writeback can be used.
 * This module provides storage only; it does not start or enable writeback.
 */

#include <kern/cache-memory.h>
#include <kern/io-scratch.h>
#include <kern/atomic.h>
#include <kern/io-pool.h>
#include <kern/page.h>
#include <hal/hal.h>
#include <errno.h>
#include <string.h>

#define WORKER_BYTES (KERN_IO_BATCH_MAX + ZEDBSD_PAGE_SIZE)

static struct io_scratch worker_memory;
static atomic_uint_t worker_state; /* 0 absent, 1 idle, 2 borrowed, 3 constructing */

/*
 * Reserves data and control storage independently of ordinary scratch borrowers.
 */
int
cache_worker_init(
	void)
{
	struct io_scratch memory;
	unsigned expected;
	int error;

	/* Serializes initialization while keeping the unavailable state observable. */
	expected = 0;
	if (!atomic_compare_exchange(&worker_state, &expected, 3))
		return EBUSY;
	memset(&memory, 0, sizeof(memory));
	error = io_scratch_alloc(WORKER_BYTES, 1, &memory);
	if (error != HAL_OK || memory.vaddr == NULL || memory.size < WORKER_BYTES) {
		if (memory.size != 0 && io_scratch_free(&memory) != HAL_OK)
			HAL_FATAL("cache worker allocation rollback failed");
		atomic_store_release(&worker_state, 0);
		return ENOMEM;
	}

	/* Charges all physical backing, including allocator rounding and controls. */
	error = cache_memory_reserve(CACHE_MEMORY_WORKER, memory.size, 0);
	if (error != 0) {
		if (io_scratch_free(&memory) != HAL_OK)
			HAL_FATAL("cache worker reservation rollback failed");
		atomic_store_release(&worker_state, 0);
		return error;
	}
	cache_memory_commit(CACHE_MEMORY_WORKER, memory.size);
	worker_memory = memory;
	atomic_store_release(&worker_state, 1);
	return 0;
}

/*
 * Borrows the whole worker reserve without waiting or allocating memory.
 */
int
cache_worker_borrow(
	struct cache_worker_buffer *buffer)
{
	unsigned expected;

	/* Refuses absent or busy storage without exposing a partial reservation. */
	if (buffer == NULL)
		return EINVAL;
	memset(buffer, 0, sizeof(*buffer));
	expected = 1;
	if (!atomic_compare_exchange(&worker_state, &expected, 2))
		return EAGAIN;

	/* Separates transfer payload from one page of worker control metadata. */
	buffer->data = worker_memory.vaddr;
	buffer->capacity = KERN_IO_BATCH_MAX;
	buffer->metadata = (char *)worker_memory.vaddr + KERN_IO_BATCH_MAX;
	buffer->metadata_capacity = ZEDBSD_PAGE_SIZE;
	return 0;
}

/*
 * Returns an exclusive worker reservation while retaining its physical backing.
 */
void
cache_worker_release(
	struct cache_worker_buffer *buffer)
{
	unsigned expected;

	/* Validates ownership before making the persistent reserve available again. */
	if (buffer == NULL || buffer->data != worker_memory.vaddr ||
	    buffer->data == NULL)
		HAL_FATAL("invalid cache worker reservation return");
	memset(buffer, 0, sizeof(*buffer));
	expected = 2;
	if (!atomic_compare_exchange(&worker_state, &expected, 1))
		HAL_FATAL("cache worker reservation returned twice");
}
