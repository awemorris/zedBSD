/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Shared I/O infrastructure.
 *
 * One unit holds the persistent failure ledger that reports a write-back
 * error once per observer, the bounded shared scratch pool, the physically
 * backed scratch regions the pool hands out, and the cumulative I/O event
 * counters.
 */

#include <kern/io-error.h>
#include <hal/hal.h>
#include <kern/cache-memory.h>
#include <kern/io-scratch.h>
#include <kern/io-pool.h>
#include <kern/atomic.h>
#include <kern/io-stats.h>
#include <string.h>
#include "kern/atomic.h"
#include "kern/io-stats.h"

#define IO_POOL_MAX_BYTES (4U * 1024U * 1024U)

#define IO_POOL_MAX_LARGE (IO_POOL_MAX_BYTES / KERN_IO_BATCH_MAX)

#define IO_POOL_MAX_SMALL (IO_POOL_MAX_BYTES / 4U / KERN_IO_SMALL_SIZE)

struct io_pool_slot {
	struct io_scratch backing;
	unsigned borrowed;
};

static struct io_scratch metadata;

static struct io_pool_slot *large_slots;

static struct io_pool_slot *small_slots;

static unsigned large_count;

static unsigned small_count;

static unsigned large_rotor;

static unsigned small_rotor;

static unsigned initialized;

static size_t budget_bytes;

static size_t resident_bytes;

static struct io_stat_value counters[IO_STAT_COUNT];

static bool io_error_lock(struct io_error_state *state);
static void io_error_unlock(struct io_error_state *state, bool enabled);
extern int cache_memory_reserve(enum cache_memory_kind, size_t, int) __attribute__((weak));
extern void cache_memory_commit(enum cache_memory_kind, size_t) __attribute__((weak));
extern void cache_memory_cancel(enum cache_memory_kind, size_t) __attribute__((weak));
extern void cache_memory_release(enum cache_memory_kind, size_t) __attribute__((weak));
extern size_t cache_memory_reclaim(size_t) __attribute__((weak));
static int allocate_backing(size_t bytes, size_t limit, struct io_scratch *memory);
static void *borrow_slots(struct io_pool_slot *slots, unsigned count, unsigned *rotor);
static int release_slots(struct io_pool_slot *slots, unsigned count, void *buffer);

/*
 * Records a new actual writeback failure without clearing earlier observations.
 */
void
io_error_record(
	struct io_error_state *state,
	int error)
{
	bool enabled;

	/* A successful retry changes dirty ownership, not historical notification. */
	if (state == NULL || error == 0)
		return;

	/* Advances the sequence and publishes the errno as one coherent pair. */
	enabled = io_error_lock(state);
	if (state->sequence != UINT64_MAX)
		state->sequence++;
	state->error = error;
	io_error_unlock(state, enabled);
}

/*
 * Captures one matching sequence and errno before an observer advances its
 * cursor.
 */
void
io_error_snapshot(
	struct io_error_state *state,
	struct io_error_snapshot *snapshot)
{
	bool enabled;

	/* Supports owners without a ledger as an empty observation. */
	snapshot->sequence = 0;
	snapshot->error = 0;
	if (state == NULL)
		return;

	/* Reads the sequence and the errno under one guarded interval. */
	enabled = io_error_lock(state);
	snapshot->sequence = state->sequence;
	snapshot->error = state->error;
	io_error_unlock(state, enabled);
}

/*
 * Reports a captured failure once per observer without consuming a later
 * event.
 */
int
io_error_observe(
	const struct io_error_snapshot *snapshot,
	volatile uint64_t *cursor)
{
	uint64_t current;

	/* Empty snapshots have nothing to acknowledge. */
	if (snapshot->sequence == 0 || snapshot->error == 0)
		return 0;

	/* Saturation cannot distinguish new events, so it stays conservatively sticky. */
	if (snapshot->sequence == UINT64_MAX) {
		atomic_u64_store_release(cursor, UINT64_MAX);
		return snapshot->error;
	}

	/* Advances only to this snapshot, preserving concurrently observed newer ones. */
	current = atomic_u64_load_acquire(cursor);
	while (current < snapshot->sequence) {
		if (atomic_u64_compare_exchange(cursor, &current, snapshot->sequence))
			return snapshot->error;
	}

	/* Reports nothing for a failure this observer already acknowledged. */
	return 0;
}

