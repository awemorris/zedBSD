#include "kern/kmem.h"
#include "kern/swap.h"
#include "kern/vm-lock.h"
#include "kern/vm-reclaim.h"
#include "kern/vmspace.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

static uint32_t query_flags;
static unsigned unmaps, frees;
static int dirty_during_protect;
static uint8_t swap_data[SWAP_PAGE_SIZE];
static mtx_t object_io_lock;
static cnd_t object_io_condition;
static int block_object_io, object_io_entered, release_object_io;
static mtx_t private_io_lock;
static cnd_t private_io_condition;
static int block_private_io, private_io_entered, release_private_io;
static int fail_private_io;
static unsigned backing_frees;

unsigned vm_test_waitq_sleep_count(void);
void vm_test_waitq_wait_for_sleep(unsigned);

int
vm_object_reclaim_one(void)
{
	if (!block_object_io)
		return ENOMEM;
	/* The object fallback must not inherit either private-VM lock. */
	assert(!vm_metadata_owned());
	assert(mtx_lock(&object_io_lock) == thrd_success);
	object_io_entered = 1;
	assert(cnd_broadcast(&object_io_condition) == thrd_success);
	while (!release_object_io)
		assert(cnd_wait(&object_io_condition, &object_io_lock) ==
		    thrd_success);
	assert(mtx_unlock(&object_io_lock) == thrd_success);
	return 0;
}

struct reclaim_thread_args {
	int result;
};

static int
reclaim_thread(void *opaque)
{
	struct reclaim_thread_args *args = opaque;

	args->result = vm_reclaim_one(NULL);
	return 0;
}

struct backing_wait_thread_args {
	struct vm_private_page *backing;
	int result;
};

static int
backing_wait_thread(void *opaque)
{
	struct backing_wait_thread_args *args = opaque;

	args->result = vm_private_page_io_acquire(args->backing);
	if (args->result == 0)
		vm_private_page_io_release(args->backing);
	vm_private_page_put(args->backing);
	return 0;
}

void *kern_calloc(size_t n, size_t s) { return calloc(n, s); }
void kern_free(void *p) { backing_frees++; free(p); }
void vm_page_free_metadata(struct vm_page *page) { free(page); }
void vmspace_ref(struct vmspace *vm) { refcount_get(&vm->refs); }
void vmspace_put_deferred(struct vmspace *vm)
{ assert(!refcount_put(&vm->refs)); }

int hal_page_query(hal_space_t s, void *v, uint32_t *flags)
{ (void)s; (void)v; *flags = query_flags; return HAL_OK; }
int hal_page_clear_flags(hal_space_t s, void *v, uint32_t flags)
{ (void)s; (void)v; query_flags &= ~flags; return HAL_OK; }
int hal_page_unmap(hal_space_t s, void *v, size_t z)
{ (void)s; (void)v; (void)z; unmaps++; return HAL_OK; }
int hal_page_map(hal_space_t s, void *v, hal_physaddr_t p, size_t z, uint32_t a)
{ (void)s; (void)v; (void)p; (void)z; (void)a; return HAL_OK; }
int hal_page_prot(hal_space_t s, void *v, size_t z, uint32_t a)
{
	(void)s;
	(void)v;
	(void)z;
	(void)a;
	if (dirty_during_protect) {
		dirty_during_protect = 0;
		query_flags |= HAL_PAGE_DIRTY;
	}
	return HAL_OK;
}
int hal_page_prot_query(hal_space_t s, void *v, size_t z, uint32_t a,
	uint32_t *flags)
{
	int error = hal_page_prot(s, v, z, a);

	if (flags != NULL)
		*flags = query_flags;
	return error;
}
void hal_fatal(const char *file, int line, const char *message)
{ fprintf(stderr, "%s:%d: %s\n", file, line, message); abort(); }
int hal_pmem_free(struct hal_pmem *p)
{ free((void *)p->vaddr); memset(p, 0, sizeof(*p)); frees++; return 0; }

static int swap_read(void *d, uint32_t slot, void *page)
{ (void)d; (void)slot; memcpy(page, swap_data, sizeof(swap_data)); return 0; }
static int swap_write(void *d, uint32_t slot, const void *page)
{
	(void)d;
	(void)slot;
	if (block_private_io) {
		assert(mtx_lock(&private_io_lock) == thrd_success);
		private_io_entered = 1;
		assert(cnd_broadcast(&private_io_condition) == thrd_success);
		while (!release_private_io)
			assert(cnd_wait(&private_io_condition, &private_io_lock) ==
			    thrd_success);
		assert(mtx_unlock(&private_io_lock) == thrd_success);
	}
	if (fail_private_io) {
		fail_private_io = 0;
		return EIO;
	}
	memcpy(swap_data, page, sizeof(swap_data));
	return 0;
}

