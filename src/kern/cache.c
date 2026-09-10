/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The page cache memory budget.
 *
 * One unit holds the accounting that divides cache memory between its
 * classes and the dedicated write-back worker reserve taken out of it.
 */

#include <kern/cache-memory.h>
#include <kern/atomic.h>
#include <kern/buf.h>
#include <kern/lock.h>
#include <kern/page.h>
#include <hal/hal.h>
#include <errno.h>
#include <string.h>
#include <kern/io-scratch.h>
#include <kern/io-pool.h>

#define CACHE_RECLAIM_BATCH (64U * 1024U)
#define CACHE_RESERVE_MIN (64U * 1024U)
#define CACHE_RESERVE_MAX (8U * 1024U * 1024U)
#define WORKER_BYTES (KERN_IO_BATCH_MAX + ZEDBSD_PAGE_SIZE)

static struct cache_memory_stats accounting;
static atomic_uint_t accounting_lock;
static struct mutex control_lock;
static struct io_scratch worker_memory;

/* 0 absent, 1 idle, 2 borrowed, 3 constructing. */
static atomic_uint_t worker_state;

extern size_t vm_object_reclaim_clean(size_t) __attribute__((weak));
extern size_t buf_reclaim(size_t, unsigned) __attribute__((weak));

static bool cache_lock(void);
static void cache_unlock(bool enabled);
static void cache_validate(enum cache_memory_kind kind);

/*
 * Derives a soft ownership target and a physical free-memory floor from RAM.
 */
void
cache_memory_policy(
	uint64_t managed,
	uint64_t *target,
	uint64_t *reserve)
{
	uint64_t floor;

	/* Keeps a modest physical reserve without inventing RAM on tiny machines. */
	floor = managed / 64U;
	if (floor < CACHE_RESERVE_MIN)
		floor = CACHE_RESERVE_MIN;
	if (floor > CACHE_RESERVE_MAX)
		floor = CACHE_RESERVE_MAX;
	if (floor > managed / 4U)
		floor = managed / 4U;
	floor &= ~(uint64_t)(ZEDBSD_PAGE_SIZE - 1U);
	*reserve = floor;
	*target = (managed / 4U) & ~(uint64_t)(ZEDBSD_PAGE_SIZE - 1U);
}

/*
 * Publishes managed-memory policy before ordinary cache construction.
 */
void
cache_memory_init(
	void)
{
	struct hal_pmem_stats memory;
	uint64_t target;
	uint64_t reserve;
	bool enabled;

	/* Samples real managed memory outside the accounting lock. */
	hal_pmem_get_stats(&memory);
	cache_memory_policy(memory.physical_total, &target, &reserve);

	if (mutex_init(&control_lock, LOCK_RANK_VM_RESIZE, "cache memory control") != 0)
		HAL_FATAL("cache memory control initialization failed");

	/* Preserves any early mandatory accounting while enabling admission. */
	enabled = cache_lock();

	if (accounting.initialized)
		HAL_FATAL("cache memory policy initialized twice");

	accounting.version = CACHE_MEMORY_VERSION;
	accounting.count = CACHE_MEMORY_KINDS;
	accounting.managed_bytes = memory.physical_total;
	accounting.reserve_bytes = reserve;
	accounting.target_bytes = target;
	accounting.initialized = 1;

	cache_unlock(enabled);
}

/*
 * Reserves ownership before allocation, rejecting optional pressure promptly.
 */