/*
 * Allocates the shared reserve once, before the first executable is loaded.
 * Allocation failure reduces capacity; callers retain their stack fallback.
 */
void
io_pool_init(void)
{
	struct hal_memstat memory;
	size_t wanted_large;
	size_t wanted_small;
	size_t wanted;
	size_t small_limit;
	size_t small_bytes;
	unsigned cpus;
	unsigned expected;

	/* Establishes a single initialization owner before publishing any slots. */
	expected = 0;
	if (!atomic_raw_compare_exchange(&initialized, &expected, 1U))
		HAL_FATAL("I/O pool initialized twice");
	hal_get_memstat(&memory);
	budget_bytes = memory.physical_total / 64U;
	if (budget_bytes > IO_POOL_MAX_BYTES)
		budget_bytes = IO_POOL_MAX_BYTES;
	cpus = hal_cpu_count();
	wanted = cpus > IO_POOL_MAX_SMALL / 2U ? IO_POOL_MAX_SMALL : (size_t)cpus * 2U;
	if (wanted < 16U)
		wanted = 16U;
	wanted_large = wanted > IO_POOL_MAX_LARGE ? IO_POOL_MAX_LARGE : wanted;

	/* Charges the complete, page-rounded descriptor table to the same budget. */
	wanted_small = wanted;
	if (!allocate_backing((wanted_large + wanted_small) * sizeof(*large_slots),
	    budget_bytes, &metadata)) {
		atomic_raw_store_release(&initialized, 2U);
		hal_printf("I/O pool unavailable budget=%llu; stack fallback active\n",
		    (unsigned long long)budget_bytes);
		return;
	}

	resident_bytes = metadata.size;
	hal_memset(metadata.vaddr, 0, metadata.size);
	large_slots = metadata.vaddr;
	small_slots = large_slots + wanted_large;

	/* Keeps the small reserve within one quarter of the total byte budget. */
	small_limit = budget_bytes / 4U;
	if (small_limit > budget_bytes - resident_bytes)
		small_limit = budget_bytes - resident_bytes;
	small_bytes = 0;
	while (small_count < wanted_small) {
		if (!allocate_backing(KERN_IO_SMALL_SIZE, small_limit - small_bytes,
		    &small_slots[small_count].backing))
			break;
		small_bytes += small_slots[small_count].backing.size;
		resident_bytes += small_slots[small_count].backing.size;
		small_count++;
	}

	/* Allocates large slots during construction. */
	while (large_count < wanted_large) {
		if (!allocate_backing(KERN_IO_BATCH_MAX, budget_bytes - resident_bytes,
		    &large_slots[large_count].backing))
			break;
		resident_bytes += large_slots[large_count].backing.size;
		large_count++;
	}

	atomic_raw_store_release(&initialized, 2U);
	hal_printf("I/O pool large=%u small=%u resident=%llu budget=%llu\n",
	    large_count, small_count, (unsigned long long)resident_bytes,
	    (unsigned long long)budget_bytes);
}

/*
 * Borrows available storage without waiting, allocating, or entering VFS/VM.
 */
void *
io_pool_borrow(
	size_t wanted,
	size_t *capacity)
{
	void *buffer;

	/* Leaves the caller's capacity untouched when neither reserve is available. */
	if (capacity == NULL || wanted == 0)
		return NULL;
	buffer = NULL;
	if (atomic_raw_load_acquire(&initialized) != 2U) {
		io_stats_record(IO_POOL_FALLBACK, wanted);
		return NULL;
	}

	/* Small requests first preserve the large reserve for full I/O batches. */
	if (wanted <= KERN_IO_SMALL_SIZE) {
		buffer = borrow_slots(small_slots, small_count, &small_rotor);
		if (buffer != NULL) {
			*capacity = KERN_IO_SMALL_SIZE;
			io_stats_record(IO_POOL_SMALL_BORROW, *capacity);
			return buffer;
		}
	}

	buffer = borrow_slots(large_slots, large_count, &large_rotor);
	if (buffer != NULL) {
		*capacity = KERN_IO_BATCH_MAX;
		io_stats_record(IO_POOL_LARGE_BORROW, *capacity);
		return buffer;
	}

	if (wanted > KERN_IO_SMALL_SIZE) {
		buffer = borrow_slots(small_slots, small_count, &small_rotor);
		if (buffer != NULL) {
			*capacity = KERN_IO_SMALL_SIZE;
			io_stats_record(IO_POOL_SMALL_BORROW, *capacity);
			return buffer;
		}
	}

	io_stats_record(IO_POOL_FALLBACK, wanted);
	return NULL;
}

