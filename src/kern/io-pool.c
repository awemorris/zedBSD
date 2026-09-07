/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

/*
 * Bounded shared scratch storage, owned for the kernel's lifetime.
 */
#include <kern/cache-memory.h>
#include <kern/io-scratch.h>
#include <hal/hal.h>
#include <kern/io-pool.h>
#include <kern/atomic.h>
#include <kern/io-stats.h>

extern int cache_memory_reserve(enum cache_memory_kind, size_t, int) __attribute__((weak));
extern void cache_memory_commit(enum cache_memory_kind, size_t) __attribute__((weak));
extern void cache_memory_cancel(enum cache_memory_kind, size_t) __attribute__((weak));
extern void cache_memory_release(enum cache_memory_kind, size_t) __attribute__((weak));
extern size_t cache_memory_reclaim(size_t) __attribute__((weak));

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

static int allocate_backing(size_t bytes, size_t limit, int allow_vmap, struct io_scratch *memory);
static void *borrow_slots(struct io_pool_slot *slots, unsigned count, unsigned *rotor);
static int release_slots(struct io_pool_slot *slots, unsigned count, void *buffer);

/*
 * Allocates the shared reserve once, before the first executable is loaded.
 * Allocation failure reduces capacity; callers retain their stack fallback.
 */
void
io_pool_init(void)
{
	struct hal_memory_stats memory;
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
	hal_memory_get_stats(&memory);
	budget_bytes = memory.physical_total / 64U;
	if (budget_bytes > IO_POOL_MAX_BYTES)
		budget_bytes = IO_POOL_MAX_BYTES;
	cpus = hal_cpu_count();
	wanted = cpus > IO_POOL_MAX_SMALL / 2U ? IO_POOL_MAX_SMALL : (size_t)cpus * 2U;
	if (wanted < 16U)
		wanted = 16U;
	wanted_large = wanted > IO_POOL_MAX_LARGE ? IO_POOL_MAX_LARGE : wanted;
	wanted_small = wanted;

	/* Charges the complete, page-rounded descriptor table to the same budget. */
	if (!allocate_backing((wanted_large + wanted_small) * sizeof(*large_slots),
	    budget_bytes, 0, &metadata)) {
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
		if (!allocate_backing(KERN_IO_SMALL_SIZE, small_limit - small_bytes, 0,
		    &small_slots[small_count].backing))
			break;
		small_bytes += small_slots[small_count].backing.size;
		resident_bytes += small_slots[small_count].backing.size;
		small_count++;
	}

	/* Allocates large slots during construction, with optional owned vmap fallback. */
	while (large_count < wanted_large) {
		if (!allocate_backing(KERN_IO_BATCH_MAX, budget_bytes - resident_bytes, 1,
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

	if (stats == NULL)
		return;
	hal_memset(stats, 0, sizeof(*stats));
	if (atomic_raw_load_acquire(&initialized) != 2U)
		return;
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

/* Allocates and accounts complete HAL backing, including architecture page rounding. */
static int
allocate_backing(size_t bytes, size_t limit, int allow_vmap, struct io_scratch *memory)
{
	size_t page;
	size_t rounded;
	int result;

	page = hal_page_get_page_size(1);
	if (page == 0 || bytes > SIZE_MAX - (page - 1U))
		return 0;
	rounded = (bytes + page - 1U) / page * page;
	if (rounded > limit)
		return 0;
	result = io_scratch_alloc(rounded, allow_vmap, memory);
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
