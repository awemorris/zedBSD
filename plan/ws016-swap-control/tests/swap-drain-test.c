/* SWAP-T005: production swap-manager and VM-drain host fixture. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include <kern/kmem.h>
#include <kern/lock.h>
#include <kern/swap.h>
#include <kern/vm-lock.h>
#include <kern/vm-reclaim.h>
#include <kern/vmspace.h>

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TEST_SOURCE_ID 2U
#define TEST_SOURCE_SLOTS 4U
#define HOST_LOCK_COUNT 64U
#define HOST_QUEUE_COUNT 64U

struct host_lock {
	const void *key;
	pthread_mutex_t mutex;
};

struct host_queue {
	const void *key;
	pthread_cond_t condition;
};

struct fake_source {
	uint8_t page[TEST_SOURCE_SLOTS][SWAP_PAGE_SIZE];
	volatile int read_error;
	unsigned reads;
	unsigned writes;
	unsigned flushes;
	unsigned destroys;
};

struct test_mapping {
	struct vm_private_page backing;
	struct vm_page page;
	struct vm_region region;
	struct vmspace vm;
};

struct drain_thread {
	unsigned source_id;
	int result;
};

static pthread_mutex_t registry_lock = PTHREAD_MUTEX_INITIALIZER;
static struct host_lock host_locks[HOST_LOCK_COUNT];
static struct host_queue host_queues[HOST_QUEUE_COUNT];
static pthread_once_t metadata_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t metadata_mutex;
static _Thread_local unsigned metadata_depth;
static volatile unsigned yield_count;
static volatile unsigned wait_sleep_count;
static volatile unsigned interruptible_wait_error;

static void
host_fail(const char *message)
{
	fprintf(stderr, "SWAP-T005 host failure: %s\n", message);
	abort();
}

static void
metadata_mutex_init(void)
{
	pthread_mutexattr_t attributes;

	assert(pthread_mutexattr_init(&attributes) == 0);
	assert(pthread_mutexattr_settype(&attributes,
	    PTHREAD_MUTEX_RECURSIVE) == 0);
	assert(pthread_mutex_init(&metadata_mutex, &attributes) == 0);
	assert(pthread_mutexattr_destroy(&attributes) == 0);
}

static struct host_lock *
host_lock_for(const void *key)
{
	struct host_lock *free_entry = NULL;
	unsigned index;

	assert(key != NULL);
	assert(pthread_mutex_lock(&registry_lock) == 0);
	for (index = 0; index < HOST_LOCK_COUNT; index++) {
		if (host_locks[index].key == key) {
			assert(pthread_mutex_unlock(&registry_lock) == 0);
			return &host_locks[index];
		}
		if (free_entry == NULL && host_locks[index].key == NULL)
			free_entry = &host_locks[index];
	}
	if (free_entry == NULL)
		host_fail("host lock registry exhausted");
	{
		pthread_mutexattr_t attributes;

		assert(pthread_mutexattr_init(&attributes) == 0);
		assert(pthread_mutexattr_settype(&attributes,
		    PTHREAD_MUTEX_RECURSIVE) == 0);
		assert(pthread_mutex_init(&free_entry->mutex, &attributes) == 0);
		assert(pthread_mutexattr_destroy(&attributes) == 0);
	}
	free_entry->key = key;
	assert(pthread_mutex_unlock(&registry_lock) == 0);
	return free_entry;
}

static struct host_queue *
host_queue_for(const void *key)
{
	struct host_queue *free_entry = NULL;
	unsigned index;

	assert(key != NULL);
	assert(pthread_mutex_lock(&registry_lock) == 0);
	for (index = 0; index < HOST_QUEUE_COUNT; index++) {
		if (host_queues[index].key == key) {
			assert(pthread_mutex_unlock(&registry_lock) == 0);
			return &host_queues[index];
		}
		if (free_entry == NULL && host_queues[index].key == NULL)
			free_entry = &host_queues[index];
	}
	if (free_entry == NULL)
		host_fail("host wait-queue registry exhausted");
	assert(pthread_cond_init(&free_entry->condition, NULL) == 0);
	free_entry->key = key;
	assert(pthread_mutex_unlock(&registry_lock) == 0);
	return free_entry;
}

void *
kern_malloc(size_t size)
{
	return malloc(size);
}

void *
kern_calloc(size_t count, size_t size)
{
	return calloc(count, size);
}

void
kern_free(void *pointer)
{
	free(pointer);
}

void
spin_init(struct spinlock *lock, enum lock_rank rank, const char *name)
{
	assert(lock != NULL);
	(void)host_lock_for(lock);
	lock->rank = rank;
	lock->name = name;
}

void
spin_lock(struct spinlock *lock)
{
	assert(pthread_mutex_lock(&host_lock_for(lock)->mutex) == 0);
}

int
spin_trylock(struct spinlock *lock)
{
	return pthread_mutex_trylock(&host_lock_for(lock)->mutex) == 0;
}

void
spin_unlock(struct spinlock *lock)
{
	assert(pthread_mutex_unlock(&host_lock_for(lock)->mutex) == 0);
}

unsigned long
spin_lock_irqsave(struct spinlock *lock)
{
	spin_lock(lock);
	return 0;
}

void
spin_unlock_irqrestore(struct spinlock *lock, unsigned long enabled)
{
	(void)enabled;
	spin_unlock(lock);
}

void
waitq_init(struct wait_queue *queue, const char *name)
{
	assert(queue != NULL);
	(void)host_queue_for(queue);
	queue->head = NULL;
	queue->tail = NULL;
	queue->sequence = 1;
	queue->name = name;
}

uint64_t
waitq_sequence(const struct wait_queue *queue)
{
	return queue->sequence;
}

int
waitq_sleep(struct wait_queue *queue, struct spinlock *condition_lock,
	uint64_t observed, uint64_t deadline, unsigned flags)
{
	struct host_lock *lock = host_lock_for(condition_lock);
	struct host_queue *host = host_queue_for(queue);

	(void)deadline;
	if (queue->sequence != observed)
		return EAGAIN;
	if ((flags & WAITQ_INTERRUPTIBLE) != 0 &&
	    __atomic_exchange_n(&interruptible_wait_error, 0U,
		__ATOMIC_SEQ_CST) != 0)
		return EINTR;
	(void)__atomic_add_fetch(&wait_sleep_count, 1U, __ATOMIC_SEQ_CST);
	assert(pthread_cond_wait(&host->condition, &lock->mutex) == 0);
	return 0;
}

void
waitq_wake_one(struct wait_queue *queue)
{
	queue->sequence++;
	assert(pthread_cond_signal(&host_queue_for(queue)->condition) == 0);
}

void
waitq_wake_all(struct wait_queue *queue)
{
	queue->sequence++;
	assert(pthread_cond_broadcast(&host_queue_for(queue)->condition) == 0);
}

int
mutex_init(struct mutex *mutex, enum lock_rank rank, const char *name)
{
	assert(mutex != NULL);
	memset(mutex, 0, sizeof(*mutex));
	spin_init(&mutex->guard, rank, name);
	waitq_init(&mutex->waiters, name);
	return 0;
}

void
mutex_lock(struct mutex *mutex)
{
	spin_lock(&mutex->guard);
	mutex->locked = 1;
}

void
mutex_unlock(struct mutex *mutex)
{
	mutex->locked = 0;
	spin_unlock(&mutex->guard);
}

int
mutex_trylock(struct mutex *mutex)
{
	if (!spin_trylock(&mutex->guard))
		return 0;
	mutex->locked = 1;
	return 1;
}

int
mutex_owned(struct mutex *mutex)
{
	return mutex != NULL && mutex->locked != 0;
}

int
mutex_lock_interruptible(struct mutex *mutex)
{
	mutex_lock(mutex);
	return 0;
}

int
mutex_wait(struct mutex *mutex, struct wait_queue *condition,
	uint64_t observed, uint64_t deadline, unsigned flags)
{
	return waitq_sleep(condition, &mutex->guard, observed, deadline, flags);
}

void
vm_metadata_init(void)
{
	assert(pthread_once(&metadata_once, metadata_mutex_init) == 0);
}

void
vm_metadata_enter(void)
{
	vm_metadata_init();
	assert(pthread_mutex_lock(&metadata_mutex) == 0);
	metadata_depth++;
}

void
vm_metadata_leave(void)
{
	assert(metadata_depth != 0);
	metadata_depth--;
	assert(pthread_mutex_unlock(&metadata_mutex) == 0);
}

int
vm_metadata_owned(void)
{
	return metadata_depth != 0;
}

int
sched_yield(void)
{
	const struct timespec delay = { .tv_sec = 0, .tv_nsec = 100000L };

	(void)__atomic_add_fetch(&yield_count, 1U, __ATOMIC_SEQ_CST);
	(void)nanosleep(&delay, NULL);
	return 0;
}

void
hal_fatal(const char *file, int line, const char *message)
{
	fprintf(stderr, "HAL_FATAL %s:%d: %s\n", file, line, message);
	abort();
}

int
hal_pmem_alloc(const struct hal_pmem_request *request, struct hal_pmem *memory)
{
	void *allocation;

	assert(request != NULL && memory != NULL);
	allocation = aligned_alloc(request->alignment, request->size);
	if (allocation == NULL)
		return HAL_ERR_NOMEM;
	memset(allocation, 0, request->size);
	memory->vaddr = allocation;
	memory->paddr = (hal_physaddr_t)(uintptr_t)allocation;
	memory->size = request->size;
	memory->type = request->type;
	memory->attr = request->attr;
	return HAL_OK;
}

int
hal_pmem_free(struct hal_pmem *memory)
{
	assert(memory != NULL);
	free(memory->vaddr);
	memset(memory, 0, sizeof(*memory));
	return HAL_OK;
}

int
hal_page_query(hal_space_t space, void *address, uint32_t *flags)
{
	(void)space;
	(void)address;
	if (flags != NULL)
		*flags = 0;
	return HAL_OK;
}

int
hal_page_prot_query(hal_space_t space, void *address, size_t size,
	uint32_t attr, uint32_t *flags)
{
	(void)space;
	(void)address;
	(void)size;
	(void)attr;
	if (flags != NULL)
		*flags = 0;
	return HAL_OK;
}

int
hal_page_map(hal_space_t space, void *address, hal_physaddr_t physical,
	size_t size, uint32_t attr)
{
	(void)space;
	(void)address;
	(void)physical;
	(void)size;
	(void)attr;
	return HAL_OK;
}

int
hal_page_prot(hal_space_t space, void *address, size_t size, uint32_t attr)
{
	(void)space;
	(void)address;
	(void)size;
	(void)attr;
	return HAL_OK;
}

int
hal_page_unmap(hal_space_t space, void *address, size_t size)
{
	(void)space;
	(void)address;
	(void)size;
	return HAL_OK;
}

void
vmspace_ref(struct vmspace *vm)
{
	assert(vm != NULL);
}

void
vmspace_put_deferred(struct vmspace *vm)
{
	assert(vm != NULL);
}

void
vm_private_page_free_metadata(struct vm_private_page *backing)
{
	assert(backing != NULL);
}

void
vm_page_free_metadata(struct vm_page *page)
{
	assert(page != NULL);
}

static int
fake_read(void *argument, uint32_t slot, void *page)
{
	struct fake_source *source = argument;

	assert(source != NULL && page != NULL && slot < TEST_SOURCE_SLOTS);
	source->reads++;
	if (source->read_error != 0)
		return source->read_error;
	memcpy(page, source->page[slot], SWAP_PAGE_SIZE);
	return 0;
}

static int
fake_write(void *argument, uint32_t slot, const void *page)
{
	struct fake_source *source = argument;

	assert(source != NULL && page != NULL && slot < TEST_SOURCE_SLOTS);
	source->writes++;
	memcpy(source->page[slot], page, SWAP_PAGE_SIZE);
	return 0;
}

static int
fake_flush(void *argument)
{
	struct fake_source *source = argument;

	assert(source != NULL);
	source->flushes++;
	return 0;
}

static void
fake_destroy(void *argument)
{
	struct fake_source *source = argument;

	assert(source != NULL);
	source->destroys++;
}

static const struct swap_backend_ops fake_ops = {
	.read_page = fake_read,
	.write_page = fake_write,
	.flush = fake_flush,
	.destroy = fake_destroy,
};

static void
mapping_init(struct test_mapping *mapping, uint32_t slot)
{
	memset(mapping, 0, sizeof(*mapping));
	vm_private_page_init(&mapping->backing);
	assert(mutex_init(&mapping->vm.lock, LOCK_RANK_VMSPACE,
	    "SWAP-T005 vmspace") == 0);
	waitq_init(&mapping->vm.fault_waitq, "SWAP-T005 fault");
	mapping->region.prot = HAL_SPACE_READ | HAL_SPACE_WRITE;
	mapping->page.vm = &mapping->vm;
	mapping->page.region = &mapping->region;
	mapping->page.private_page = &mapping->backing;
	mapping->backing.mapping_count = 1;
	mapping->backing.mappings = &mapping->page;
	mapping->backing.flags = VM_PAGE_SWAPPED;
	mapping->backing.swap_slot = slot;
	vm_page_track(&mapping->page);
}

static void
mapping_destroy(struct test_mapping *mapping)
{
	vm_page_untrack(&mapping->page);
	assert(mapping->page.private_page == NULL);
}

static uint32_t
source_fill(struct swap_backend *backend, uint8_t pattern)
{
	uint8_t page[SWAP_PAGE_SIZE];
	uint32_t slot;

	memset(page, pattern, sizeof(page));
	assert(swap_alloc_slot(backend, &slot) == 0);
	assert(swap_write_page(backend, slot, page) == 0);
	return slot;
}

static void
assert_resident_pattern(const struct test_mapping *mapping, uint8_t pattern)
{
	const uint8_t *page = mapping->backing.pmem.vaddr;
	unsigned index;

	assert((mapping->backing.flags & VM_PAGE_SWAPPED) == 0);
	assert((mapping->backing.flags &
	    (VM_PAGE_RESIDENT | VM_PAGE_DIRTY)) ==
	    (VM_PAGE_RESIDENT | VM_PAGE_DIRTY));
	assert(mapping->backing.swap_slot == SWAP_SLOT_NONE);
	assert(mapping->backing.pmem.size == SWAP_PAGE_SIZE && page != NULL);
	for (index = 0; index < SWAP_PAGE_SIZE; index++)
		assert(page[index] == pattern);
}

static void
assert_source_empty(struct swap_backend *backend)
{
	struct swap_source_stats stats;

	assert(swap_source_get_stats(backend, TEST_SOURCE_ID, &stats) == 0);
	assert(stats.state == SWAP_SOURCE_STATE_DRAINING);
	assert(stats.allocated_slots == 0 && stats.inflight == 0);
}

static void
source_install(struct swap_backend *backend, struct fake_source *source,
	int publish)
{
	memset(source, 0, sizeof(*source));
	assert(swap_source_add(backend, TEST_SOURCE_ID, &fake_ops, source,
	    SWAP_PAGE_SIZE, TEST_SOURCE_SLOTS) == 0);
	if (publish)
		assert(swap_set_system_backend(backend) == 0);
}

static void
source_remove(struct swap_backend *backend, struct fake_source *source)
{
	assert(swap_source_remove(backend, TEST_SOURCE_ID) == 0);
	assert(source->flushes == 1 && source->destroys == 1);
}

static void
test_successful_page_in(struct swap_backend *backend)
{
	struct fake_source source;
	struct test_mapping mapping;
	uint32_t slot;

	source_install(backend, &source, 1);
	slot = source_fill(backend, 0x5aU);
	assert((slot >> SWAP_SLOT_SOURCE_SHIFT) == TEST_SOURCE_ID);
	mapping_init(&mapping, slot);
	assert(swap_source_begin_drain(backend, TEST_SOURCE_ID) == 0);
	assert(vm_reclaim_drain_swap_source(TEST_SOURCE_ID) == 0);
	assert_resident_pattern(&mapping, 0x5aU);
	assert_source_empty(backend);
	source_remove(backend, &source);
	mapping_destroy(&mapping);
}

static void
test_read_error_and_retry(struct swap_backend *backend)
{
	struct fake_source source;
	struct swap_source_stats stats;
	struct test_mapping mapping;
	uint32_t slot;

	source_install(backend, &source, 0);
	slot = source_fill(backend, 0xa5U);
	mapping_init(&mapping, slot);
	source.read_error = EIO;
	assert(swap_source_begin_drain(backend, TEST_SOURCE_ID) == 0);
	assert(vm_reclaim_drain_swap_source(TEST_SOURCE_ID) == EIO);
	assert((mapping.backing.flags & VM_PAGE_SWAPPED) != 0);
	assert((mapping.backing.flags & VM_PAGE_RESIDENT) == 0);
	assert(mapping.backing.swap_slot == slot && mapping.backing.pmem.size == 0);
	assert(swap_source_get_stats(backend, TEST_SOURCE_ID, &stats) == 0);
	assert(stats.state == SWAP_SOURCE_STATE_DRAINING &&
	    stats.allocated_slots == 1 && stats.inflight == 0);
	assert(swap_source_abort_drain(backend, TEST_SOURCE_ID) == 0);
	source.read_error = 0;
	assert(swap_source_begin_drain(backend, TEST_SOURCE_ID) == 0);
	assert(vm_reclaim_drain_swap_source(TEST_SOURCE_ID) == 0);
	assert_resident_pattern(&mapping, 0xa5U);
	assert_source_empty(backend);
	source_remove(backend, &source);
	mapping_destroy(&mapping);
}

static void *
drain_main(void *argument)
{
	struct drain_thread *thread = argument;

	thread->result = vm_reclaim_drain_swap_source(thread->source_id);
	return NULL;
}

static void
wait_for_counter(volatile unsigned *counter, unsigned baseline,
	const char *message)
{
	const struct timespec delay = { .tv_sec = 0, .tv_nsec = 1000000L };
	unsigned attempt;

	for (attempt = 0; attempt < 2000U; attempt++) {
		if (__atomic_load_n(counter, __ATOMIC_SEQ_CST) > baseline)
			return;
		(void)nanosleep(&delay, NULL);
	}
	host_fail(message);
}

static void
test_allocator_publication_window(struct swap_backend *backend)
{
	struct fake_source source;
	struct test_mapping mapping;
	struct drain_thread drain = { .source_id = TEST_SOURCE_ID, .result = -1 };
	pthread_t worker;
	uint32_t slot;
	unsigned baseline;

	source_install(backend, &source, 0);
	/* The allocated token intentionally has no VM backing yet. */
	slot = source_fill(backend, 0x3cU);
	assert(swap_source_begin_drain(backend, TEST_SOURCE_ID) == 0);
	baseline = __atomic_load_n(&yield_count, __ATOMIC_SEQ_CST);
	assert(pthread_create(&worker, NULL, drain_main, &drain) == 0);
	wait_for_counter(&yield_count, baseline,
	    "drain did not observe the allocation/publication window");
	mapping_init(&mapping, slot);
	assert(pthread_join(worker, NULL) == 0);
	assert(drain.result == 0);
	assert_resident_pattern(&mapping, 0x3cU);
	assert_source_empty(backend);
	source_remove(backend, &source);
	mapping_destroy(&mapping);
}