/*
 * Returns a borrowed slot; an invalid or repeated return violates ownership.
 */
void
io_pool_release(
	void *buffer)
{
	if (buffer == NULL)
		return;
	if (release_slots(large_slots, large_count, buffer)) {
		io_stats_record(IO_POOL_RETURN, KERN_IO_BATCH_MAX);
		return;
	}

	if (release_slots(small_slots, small_count, buffer)) {
		io_stats_record(IO_POOL_RETURN, KERN_IO_SMALL_SIZE);
		return;
	}

	HAL_FATAL("I/O pool return without ownership");
}

/*
 * Reports resident backing separately from the currently borrowed capacity.
 */
void
io_pool_get_stats(
	struct io_pool_stats *stats)
{
	unsigned index;

	/* Reports an empty snapshot before the pool exists. */
	if (stats == NULL)
		return;
	hal_memset(stats, 0, sizeof(*stats));
	if (atomic_raw_load_acquire(&initialized) != 2U)
		return;

	/* Copies the fixed pool geometry. */
	stats->budget_bytes = budget_bytes;
	stats->resident_bytes = resident_bytes;
	stats->large_count = large_count;
	stats->small_count = small_count;

	/* Samples independent slot ownership without stopping borrowers. */
	for (index = 0; index < large_count; index++)
		stats->in_use += atomic_raw_load_acquire(&large_slots[index].borrowed);
	for (index = 0; index < small_count; index++)
		stats->in_use += atomic_raw_load_acquire(&small_slots[index].borrowed);
}

/*
 * Allocates one contiguous scratch region of at least the requested size.
 */
int
io_scratch_alloc(
	size_t size,
	struct io_scratch *result)
{
	struct io_scratch scratch;
	size_t page;
	size_t rounded;
	int error;

	/* Rejects a missing output and empties it before any allocation. */
	if (result == NULL)
		return HAL_ERR_INVALID;
	memset(result, 0, sizeof(*result));

	/* Rounds the request up to whole pages, refusing an unusable size. */
	page = hal_space_get_page_size(1);
	if (size == 0 || page == 0 || size > SIZE_MAX - (page - 1U))
		return HAL_ERR_INVALID;
	rounded = (size + page - 1U) / page * page;

	/* Asks for contiguous physical memory of at least the rounded size. */
	memset(&scratch, 0, sizeof(scratch));
	scratch.physical.size = rounded;
	error = hal_pmem_alloc(rounded, page, &scratch.physical.paddr);
	if (error == HAL_OK &&
	    hal_pmem_to_kernel(scratch.physical.paddr) != NULL) {
		scratch.vaddr = hal_pmem_to_kernel(scratch.physical.paddr);
		scratch.size = scratch.physical.size;
		*result = scratch;
		return HAL_OK;
	}

	/* Returns a short or failed physical allocation. */
	if (scratch.physical.size != 0 &&
	    hal_pmem_free(&scratch.physical.paddr,
			  scratch.physical.size) != HAL_OK)
		HAL_FATAL("scratch physical rollback failed");

	/* Failed. */
	return HAL_ERR_NOMEM;
}

/*
 * Releases one scratch region and empties its description.
 *
 * The scratch owner's pin spans every borrower until its existing drain gate,
 * so a region is freed only after the last borrower is gone.
 */
int
io_scratch_free(
	struct io_scratch *scratch)
{
	int error;

	/* Rejects a description that owns no region. */
	if (scratch == NULL || scratch->vaddr == NULL || scratch->size == 0)
		return HAL_ERR_INVALID;

	/* Returns the contiguous physical memory. */
	error = hal_pmem_free(&scratch->physical.paddr,
			      scratch->physical.size);

	/* Empties the description only once its region is really gone. */
	if (error == HAL_OK)
		memset(scratch, 0, sizeof(*scratch));

	/* Reports why the release failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Records one event without allocating memory or taking an I/O lock.
 *
 * Counters are cumulative since boot and intentionally cannot be reset.
 */
void
io_stats_record(
	enum io_stat_event event,
	uint64_t bytes)
{
	/* Keeps a malformed instrumentation site outside the counter array. */
	if ((unsigned)event >= IO_STAT_COUNT)
		return;

	/* Counts the request and its byte quantity independently. */
	atomic_u64_fetch_add_relaxed(&counters[event].calls, 1);
	atomic_u64_fetch_add_relaxed(&counters[event].bytes, bytes);
}

/*
 * Copies a versioned snapshot of the cumulative event counters.
 *
 * Quiescent before and after samples give exact deltas; a snapshot taken
 * while I/O runs may skew individual fields against each other.
 */
void
io_stats_snapshot(
	struct io_stats *snapshot)
{
	unsigned i;

