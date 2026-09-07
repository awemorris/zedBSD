/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <kern/cache-memory.h>
#include <kern/lock.h>
#include <kern/atomic.h>
#include <hal/hal.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAGE 4096U
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "cache budget failed line %d: %s\n", __LINE__, #x); abort(); } (void)__atomic_add_fetch(&checks, 1U, __ATOMIC_RELAXED); } while (0)
static unsigned checks;
static uint64_t physical_free = 64U * 1024U * 1024U;
static size_t clean_bytes;
static unsigned check_pending_gate;
static unsigned reclaim_calls;

bool hal_irq_disable(void) { return false; }
void hal_irq_enable(void) { }
void hal_fatal(const char *file, int line, const char *message)
{
	fprintf(stderr, "%s:%d %s\n", file, line, message);
	abort();
}
void hal_memory_get_stats(struct hal_memory_stats *stats)
{
	memset(stats, 0, sizeof(*stats));
	stats->physical_total = 64U * 1024U * 1024U;
	stats->physical_free = physical_free;
}
int mutex_init(struct mutex *lock, enum lock_rank rank, const char *name)
{
	memset(lock, 0, sizeof(*lock));
	lock->guard.rank = rank;
	lock->guard.name = name;
	return 0;
}
void mutex_lock(struct mutex *lock)
{
	unsigned expected;

	expected = 0;
	while (!atomic_raw_compare_exchange(&lock->locked, &expected, 1)) {
		expected = 0;
		sched_yield();
	}
}
void mutex_unlock(struct mutex *lock)
{
	atomic_raw_store_release(&lock->locked, 0);
}
size_t vm_object_reclaim_clean(size_t target)
{
	size_t freed;

	reclaim_calls++;
	CHECK(target <= 65536);
	if (check_pending_gate)
		CHECK(cache_memory_reserve(CACHE_MEMORY_FILE_DATA, PAGE, 1) == ENOMEM);
	freed = target < clean_bytes ? target : clean_bytes;
	clean_bytes -= freed;
	cache_memory_release(CACHE_MEMORY_FILE_DATA, freed);
	return freed;
}
size_t buf_reclaim(size_t target, unsigned flags)
{
	(void)target;
	CHECK(flags == 0);
	return 0;
}
static void invariant(void)
{
	struct cache_memory_stats stats;
	uint64_t resident, pending;
	unsigned i;

	cache_memory_get_stats(&stats);
	resident = 0;
	pending = 0;
	for (i = 0; i < CACHE_MEMORY_KINDS; i++) {
		resident += stats.usage[i].resident_bytes;
		pending += stats.usage[i].pending_bytes;
	}
	CHECK(resident == stats.resident_bytes);
	CHECK(pending == stats.pending_bytes);
}
static void *worker(void *arg)
{
	unsigned i;
	enum cache_memory_kind kind;

	kind = (enum cache_memory_kind)(uintptr_t)arg;
	for (i = 0; i < 10000; i++) {
		CHECK(cache_memory_reserve(kind, PAGE, 1) == 0);
		if (i & 1U) {
			cache_memory_commit(kind, PAGE);
			invariant();
			cache_memory_release(kind, PAGE);
		} else {
			cache_memory_cancel(kind, PAGE);
		}
	}
	return NULL;
}
int main(void)
{
	static const uint64_t sizes[] = { 8U * 1024U * 1024U, 16U * 1024U * 1024U,
	    256U * 1024U * 1024U, UINT64_C(1) << 30, UINT64_C(16) << 30,
	    UINT64_C(64) << 40 };
	struct cache_memory_stats stats;
	pthread_t threads[4];
	uint64_t target, reserve, old;
	unsigned i;

	for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
		cache_memory_policy(sizes[i], &target, &reserve);
		CHECK(target == sizes[i] / 4U);
		CHECK(reserve >= 65536 && reserve <= 8U * 1024U * 1024U);
		CHECK(reserve <= sizes[i] / 4U);
	}
	CHECK(cache_memory_set_target(4096) == EINVAL);
	cache_memory_init();
	cache_memory_get_stats(&stats);
	CHECK(stats.version == CACHE_MEMORY_VERSION && stats.count == CACHE_MEMORY_KINDS);
	old = stats.target_bytes;
	CHECK(cache_memory_reserve(CACHE_MEMORY_FILE_DATA, (size_t)old, 1) == 0);
	CHECK(cache_memory_reserve(CACHE_MEMORY_BUF_META, PAGE, 1) == ENOMEM);
	cache_memory_cancel(CACHE_MEMORY_FILE_DATA, (size_t)old);
	CHECK(cache_memory_reserve(CACHE_MEMORY_DMA, (size_t)old + PAGE, 0) == 0);
	cache_memory_commit(CACHE_MEMORY_DMA, (size_t)old + PAGE);
	CHECK(cache_memory_reserve(CACHE_MEMORY_FILE_DATA, PAGE, 1) == ENOMEM);
	CHECK(cache_memory_set_target(old / 2) == EBUSY);
	cache_memory_get_stats(&stats);
	CHECK(stats.target_bytes == old && !stats.resizing);
	cache_memory_release(CACHE_MEMORY_DMA, (size_t)old + PAGE);

	physical_free = stats.reserve_bytes;
	CHECK(cache_memory_reserve(CACHE_MEMORY_FILE_DATA, PAGE, 1) == ENOMEM);
	physical_free = 64U * 1024U * 1024U;
	CHECK(cache_memory_reserve(CACHE_MEMORY_FILE_DATA, 4U * PAGE, 1) == 0);
	cache_memory_commit(CACHE_MEMORY_FILE_DATA, 4U * PAGE);
	clean_bytes = 4U * PAGE;
	check_pending_gate = 1;
	CHECK(cache_memory_set_target(0) == 0);
	check_pending_gate = 0;
	cache_memory_get_stats(&stats);
	CHECK(stats.target_bytes == 0 && stats.resident_bytes == 0);
	CHECK(stats.reclaimed_bytes == 4U * PAGE);
	CHECK(cache_memory_reserve(CACHE_MEMORY_FILE_DATA, PAGE, 1) == ENOMEM);
	CHECK(cache_memory_set_target(old) == 0);
	CHECK(cache_memory_set_target(1) == EINVAL);
	CHECK(cache_memory_set_target(UINT64_MAX & ~(uint64_t)(PAGE - 1)) == EINVAL);

	CHECK(cache_memory_reserve(CACHE_MEMORY_FILE_META, PAGE, 0) == 0);
	CHECK(cache_memory_reserve(CACHE_MEMORY_DMA, SIZE_MAX, 0) == ENOMEM);
	cache_memory_cancel(CACHE_MEMORY_FILE_META, PAGE);
	for (i = 0; i < 4; i++)
		CHECK(pthread_create(&threads[i], NULL, worker, (void *)(uintptr_t)i) == 0);
	for (i = 0; i < 4; i++)
		CHECK(pthread_join(threads[i], NULL) == 0);
	invariant();
	cache_memory_get_stats(&stats);
	CHECK(stats.pending_bytes == 0 && stats.resident_bytes == 0);
	CHECK(reclaim_calls != 0);
	printf("cache memory accounting PASS (%u checks)\n", checks);
	return 0;
}
