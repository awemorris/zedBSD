/* -*- mode: c; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */

/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

/* Reuses maintained host services with real file, VM and claim production code. */
#define ZEDBSD_FILE_CACHE_HOST
#define waitq_sleep reservation_waitq_sleep
#define waitq_sequence reservation_waitq_sequence
#define waitq_wake_all reservation_waitq_wake_all
#define hal_pmem_alloc reservation_pmem_alloc
#define hal_pmem_free reservation_pmem_free
#define main reservation_main
#include "plan/ws019-installation/tests/format-reservation-test.c"
#undef main
#undef hal_pmem_alloc
#undef hal_pmem_free
#undef waitq_sleep
#undef waitq_sequence
#undef waitq_wake_all
#include <time.h>
#include <kern/cache-memory.h>
#include <kern/writeback.h>
#include <kern/page.h>
#include <kern/filedesc.h>

void poll_notify(void) {}
void record_lock_release_process_inode(struct process *process,struct inode *inode)
{ (void)process;(void)inode; }
static struct filedesc *read_table;
static uint64_t confirmed_prefetch, unused_prefetch;
void readahead_consumed(size_t useful,size_t unused)
{
 __atomic_add_fetch(&confirmed_prefetch,useful,__ATOMIC_RELAXED);
 __atomic_add_fetch(&unused_prefetch,unused,__ATOMIC_RELAXED);
}
static void test_prefetch_feedback(void);
static __thread unsigned read_priority;
static unsigned read_priority_total;
static unsigned read_capture, read_submissions, read_cancellations;
static struct file *read_invalidate_on_end;
static struct readahead_request read_candidate;
static void test_read_observation(void);
int readahead_demand_begin(void)
{
 read_priority++;__atomic_add_fetch(&read_priority_total,1,__ATOMIC_RELAXED);return 0;
}
void readahead_demand_end(void)
{
 struct file *file;
 CHECK(read_priority>0);read_priority--;
 CHECK(__atomic_fetch_sub(&read_priority_total,1,__ATOMIC_RELAXED)>0);
 if(read_capture && read_invalidate_on_end!=NULL) {
  file=read_invalidate_on_end;read_invalidate_on_end=NULL;file_readahead_invalidate(file);
 }
}
void readahead_cancel(struct file *file)
{
 (void)file;
 if(read_capture) {
  if(read_table!=NULL)CHECK(read_table->lock.held.value==0);
  read_cancellations++;
 }
}
int readahead_submit(struct file *file,struct inode *inode,const struct readahead_request *request)
{
 if(read_capture) {
  CHECK(read_priority==0 && file->f_lock.locked==0 && inode->i_io_lock.locked==0);
  CHECK(readahead_current(&file->f_readahead,request));
  read_submissions++;read_candidate=*request;
 }
 return EAGAIN;
}

static pthread_mutex_t cached_gate_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cached_gate_changed=PTHREAD_COND_INITIALIZER;
static unsigned cached_backend_arm,cached_backend_paused,cached_backend_release,cached_demand_done;
static void test_cached_demand_progress(void);
static unsigned cache_pmem_count;
static unsigned cache_pmem_failure;
static size_t cache_live_bytes;
static unsigned cache_backend_reads;
static int cache_read_error;
static size_t cache_short_limit;
static unsigned cache_generated;
static int cache_disk_error;
static unsigned cache_disk_users;
static unsigned char cache_pool[64U * 1024U];
static unsigned cache_pool_used;
static unsigned cache_pool_disabled;
static struct file_ops cache_ops;

static ssize_t cache_pread(struct file *file, void *buffer, size_t size, off_t offset);
static void test_cache_lifetime(void);
static void test_cache_pressure(void);
static void test_file_error_observers(void);
static void test_exec_input_leases(void);
static void test_exec_snapshot_pages(void);
static void test_dirty_orphan_index(void);
static void test_claim_references(void);
static void test_dirty_credits(void);
static void test_delayed_content(void);
static void test_writeback_batch(void);
static void test_writeback_redirty(void);
static void test_prefetch_publication(void);
static void test_writeback_64k(void);
static unsigned synchronous_barriers;
static int synchronous_backend(struct file *file)
{
	synchronous_barriers++;
	return backend_fsync(file);
}
static void test_writeback_file_io(void);
static ssize_t batch_pwrite(struct file *, const void *, size_t, off_t);
static void test_cache_failures(void);
static void test_cache_window(void);
static void test_cache_budget(void);
static void test_cache_resize(void);
static void *cache_writer(void *argument);
static void *cache_mapper(void *argument);
static void test_cache_dirty_retention(void);
static void *cache_racer(void *argument);
static void test_cache_races(void);

#ifndef FILE_CACHE_MAIN
#define FILE_CACHE_MAIN main
#endif
int
FILE_CACHE_MAIN(void)
{
	struct cache_memory_stats stats;

	cache_memory_init();
	CHECK(reservation_main() == 0);
	test_cache_lifetime();
	test_cache_races();
	test_cache_failures();
	test_cache_window();
	test_cache_budget();
	test_cache_resize();
	test_cache_dirty_retention();
	test_cache_pressure();
	test_file_error_observers();
	test_exec_input_leases();
	test_exec_snapshot_pages();
	test_dirty_orphan_index();
	test_claim_references();
	test_dirty_credits();
	test_delayed_content();
	test_writeback_batch();
	test_writeback_64k();
	test_writeback_file_io();
	test_writeback_redirty();
	test_prefetch_publication();
	test_read_observation();
	test_prefetch_feedback();
	test_cached_demand_progress();
	CHECK(read_priority_total==0);
	CHECK(file_count() == 0);
	CHECK(vm_object_count() == 0);
	CHECK(vm_object_page_count() == 0);
	cache_memory_get_stats(&stats);
	CHECK(stats.pending_bytes == 0 && stats.resident_bytes == 0);
	CHECK(cache_live_bytes == 0);
	printf("WS025 file cache lifetime: PASS (%u checks)\n", checks);
	return 0;
}

/* Counts actual backend calls without counting cache copies. */
static ssize_t
cache_pread(
	struct file *file,
	void *buffer,
	size_t size,
	off_t offset)
{
	size_t index;

	(void)__atomic_add_fetch(&cache_backend_reads, 1U, __ATOMIC_RELAXED);
	if (__atomic_exchange_n(&cached_backend_arm,0,__ATOMIC_ACQ_REL)) {
		pthread_mutex_lock(&cached_gate_lock);cached_backend_paused=1;
		pthread_cond_broadcast(&cached_gate_changed);
		while(!cached_backend_release)pthread_cond_wait(&cached_gate_changed,&cached_gate_lock);
		pthread_mutex_unlock(&cached_gate_lock);
	}
	if (cache_read_error != 0)
		return -cache_read_error;
	if (cache_short_limit != 0 && size > cache_short_limit)
		size = cache_short_limit;
	if (cache_generated) {
		for (index = 0; index < size; index++)
			((unsigned char *)buffer)[index] = (unsigned char)((offset + index) % 251U);
		return (ssize_t)size;
	}
	return backend_pread(file, buffer, size, offset);
}

/* Keeps clean pages across last close, then releases them for real ownership. */
static void
test_cache_lifetime(
	void)
{
	struct fixture fixture;
	struct path path;
	struct vm_object *object;
	struct vm_object_page *page;
	unsigned char observed;
	unsigned char run[FILE_BYTES];
	unsigned before;

	make_fixture(&fixture);
	cache_ops = backend_ops;
	cache_ops.pread = cache_pread;
	fixture.inode.i_fop = &cache_ops;
	fixture.owner->f_ops = &cache_ops;
	cache_backend_reads = 0;
	vm_object_cache_prepare(fixture.owner);
	CHECK(vm_object_count() == 1);
	CHECK(file_pread(fixture.owner, run, sizeof(run), 0) == sizeof(run));
	CHECK(memcmp(run, fixture.bytes, sizeof(run)) == 0);
	CHECK(vm_object_page_count() == FILE_BYTES / 4096U);
	CHECK(file_pread(fixture.owner, &observed, 1, 13) == 1);
	CHECK(observed == 0xa5);
	CHECK(cache_backend_reads == 1);
	CHECK(file_close(fixture.owner) == 0);
	fixture.owner = NULL;
	CHECK(file_count() == 2);
	CHECK(vm_object_count() == 1);

	/* Reopen the same inode and require no backend read for the warm byte. */
	path.p_mount = &fixture.mount;
	path.p_inode = &fixture.inode;
	CHECK(file_open_resolved(&path, O_RDWR | O_NOFOLLOW, &fixture.owner) == 0);
	before = cache_backend_reads;
	CHECK(file_pread(fixture.owner, &observed, 1, 13) == 1);
	CHECK(observed == 0xa5);
	CHECK(cache_backend_reads == before);

	/* A mapping and then a fault hold prevent optional eviction. */
	CHECK(vm_object_get_shared(fixture.owner, &object) == 0);
	CHECK(vm_object_cache_drain(&fixture.mount) == 0);
	CHECK(vm_object_fault(object, 0, &page) == 0);
	CHECK(vm_object_page_pin(page) == 0);
	vm_object_fault_release(page);
	CHECK(vm_object_cache_drain(NULL) == 0);
	vm_object_page_unpin(page);
	vm_object_put(object);
	CHECK(vm_object_count() == 1);
	CHECK(vm_object_retained_count() == 0);

	/* A normal write updates the retained content domain. */
	observed = 0x72;
	CHECK(file_pwrite(fixture.owner, &observed, 1, 13) == 1);
	observed = 0;
	CHECK(file_pread(fixture.owner, &observed, 1, 13) == 1);
	CHECK(observed == 0x72);
	CHECK(cache_backend_reads == before);

	/* Formatting retires clean optional ownership, then excludes readmission. */
	CHECK(file_format_reserve(fixture.owner, FILE_BYTES) == 0);
	CHECK(vm_object_count() == 0);
	vm_object_cache_prepare(fixture.foreign);
	CHECK(vm_object_count() == 0);
	close_fixture(&fixture);
}

