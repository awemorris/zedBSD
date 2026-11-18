/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Process virtual-memory ownership and demand paging.
 *
 * A vmspace owns a sorted list of regions, each backed by anonymous
 * memory, a file, or a shared VM object.  Pages are materialized on
 * demand by vmspace_fault(), which pins the region and marks the page
 * BUSY so that unmap, protect, fork, and the reclaimer wait rather than
 * race the HAL mapping.  Per-page metadata lives in slab pages so that
 * the fault path never depends on the general kernel heap.
 */

#include "kern/vmspace.h"
#include "kern/vm-device.h"
#include "kern/file.h"
#include "kern/kmem.h"
#include "kern/lock.h"
#include "kern/page.h"
#include "kern/sched.h"
#include "kern/swap.h"
#include "kern/vm-commit.h"
#include "kern/vm-lock.h"
#include "kern/vm-object.h"
#include "kern/vm-reclaim.h"
#include <uapi/errno.h>
#include <uapi/mman.h>
#include <kern/kcrt.h>
#include "kern/clock.h"
#include "vmspace-internal.h"

/*
 * The address space the kernel itself runs in.
 *
 * It is statically constructed with a reference that is never dropped, so
 * that early code can map before any allocator is up.
 */
struct vmspace kernel_vmspace = {
	.space = HAL_SPACE_SYS,
	.refs = { 1 },
};

/*
 * Guards the reap queue and the reaper callback below.
 */
static struct spinlock vmspace_reap_lock;

/*
 * The head of the queue of address spaces waiting to be torn down.
 *
 * A final reference is dropped from scheduler context, which may not sleep,
 * so the address space is queued here and the reaper finishes it.
 */
static struct vmspace *vmspace_reap_head;

/*
 * The tail of that queue, so an enqueue is a constant-time append.
 */
static struct vmspace *vmspace_reap_tail;

/*
 * The address layout this platform gives every user address space.
 *
 * It is filled once, on the first call that needs it, and read without a
 * lock afterwards.
 */
struct vm_layout vm_layout;

/*
 * Whether the layout above has been filled.
 */
static int vm_layout_initialized;

/*
 * How many user address spaces exist.
 *
 * It is raised when one is created and lowered when one is finally reaped,
 * so a shutdown can wait for it to fall to zero.
 */
static atomic_uint_t vmspace_live;

/*
 * The argument handed back to the reaper callback.
 *
 * It is published under the reap lock together with the callback itself, so
 * a queue item that predates registration still reaches the right reaper.
 */
static void *vmspace_reap_notify_argument;

static void (*vmspace_reap_notify)(void *);
static void vmspace_destroy(struct vmspace *vm);
/*
 * Initializes the user address layout and the metadata allocators once.
 */
void
vmspace_layout_init(
	void)
{
	uintptr_t minimum;
	uintptr_t limit;

	if (vm_layout_initialized)
		return;

	/* Initializes the slab and reaper locks. */
	spin_init(&vm_page_slab_lock, LOCK_RANK_VM_OBJECT, "VM page metadata");
	spin_init(&vm_private_page_slab_lock, LOCK_RANK_VM_OBJECT, "VM private metadata");
	spin_init(&vmspace_reap_lock, LOCK_RANK_VMSPACE, "VM space reaper");

	/* Starts every slab list, reap queue and reaper hook out empty. */
	vm_page_slabs = NULL;
	vm_private_page_slabs = NULL;
	vmspace_reap_head = NULL;
	vmspace_reap_tail = NULL;
	vmspace_reap_notify = NULL;
	vmspace_reap_notify_argument = NULL;
	vm_metadata_init();

	/* Takes the user range from the HAL and derives the fixed layout. */
	hal_space_get_user_range(&minimum, &limit);
	if (minimum < PAGE_SIZE ||
	    (minimum & (PAGE_SIZE - 1U)) != 0 ||
	    limit <= minimum ||
	    (limit & (PAGE_SIZE - 1U)) != 0)
		HAL_FATAL("invalid HAL user address range");

	vm_layout.user_minimum = minimum;
	vm_layout.user_limit = limit;

	/* Places the break and the mmap area where this ABI expects them. */
#ifdef KERN_USER_ABI_LP64
	vm_layout.brk_limit = 0x0000000100000000ULL;
	vm_layout.mmap_base = 0x0000000100000000ULL;
#else
	vm_layout.brk_limit = 0x10000000U;
	vm_layout.mmap_base = 0x10000000U;
#endif

	/*
	 * A user range that starts at or above that place (the 32-bit
	 * PowerPC's starts at 1 GiB, above the kernel's RAM) keeps the same
	 * layout from its own start.
	 */
	if (minimum >= vm_layout.brk_limit) {
		vm_layout.brk_limit += minimum;
		vm_layout.mmap_base += minimum;
	}

	if (vm_layout.brk_limit >= limit ||
	    vm_layout.mmap_base >= limit ||
	    limit - minimum <= PAGE_SIZE)
		HAL_FATAL("user address range too small");

	vm_layout.stack_top = limit - PAGE_SIZE;
	vm_layout_initialized = 1;
}