static void
test_io_owner_wait(struct swap_backend *backend)
{
	struct fake_source source;
	struct test_mapping mapping;
	struct drain_thread drain = { .source_id = TEST_SOURCE_ID, .result = -1 };
	pthread_t worker;
	uint32_t slot;
	unsigned baseline;

	source_install(backend, &source, 0);
	slot = source_fill(backend, 0xc3U);
	mapping_init(&mapping, slot);
	assert(vm_private_page_io_try_acquire(&mapping.backing) == 0);
	assert(swap_source_begin_drain(backend, TEST_SOURCE_ID) == 0);
	baseline = __atomic_load_n(&wait_sleep_count, __ATOMIC_SEQ_CST);
	assert(pthread_create(&worker, NULL, drain_main, &drain) == 0);
	wait_for_counter(&wait_sleep_count, baseline,
	    "drain did not wait for the existing I/O owner");
	vm_private_page_io_release(&mapping.backing);
	vm_private_page_put(&mapping.backing);
	assert(pthread_join(worker, NULL) == 0);
	assert(drain.result == 0);
	assert_resident_pattern(&mapping, 0xc3U);
	assert_source_empty(backend);
	source_remove(backend, &source);
	mapping_destroy(&mapping);
}

static int
cancel_never(void *argument)
{
	(void)argument;
	return 0;
}