/* Races independent operation admission against bounded idle detach. */
static void *
cache_racer(
	void *argument)
{
	struct file *file;
	unsigned i;
	unsigned char byte;

	file = argument;
	for (i = 0; i < 2000; i++) {
		vm_object_cache_prepare(file);
		CHECK(file_pread(file, &byte, 1, 0) == 1);
		CHECK(byte == 0xa5 || byte == 0x55);
		(void)vm_object_cache_drain(NULL);
	}
	return NULL;
}

/* Verifies that registry removal cannot free a concurrent operation's object. */
static void
test_cache_races(
	void)
{
	struct fixture fixture;
	pthread_t threads[4];
	pthread_t writer;
	pthread_t mapper;
	unsigned i;

	make_fixture(&fixture);
	CHECK(pthread_create(&writer, NULL, cache_writer, fixture.owner) == 0);
	CHECK(pthread_create(&mapper, NULL, cache_mapper, fixture.owner) == 0);
	for (i = 0; i < 4; i++)
		CHECK(pthread_create(&threads[i], NULL, cache_racer, fixture.owner) == 0);
	for (i = 0; i < 4; i++)
		CHECK(pthread_join(threads[i], NULL) == 0);
	CHECK(pthread_join(writer, NULL) == 0);
	CHECK(pthread_join(mapper, NULL) == 0);
	CHECK(cache_disk_users == 0);
	(void)vm_object_cache_drain(NULL);
	close_fixture(&fixture);
}