	/* Permits callers to skip an optional snapshot. */
	if (snapshot == NULL)
		return;

	/* Describes the layout the caller is about to read. */
	snapshot->version = IO_STATS_VERSION;
	snapshot->count = IO_STAT_COUNT;

	/* Reads each field through the architecture's atomic implementation. */
	for (i = 0; i < IO_STAT_COUNT; i++) {
		snapshot->events[i].calls =
		    atomic_u64_load_acquire(&counters[i].calls);
		snapshot->events[i].bytes =
		    atomic_u64_load_acquire(&counters[i].bytes);
	}
}

/* Guards the paired sequence and errno without sleeping or entering another owner. */
static bool
io_error_lock(
	struct io_error_state *state)
{
	bool enabled;
	unsigned expected;

	/* Keeps completion-context writers from interrupting an owner of this guard. */
	enabled = hal_irq_disable();
	expected = 0;
	while (!atomic_compare_exchange(&state->guard, &expected, 1)) {
		expected = 0;
		hal_compiler_barrier();
	}

	/* Reports the interrupt state the unlock has to restore. */
	return enabled;
}

/* Publishes a coherent pair before restoring the caller's interrupt state. */
static void
io_error_unlock(
	struct io_error_state *state,
	bool enabled)
{
	/* Releases the guard, then restores interrupts only when they were enabled. */
	atomic_store_release(&state->guard, 0);
	if (enabled)
		hal_irq_enable();
}

/* Allocates and accounts complete HAL backing, including architecture page rounding. */
static int
allocate_backing(size_t bytes, size_t limit, struct io_scratch *memory)
{
	size_t page;
	size_t rounded;
	int result;

	page = hal_space_get_page_size(1);
	if (page == 0 || bytes > SIZE_MAX - (page - 1U))
		return 0;
	rounded = (bytes + page - 1U) / page * page;
	if (rounded > limit)
		return 0;
	result = io_scratch_alloc(rounded, memory);
	if (result != HAL_OK)
		return 0;
	if (memory->size < rounded || memory->size > limit || memory->vaddr == NULL) {
		if (io_scratch_free(memory) != HAL_OK)
			HAL_FATAL("I/O pool allocation rollback failed");
		return 0;
	}

	/* Accounts persistent scratch independently of borrowers and virtual reservations. */
	if (cache_memory_reserve != NULL) {
		if (cache_memory_reserve(CACHE_MEMORY_IO_POOL, memory->size, 0) != 0) {
			if (io_scratch_free(memory) != HAL_OK)
				HAL_FATAL("I/O pool accounting rollback failed");
			return 0;
		}

		cache_memory_commit(CACHE_MEMORY_IO_POOL, memory->size);
	}

	io_stats_record(IO_POOL_BACKING_ALLOC, memory->size);
	return 1;
}

/* Tries each immutable slot once; the successful CAS transfers ownership. */
static void *
borrow_slots(struct io_pool_slot *slots, unsigned count, unsigned *rotor)
{
	unsigned start;
	unsigned offset;
	unsigned index;
	unsigned expected;

	if (count == 0)
		return NULL;
	start = atomic_raw_fetch_add_relaxed(rotor, 1U) % count;
	for (offset = 0; offset < count; offset++) {
		index = (start + offset) % count;
		expected = 0;
		if (atomic_raw_compare_exchange(&slots[index].borrowed, &expected, 1U))
			return slots[index].backing.vaddr;
	}

	return NULL;
}

/* Publishes a slot only after its caller has finished accessing its bytes. */
static int
release_slots(struct io_pool_slot *slots, unsigned count, void *buffer)
{
	unsigned index;
	unsigned expected;

	for (index = 0; index < count; index++) {
		if (slots[index].backing.vaddr != buffer)
			continue;
		expected = 1;
		if (!atomic_raw_compare_exchange(&slots[index].borrowed, &expected, 0U))
			HAL_FATAL("I/O pool slot returned twice");
		return 1;
	}

	return 0;
}