int
cache_memory_reserve(
	enum cache_memory_kind kind,
	size_t bytes,
	int optional)
{
	struct hal_pmem_stats memory;
	uint64_t target;
	uint64_t current;
	uint64_t floor;
	bool enabled;
	int refused;

	/* Validates category and samples physical availability without global locks. */
	cache_validate(kind);
	memset(&memory, 0, sizeof(memory));

	if (optional)
		hal_pmem_get_stats(&memory);

	enabled = cache_lock();
	current = accounting.resident_bytes + accounting.pending_bytes;
	refused = bytes > UINT64_MAX - current;

	/* Mandatory commitments remain visible even when they exceed the soft target. */
	if (optional && accounting.initialized) {
		target = accounting.resizing ? accounting.pending_target_bytes :
		    accounting.target_bytes;
		floor = accounting.reserve_bytes;
		if (current > target || bytes > target - current)
			refused = 1;
		if (memory.physical_free < floor ||
		    accounting.pending_bytes > memory.physical_free - floor ||
		    bytes > memory.physical_free - floor - accounting.pending_bytes)
			refused = 1;
	}

	if (refused) {
		if (accounting.refusals != UINT64_MAX)
			accounting.refusals++;
		cache_unlock(enabled);
		return ENOMEM;
	}

	/* Charges pending bytes exactly once, before any caller publishes storage. */
	accounting.pending_bytes += bytes;
	accounting.usage[kind].pending_bytes += bytes;
	cache_unlock(enabled);

	/* Reports the reservation which the caller must commit or cancel. */
	return 0;
}

/*
 * Converts an allocation reservation into resident ownership.
 */
void
cache_memory_commit(
	enum cache_memory_kind kind,
	size_t bytes)
{
	bool enabled;

	/* Moves the same bytes between states without changing total ownership. */
	cache_validate(kind);

	enabled = cache_lock();

	if (accounting.usage[kind].pending_bytes < bytes)
		HAL_FATAL("cache memory commit without reservation");

	accounting.usage[kind].pending_bytes -= bytes;
	accounting.pending_bytes -= bytes;
	accounting.usage[kind].resident_bytes += bytes;
	accounting.resident_bytes += bytes;

	cache_unlock(enabled);
}

/*
 * Cancels an allocation which never became resident.
 */
void
cache_memory_cancel(
	enum cache_memory_kind kind,
	size_t bytes)
{
	bool enabled;

	/* Returns only pending ownership for the selected category. */
	cache_validate(kind);

	enabled = cache_lock();

	if (accounting.usage[kind].pending_bytes < bytes)
		HAL_FATAL("cache memory cancellation without reservation");

	accounting.usage[kind].pending_bytes -= bytes;
	accounting.pending_bytes -= bytes;

	cache_unlock(enabled);
}

/*
 * Releases ownership only after the associated storage has been freed.
 */
void
cache_memory_release(
	enum cache_memory_kind kind,
	size_t bytes)
{
	bool enabled;

	/* Keeps dirty, pinned and quarantined storage charged until real retirement. */
	cache_validate(kind);
	enabled = cache_lock();
	if (accounting.usage[kind].resident_bytes < bytes)
		HAL_FATAL("cache memory release without ownership");
	accounting.usage[kind].resident_bytes -= bytes;
	accounting.resident_bytes -= bytes;
	cache_unlock(enabled);
}

/*
 * Reclaims a bounded amount of immediately disposable cache without I/O.
 */
size_t
cache_memory_reclaim(
	size_t target)
{
	size_t freed;
	bool enabled;

	/* Bounds each pass before entering the independent clean-cache owners. */
	if (target > CACHE_RECLAIM_BATCH)
		target = CACHE_RECLAIM_BATCH;

	freed = 0;

	if (vm_object_reclaim_clean != NULL && target != 0)
		freed = vm_object_reclaim_clean(target);

	if (buf_reclaim != NULL && freed < target)
		freed += buf_reclaim(target - freed, 0);

	/* Records completed reclaim after every owner has dropped its locks. */
	enabled = cache_lock();

	if (freed > UINT64_MAX - accounting.reclaimed_bytes)
		accounting.reclaimed_bytes = UINT64_MAX;
	else
		accounting.reclaimed_bytes += freed;

	cache_unlock(enabled);

	/* Reports actual retired bytes, never a prediction about dirty memory. */
	return freed;
}

/*
 * Changes the ownership target only after eligible clean storage fits it.
 */