/* Supplies the nonblocking pool boundary used by production populate. */
void *
io_pool_borrow(size_t wanted, size_t *capacity)
{
	unsigned expected;

	expected = 0;
	if (cache_pool_disabled || wanted > sizeof(cache_pool) ||
	    !__atomic_compare_exchange_n(&cache_pool_used, &expected, 1, 0,
	    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
		return NULL;
	*capacity = sizeof(cache_pool);
	return cache_pool;
}

/* Returns the borrowed run after all private copies have been published. */
void
io_pool_release(void *buffer)
{
	CHECK(buffer == cache_pool);
	CHECK(__atomic_exchange_n(&cache_pool_used, 0, __ATOMIC_RELEASE) == 1);
}

/* Models the production lifecycle token independently of backend read counts. */
int
disk_cache_acquire(struct disk *disk, struct disk **leaf)
{
	*leaf = NULL;
	if (cache_disk_error != 0)
		return cache_disk_error;
	(void)__atomic_add_fetch(&cache_disk_users, 1U, __ATOMIC_RELAXED);
	*leaf = disk;
	return 0;
}

/* Requires all success, error and EOF exits to retire their token. */
void
disk_cache_release(struct disk *disk)
{
	CHECK(disk != NULL);
	CHECK(__atomic_fetch_sub(&cache_disk_users, 1U, __ATOMIC_RELAXED) != 0);
}

/* Exercises failed fills, partial completion, missing scratch, EOF and detach. */
static void
test_cache_failures(void)
{
	struct fixture fixture;
	unsigned char bytes[FILE_BYTES];
	unsigned before;
	unsigned fault;

	for (fault = 0; fault < 8; fault++) {
		make_fixture(&fixture);
		fixture.inode.i_fop = &cache_ops;
		fixture.owner->f_ops = &cache_ops;
		vm_object_cache_prepare(fixture.owner);
		cache_backend_reads = 0;
		cache_read_error = fault == 0 ? EIO : 0;
		cache_short_limit = fault == 1 ? 5000 : (fault == 7 ? 50 : 0);
		cache_pool_disabled = fault == 2;
		if (fault >= 3 && fault < 7)
			cache_pmem_failure = cache_pmem_count + fault - 2;
		memset(bytes, 0, sizeof(bytes));
		if (fault == 7) {
			CHECK(file_pread(fixture.owner, bytes, 1, 100) == -EIO);
		} else if (fault == 0) {
			CHECK(file_pread(fixture.owner, bytes, sizeof(bytes), 0) == -EIO);
		} else if (fault == 1) {
			CHECK(file_pread(fixture.owner, bytes, sizeof(bytes), 0) == 5000);
			CHECK(memcmp(bytes, fixture.bytes, 5000) == 0);
			CHECK(bytes[5000] == 0);
		} else {
			CHECK(file_pread(fixture.owner, bytes, sizeof(bytes), 0) == sizeof(bytes));
			CHECK(memcmp(bytes, fixture.bytes, sizeof(bytes)) == 0);
		}
		CHECK(cache_backend_reads == 1);
		CHECK(cache_disk_users == 0);
		CHECK(cache_pool_used == 0);
		cache_read_error = 0;
		cache_short_limit = 0;
		cache_pool_disabled = 0;
		cache_pmem_failure = 0;
		CHECK(file_pread(fixture.owner, bytes, sizeof(bytes), 0) == sizeof(bytes));
		CHECK(memcmp(bytes, fixture.bytes, sizeof(bytes)) == 0);
		before = cache_backend_reads;
		CHECK(file_pread(fixture.owner, bytes, 1, FILE_BYTES) == 0);
		CHECK(cache_backend_reads == before);
		cache_disk_error = ENXIO;
		CHECK(file_pread(fixture.owner, bytes, 1, 0) == -ENXIO);
		CHECK(cache_backend_reads == before);
		CHECK(cache_disk_users == 0);
		cache_disk_error = 0;
		(void)vm_object_cache_drain(NULL);
		CHECK(vm_object_page_count() == 0);
		close_fixture(&fixture);
	}
}

/* Retains a growing sequential working set with bounded backend transfers. */
static void
test_cache_window(void)
{
	struct fixture fixture;
	unsigned char bytes[65536];
	unsigned run;
	unsigned index;
	unsigned before;

	make_fixture(&fixture);
	fixture.inode.i_fop = &cache_ops;
	fixture.owner->f_ops = &cache_ops;
	fixture.inode.i_size = 4U * sizeof(bytes);
	cache_generated = 1;
	cache_backend_reads = 0;
	for (run = 0; run < 4; run++) {
		CHECK(file_pread(fixture.owner, bytes, sizeof(bytes), run * sizeof(bytes)) == sizeof(bytes));
		CHECK(cache_backend_reads == run + 1);
		CHECK(vm_object_page_count() == 16 * (run + 1));
		for (index = 0; index < sizeof(bytes); index++)
			CHECK(bytes[index] == (unsigned char)((run * sizeof(bytes) + index) % 251U));
	}
	before = cache_backend_reads;
	CHECK(file_pread(fixture.owner, bytes, sizeof(bytes), 3U * sizeof(bytes)) == sizeof(bytes));
	CHECK(cache_backend_reads == before);
	(void)vm_object_cache_drain(NULL);
	CHECK(vm_object_page_count() == 0);
	cache_generated = 0;
	close_fixture(&fixture);
}

/* Bounds last-close retention independently of the number of files touched. */
static void
test_cache_budget(void)
{
	struct fixture fixtures[40];
	unsigned char byte;
	unsigned i;

	for (i = 0; i < 40; i++) {
		make_fixture(&fixtures[i]);
		CHECK(file_pread(fixtures[i].owner, &byte, 1, 0) == 1);
		CHECK(byte == 0xa5);
		CHECK(file_close(fixtures[i].owner) == 0);
		fixtures[i].owner = NULL;
		CHECK(file_close(fixtures[i].foreign) == 0);
		fixtures[i].foreign = NULL;
		CHECK(vm_object_count() <= 32);
		CHECK(vm_object_page_count() <= 32);
		CHECK(file_count() <= 32);
	}
	CHECK(vm_object_count() == 32);
	CHECK(vm_object_cache_drain(NULL) == 32);
	for (i = 0; i < 40; i++)
		close_fixture(&fixtures[i]);
}

/* Applies the production EOF transaction while retaining a populated object. */
static void
test_cache_resize(void)
{
	struct fixture fixture;
	struct vm_object_resize resize;
	unsigned char bytes[FILE_BYTES];

	make_fixture(&fixture);
	CHECK(file_pread(fixture.owner, bytes, sizeof(bytes), 0) == sizeof(bytes));
	mutex_lock(&fixture.inode.i_io_lock);
	CHECK(vm_object_resize_begin(&fixture.inode, 5000, &resize) == 0);
	mutex_unlock(&fixture.inode.i_io_lock);
	CHECK(vm_object_resize_prepare(&resize) == 0);
	mutex_lock(&fixture.inode.i_io_lock);
	fixture.inode.i_size = 5000;
	vm_object_resize_commit(&resize, 5000);
	mutex_unlock(&fixture.inode.i_io_lock);
	/* Resize revokes the partial tail too; the next read repopulates it. */
	CHECK(vm_object_page_count() == 1);
	CHECK(file_pread(fixture.owner, bytes, sizeof(bytes), 0) == 5000);
	CHECK(memcmp(bytes, fixture.bytes, 5000) == 0);
	CHECK(file_pread(fixture.owner, bytes, 1, 5000) == 0);
	(void)vm_object_cache_drain(NULL);
	close_fixture(&fixture);
}

/* Races write-through updates against shared read leases and cache retirement. */
static void *
cache_writer(void *argument)
{
	struct file *file;
	unsigned i;
	unsigned char byte;

	file = argument;
	for (i = 0; i < 1000; i++) {
		byte = (i & 1U) ? 0xa5 : 0x55;
		CHECK(file_pwrite(file, &byte, 1, 0) == 1);
	}
	return NULL;
}

/* Uses atomic sequence publication for real concurrent waits in this fixture. */
uint64_t
waitq_sequence(const struct wait_queue *queue)
{
	return __atomic_load_n(&queue->sequence, __ATOMIC_ACQUIRE);
}

/* Publishes a completed condition transition without losing an early wake. */
void
waitq_wake_all(struct wait_queue *queue)
{
	(void)__atomic_add_fetch(&queue->sequence, UINT64_C(1), __ATOMIC_RELEASE);
}

/* Drops the condition lock while waiting, with a bounded deadlock diagnostic. */
int
waitq_sleep(struct wait_queue *queue, struct spinlock *lock,
    uint64_t observed, uint64_t deadline, unsigned flags)
{
	struct timespec start, now;
	unsigned spins;

	(void)deadline;
	(void)flags;
	if (waitq_sequence(queue) != observed)
		return EAGAIN;
	CHECK(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
	if (lock != NULL)
		spin_unlock_irqrestore(lock, 0);
	spins = 0;
	while (waitq_sequence(queue) == observed) {
		sched_yield();
		if ((++spins & 4095U) == 0) {
			CHECK(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
			if (now.tv_sec > start.tv_sec + 5) {
				fprintf(stderr, "cache wait timeout: %s\n", queue->name);
				abort();
			}
		}
	}
	if (lock != NULL)
		(void)spin_lock_irqsave(lock);
	return EAGAIN;
}

/* Keeps failed shared writeback distinct from disposable clean retention. */
static void
test_cache_dirty_retention(void)
{
	struct fixture fixture;
	struct vm_object *object;
	struct vm_object_page *page;
	unsigned char byte;

	make_fixture(&fixture);
	CHECK(file_pread(fixture.owner, &byte, 1, 0) == 1);
	CHECK(vm_object_get_shared(fixture.owner, &object) == 0);
	CHECK(vm_object_fault(object, 0, &page) == 0);
	((unsigned char *)page->pmem.vaddr)[0] = 0x42;
	vm_object_mark_dirty(page);
	vm_object_fault_release(page);
	backend_sync_error = EIO;
	vm_object_put(object);
	CHECK(vm_object_retained_count() == 1);
	CHECK(vm_object_cache_drain(NULL) == 0);
	CHECK(file_format_reserve(fixture.owner, FILE_BYTES) == EBUSY);
	backend_sync_error = 0;
	CHECK(vm_object_sync_inode(&fixture.inode) == 0);
	CHECK(fixture.bytes[0] == 0x42);
	CHECK(vm_object_retained_count() == 0);
	CHECK(vm_object_count() == 1);
	CHECK(vm_object_cache_drain(NULL) == 1);
	close_fixture(&fixture);
}

/* Races final region ownership against ordinary readers, writers and cache detach. */
static void *
cache_mapper(void *argument)
{
	struct file *file;
	struct vm_object *object;
	struct vm_object_page *page;
	unsigned i;
	int error;

	file = argument;
	for (i = 0; i < 100; i++) {
		CHECK(vm_object_get_shared(file, &object) == 0);
		do {
			error = vm_object_fault(object, 0, &page);
		} while (error == EAGAIN);
		CHECK(error == 0);
		vm_object_fault_release(page);
		vm_object_put(object);
	}
	return NULL;
}

/* Accounts physical ownership independently from the manager under test. */
int hal_pmem_alloc(const struct hal_pmem_request *request, struct hal_pmem *memory)
{
	unsigned call;
	int error;

	call = __atomic_add_fetch(&cache_pmem_count, 1U, __ATOMIC_RELAXED);
	if (cache_pmem_failure != 0 && call == cache_pmem_failure)
		return HAL_ERR_NOMEM;
	error = reservation_pmem_alloc(request, memory);
	if (error == HAL_OK)
		(void)__atomic_add_fetch(&cache_live_bytes, memory->size, __ATOMIC_RELAXED);
	return error;
}
int hal_pmem_free(struct hal_pmem *memory)
{
	size_t size = memory->size;
	int error = reservation_pmem_free(memory);
	if (error == HAL_OK)
		(void)__atomic_sub_fetch(&cache_live_bytes, size, __ATOMIC_RELAXED);
	return error;
}
void hal_memory_get_stats(struct hal_memory_stats *stats)
{
	memset(stats, 0, sizeof(*stats));
	stats->physical_total = 64U * 1024U * 1024U;
	stats->physical_free = stats->physical_total -
	    __atomic_load_n(&cache_live_bytes, __ATOMIC_RELAXED);
}

/* Shrinks real retained pages, preserving mappings and a zero-target fallback. */
static void test_cache_pressure(void)
{
	struct fixture fixture;
	struct cache_memory_stats stats;
	struct vm_object *object;
	struct vm_object_page *page;
	unsigned char bytes[65536];
	unsigned index, before;

	make_fixture(&fixture);
	fixture.inode.i_fop = &cache_ops;
	fixture.owner->f_ops = &cache_ops;
	fixture.inode.i_size = 4U * sizeof(bytes);
	cache_generated = 1;
	CHECK(cache_memory_set_target(1024U * 1024U) == 0);
	for (index = 0; index < 4; index++)
		CHECK(file_pread(fixture.owner, bytes, sizeof(bytes), index * sizeof(bytes)) == sizeof(bytes));
	cache_memory_get_stats(&stats);
	CHECK(stats.resident_bytes == cache_live_bytes && stats.pending_bytes == 0);
	CHECK(stats.usage[CACHE_MEMORY_FILE_DATA].resident_bytes == 262144);
	CHECK(stats.usage[CACHE_MEMORY_FILE_META].resident_bytes > 4096);
	CHECK(stats.usage[CACHE_MEMORY_FILE_META].resident_bytes < 32768);
	before = cache_backend_reads;
	CHECK(file_pread(fixture.owner, bytes, sizeof(bytes), 0) == sizeof(bytes));
	CHECK(cache_backend_reads == before);
	CHECK(vm_object_get_shared(fixture.owner, &object) == 0);
	CHECK(vm_object_fault(object, 0, &page) == 0);
	CHECK(cache_memory_set_target(0) == EBUSY);
	CHECK(cache_backend_reads == before);
	vm_object_fault_release(page);
	vm_object_put(object);
	CHECK(cache_memory_set_target(0) == 0);
	CHECK(cache_backend_reads == before);
	cache_memory_get_stats(&stats);
	CHECK(stats.resident_bytes == 0 && stats.pending_bytes == 0);
	CHECK(cache_live_bytes == 0);
	CHECK(file_pread(fixture.owner, bytes, sizeof(bytes), 0) == sizeof(bytes));
	CHECK(cache_backend_reads == before + 1 && vm_object_page_count() == 0);
	for (index = 0; index < sizeof(bytes); index++)
		CHECK(bytes[index] == (unsigned char)(index % 251U));
	(void)vm_object_cache_drain(NULL);
	cache_generated = 0;
	close_fixture(&fixture);
	CHECK(cache_memory_set_target(16U * 1024U * 1024U) == 0);
}

bool hal_irq_disable(void) { return false; }
void hal_irq_enable(void) { }

/* Exercises actual descriptor cursors across independent opens and shared refs. */
static void test_file_error_observers(void)
{
	struct fixture fixture;
	struct path path;
	struct file *second;
	struct file *duplicate;
	struct vm_object *object;
	struct vm_object_page *page;
	struct io_error_snapshot failure;
	unsigned char byte;

	make_fixture(&fixture);
	path.p_mount = &fixture.mount;
	path.p_inode = &fixture.inode;
	CHECK(file_open_resolved(&path, O_RDWR | O_NOFOLLOW, &second) == 0);
	backend_sync_error = EIO;
	CHECK(file_fsync(fixture.owner) == EIO);
	backend_sync_error = 0;
	CHECK(file_fsync(fixture.owner) == 0);
	CHECK(file_fsync(second) == EIO);
	CHECK(file_fsync(second) == 0);

	/* dup shares the same description and therefore the same error observation. */
	duplicate = second;
	file_ref(duplicate);
	io_error_record(&fixture.inode.i_write_error, ENOSPC);
	CHECK(file_fsync(second) == ENOSPC);
	CHECK(file_fsync(duplicate) == 0);
	CHECK(file_close(duplicate) == 0);
	CHECK(file_close(second) == 0);

	/* A fresh observer can still see an owner failure after earlier closes. */
	CHECK(file_open_resolved(&path, O_RDWR | O_NOFOLLOW, &second) == 0);
	CHECK(file_fsync(second) == ENOSPC);
	CHECK(file_fsync(second) == 0);
	CHECK(file_fsync(fixture.owner) == ENOSPC);

	/* Repeated dirty retries fail even after this fd acknowledged the old error. */
	CHECK(file_pread(fixture.owner, &byte, 1, 0) == 1);
	CHECK(vm_object_get_shared(fixture.owner, &object) == 0);
	CHECK(vm_object_fault(object, 0, &page) == 0);
	((unsigned char *)page->pmem.vaddr)[0] = 0x77;
	vm_object_mark_dirty(page);
	vm_object_fault_release(page);
	backend_sync_error = EIO;
	CHECK(file_fsync(fixture.owner) == EIO);
	CHECK(file_fsync(fixture.owner) == EIO);
	backend_sync_error = 0;
	CHECK(file_fsync(fixture.owner) == 0);
	CHECK(fixture.bytes[0] == 0x77);
	CHECK(file_fsync(second) == EIO);
	CHECK(file_fsync(second) == 0);

	/* File observations do not consume the mount observer's notification. */
	io_error_snapshot(&fixture.mount.m_write_error, &failure);
	CHECK(io_error_observe(&failure, &fixture.mount.m_write_error_cursor) == EIO);
	CHECK(io_error_observe(&failure, &fixture.mount.m_write_error_cursor) == 0);
	/* A recovered shared metadata failure belongs to every independent observer. */
	io_error_record(&fixture.mount.m_metadata_error, EIO);
	CHECK(file_fsync(fixture.owner) == EIO);
	CHECK(file_fsync(fixture.owner) == 0);
	duplicate = second;
	file_ref(duplicate);
	CHECK(file_fsync(second) == EIO);
	CHECK(file_fsync(duplicate) == 0);
	CHECK(file_close(duplicate) == 0);
	io_error_record(&fixture.mount.m_metadata_error, ENOSPC);
	CHECK(file_fsync(second) == ENOSPC);
	CHECK(file_fsync(fixture.owner) == ENOSPC);
	CHECK(file_fsync(fixture.owner) == 0);
	vm_object_put(object);
	CHECK(vm_object_retained_count() == 0);
	(void)vm_object_cache_drain(NULL);
	CHECK(file_close(second) == 0);
	close_fixture(&fixture);
}

/* Immutable input readers share cache without revoking each other's contents. */
static void test_exec_input_leases(void)
{
	struct fixture fixture;
	struct disk disk = {0};
	struct file_content_lease first, second;
	struct cache_memory_stats previous;
	unsigned char bytes[8192];
	unsigned before;
	uint64_t generation;
	off_t size;

	make_fixture(&fixture);
	cache_ops = backend_ops;
	cache_ops.pread = cache_pread;
	fixture.inode.i_fop = fixture.owner->f_ops = &cache_ops;
	fixture.mount.m_disk = &disk;
	fixture.mount.m_flags |= MOUNT_READ_ONLY;
	disk.d_flags = DISK_READ_ONLY;
	generation = fixture.inode.i_vm_content_generation;
	CHECK(file_exec_snapshot_begin(NULL, &first) == EINVAL);
	cache_disk_error = ENODEV;
	CHECK(file_exec_snapshot_begin(fixture.owner, &first) == ENODEV);
	CHECK(!first.active && first.read_disk == NULL && cache_disk_users == 0);
	cache_disk_error = 0;
	CHECK(file_exec_snapshot_begin(fixture.owner, &first) == 0);
	CHECK(first.shared_read && first.read_object != NULL);
	CHECK(first.read_disk == &disk && cache_disk_users == 1);
	CHECK(file_exec_snapshot_begin(fixture.owner, &second) == 0);
	CHECK(second.shared_read && second.read_object == first.read_object);
	CHECK(cache_disk_users == 2);
	CHECK(fixture.inode.i_vm_content_readers == 2 && !fixture.inode.i_vm_content_active);
	CHECK(file_content_lease_pread(&first, bytes, sizeof(bytes), 0) == sizeof(bytes));
	CHECK(memcmp(bytes, fixture.bytes, sizeof(bytes)) == 0);
	before = cache_backend_reads;
	CHECK(file_content_lease_pread(&second, bytes, sizeof(bytes), 0) == sizeof(bytes));
	CHECK(cache_backend_reads == before && fixture.inode.i_vm_content_generation == generation);
	CHECK(fixture.owner->f_offset == 0);
	CHECK(file_content_lease_pread(&first, bytes, 1, -1) == -EINVAL);
	CHECK(file_content_lease_pread(&first, bytes, 1, first.size) == 0);
	CHECK(file_content_lease_pread(&first, bytes, 8, first.size - 1) == 1);
	cache_disk_error = ENODEV;
	CHECK(file_content_lease_pread(&second, bytes, 1, 0) == -ENODEV);
	cache_disk_error = 0;
	file_content_lease_end(&first);
	CHECK(fixture.inode.i_vm_content_readers == 1);
	file_content_lease_end(&first);
	file_content_lease_end(&second);
	CHECK(fixture.inode.i_vm_content_readers == 0 && cache_disk_users == 0);
	(void)vm_object_cache_drain(NULL);

	/* Optional cache refusal cannot wait on the caller's own shared content gate. */
	cache_memory_get_stats(&previous);
	CHECK(cache_memory_set_target(0) == 0);
	allocation_failure = allocation_count + 1;
	CHECK(file_exec_snapshot_begin(fixture.owner, &first) == 0);
	allocation_failure = 0;
	CHECK(first.shared_read && first.read_object == NULL);
	CHECK(file_content_lease_pread(&first, bytes, sizeof(bytes), 0) == sizeof(bytes));
	CHECK(memcmp(bytes, fixture.bytes, sizeof(bytes)) == 0);
	file_content_lease_end(&first);
	size = fixture.inode.i_size;fixture.inode.i_size = -1;
	CHECK(file_exec_snapshot_begin(fixture.owner, &first) == EIO);
	CHECK(!first.active && fixture.inode.i_vm_content_readers == 0);
	fixture.inode.i_size = size;
	CHECK(cache_memory_set_target(previous.target_bytes) == 0);

	/* Mutable input retains the exclusive snapshot protocol and releases it. */
	fixture.mount.m_flags &= ~MOUNT_READ_ONLY;
	CHECK(file_exec_snapshot_begin(fixture.owner, &first) == 0);
	CHECK(!first.shared_read && fixture.inode.i_vm_content_active);
	CHECK(file_content_lease_pread(&first, bytes, sizeof(bytes), 0) == sizeof(bytes));
	file_content_lease_end(&first);
	CHECK(!fixture.inode.i_vm_content_active);
	fixture.mount.m_flags |= MOUNT_READ_ONLY;disk.d_flags = 0;
	CHECK(file_exec_snapshot_begin(fixture.owner, &first) == 0 && !first.shared_read);
	file_content_lease_end(&first);
	fixture.mount.m_flags &= ~MOUNT_READ_ONLY;
	fixture.mount.m_disk = NULL;
	close_fixture(&fixture);
}

/* Snapshot owners retain canonical frame identity and unwind partial capture. */
static void test_exec_snapshot_pages(void)
{
	struct fixture fixture;
	struct disk disk = {0};
	struct file_content_lease input;
	struct file_exec_snapshot *first, *second;
	struct cache_memory_stats before, after;
	unsigned char bytes[4096];
	unsigned reads;

	make_fixture(&fixture);
	cache_ops = backend_ops;cache_ops.pread = cache_pread;
	fixture.inode.i_fop = fixture.owner->f_ops = &cache_ops;
	fixture.mount.m_disk = &disk;fixture.mount.m_flags |= MOUNT_READ_ONLY;
	disk.d_flags = DISK_READ_ONLY;
	CHECK(file_exec_snapshot_begin(fixture.owner, &input) == 0);
	CHECK(file_exec_snapshot_create(&input, 1, 4096, &first) == EINVAL && first == NULL);
	CHECK(file_exec_snapshot_create(&input, 0, 4095, &first) == EINVAL && first == NULL);
	CHECK(file_exec_snapshot_create(&input, input.size, 4096, &first) == EINVAL);
	cache_memory_get_stats(&before);
	allocation_failure = allocation_count + 1;
	CHECK(file_exec_snapshot_create(&input, 0, 8192, &first) == ENOMEM && first == NULL);
	allocation_failure = 0;cache_memory_get_stats(&after);
	CHECK(before.resident_bytes == after.resident_bytes && after.pending_bytes == 0);
	CHECK(fixture.inode.i_vm_content_readers == 1 && cache_disk_users == 1);

	/* Fail after one cached page has already been pinned by the new owner. */
	CHECK(file_content_lease_pread(&input, bytes, sizeof(bytes), 0) == sizeof(bytes));
	cache_read_error = EIO;
	CHECK(file_exec_snapshot_create(&input, 0, 8192, &first) == EIO && first == NULL);
	cache_read_error = 0;
	CHECK(fixture.inode.i_vm_content_readers == 1 && cache_disk_users == 1);
	CHECK(file_exec_snapshot_create(&input, 0, 8192, &first) == 0);
	reads = cache_backend_reads;
	CHECK(file_exec_snapshot_create(&input, 0, 8192, &second) == 0);
	CHECK(first->page_count == 2 && second->page_count == 2);
	CHECK(first->pages[0] == second->pages[0] && first->pages[1] == second->pages[1]);
	CHECK(first->pages[0]->pmem.paddr == second->pages[0]->pmem.paddr);
	CHECK(cache_backend_reads == reads && first->pages[0]->pin_count == 2);
	file_content_lease_end(&input);
	CHECK(vm_object_reclaim_clean(SIZE_MAX) == 0);
	CHECK(vm_object_cache_drain(NULL) == 0);
	CHECK(vm_object_page_pin_read(first->pages[0], 0, bytes, sizeof(bytes)) == 0);
	CHECK(memcmp(bytes, fixture.bytes, sizeof(bytes)) == 0);
	file_exec_snapshot_ref(first);file_exec_snapshot_put(first);
	CHECK(fixture.inode.i_vm_content_readers == 2);
	file_exec_snapshot_put(first);
	CHECK(second->pages[0]->pin_count == 1 && fixture.inode.i_vm_content_readers == 1);
	file_exec_snapshot_put(second);
	CHECK(fixture.inode.i_vm_content_readers == 0 && cache_disk_users == 0);
	(void)vm_object_cache_drain(NULL);
	fixture.mount.m_flags &= ~MOUNT_READ_ONLY;fixture.mount.m_disk = NULL;
	close_fixture(&fixture);
}

/* A dirty orphan is invisible to writeback until a resize abort restores it. */
static void test_dirty_orphan_index(void)
{
	struct fixture fixture;
	struct vm_object *object;
	struct vm_object_page *page;
	struct vm_object_resize resize;
	unsigned char byte = 0x67;

	make_fixture(&fixture);
	CHECK(vm_object_get_shared(fixture.owner, &object) == 0);
	CHECK(vm_object_fault(object, 0, &page) == 0);
	CHECK(vm_object_page_pin(page) == 0);
	vm_object_fault_release(page);
	mutex_lock(&fixture.inode.i_io_lock);
	CHECK(vm_object_resize_begin(&fixture.inode, 0, &resize) == 0);
	mutex_unlock(&fixture.inode.i_io_lock);
	CHECK(vm_object_resize_prepare(&resize) == 0);
	CHECK(vm_object_page_pin_write(page, 0, &byte, 1) == 0);
	CHECK((page->flags & (VM_OBJECT_PAGE_ORPHANED | VM_OBJECT_PAGE_DIRTY)) ==
	    (VM_OBJECT_PAGE_ORPHANED | VM_OBJECT_PAGE_DIRTY));
	CHECK(object->dirty_pages == NULL && !page->dirty_linked);
	mutex_lock(&fixture.inode.i_io_lock);
	vm_object_resize_abort(&resize);
	mutex_unlock(&fixture.inode.i_io_lock);
	CHECK(object->dirty_pages == page && page->dirty_linked);
	vm_object_page_unpin(page);
	CHECK(file_fsync(fixture.owner) == 0);
	CHECK(fixture.bytes[0] == byte && object->dirty_pages == NULL);

	/* A committed truncation retires a later dirty orphan without indexing it. */
	CHECK(vm_object_fault(object, 0, &page) == 0);
	CHECK(vm_object_page_pin(page) == 0);
	vm_object_fault_release(page);
	mutex_lock(&fixture.inode.i_io_lock);
	CHECK(vm_object_resize_begin(&fixture.inode, 0, &resize) == 0);
	mutex_unlock(&fixture.inode.i_io_lock);
	CHECK(vm_object_resize_prepare(&resize) == 0);
	mutex_lock(&fixture.inode.i_io_lock);
	fixture.inode.i_size = 0;
	vm_object_resize_commit(&resize, 0);
	mutex_unlock(&fixture.inode.i_io_lock);
	byte = 0x89;
	CHECK(vm_object_page_pin_write(page, 0, &byte, 1) == 0);
	CHECK(object->dirty_pages == NULL && !page->dirty_linked);
	vm_object_page_unpin(page);
	CHECK(object->orphan_pages == NULL && vm_object_page_count() == 0);
	vm_object_put(object);
	close_fixture(&fixture);
}

/* A deferred owner keeps exclusion after the original owner releases. */
static void test_claim_references(void)
{
	struct fixture fixture;
	struct backing_claim *claim;
	struct backing_mutation_guard guard;

	make_fixture(&fixture);
	CHECK(backing_claim_prepare_inode(&fixture.inode, BACKING_CLAIM_LOOP, &claim) == 0);
	backing_claim_ref(claim);
	backing_claim_release(claim);
	CHECK(backing_mutation_begin_inode(&fixture.alias, &guard) == EBUSY);
	backing_claim_release(claim);
	CHECK(backing_mutation_begin_inode(&fixture.alias, &guard) == 0);
	backing_mutation_end(&guard);
	close_fixture(&fixture);
}

/* Failed persistence retains dirty credits; only confirmed clean retires them. */
static void test_dirty_credits(void)
{
	struct fixture fixture;
	struct vm_object *object;
	struct vm_object_page *page;
	struct writeback_budget budget = {0};
	struct writeback_ticket ticket = {0};
	struct writeback_budget_stats stats;
	struct cache_memory_stats previous;

	cache_memory_get_stats(&previous);
	CHECK(cache_memory_set_target(16U * 1024U * 1024U) == 0);
	make_fixture(&fixture);
	CHECK(writeback_budget_attach(&budget, &fixture.leaf) == 0);
	CHECK(writeback_ticket_reserve(&budget, &ticket) == 0);
	CHECK(vm_object_get_shared(fixture.owner, &object) == 0);
	CHECK(vm_object_fault(object, 0, &page) == 0);
	memset(page->pmem.vaddr, 0x58, ZEDBSD_PAGE_SIZE);
	vm_object_mark_dirty(page);
	writeback_ticket_commit(&ticket, ZEDBSD_PAGE_SIZE);
	page->writeback_budget = &budget;
	writeback_ticket_release(&ticket);
	vm_object_fault_release(page);
	backend_sync_error = EIO;
	CHECK(file_fsync(fixture.owner) == EIO);
	writeback_budget_snapshot(&stats);
	CHECK(stats.dirty == ZEDBSD_PAGE_SIZE && page->writeback_budget == &budget);
	CHECK(writeback_budget_detach(&budget) == EBUSY);
	backend_sync_error = 0;
	CHECK(file_fsync(fixture.owner) == 0);
	writeback_budget_snapshot(&stats);
	CHECK(stats.dirty == 0 && page->writeback_budget == NULL);
	CHECK(fixture.bytes[0] == 0x58);
	vm_object_put(object);
	CHECK(writeback_budget_detach(&budget) == 0);
	close_fixture(&fixture);
	CHECK(cache_memory_set_target(previous.target_bytes) == 0);
}

/* Delayed content shares ordinary read/truncate gates and owns its own writer. */
static void test_delayed_content(void)
{
	struct fixture fixture;
	struct vm_object *object;
	struct vm_object_content content;
	struct writeback_budget budget = {0};
	struct writeback_ticket ticket = {0};
	struct cache_memory_stats previous;
	unsigned char input[64], output[64];
	unsigned round, files;

	cache_memory_get_stats(&previous);
	CHECK(cache_memory_set_target(16U * 1024U * 1024U) == 0);
	make_fixture(&fixture);
	CHECK(writeback_budget_attach(&budget, &fixture.leaf) == 0);
	CHECK(writeback_ticket_reserve(&budget, &ticket) == 0);
	CHECK(vm_object_writeback_prepare(fixture.owner, &object) == 0);
	CHECK(object->write_file != fixture.owner && object->file != fixture.owner);
	mutex_lock(&fixture.inode.i_io_lock);
	CHECK(vm_object_content_begin(fixture.owner, 0, 8192, NULL, &content) == 0);
	mutex_unlock(&fixture.inode.i_io_lock);
	cache_pmem_failure = cache_pmem_count + 1;
	CHECK(vm_object_content_prepare_delayed(&content, object, &ticket) == EAGAIN);
	cache_pmem_failure = 0;
	mutex_lock(&fixture.inode.i_io_lock);
	vm_object_content_abort(&content);
	mutex_unlock(&fixture.inode.i_io_lock);
	CHECK(vm_object_page_count() == 0 && backend_writes == 0 && budget.dirty == 0);

	/* Aborting a prepared full overwrite must leave old cache bytes readable. */
	mutex_lock(&fixture.inode.i_io_lock);
	CHECK(vm_object_content_begin(fixture.owner, 0, 8192, NULL, &content) == 0);
	mutex_unlock(&fixture.inode.i_io_lock);
	CHECK(vm_object_content_prepare_delayed(&content, object, &ticket) == 0);
	mutex_lock(&fixture.inode.i_io_lock);
	vm_object_content_abort(&content);
	mutex_unlock(&fixture.inode.i_io_lock);
	writeback_ticket_release(&ticket);
	CHECK(file_pread(fixture.owner, output, sizeof(output), 0) == sizeof(output));
	CHECK(output[0] == 0xa5 && budget.dirty == 0);
	for (round = 0; round < 30; round++) {
		CHECK(writeback_ticket_reserve(&budget, &ticket) == 0);
		mutex_lock(&fixture.inode.i_io_lock);
		CHECK(vm_object_content_begin(fixture.owner, 3, sizeof(input), NULL, &content) == 0);
		mutex_unlock(&fixture.inode.i_io_lock);
		CHECK(vm_object_content_prepare_delayed(&content, object, &ticket) == 0);
		memset(input, 0x20 + round, sizeof(input));
		mutex_lock(&fixture.inode.i_io_lock);
		vm_object_content_commit(&content, input, sizeof(input));
		mutex_unlock(&fixture.inode.i_io_lock);
		writeback_ticket_release(&ticket);
		CHECK(budget.dirty == ZEDBSD_PAGE_SIZE && backend_writes == 0);
	}
	CHECK(file_pread(fixture.owner, output, sizeof(output), 3) == sizeof(output));
	CHECK(memcmp(input, output, sizeof(input)) == 0 && fixture.bytes[3] == 0xa5);
	files = file_count();
	CHECK(file_close(fixture.owner) == 0);
	fixture.owner = NULL;
	CHECK(file_count() == files - 1);
	vm_object_writeback_release(object);
	backend_sync_error = EIO;
	CHECK(vm_object_sync_inode(&fixture.inode) == EIO && budget.dirty == ZEDBSD_PAGE_SIZE);
	backend_sync_error = 0;
	CHECK(vm_object_sync_inode(&fixture.inode) == 0 && budget.dirty == 0);
	CHECK(memcmp(fixture.bytes + 3, input, sizeof(input)) == 0);
	CHECK(fixture.bytes[0] == 0xa5 && fixture.bytes[67] == 0xa5);
	CHECK(vm_object_cache_drain(NULL) == 1);
	CHECK(writeback_budget_detach(&budget) == 0);
	close_fixture(&fixture);
	CHECK(cache_memory_set_target(previous.target_bytes) == 0);
}

/* Verifies a coalesced durability transaction and the nonblocking scalar fallback. */
static void test_writeback_batch(void)
{
	struct fixture fixture;
	struct vm_object *object;
	struct vm_object_content content;
	struct writeback_budget budget = {0};
	struct writeback_ticket ticket = {0};
	struct cache_memory_stats previous;
	unsigned char input[FILE_BYTES];
	unsigned char scratch[64U * 1024U];
	unsigned mode, before;

	cache_memory_get_stats(&previous);
	CHECK(cache_memory_set_target(16U * 1024U * 1024U) == 0);
	for (mode = 0; mode < 3; mode++) {
		make_fixture(&fixture);
		memset(input, 0x61 + mode, sizeof(input));
		CHECK(writeback_budget_attach(&budget, &fixture.leaf) == 0);
		CHECK(writeback_ticket_reserve(&budget, &ticket) == 0);
		CHECK(vm_object_writeback_prepare(fixture.owner, &object) == 0);
		mutex_lock(&fixture.inode.i_io_lock);
		CHECK(vm_object_content_begin(fixture.owner, 0, FILE_BYTES, NULL, &content) == 0);
		mutex_unlock(&fixture.inode.i_io_lock);
		CHECK(vm_object_content_prepare_delayed(&content, object, &ticket) == 0);
		mutex_lock(&fixture.inode.i_io_lock);
		vm_object_content_commit(&content, input, sizeof(input));
		mutex_unlock(&fixture.inode.i_io_lock);
		writeback_ticket_release(&ticket);
		CHECK(budget.dirty == FILE_BYTES && backend_writes == 0);

		/* A failed final barrier retains every page even after successful data I/O. */
		cache_pool_disabled = mode;
		backend_sync_error = EIO;
		CHECK(vm_object_sync_mount_buffer(&fixture.mount,
		    mode == 2 ? scratch : NULL, sizeof(scratch)) == EIO);
		CHECK(budget.dirty == FILE_BYTES);
		CHECK(backend_writes == (mode == 1 ? FILE_BYTES / ZEDBSD_PAGE_SIZE : 1));
		before = backend_writes;
		backend_sync_error = 0;
		CHECK(vm_object_sync_mount_buffer(&fixture.mount,
		    mode == 2 ? scratch : NULL, sizeof(scratch)) == 0);
		CHECK(budget.dirty == 0 && !cache_pool_used);
		CHECK(backend_writes - before == (mode == 1 ? FILE_BYTES / ZEDBSD_PAGE_SIZE : 1));
		CHECK(memcmp(fixture.bytes, input, sizeof(input)) == 0);
		cache_pool_disabled = 0;
		vm_object_writeback_release(object);
		CHECK(vm_object_cache_drain(NULL) == 1);
		CHECK(writeback_budget_detach(&budget) == 0);
		close_fixture(&fixture);
	}
	CHECK(cache_memory_set_target(previous.target_bytes) == 0);
}

static unsigned char batch_media[65536];
static unsigned batch_calls;
static size_t batch_bytes;

/* Records real VM-to-file drain boundaries in a larger deterministic backend. */
static ssize_t batch_pwrite(struct file *file, const void *bytes, size_t size, off_t offset)
{
	(void)file;
	CHECK(offset >= 0 && (uint64_t)offset <= sizeof(batch_media));
	CHECK(size <= sizeof(batch_media) - (size_t)offset);
	batch_calls++;
	batch_bytes += size;
	memcpy(batch_media + offset, bytes, size);
	return (ssize_t)size;
}

static void test_writeback_64k(void)
{
	struct fixture fixture;
	struct file_ops ops;
	struct vm_object *object;
	struct vm_object_content content;
	struct writeback_budget budget = {0};
	struct writeback_ticket ticket = {0};
	struct cache_memory_stats previous;
	unsigned char input[65536];
	unsigned i;

	cache_memory_get_stats(&previous);
	CHECK(cache_memory_set_target(16U * 1024U * 1024U) == 0);
	make_fixture(&fixture);
	ops = backend_ops;
	ops.pread = cache_pread;
	ops.pwrite = batch_pwrite;
	fixture.inode.i_size = sizeof(input);
	fixture.inode.i_fop = &ops;
	fixture.owner->f_ops = &ops;
	cache_generated = 1;
	batch_calls = 0;
	batch_bytes = 0;
	for (i = 0; i < sizeof(input); i++)
		input[i] = (unsigned char)(i % 247U);
	CHECK(writeback_budget_attach(&budget, &fixture.leaf) == 0);
	CHECK(writeback_ticket_reserve(&budget, &ticket) == 0);
	CHECK(vm_object_writeback_prepare(fixture.owner, &object) == 0);
	mutex_lock(&fixture.inode.i_io_lock);
	CHECK(vm_object_content_begin(fixture.owner, 0, sizeof(input), NULL, &content) == 0);
	mutex_unlock(&fixture.inode.i_io_lock);
	CHECK(vm_object_content_prepare_delayed(&content, object, &ticket) == 0);
	mutex_lock(&fixture.inode.i_io_lock);
	vm_object_content_commit(&content, input, sizeof(input));
	mutex_unlock(&fixture.inode.i_io_lock);
	writeback_ticket_release(&ticket);
	CHECK(budget.dirty == sizeof(input) && batch_calls == 0);
	CHECK(vm_object_sync_inode(&fixture.inode) == 0);
	CHECK(batch_calls == 1 && batch_bytes == sizeof(input));
	CHECK(memcmp(batch_media, input, sizeof(input)) == 0 && budget.dirty == 0);
	vm_object_writeback_release(object);
	CHECK(vm_object_cache_drain(NULL) == 1);
	CHECK(writeback_budget_detach(&budget) == 0);
	cache_generated = 0;
	close_fixture(&fixture);
	CHECK(cache_memory_set_target(previous.target_bytes) == 0);
}

static struct mount *admitted_mount;
static struct writeback_budget *admitted_budget;
static int eligible_result=1;

/* The actual worker/control implementation is exercised by its concurrent fixture. */
int writeback_mount_admit(struct mount *mount, struct writeback_ticket *ticket)
{
	if (mount == NULL || mount != admitted_mount)
		return EAGAIN;
	return writeback_ticket_reserve(admitted_budget, ticket);
}
void writeback_pressure(struct writeback_budget *budget)
{
	CHECK(budget == admitted_budget);
}
int ws025_writeback_eligible(struct file *file, off_t offset, size_t size)
{
	if (eligible_result != 1)
		return eligible_result;
	if (offset < 0 || offset > file->f_inode->i_size ||
	    size > (uint64_t)(file->f_inode->i_size-offset))
		return 0;
	return 1;
}

/* Exercises delayed writes through the actual public file I/O entry points. */
static void test_writeback_file_io(void)
{
	struct file_ops synchronous_ops;
	struct file_io transaction;
	struct io_context drain = { 0 };
	struct fixture fixture;
	struct writeback_budget budget={0};
	struct cache_memory_stats previous;
	struct writeback_budget snapshot;
	unsigned char bytes[64], output[64];
	unsigned round, before;

	cache_memory_get_stats(&previous);
	CHECK(cache_memory_set_target(16U*1024U*1024U)==0);
	make_fixture(&fixture);
	CHECK(writeback_budget_attach(&budget,&fixture.leaf)==0);
	admitted_mount=&fixture.mount;admitted_budget=&budget;
	/* Optional page refusal completes through and releases its pre-lease ticket. */
	memset(bytes,0x72,sizeof(bytes));
	cache_pmem_failure=cache_pmem_count+1;
	CHECK(file_pwrite(fixture.owner,bytes,sizeof(bytes),256)==sizeof(bytes));
	cache_pmem_failure=0;
	CHECK(backend_writes==1 && budget.dirty==0 && budget.tickets==0);
	CHECK(vm_object_page_count()==0);
	backend_writes=0;
	for(round=0;round<30;round++) {
		memset(bytes,0x30+round,sizeof(bytes));
		CHECK(file_pwrite(fixture.owner,bytes,sizeof(bytes),3)==sizeof(bytes));
		CHECK(backend_writes==0 && budget.dirty==4096);
		CHECK(writeback_budget_read(&budget,&snapshot)==0 && snapshot.tickets==0);
	}
	CHECK(file_pread(fixture.owner,output,sizeof(output),3)==sizeof(output));
	CHECK(memcmp(bytes,output,sizeof(bytes))==0 && fixture.bytes[3]==0xa5);
	CHECK(file_fsync(fixture.owner)==0 && budget.dirty==0 && backend_writes==1);
	CHECK(memcmp(bytes,fixture.bytes+3,sizeof(bytes))==0);

	/* Each explicit fsync remains its own durability boundary. */
	before=backend_writes;
	for(round=0;round<30;round++) {
		bytes[0]=(unsigned char)round;
		CHECK(file_pwrite(fixture.owner,bytes,sizeof(bytes),3)==sizeof(bytes));
		CHECK(file_fsync(fixture.owner)==0);
	}
	CHECK(backend_writes-before==30);

	/* Missing allocation and validation failure retain the established semantics. */
	eligible_result=0;before=backend_writes;
	CHECK(file_pwrite(fixture.owner,bytes,sizeof(bytes),3)==sizeof(bytes));
	CHECK(backend_writes==before+1 && budget.dirty==0);
	eligible_result=-EIO;before=backend_writes;
	CHECK(file_pwrite(fixture.owner,bytes,sizeof(bytes),3)==-EIO);
	CHECK(backend_writes==before && budget.dirty==0 && budget.tickets==0);
	eligible_result=1;

	/* Append/synchronous descriptions and internal drains never delay acceptance. */
	for(round=0;round<3;round++) {
		int flag=round==0?O_APPEND:(round==1?O_SYNC:O_DSYNC);
		file_status_flags_update(fixture.owner,flag,flag);before=backend_writes;
		CHECK(file_pwrite(fixture.owner,bytes,sizeof(bytes),3)==sizeof(bytes));
		CHECK(backend_writes==before+1 && budget.dirty==0);
		file_status_flags_update(fixture.owner,flag,0);
	}
	before=backend_writes;
	CHECK(file_pwrite_internal(fixture.owner,bytes,sizeof(bytes),3,FILE_IO_DRAIN)==sizeof(bytes));
	CHECK(backend_writes==before+1 && budget.dirty==0);

	/* Multiple copy chunks share one checked outer completion barrier. */
	synchronous_ops=*fixture.owner->f_ops;
	synchronous_ops.fsync=synchronous_backend;
	fixture.owner->f_ops=&synchronous_ops;
	file_status_flags_update(fixture.owner,O_SYNC,O_SYNC);
	synchronous_barriers=0;
	CHECK(file_io_begin(fixture.owner,FILE_IO_PWRITE,3,0,&transaction)==0);
	CHECK(file_io_transfer(&transaction,bytes,64)==64);
	CHECK(file_io_transfer(&transaction,bytes,64)==64);
	CHECK(synchronous_barriers==0);
	CHECK(file_io_complete(&transaction,128)==128 && synchronous_barriers==1);
	CHECK(file_io_begin(fixture.owner,FILE_IO_PWRITE,3,0,&transaction)==0);
	CHECK(file_io_transfer(&transaction,bytes,64)==64);
	backend_sync_error=EIO;
	CHECK(file_io_complete(&transaction,-EFAULT)==-EFAULT && synchronous_barriers==2);
	backend_sync_error=0;
	/* Explicit inherited drain must not start an outer fsync recursively. */
	drain.flags=IO_CONTEXT_DRAIN;
	CHECK(file_pwrite_context(fixture.owner,bytes,64,3,0,NULL,&drain)==64);
	CHECK(synchronous_barriers==2);
	file_status_flags_update(fixture.owner,O_SYNC,0);

	/* Synchronous completion reports a failed barrier and permits a durable retry. */
	file_status_flags_update(fixture.owner,O_SYNC,O_SYNC);
	backend_sync_error=EIO;
	CHECK(file_pwrite(fixture.owner,bytes,sizeof(bytes),3)==-EIO);
	backend_sync_error=0;
	CHECK(file_pwrite(fixture.owner,bytes,sizeof(bytes),3)==sizeof(bytes));
	file_status_flags_update(fixture.owner,O_SYNC,0);
	file_status_flags_update(fixture.owner,O_DSYNC,O_DSYNC);
	backend_sync_error=EIO;
	CHECK(file_pwrite(fixture.owner,bytes,sizeof(bytes),3)==-EIO);
	backend_sync_error=0;
	CHECK(file_pwrite(fixture.owner,bytes,sizeof(bytes),3)==sizeof(bytes));
	file_status_flags_update(fixture.owner,O_DSYNC,0);

	/* Closing the caller preserves dirty bytes in the independent VM writer. */
	CHECK(file_pwrite(fixture.owner,bytes,sizeof(bytes),3)==sizeof(bytes));
	CHECK(file_close(fixture.owner)==0);fixture.owner=NULL;
	backend_sync_error=EIO;
	CHECK(vm_object_sync_inode(&fixture.inode)==EIO && budget.dirty==4096);
	backend_sync_error=0;
	CHECK(vm_object_sync_inode(&fixture.inode)==0 && budget.dirty==0);
	admitted_mount=NULL;admitted_budget=NULL;
	CHECK(vm_object_cache_drain(NULL)==1);
	CHECK(writeback_budget_detach(&budget)==0);
	close_fixture(&fixture);
	CHECK(cache_memory_set_target(previous.target_bytes)==0);
}

static unsigned redirty_armed, redirty_entered, redirty_release;
static ssize_t redirty_backend(struct file *file,const void *buffer,size_t length,off_t offset)
{
	if(__atomic_exchange_n(&redirty_armed,0,__ATOMIC_ACQ_REL)) {
		CHECK(length==4096 && offset==0 && ((const unsigned char *)buffer)[0]==0x41);
		__atomic_store_n(&redirty_entered,1,__ATOMIC_RELEASE);
		while(!__atomic_load_n(&redirty_release,__ATOMIC_ACQUIRE))sched_yield();
		CHECK(((const unsigned char *)buffer)[0]==0x41);
	}
	return backend_pwrite(file,buffer,length,offset);
}
struct redirty_call { struct file *file; ssize_t result; };
static void *redirty_flush(void *argument)
{
	struct redirty_call *call=argument;
	call->result=file_fsync(call->file);return NULL;
}
static void *redirty_write(void *argument)
{
	struct redirty_call *call=argument;
	unsigned char byte=0x62;
	call->result=file_pwrite(call->file,&byte,1,0);return NULL;
}
static void test_writeback_redirty(void)
{
	struct fixture fixture;
	struct file_ops ops_copy;
	struct path path;
	struct file *second;
	struct writeback_budget budget={0},snapshot;
	struct cache_memory_stats previous;
	struct redirty_call flush,write;
	pthread_t flusher,writer;
	unsigned char byte=0x41;

	cache_memory_get_stats(&previous);
	CHECK(cache_memory_set_target(16U*1024U*1024U)==0);
	make_fixture(&fixture);ops_copy=*fixture.owner->f_ops;ops_copy.pwrite=redirty_backend;
	fixture.owner->f_ops=&ops_copy;fixture.inode.i_fop=&ops_copy;
	path.p_inode=&fixture.inode;path.p_mount=&fixture.mount;
	CHECK(file_open_resolved(&path,O_RDWR,&second)==0);
	CHECK(writeback_budget_attach(&budget,&fixture.leaf)==0);
	admitted_mount=&fixture.mount;admitted_budget=&budget;
	CHECK(file_pwrite(fixture.owner,&byte,1,0)==1 && budget.dirty==4096);
	redirty_entered=redirty_release=0;redirty_armed=1;
	flush.file=fixture.owner;write.file=second;
	CHECK(pthread_create(&flusher,NULL,redirty_flush,&flush)==0);
	while(!__atomic_load_n(&redirty_entered,__ATOMIC_ACQUIRE))sched_yield();
	CHECK(pthread_create(&writer,NULL,redirty_write,&write)==0);
	for(;;) {
		CHECK(writeback_budget_read(&budget,&snapshot)==0);
		if(snapshot.tickets!=0)break;
		sched_yield();
	}
	/* The new operation has entered admission while the old immutable payload is in flight. */
	CHECK(snapshot.dirty==4096);
	__atomic_store_n(&redirty_release,1,__ATOMIC_RELEASE);
	CHECK(pthread_join(flusher,NULL)==0 && pthread_join(writer,NULL)==0);
	CHECK(flush.result==0 && write.result==1 && budget.dirty==4096);
	CHECK(fixture.bytes[0]==0x41);
	CHECK(file_pread(second,&byte,1,0)==1 && byte==0x62);
	CHECK(file_fsync(second)==0 && budget.dirty==0 && fixture.bytes[0]==0x62);
	CHECK(file_close(second)==0);
	admitted_mount=NULL;admitted_budget=NULL;
	CHECK(vm_object_cache_drain(NULL)==1);
	CHECK(writeback_budget_detach(&budget)==0);close_fixture(&fixture);
	CHECK(cache_memory_set_target(previous.target_bytes)==0);
}

/* Exercises private fill retirement against real demand/content/resize operations. */
static void test_prefetch_publication(void)
{
	struct fixture fixture;
	struct vm_object_prefetch fill={0};
	struct vm_object_resize resize;
	struct vm_object *object;
	struct vm_object_page *page;
	unsigned char bytes[8192],observed;
	unsigned mode,before;
	size_t published;
	ssize_t received;
	int error;

	for(mode=0;mode<9;mode++) {
		make_fixture(&fixture);
		cache_ops=backend_ops;cache_ops.pread=cache_pread;
		fixture.inode.i_fop=&cache_ops;fixture.owner->f_ops=&cache_ops;
		if(mode==7)fixture.inode.i_size=5000;
		CHECK(vm_object_prefetch_prepare(&fixture.inode,0,8192,&fill)==EAGAIN);
		vm_object_cache_prepare(fixture.owner);
		CHECK(vm_object_prefetch_prepare(&fixture.inode,0,1,&fill)==EINVAL);
		CHECK(vm_object_prefetch_prepare(&fixture.inode,1,8192,&fill)==EINVAL);
		if(mode==6)cache_pmem_failure=cache_pmem_count+2;
		error=vm_object_prefetch_prepare(&fixture.inode,0,8192,&fill);
		cache_pmem_failure=0;
		if(mode==6) {
			CHECK(error==EAGAIN && fill.object==NULL && vm_object_page_count()==0);
			CHECK(vm_object_cache_drain(NULL)==1);close_fixture(&fixture);continue;
		}
		CHECK(error==0 && fill.object!=NULL && fill.count==2);
		CHECK(vm_object_page_count()==0 && vm_object_cache_drain(NULL)==0);
		if(mode==8) {
			vm_object_prefetch_abort(&fill);vm_object_prefetch_abort(&fill);
			CHECK(fill.object==NULL && vm_object_cache_drain(NULL)==1);
			close_fixture(&fixture);continue;
		}
		received=file_pread_internal(fill.object->file,bytes,fill.length,fill.offset,FILE_IO_VM_OBJECT);
		CHECK(received==(ssize_t)fill.length && bytes[0]==0xa5);
		if(mode==1)received--;
		if(mode==2)received=-EIO;
		if(mode==3) { observed=0x77;CHECK(file_pwrite(fixture.owner,&observed,1,0)==1); }
		if(mode==4) { CHECK(file_pread(fixture.owner,&observed,1,0)==1 && observed==0xa5); }
		if(mode==5) {
			CHECK(vm_object_resize_begin(&fixture.inode,4096,&resize)==0);
			CHECK(vm_object_resize_prepare(&resize)==0);
			fixture.inode.i_size=4096;vm_object_resize_commit(&resize,4096);
		}
		published=99;error=vm_object_prefetch_complete(&fill,bytes,received,&published);
		CHECK(fill.object==NULL && cache_disk_users==0);
		if(mode==0 || mode==7) {
			CHECK(error==0 && published==(mode==7?5000:8192) && vm_object_page_count()==2);
			before=cache_backend_reads;
			CHECK(file_pread(fixture.owner,&observed,1,4096)==1 && observed==0xa5);
			CHECK(cache_backend_reads==before);
			if(mode==7) {
				CHECK(vm_object_get_shared(fixture.owner,&object)==0);
				CHECK(vm_object_fault(object,4096,&page)==0);
				CHECK(((unsigned char *)page->pmem.vaddr)[903]==0xa5);
				CHECK(((unsigned char *)page->pmem.vaddr)[904]==0);
				vm_object_fault_release(page);vm_object_put(object);
			}
		}else {
			CHECK(error==(mode<3?EIO:EAGAIN) && published==0);
			CHECK(file_pread(fixture.owner,&observed,1,0)==1);
			CHECK(observed==(mode==3?0x77:0xa5));
		}
		CHECK(vm_object_cache_drain(NULL)==1);close_fixture(&fixture);
	}
}

/* Checks real file/VM completion around optional admission and description resets. */
static void test_read_observation(void)
{
 struct fixture fixture;unsigned char bytes[4096];uint64_t generation;unsigned before;
 struct file *temporary;int descriptor,duplicate;
 make_fixture(&fixture);cache_ops=backend_ops;cache_ops.pread=cache_pread;
 fixture.inode.i_fop=&cache_ops;fixture.owner->f_ops=&cache_ops;
 fixture.inode.i_size=262144;cache_generated=1;read_capture=1;
 read_submissions=0;read_cancellations=0;
 CHECK(file_pread(fixture.owner,bytes,sizeof(bytes),0)==4096 && read_submissions==0);
 CHECK(file_pread(fixture.owner,bytes,sizeof(bytes),4096)==4096 && read_submissions==1);
 CHECK(read_candidate.offset==8192 && read_candidate.length==65536);
 /* A positional read never waits for optional stream state owned by another actor. */
 fixture.owner->f_lock.locked=1;fixture.owner->f_lock.owner=NULL;before=read_submissions;
 CHECK(file_pread(fixture.owner,bytes,sizeof(bytes),8192)==4096 && read_submissions==before);
 fixture.owner->f_lock.locked=0;CHECK(read_priority==0);
 /* A temporary syscall reference drop does not cancel an active stream. */
 generation=fixture.owner->f_readahead.generation;before=read_cancellations;
 file_ref(fixture.owner);CHECK(file_close(fixture.owner)==0);
 CHECK(fixture.owner->f_readahead.generation==generation && read_cancellations==before);
 CHECK(file_seek(fixture.owner,0,0)==0 && fixture.owner->f_readahead.generation!=generation);
 CHECK(!fixture.owner->f_readahead.valid);
 CHECK(file_pread(fixture.owner,bytes,sizeof(bytes),0)==4096);
 /* Reset after releasing read leases invalidates the saved completion witness. */
 read_invalidate_on_end=fixture.owner;before=read_submissions;
 CHECK(file_pread(fixture.owner,bytes,sizeof(bytes),4096)==4096);
 CHECK(read_submissions==before && !fixture.owner->f_readahead.valid);
 CHECK(file_pread(fixture.owner,bytes,sizeof(bytes),0)==4096);
 CHECK(file_pread(fixture.owner,bytes,0,4096)==0 && !fixture.owner->f_readahead.valid);
 CHECK(file_pread(fixture.owner,bytes,sizeof(bytes),0)==4096);
 cache_read_error=EIO;
 CHECK(file_pread(fixture.owner,bytes,sizeof(bytes),131072)==-EIO);
 cache_read_error=0;CHECK(!fixture.owner->f_readahead.valid && read_priority==0);
 /* Descriptor teardown cancels, whereas lookup/temporary release preserves state. */
 read_table=filedesc_create(NULL);CHECK(read_table!=NULL);
 file_ref(fixture.owner);CHECK(filedesc_install(read_table,fixture.owner,&descriptor)==0);
 before=read_cancellations;temporary=filedesc_get_ref(read_table,descriptor);
 CHECK(temporary==fixture.owner && file_close(temporary)==0 && read_cancellations==before);
 CHECK(filedesc_dup(read_table,descriptor,0,0,&duplicate)==0);
 CHECK(filedesc_close(read_table,duplicate)==0 && read_cancellations==before+1);
 file_ref(fixture.foreign);CHECK(filedesc_install_at(read_table,fixture.foreign,duplicate)==0);
 generation=fixture.foreign->f_readahead.generation;
 CHECK(filedesc_dup2(read_table,descriptor,duplicate,0,0)==0);
 CHECK(fixture.foreign->f_readahead.generation!=generation);
 CHECK(filedesc_set_flags(read_table,duplicate,FILEDESC_CLOEXEC)==0);
 before=read_cancellations;filedesc_close_on_exec(read_table);CHECK(read_cancellations==before+1);
 before=read_cancellations;filedesc_destroy(read_table);read_table=NULL;CHECK(read_cancellations==before+1);
 CHECK(vm_object_cache_drain(NULL)==1);read_capture=0;cache_generated=0;close_fixture(&fixture);
}

/* Grows the actual file stream from real speculative page consumption, not mock hints. */
static void test_prefetch_feedback(void)
{
 struct fixture fixture;struct vm_object_prefetch fill={0};struct vm_object_resize resize;
 unsigned char *payload;unsigned char bytes[4096];unsigned index;
 uint64_t useful_before,unused_before;size_t published;
 make_fixture(&fixture);cache_ops=backend_ops;cache_ops.pread=cache_pread;
 fixture.inode.i_fop=&cache_ops;fixture.owner->f_ops=&cache_ops;
 fixture.inode.i_size=262144;cache_generated=1;
 payload=malloc(65536);CHECK(payload!=NULL);memset(payload,0x5a,65536);
 CHECK(file_pread(fixture.owner,bytes,4096,0)==4096);
 CHECK(vm_object_prefetch_prepare(&fixture.inode,4096,65536,&fill)==0);
 CHECK(vm_object_prefetch_complete(&fill,payload,65536,&published)==0 && published==65536);
 useful_before=confirmed_prefetch;unused_before=unused_prefetch;
 for(index=1;index<=16;index++) {
  CHECK(file_pread(fixture.owner,bytes,4096,(off_t)index*4096)==4096 && bytes[0]==0x5a);
 }
 CHECK(confirmed_prefetch-useful_before==65536);
 CHECK(fixture.owner->f_readahead.window==READAHEAD_MAX_WINDOW);
 CHECK(file_pread(fixture.owner,bytes,4096,4096)==4096);
 CHECK(confirmed_prefetch-useful_before==65536);
 /* Partial forward consumption is exact, repeated spans never earn another credit. */
 CHECK(vm_object_prefetch_prepare(&fixture.inode,73728,8192,&fill)==0);
 CHECK(vm_object_prefetch_complete(&fill,payload,8192,&published)==0 && published==8192);
 CHECK(file_pread(fixture.owner,bytes,100,73728)==100);
 CHECK(file_pread(fixture.owner,bytes,100,73728)==100);
 CHECK(file_pread(fixture.owner,bytes,100,73828)==100);
 CHECK(confirmed_prefetch-useful_before==65536+200);
 /* A committed EOF change retires attribution even for a surviving prefix page. */
 CHECK(vm_object_prefetch_prepare(&fixture.inode,81920,8192,&fill)==0);
 CHECK(vm_object_prefetch_complete(&fill,payload,8192,&published)==0 && published==8192);
 CHECK(vm_object_resize_begin(&fixture.inode,82500,&resize)==0);
 CHECK(vm_object_resize_prepare(&resize)==0);fixture.inode.i_size=82500;
 vm_object_resize_commit(&resize,82500);
 CHECK(file_pread(fixture.owner,bytes,200,81920)==200);
 CHECK(confirmed_prefetch-useful_before==65536+200);
 CHECK(vm_object_cache_drain(NULL)==1);
 CHECK(unused_prefetch-unused_before==16384-200);
 free(payload);cache_generated=0;close_fixture(&fixture);
}

struct cached_progress_case {
 struct vm_object_prefetch fill;
 struct file *reader;
 unsigned char payload[8192];
 ssize_t read_result;
 int completion;
};
static void *cached_progress_backend(void *argument)
{
 struct cached_progress_case *test=argument;ssize_t count;size_t published;
 count=file_pread_internal(test->fill.object->file,test->payload,test->fill.length,
  test->fill.offset,FILE_IO_VM_OBJECT);
 test->completion=vm_object_prefetch_complete(&test->fill,test->payload,count,&published);
 return NULL;
}
static void *cached_progress_reader(void *argument)
{
 struct cached_progress_case *test=argument;unsigned char byte;
 test->read_result=file_pread(test->reader,&byte,1,0);
 CHECK(test->read_result==1 && byte==0);
 pthread_mutex_lock(&cached_gate_lock);cached_demand_done=1;
 pthread_cond_broadcast(&cached_gate_changed);pthread_mutex_unlock(&cached_gate_lock);
 return NULL;
}
/* A real backend owns future-page inode I/O while demand copies an existing page. */
static void test_cached_demand_progress(void)
{
 struct fixture fixture;struct cached_progress_case test={0};struct timespec deadline;
 pthread_t backend,reader;unsigned char byte;int wait_error,progress;
 struct file_io transaction;struct vm_object_resize resize;struct vm_object_content content;
 make_fixture(&fixture);cache_ops=backend_ops;cache_ops.pread=cache_pread;
 fixture.inode.i_fop=&cache_ops;fixture.owner->f_ops=&cache_ops;
 fixture.inode.i_size=65536;cache_generated=1;
 CHECK(file_open_resolved(&fixture.owner->f_path,O_RDONLY,&test.reader)==0);
 CHECK(file_pread(fixture.owner,&byte,1,0)==1 && byte==0);
 CHECK(vm_object_prefetch_prepare(&fixture.inode,4096,8192,&test.fill)==0);
 cached_backend_paused=0;cached_backend_release=0;cached_demand_done=0;cached_backend_arm=1;
 CHECK(pthread_create(&backend,NULL,cached_progress_backend,&test)==0);
 pthread_mutex_lock(&cached_gate_lock);
 while(!cached_backend_paused)pthread_cond_wait(&cached_gate_changed,&cached_gate_lock);
 pthread_mutex_unlock(&cached_gate_lock);
 CHECK(pthread_create(&reader,NULL,cached_progress_reader,&test)==0);
 CHECK(clock_gettime(CLOCK_REALTIME,&deadline)==0);deadline.tv_sec+=2;
 pthread_mutex_lock(&cached_gate_lock);wait_error=0;
 while(!cached_demand_done && wait_error==0)wait_error=pthread_cond_timedwait(&cached_gate_changed,&cached_gate_lock,&deadline);
 progress=cached_demand_done;cached_backend_release=1;pthread_cond_broadcast(&cached_gate_changed);
 pthread_mutex_unlock(&cached_gate_lock);
 CHECK(pthread_join(backend,NULL)==0 && pthread_join(reader,NULL)==0);
 CHECK(test.completion==0 && test.read_result==1);
 /* One outer transaction pins identity and excludes mutations across copy chunks. */
 CHECK(file_io_begin(test.reader,FILE_IO_PREAD,0,0,&transaction)==0);
 CHECK(transaction.read_object!=NULL && transaction.held_content_read && !transaction.held_inode_io);
 CHECK(file_io_transfer(&transaction,&byte,1)==1 && byte==0);
 CHECK(vm_object_cache_drain(NULL)==0);
 CHECK(vm_object_resize_begin(&fixture.inode,0,&resize)==EBUSY);
 CHECK(vm_object_content_begin(fixture.owner,0,1,NULL,&content)==EBUSY);
 CHECK(file_io_transfer(&transaction,&byte,1)==1 && byte==1);
 file_io_end(&transaction);
 CHECK(vm_object_resize_begin(&fixture.inode,0,&resize)==0);
 vm_object_resize_abort(&resize);
 CHECK(file_close(test.reader)==0 && vm_object_cache_drain(NULL)==1);
 cache_generated=0;close_fixture(&fixture);
 CHECK(progress);
}