static void
test_interruptible_io_owner_wait(struct swap_backend *backend)
{
	struct fake_source source;
	struct swap_source_stats stats;
	struct test_mapping mapping;
	uint32_t slot;

	source_install(backend, &source, 0);
	slot = source_fill(backend, 0x69U);
	mapping_init(&mapping, slot);
	assert(vm_private_page_io_try_acquire(&mapping.backing) == 0);
	assert(swap_source_begin_drain(backend, TEST_SOURCE_ID) == 0);
	__atomic_store_n(&interruptible_wait_error, 1U, __ATOMIC_SEQ_CST);
	assert(vm_reclaim_drain_swap_source_cancelable(TEST_SOURCE_ID,
	    cancel_never, NULL) == EINTR);
	assert(swap_source_get_stats(backend, TEST_SOURCE_ID, &stats) == 0);
	assert(stats.state == SWAP_SOURCE_STATE_DRAINING &&
	    stats.allocated_slots == 1U);
	assert((mapping.backing.flags & VM_PAGE_SWAPPED) != 0 &&
	    mapping.backing.swap_slot == slot);
	vm_private_page_io_release(&mapping.backing);
	vm_private_page_put(&mapping.backing);
	assert(vm_reclaim_drain_swap_source(TEST_SOURCE_ID) == 0);
	assert_resident_pattern(&mapping, 0x69U);
	assert_source_empty(backend);
	source_remove(backend, &source);
	mapping_destroy(&mapping);
}

int
main(void)
{
	struct swap_backend backend;

	swap_init(&backend);
	vm_reclaim_init();
	test_successful_page_in(&backend);
	test_read_error_and_retry(&backend);
	test_allocator_publication_window(&backend);
	test_io_owner_wait(&backend);
	test_interruptible_io_owner_wait(&backend);
	assert(swap_shutdown(&backend) == 0);
	puts("SWAP-T005: production VM source drain: PASS");
	return 0;
}