int
cache_memory_set_target(
	uint64_t target)
{
	uint64_t current;
	uint64_t attempts;
	size_t wanted;
	bool enabled;
	int error;

	/* Rejects early callers before touching the not-yet-initialized mutex. */
	enabled = cache_lock();
	if (!accounting.initialized) {
		cache_unlock(enabled);
		return EINVAL;
	}

	cache_unlock(enabled);

	/* Serializes policy changes while allocations inspect the pending gate. */
	mutex_lock(&control_lock);

	enabled = cache_lock();
	if (!accounting.initialized ||
	    (target & (ZEDBSD_PAGE_SIZE - 1U)) != 0 ||
	    target > accounting.managed_bytes - accounting.reserve_bytes) {
		cache_unlock(enabled);
		mutex_unlock(&control_lock);
		return EINVAL;
	}

	accounting.pending_target_bytes = target;
	accounting.resizing = 1;
	current = accounting.resident_bytes + accounting.pending_bytes;
	attempts = current / ZEDBSD_PAGE_SIZE + 1U;
	cache_unlock(enabled);

	/* Drains clean ownership in finite batches; mandatory growth may refuse shrink. */
	error = 0;
	for (;;) {
		enabled = cache_lock();
		current = accounting.resident_bytes + accounting.pending_bytes;
		if (current <= target) {
			accounting.target_bytes = target;
			accounting.resizing = 0;
			accounting.pending_target_bytes = 0;
			cache_unlock(enabled);
			break;
		}

		cache_unlock(enabled);
		wanted = current - target > CACHE_RECLAIM_BATCH ?
		    CACHE_RECLAIM_BATCH : (size_t)(current - target);
		if (attempts == 0 || cache_memory_reclaim(wanted) == 0) {
			error = EBUSY;
			break;
		}

		attempts--;
	}

	/* Restores the published policy after a failed pending transition. */
	if (error != 0) {
		enabled = cache_lock();
		accounting.resizing = 0;
		accounting.pending_target_bytes = 0;
		cache_unlock(enabled);
	}

	mutex_unlock(&control_lock);

	/* Reports whether the new target was actually published. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Reports exact ownership counters with a separately sampled physical-free value.
 */
void
cache_memory_get_stats(
	struct cache_memory_stats *stats)
{
	struct hal_pmem_stats memory;
	bool enabled;

	/* Copies the ownership transaction under its private lock. */
	if (stats == NULL)
		return;

	enabled = cache_lock();
	memcpy(stats, &accounting, sizeof(*stats));
	cache_unlock(enabled);

	/* Avoids acquiring the physical allocator under cache accounting ownership. */
	hal_pmem_get_stats(&memory);
	stats->free_bytes = memory.physical_free;
}

/*
 * Reserves the worker storage.
 *
 * The reserve is independent of ordinary scratch borrowers, so a writeback
 * pass never competes with them for memory.
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

	/* Allocates the region, releasing a short allocation before reporting it. */
	memset(&memory, 0, sizeof(memory));
	error = io_scratch_alloc(WORKER_BYTES, &memory);
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

	/* Publishes the reserve as idle once its charge is committed. */
	cache_memory_commit(CACHE_MEMORY_WORKER, memory.size);
	worker_memory = memory;
	atomic_store_release(&worker_state, 1);

	/* Reports the completed reservation. */
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

	/* Reports the loan. */
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

	/* Clears the loan and publishes the reserve as idle again. */
	memset(buffer, 0, sizeof(*buffer));
	expected = 2;
	if (!atomic_compare_exchange(&worker_state, &expected, 1))
		HAL_FATAL("cache worker reservation returned twice");
}

/* Acquires the early-boot-safe accounting lock without involving VFS or VM. */
static bool
cache_lock(
	void)
{
	bool enabled;

	/* Serializes fixed-size counters with interrupts disabled on this CPU. */
	enabled = hal_irq_disable();
	while (!atomic_try_acquire_zero(&accounting_lock))
		hal_compiler_barrier();

	/* Preserves the caller's interrupt state. */
	return enabled;
}

/* Releases accounting ownership before restoring interrupts. */
static void
cache_unlock(
	bool enabled)
{
	/* Publishes all counter changes before waking another allocator. */
	atomic_store_release(&accounting_lock, 0);
	if (enabled)
		hal_irq_enable();
}

/* Validates caller-owned accounting categories before indexing fixed storage. */
static void
cache_validate(
	enum cache_memory_kind kind)
{
	/* Rejects programming errors rather than corrupting another ownership class. */
	if (kind < 0 || kind >= CACHE_MEMORY_KINDS)
		HAL_FATAL("invalid cache memory category");
}