/*
 * Creates an empty vmspace with a fresh HAL address space.
 */
struct vmspace *
vmspace_create(
	void)
{
	struct vmspace *vm;

	/* Allocates the address space record and the layout it uses. */
	vm = kern_calloc(1, sizeof(*vm));
	vmspace_layout_init();
	if (vm == NULL)
		return NULL;

	/* Asks the HAL for the hardware address space. */
	vm->space = hal_space_create();
	if (vm->space == NULL) {
		kern_free(vm);
		return NULL;
	}

	/* Starts with one reference and the maximum limits. */
	refcount_init(&vm->refs, 1);
	(void)mutex_init(&vm->lock, LOCK_RANK_VMSPACE, "VM space");
	waitq_init(&vm->fault_waitq, "VM fault");

	vm->generation = 1;
	vm->address_limit = vmspace_address_cap();
	vm->data_limit = vm->address_limit;
	vm->stack_limit = vm->address_limit;
	(void)atomic_fetch_add_relaxed(&vmspace_live, 1U);

	return vm;
}

/*
 * Takes a reference on a vmspace unless it is already being destroyed.
 */
int
vmspace_tryref(
	struct vmspace *vm)
{
	int acquired;

	if (vm == NULL)
		return 0;

	acquired = refcount_tryget(&vm->refs);

	return acquired;
}

/*
 * Takes a reference on a vmspace.
 */
void
vmspace_ref(
	struct vmspace *vm)
{
	if (vm != NULL)
		refcount_get(&vm->refs);
}

/*
 * Drops a reference on a vmspace, destroying it with the last one.
 */
void
vmspace_put(
	struct vmspace *vm)
{
	if (vm == NULL || vm == &kernel_vmspace)
		return;
	if (refcount_put(&vm->refs))
		vmspace_destroy(vm);
}

/*
 * Drops a reference on a vmspace from a non-sleeping context, handing a
 * final reference to the reaper.
 */
void
vmspace_put_deferred(
	struct vmspace *vm)
{
	void *notify_argument;
	unsigned long irq;
	void (*notify)(void *);

	if (vm == NULL || vm == &kernel_vmspace)
		return;
	if (!refcount_put(&vm->refs))
		return;

	/*
	 * Scheduler retirement is non-sleeping.  Transfer final ownership
	 * to the process reaper instead of entering VFS/object teardown
	 * here.
	 */
	vm->reap_next = NULL;
	irq = spin_lock_irqsave(&vmspace_reap_lock);

	if (vmspace_reap_tail != NULL)
		vmspace_reap_tail->reap_next = vm;
	else
		vmspace_reap_head = vm;
	vmspace_reap_tail = vm;
	notify = vmspace_reap_notify;
	notify_argument = vmspace_reap_notify_argument;

	spin_unlock_irqrestore(&vmspace_reap_lock, irq);

	/*
	 * Enqueue owns the wakeup.  The callback is deliberately invoked
	 * after dropping the queue lock and must only retain a scheduler
	 * notification; final puts can arrive while higher-ranked
	 * VM/reclaim locks are held.
	 */
	if (notify != NULL)
		notify(notify_argument);
}