static struct vm_page *make_page(struct vmspace *vm, struct vm_region *region,
				 unsigned flags, uint8_t fill)
{
	struct vm_page *page = calloc(1, sizeof(*page));
	struct vm_private_page *backing = calloc(1, sizeof(*backing));
	assert(page != NULL && backing != NULL);
	vm_private_page_init(backing);
	backing->mapping_count = 1;
	backing->pmem.vaddr = malloc(4096);
	backing->pmem.paddr = (hal_physaddr_t)(uintptr_t)backing->pmem.vaddr &
	    ~(hal_physaddr_t)4095U;
	backing->pmem.size = 4096;
	backing->pmem.type = HAL_PMEM_TYPE_RAM;
	backing->flags = flags | VM_PAGE_RESIDENT;
	backing->swap_slot = SWAP_SLOT_NONE;
	memset(backing->pmem.vaddr, fill, 4096);
	page->address = region->start;
	page->vm = vm;
	page->region = region;
	page->flags = VM_MAPPING_MAPPED;
	page->private_page = backing;
	backing->mappings = page;
	region->pages = page;
	vm_page_track(page);
	return page;
}

static struct vm_page *
share_page(struct vm_page *source, struct vmspace *vm,
	struct vm_region *region)
{
	struct vm_page *copy = calloc(1, sizeof(*copy));
	struct vm_private_page *backing = source->private_page;

	assert(copy != NULL && backing != NULL);
	copy->address = region->start;
	copy->vm = vm;
	copy->region = region;
	copy->flags = VM_MAPPING_MAPPED;
	assert(vm_page_share_private(source, copy) == 0);
	copy->next = region->pages;
	region->pages = copy;
	vm_private_page_operation_end(backing);
	return copy;
}