/*
 * Installs the callback that wakes the reaper when a vmspace is queued.
 */
void
vmspace_set_reaper_notify(
	void (*notify)(void *),
	void *argument)
{
	int pending;
	unsigned long irq;

	/* Publishes the callback and notes whether work already waits. */
	vmspace_layout_init();

	irq = spin_lock_irqsave(&vmspace_reap_lock);

	vmspace_reap_notify = notify;
	vmspace_reap_notify_argument = argument;
	pending = vmspace_reap_head != NULL;

	spin_unlock_irqrestore(&vmspace_reap_lock, irq);

	/*
	 * A queue item may predate registration.  Publish the retained wake
	 * only after the callback and argument have been installed
	 * atomically.
	 */
	if (pending && notify != NULL)
		notify(argument);
}

/*
 * Destroys every vmspace queued for the reaper and counts them.
 */
unsigned
vmspace_reap_pending(
	void)
{
	struct vmspace *list;
	unsigned count;
	unsigned long irq;
	struct vmspace *next;

	count = 0;

	/* Takes the whole queue, then destroys each entry unlocked. */
	irq = spin_lock_irqsave(&vmspace_reap_lock);

	list = vmspace_reap_head;
	vmspace_reap_head = NULL;
	vmspace_reap_tail = NULL;

	spin_unlock_irqrestore(&vmspace_reap_lock, irq);

	/* Destroys every queued address space outside the queue lock. */
	while (list != NULL) {
		next = list->reap_next;
		list->reap_next = NULL;
		vmspace_destroy(list);
		list = next;
		count++;
	}

	return count;
}

/*
 * Reports the number of live user vmspaces.
 */
unsigned
vmspace_count(
	void)
{
	unsigned count;

	count = atomic_load_acquire(&vmspace_live);
	return count;
}

/*
 * Reports the size of the whole user address range.
 */
uint64_t
vmspace_address_cap(
	void)
{
	vmspace_layout_init();
	return (uint64_t)(vm_layout.user_limit - vm_layout.user_minimum);
}

/*
 * Advances the generation, skipping zero; the caller holds the VM lock.
 */
void
vmspace_generation_advance_locked(
	struct vmspace *vm)
{
	vm->generation++;
	if (vm->generation == 0)
		vm->generation++;
}

/* Tears down a vmspace whose last reference is gone. */
static void
vmspace_destroy(
	struct vmspace *vm)
{
	struct vm_region *region;
	struct vm_page *page;

	/* Takes the regions off one at a time, freeing each outside the lock. */
	vm_metadata_enter();

	/* Starts at the first region. */
	region = vm->regions;
	while (region != NULL) {
		/*
		 * Only the reverse mappings are removed under the lock; the
		 * backings and the page records are freed after it, as unmap
		 * does.  The space is not unmapped page by page: it goes whole.
		 */
		vm->regions = region->next;
		for (page = region->pages;
		     page != NULL;
		     page = page->next) {
			vmspace_detach_page(page);
		}

		/* Frees the pages and the region's resources outside the lock. */
		vm_metadata_leave();
		vmspace_release_detached_region_pages(region);
		if (region->file != NULL)
			(void)file_close(region->file);
		if (region->object != NULL)
			vm_object_put(region->object);
		if (region->snapshot != NULL)
			file_exec_snapshot_put(region->snapshot);
		if (region->device != NULL)
			vm_device_put(region->device);
		if (region->commit_size != 0)
			vm_commit_release(region->commit_size);
		vmspace_region_page_index_free(region);
		kern_free(region);

		/* Takes the lock back for the next region. */
		vm_metadata_enter();
		region = vm->regions;
	}

	/* The space goes whole, and one fewer address space lives. */
	hal_space_destroy(vm->space);
	(void)atomic_raw_fetch_add_relaxed(&vmspace_live.value, (unsigned)-1);

	/* Leaves the lock. */
	vm_metadata_leave();

	/* Frees the address space itself. */
	kern_free(vm);
}