int main(void)
{
	static const struct swap_backend_ops ops = {
		.read_page = swap_read, .write_page = swap_write,
	};
	struct swap_backend backend;
	struct vmspace vm = { .space = (hal_space_t)1 };
	struct vm_region clean = { .start = 0x400000, .size = 4096,
		.prot = HAL_SPACE_READ, .backing = VM_BACKING_FILE };
	struct vm_region dirty = { .start = 0x500000, .size = 4096,
		.prot = HAL_SPACE_READ | HAL_SPACE_WRITE,
		.backing = VM_BACKING_ANON };
	struct vm_page *page;
	struct vm_reclaim_stats stats;

	assert(mtx_init(&object_io_lock, mtx_plain) == thrd_success);
	assert(cnd_init(&object_io_condition) == thrd_success);
	assert(mtx_init(&private_io_lock, mtx_plain) == thrd_success);
	assert(cnd_init(&private_io_condition) == thrd_success);
	refcount_init(&vm.refs, 1);
	assert(mutex_init(&vm.lock, LOCK_RANK_VMSPACE, "reclaim test VM") == 0);
	waitq_init(&vm.fault_waitq, "reclaim test VM");
	vm_reclaim_init();
	page = make_page(&vm, &clean, 0, 0x11);
	query_flags = HAL_PAGE_PRESENT | HAL_PAGE_ACCESSED;
	assert(vm_reclaim_one(NULL) == 0);
	assert(clean.pages == NULL && unmaps == 1 && frees == 1);
	(void)page;

	swap_init(&backend);
	assert(swap_activate(&backend, &ops, NULL, SWAP_PAGE_SIZE, 2) == 0);
	swap_set_system_backend(&backend);
	page = make_page(&vm, &dirty, VM_PAGE_DIRTY, 0x5a);
	query_flags = HAL_PAGE_PRESENT;
	assert(vm_reclaim_one(NULL) == 0);
	assert((page->private_page->flags & VM_PAGE_SWAPPED) != 0);
	assert((page->private_page->flags & VM_PAGE_RESIDENT) == 0);
	assert(page->private_page->swap_slot != SWAP_SLOT_NONE &&
	    swap_data[123] == 0x5a);
	vm_reclaim_get_stats(&stats);
	assert(stats.page_outs == 1 && stats.swapped == 1);
	vm_page_untrack(page);
	vm_page_free_metadata(page);

	/*
	 * A final userspace write after candidate selection but before the
	 * write-protect shootdown must turn a planned file discard into swap-out.
	 */
	{
		struct vm_region late_dirty = {
			.start = 0x580000, .size = 4096,
			.prot = HAL_SPACE_READ | HAL_SPACE_WRITE,
			.backing = VM_BACKING_FILE
		};

		page = make_page(&vm, &late_dirty, 0, 0x4c);
		query_flags = HAL_PAGE_PRESENT;
		dirty_during_protect = 1;
		assert(vm_reclaim_one(NULL) == 0);
		assert(late_dirty.pages == page);
		assert((page->private_page->flags & VM_PAGE_SWAPPED) != 0 &&
		    swap_data[321] == 0x4c);
		vm_page_untrack(page);
		vm_page_free_metadata(page);
	}

	/* Swap-write failure restores every PTE/protection and releases all pins. */
	{
		struct vm_region io_error = {
			.start = 0x590000, .size = 4096,
			.prot = HAL_SPACE_READ | HAL_SPACE_WRITE,
			.backing = VM_BACKING_ANON
		};
		struct vm_private_page *backing;

		page = make_page(&vm, &io_error, VM_PAGE_DIRTY, 0x3e);
		backing = page->private_page;
		query_flags = HAL_PAGE_PRESENT;
		fail_private_io = 1;
		assert(vm_reclaim_one(NULL) == EIO);
		assert(io_error.pages == page && page->private_page == backing);
		assert((backing->flags & VM_PAGE_RESIDENT) != 0 &&
		    (backing->flags & (VM_PAGE_BUSY | VM_PAGE_SWAPPED)) == 0);
		assert((page->flags & (VM_MAPPING_MAPPED |
		    VM_MAPPING_RECLAIM_UNMAPPED |
		    VM_MAPPING_RECLAIM_PROTECTED)) == VM_MAPPING_MAPPED);
		assert(io_error.hold_count == 0 &&
		    refcount_load(&backing->refs) == 1);
		assert(vm_reclaim_one(NULL) == 0);
		assert((backing->flags & VM_PAGE_SWAPPED) != 0);
		vm_page_untrack(page);
		vm_page_free_metadata(page);
	}

	/*
	 * Reclaim owns a fork-shared backing once, but performs PTE shootdown and
	 * swap I/O with neither global metadata nor either vmspace lock held.  A
	 * simultaneous fault waits on the backing owner and resumes on the single
	 * committed SWAPPED state.
	 */
	{
		struct vmspace peer = { .space = (hal_space_t)2 };
		struct vm_region first_region = {
			.start = 0x600000, .size = 4096,
			.prot = HAL_SPACE_READ | HAL_SPACE_WRITE,
			.backing = VM_BACKING_ANON
		};
		struct vm_region second_region = {
			.start = 0x700000, .size = 4096,
			.prot = HAL_SPACE_READ | HAL_SPACE_WRITE,
			.backing = VM_BACKING_ANON
		};
		struct vm_page *first_page, *second_page;
		struct vm_private_page *backing;
		struct reclaim_thread_args reclaim = { -1 };
		struct backing_wait_thread_args waiter;
		struct hal_pmem pinned;
		thrd_t reclaim_handle, waiter_handle;
		unsigned before_sleeps;

		refcount_init(&peer.refs, 1);
		assert(mutex_init(&peer.lock, LOCK_RANK_VMSPACE,
		    "reclaim peer VM") == 0);
		waitq_init(&peer.fault_waitq, "reclaim peer VM");
		first_page = make_page(&vm, &first_region, VM_PAGE_DIRTY, 0x7b);
		second_page = share_page(first_page, &peer, &second_region);
		backing = first_page->private_page;
		assert(second_page->private_page == backing &&
		    backing->mapping_count == 2);

		query_flags = HAL_PAGE_PRESENT;
		block_private_io = 1;
		private_io_entered = release_private_io = 0;
		assert(thrd_create(&reclaim_handle, reclaim_thread, &reclaim) ==
		    thrd_success);
		assert(mtx_lock(&private_io_lock) == thrd_success);
		while (!private_io_entered)
			assert(cnd_wait(&private_io_condition, &private_io_lock) ==
			    thrd_success);
		assert(mtx_unlock(&private_io_lock) == thrd_success);

		/* These acquisitions would deadlock if reclaim slept under them. */
		vm_metadata_enter();
		mutex_lock(&vm.lock);
		mutex_unlock(&vm.lock);
		mutex_lock(&peer.lock);
		mutex_unlock(&peer.lock);
		vm_metadata_leave();
		{
			unsigned before_refs = refcount_load(&backing->refs);

			assert(vm_private_page_pin(backing, &pinned) == EBUSY);
			assert(refcount_load(&backing->refs) == before_refs);
		}

		vm_private_page_ref(backing);
		waiter = (struct backing_wait_thread_args){ backing, -1 };
		before_sleeps = vm_test_waitq_sleep_count();
		assert(thrd_create(&waiter_handle, backing_wait_thread, &waiter) ==
		    thrd_success);
		vm_test_waitq_wait_for_sleep(before_sleeps + 1U);

		assert(mtx_lock(&private_io_lock) == thrd_success);
		release_private_io = 1;
		assert(cnd_broadcast(&private_io_condition) == thrd_success);
		assert(mtx_unlock(&private_io_lock) == thrd_success);
		assert(thrd_join(reclaim_handle, NULL) == thrd_success);
		assert(thrd_join(waiter_handle, NULL) == thrd_success);
		block_private_io = 0;
		assert(reclaim.result == 0 && waiter.result == 0);
		assert((backing->flags & VM_PAGE_SWAPPED) != 0 &&
		    (backing->flags & (VM_PAGE_RESIDENT | VM_PAGE_BUSY)) == 0);
		assert((first_page->flags &
		    (VM_MAPPING_MAPPED | VM_MAPPING_BUSY)) == 0);
		assert((second_page->flags &
		    (VM_MAPPING_MAPPED | VM_MAPPING_BUSY)) == 0);
		assert(first_region.hold_count == 0 &&
		    second_region.hold_count == 0);
		vm_page_untrack(first_page);
		vm_page_free_metadata(first_page);
		vm_page_untrack(second_page);
		vm_page_free_metadata(second_page);
		assert(refcount_load(&vm.refs) == 1 &&
		    refcount_load(&peer.refs) == 1);
	}

	/* A waiter owns a lifetime ref even after the last non-waiting hold drops. */
	{
		struct vm_private_page *backing = calloc(1, sizeof(*backing));
		struct backing_wait_thread_args waiter;
		thrd_t waiter_handle;
		unsigned before_sleeps = vm_test_waitq_sleep_count();
		unsigned before_frees = backing_frees;

		assert(backing != NULL);
		vm_private_page_init(backing);
		assert(vm_private_page_io_acquire(backing) == 0);
		vm_private_page_ref(backing);
		waiter = (struct backing_wait_thread_args){ backing, -1 };
		assert(thrd_create(&waiter_handle, backing_wait_thread, &waiter) ==
		    thrd_success);
		vm_test_waitq_wait_for_sleep(before_sleeps + 1U);
		vm_private_page_io_release(backing);
		vm_private_page_put(backing);
		assert(thrd_join(waiter_handle, NULL) == thrd_success);
		assert(waiter.result == 0 && backing_frees == before_frees + 1U);
	}
	assert(swap_shutdown(&backend) == 0);

	/* Object I/O runs after the private queue and metadata locks are gone. */
	{
		struct reclaim_thread_args args = { -1 };
		thrd_t worker;

		block_object_io = 1;
		object_io_entered = release_object_io = 0;
		assert(thrd_create(&worker, reclaim_thread, &args) == thrd_success);
		assert(mtx_lock(&object_io_lock) == thrd_success);
		while (!object_io_entered)
			assert(cnd_wait(&object_io_condition, &object_io_lock) ==
			    thrd_success);
		assert(mtx_unlock(&object_io_lock) == thrd_success);

		/* This acquisition would deadlock if fallback still held metadata. */
		vm_metadata_enter();
		vm_metadata_leave();

		assert(mtx_lock(&object_io_lock) == thrd_success);
		release_object_io = 1;
		assert(cnd_broadcast(&object_io_condition) == thrd_success);
		assert(mtx_unlock(&object_io_lock) == thrd_success);
		assert(thrd_join(worker, NULL) == thrd_success);
		block_object_io = 0;
		assert(args.result == 0);
	}
	puts("zedBSD VM reclaim/swap host tests: PASS");
	return 0;
}
