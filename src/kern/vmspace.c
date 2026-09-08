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
#include "kern/file.h"
#include "kern/kmem.h"
#include "kern/lock.h"
#include "kern/page.h"
#include "kern/swap.h"
#include "kern/vm-commit.h"
#include "kern/vm-lock.h"
#include "kern/vm-object.h"
#include "kern/vm-reclaim.h"
#include <errno.h>
#include <string.h>
#include <sys/mman.h>

#define PAGE_SIZE			ZEDBSD_PAGE_SIZE
#define VM_PAGE_SLAB_SLOTS		32U
#define VM_PRIVATE_PAGE_SLAB_SLOTS	24U
#define VM_PRIVATE_PAGE_SLAB_FREE_MASK	((1U << VM_PRIVATE_PAGE_SLAB_SLOTS) - 1U)
#define VM_PAGE_MAP_RECLAIM_RETRIES	4U
#define VM_PAGE_MAP_RESERVE_PAGES	4U

/*
 * One page of storage carved into mapping descriptors.
 *
 * Descriptors are handed out from a slab's free mask rather than allocated
 * one at a time, so a mapping can be created on a fault path that must not
 * enter the general allocator.  A slab stays on the list for as long as it
 * holds any live slot.
 */
struct vm_page_slab {
	struct hal_pmem memory;
	struct vm_page_slab *next;
	uint32_t free_mask;
	unsigned used;
	struct vm_page slots[VM_PAGE_SLAB_SLOTS];
};

/*
 * One page of storage carved into private page backings.
 *
 * It works exactly like the mapping slab above, but for the anonymous
 * backings that reclaim and copy-on-write hand around.
 */
struct vm_private_page_slab {
	struct hal_pmem memory;
	struct vm_private_page_slab *next;
	uint32_t free_mask;
	unsigned used;
	struct vm_private_page slots[VM_PRIVATE_PAGE_SLAB_SLOTS];
};

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
 * Guards the mapping slab list below.
 *
 * It is a spinlock rather than a mutex because descriptors are taken on
 * fault paths that must not sleep.
 */
static struct spinlock vm_page_slab_lock;

/*
 * The mapping slabs, in most-recently-added order.
 *
 * Slabs are added when no slot is free and are never returned to the
 * system, because the descriptors they hold outlive any single mapping.
 */
static struct vm_page_slab *vm_page_slabs;

/*
 * Guards the private page slab list below.
 */
static struct spinlock vm_private_page_slab_lock;

/*
 * The private page slabs, in most-recently-added order.
 *
 * They live for the lifetime of the kernel, on the same grounds as the
 * mapping slabs above.
 */
static struct vm_private_page_slab *vm_private_page_slabs;

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

/*
 * Test checkpoints.
 *
 * The test build defines these to observe a step that leaves no other
 * trace.  They are weak so that a production kernel links without them and
 * every call site is a null test the compiler removes.
 */
extern void vmspace_unmap_retire_checkpoint(struct vmspace *vm, uintptr_t start, size_t size) __attribute__((weak));
extern void vmspace_pin_page_checkpoint(struct vmspace *vm, size_t index, size_t page_count) __attribute__((weak));
extern void vmspace_object_revoke_checkpoint(struct vmspace *vm, uintptr_t address) __attribute__((weak));

static void (*vmspace_reap_notify)(void *);
static int alloc_vm_page(struct hal_pmem *memory);
static int alloc_vm_metadata_page(struct hal_pmem *memory);
static struct vm_page * vm_page_slab_take_locked(void);
static struct vm_private_page * vm_private_page_slab_take_locked(void);
static struct vm_private_page * vm_private_page_alloc_metadata(void);
static int range_valid(uintptr_t start, size_t size);
static int overlaps(struct vmspace *vm, uintptr_t start, size_t size);
static void insert_region(struct vmspace *vm, struct vm_region *region);
static unsigned reclaim_page_map_reserve(struct vm_page *avoid);
static struct vm_private_page * private_page_alloc(void);
static void private_page_attach_new(struct vm_page *page, struct vm_private_page *backing);
static int vmspace_exec_cache_fault(struct vmspace *vm, struct vm_region *region, struct vm_page *page, uint32_t required);
static int vmspace_fork_locked(struct vmspace *source, struct vmspace **result, struct vm_private_page **wait_backing, struct vmspace **failed_copy);
static int map_region(struct vmspace *vm, uintptr_t start, size_t size, uint32_t prot, enum vm_region_backing backing, struct file *file, off_t file_offset, uintptr_t data_start, size_t data_size, unsigned flags, size_t commit_size, struct vm_region **result);
static int prepare_region(uintptr_t start, size_t size, uint32_t prot, enum vm_region_backing backing, struct file *file, off_t file_offset, uintptr_t data_start, size_t data_size, unsigned flags, size_t commit_size, struct vm_region **result);
static void discard_prepared_region(struct vm_region *region);
static int vmspace_map_anon_locked(struct vmspace *vm, uintptr_t start, size_t size, uint32_t prot, struct vm_region **result);
static int vmspace_map_anon_fixed_noreplace_locked(struct vmspace *vm, uintptr_t start, size_t size, uint32_t prot, struct vm_region **result);
static int vmspace_map_file_locked(struct vmspace *vm, uintptr_t start, size_t size, uint32_t prot, struct file *file, off_t file_offset, uintptr_t data_start, size_t data_size, struct vm_region **result);
static int vmspace_map_file_shared_locked(struct vmspace *vm, uintptr_t start, size_t size, uint32_t prot, struct file *file, off_t file_offset, size_t data_size, struct vm_object *object, struct vm_region **result);
static int vmspace_map_stack_locked(struct vmspace *vm, uintptr_t top, size_t size, size_t guard_size);
static struct vm_region * find_region_locked(struct vmspace *vm, uintptr_t address, size_t size);
static struct vm_page * find_page(struct vm_region *region, uintptr_t address);
static void vmspace_wait_fault_event(struct vmspace *vm, uint64_t sequence);
static void vmspace_wait_faults_locked(struct vmspace *vm);
static int allocate_page_frame(struct vm_private_page *backing, struct vm_page *avoid);
static int fill_file_page(struct vm_region *region, struct vm_page *page);
static int prepare_cow_copy(struct vm_page *page, struct vm_private_page *old, struct vm_private_page **result);
static void vmspace_fault_wake_locked(struct vmspace *vm);
static int vmspace_check_locked(struct vmspace *vm, uintptr_t address, size_t size, uint32_t required);
static int vmspace_pin_mapping_ready(struct vmspace *vm, uintptr_t address, uint32_t required);
static void vmspace_unwire_range_locked(struct vmspace *vm, uintptr_t address, size_t size);
static int copy_backing(struct vmspace *vm, uintptr_t user_address, void *kernel, size_t size, uint32_t required, int to_user);
static int vmspace_copy_to_locked(struct vmspace *vm, uintptr_t destination, const void *source, size_t size);
static int vmspace_copy_from_locked(struct vmspace *vm, void *destination, uintptr_t source, size_t size);
static int vmspace_find_free_range_bounded_locked(struct vmspace *vm, uintptr_t minimum, uintptr_t maximum, size_t size, size_t alignment, uintptr_t *mapped);
static int vmspace_find_free_range_locked(struct vmspace *vm, uintptr_t hint, size_t size, size_t alignment, uintptr_t *mapped);
static int vmspace_map_find_locked(struct vmspace *vm, uintptr_t hint, size_t size, uint32_t prot, uintptr_t *mapped);
static int vmspace_map_file_find_locked(struct vmspace *vm, uintptr_t hint, size_t size, uint32_t prot, struct file *file, off_t offset, size_t data_size, uintptr_t *mapped);
static int vmspace_map_file_shared_find_locked(struct vmspace *vm, uintptr_t hint, size_t size, uint32_t prot, struct file *file, off_t offset, size_t data_size, struct vm_object *object, uintptr_t *mapped);
static void free_vm_page(struct vmspace *vm, struct vm_page *page);
static void free_region_pages(struct vmspace *vm, struct vm_region *region);
static void detach_vm_page_for_unmap(struct vmspace *vm, struct vm_page *page);
static void detach_region_pages_for_unmap(struct vmspace *vm, struct vm_region *region);
static void release_detached_region_pages(struct vm_region *region);
static int split_region_prepared(struct vm_region *region, uintptr_t address, struct vm_region *right);
static int split_region(struct vm_region *region, uintptr_t address);
static void release_retired_regions(struct vm_region *retired);
static int vmspace_replace_prepared(struct vmspace *vm, struct vm_region *prepared, struct vm_region **result);
static int vmspace_set_brk_start_locked(struct vmspace *vm, uintptr_t start, uint64_t static_data_bytes);
static int vmspace_brk_locked(struct vmspace *vm, uintptr_t requested, uintptr_t *result);
static int vmspace_unmap_locked(struct vmspace *vm, uintptr_t start, size_t size, struct vm_region **retired);
static int vmspace_protect_locked(struct vmspace *vm, uintptr_t start, size_t size, uint32_t prot);
static void vmspace_destroy(struct vmspace *vm);
static int vmspace_set_address_limit_locked(struct vmspace *vm, uint64_t limit);
static void vmspace_set_stack_limit_locked(struct vmspace *vm, uint64_t limit);
static int vmspace_set_data_limit_locked(struct vmspace *vm, uint64_t limit);
static void vmspace_generation_advance_locked(struct vmspace *vm);

/*
 * Static asserts.
 */
_Static_assert(sizeof(struct vm_page_slab) <= PAGE_SIZE, "VM page metadata slab exceeds one physical page");
_Static_assert(VM_PRIVATE_PAGE_SLAB_SLOTS < 32U, "VM private metadata slab bitmap must fit uint32_t");
_Static_assert(sizeof(struct vm_private_page_slab) <= PAGE_SIZE, "VM private metadata slab exceeds one physical page");

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
	hal_page_get_user_range(&minimum, &limit);
	if (minimum < PAGE_SIZE ||
	    (minimum & (PAGE_SIZE - 1U)) != 0 ||
	    limit <= minimum ||
	    (limit & (PAGE_SIZE - 1U)) != 0)
		HAL_FATAL("invalid HAL user address range");

	vm_layout.user_minimum = minimum;
	vm_layout.user_limit = limit;

	/* Places the break and the mmap area where this ABI expects them. */
#ifdef ZEDBSD_USER_ABI_LP64
	vm_layout.brk_limit = 0x0000000100000000ULL;
	vm_layout.mmap_base = 0x0000000100000000ULL;
#else
	vm_layout.brk_limit = 0x10000000U;
	vm_layout.mmap_base = 0x10000000U;
#endif

	if (vm_layout.brk_limit >= limit ||
	    vm_layout.mmap_base >= limit ||
	    limit - minimum <= PAGE_SIZE)
		HAL_FATAL("user address range too small");

	vm_layout.stack_top = limit - PAGE_SIZE;
	vm_layout_initialized = 1;
}

/*
 * Allocates a zeroed page-metadata record from the slab pool, growing
 * the pool by one physical page when it is empty.
 */
struct vm_page *
vm_page_alloc_metadata(
	void)
{
	struct hal_pmem memory;
	struct vm_page_slab *fresh;
	struct vm_page *page;
	unsigned long irq;

	vmspace_layout_init();

	/* Takes a free slot from an existing slab. */
	irq = spin_lock_irqsave(&vm_page_slab_lock);

	page = vm_page_slab_take_locked();

	spin_unlock_irqrestore(&vm_page_slab_lock, irq);

	if (page != NULL)
		return page;

	/* Reclaim or a concurrent free may have returned a slab slot. */
	if (alloc_vm_metadata_page(&memory) != HAL_OK) {
		irq = spin_lock_irqsave(&vm_page_slab_lock);
		page = vm_page_slab_take_locked();
		spin_unlock_irqrestore(&vm_page_slab_lock, irq);
		return page;
	}

	/* Publishes the new slab unless another one appeared meanwhile. */
	fresh = memory.vaddr;
	memset(fresh, 0, PAGE_SIZE);
	fresh->memory = memory;
	fresh->free_mask = UINT32_MAX;

	irq = spin_lock_irqsave(&vm_page_slab_lock);

	/* Adds the freshly built slab only when no slot became free meanwhile. */
	page = vm_page_slab_take_locked();
	if (page == NULL) {
		fresh->next = vm_page_slabs;
		vm_page_slabs = fresh;
		page = vm_page_slab_take_locked();
		fresh = NULL;
	}

	spin_unlock_irqrestore(&vm_page_slab_lock, irq);

	if (fresh != NULL)
		(void)hal_pmem_free(&memory);

	return page;
}

/*
 * Returns a page-metadata record to its slab, freeing an emptied slab.
 */
void
vm_page_free_metadata(
	struct vm_page *page)
{
	struct vm_page_slab **link;
	struct vm_page_slab *slab;
	struct hal_pmem released;
	uintptr_t address;
	unsigned long irq;
	int release;
	uintptr_t first;
	uintptr_t end;
	unsigned slot;

	release = 0;

	if (page == NULL)
		return;
	address = (uintptr_t)page;

	/* Finds the slab that contains the record. */
	irq = spin_lock_irqsave(&vm_page_slab_lock);

	for (link = &vm_page_slabs; *link != NULL; link = &slab->next) {
		slab = *link;
		first = (uintptr_t)&slab->slots[0];
		end = (uintptr_t)&slab->slots[VM_PAGE_SLAB_SLOTS];
		if (address < first ||
		    address >= end ||
		    (address - first) % sizeof(struct vm_page) != 0)
			continue;

		slot = (unsigned)((address - first) / sizeof(struct vm_page));
		if ((slab->free_mask & (1U << slot)) != 0 || slab->used == 0)
			HAL_FATAL("invalid VM page metadata free");

		/* Clears the slot and unlinks a slab left empty. */
		memset(page, 0, sizeof(*page));
		slab->free_mask |= 1U << slot;
		slab->used--;
		if (slab->used == 0) {
			*link = slab->next;
			released = slab->memory;
			release = 1;
		}

		spin_unlock_irqrestore(&vm_page_slab_lock, irq);

		if (release && hal_pmem_free(&released) != HAL_OK)
			HAL_FATAL("VM page metadata slab free failed");

		return;
	}

	spin_unlock_irqrestore(&vm_page_slab_lock, irq);

	HAL_FATAL("foreign VM page metadata free");
}

/*
 * Returns a private-backing record to its slab, freeing an emptied slab.
 */
void
vm_private_page_free_metadata(
	struct vm_private_page *backing)
{
	struct vm_private_page_slab **link;
	struct vm_private_page_slab *slab;
	struct hal_pmem released;
	uintptr_t address;
	unsigned long irq;
	int release;
	uintptr_t first;
	uintptr_t end;
	unsigned slot;

	release = 0;

	if (backing == NULL)
		return;

	address = (uintptr_t)backing;

	irq = spin_lock_irqsave(&vm_private_page_slab_lock);

	/* Finds the slab that contains the record. */
	for (link = &vm_private_page_slabs; *link != NULL; link = &slab->next) {
		slab = *link;
		first = (uintptr_t)&slab->slots[0];
		end = (uintptr_t)&slab->slots[VM_PRIVATE_PAGE_SLAB_SLOTS];

		if (address < first ||
		    address >= end ||
		    (address - first) % sizeof(struct vm_private_page) != 0)
			continue;

		slot = (unsigned)((address - first) / sizeof(struct vm_private_page));

		if ((slab->free_mask & (1U << slot)) != 0 || slab->used == 0)
			HAL_FATAL("invalid VM private metadata free");

		/* Clears the slot and unlinks a slab left empty. */
		memset(backing, 0, sizeof(*backing));
		slab->free_mask |= 1U << slot;
		slab->used--;
		if (slab->used == 0) {
			*link = slab->next;
			released = slab->memory;
			release = 1;
		}

		spin_unlock_irqrestore(&vm_private_page_slab_lock, irq);

		if (release && hal_pmem_free(&released) != HAL_OK)
			HAL_FATAL("VM private metadata slab free failed");

		return;
	}

	spin_unlock_irqrestore(&vm_private_page_slab_lock, irq);

	HAL_FATAL("foreign VM private metadata free");
}

/*
 * Tests whether a byte range lies entirely inside the user address range.
 */
int
vmspace_user_range_valid(
	uintptr_t start,
	size_t size)
{
	vmspace_layout_init();

	/* Rejects an empty range, which lies nowhere. */
	if (size == 0)
		return 0;

	/* Rejects a range that starts below the user address range. */
	if (start < vm_layout.user_minimum)
		return 0;

	/* Rejects a range that starts at or above its end. */
	if (start >= vm_layout.user_limit)
		return 0;

	/* Rejects a range that reaches past that end. */
	if (size > vm_layout.user_limit - start)
		return 0;

	/* Succeeded: the whole range lies inside the user address range. */
	return 1;
}

/*
 * Revokes every process mapping of a BUSY shared object page and reports
 * the accessed and dirty state observed while doing so.
 *
 * A shared-file content writer first makes its object page BUSY.
 * Existing fault holds drain before this routine is called, so no new
 * reverse mapping can appear until the writer publishes the new cache
 * bytes.  Each mapping is then pinned in metadata, revoked with no
 * VM/object lock held, and finally detached.  hal_page_prot_query()
 * returns A/D state only after the remote TLB acknowledgement, closing
 * the late-store window between a dirty snapshot and unmap.  A
 * write-only mapping cannot be represented read-only by the current HAL
 * contract; conservatively treating it as dirty is safe.
 */
int
vmspace_object_page_revoke(
	struct vm_object_page *object_page,
	uint32_t *observed_flags)
{
	struct vm_object *object;
	uint32_t observed;
	struct vm_page *mapping;
	struct vm_region *region;
	struct vmspace *vm;
	uint32_t readonly;
	unsigned long irq;
	int error;
	int was_mapped;
	struct vm_page **link;
	uint32_t flags;

	observed = 0;

	/* Rejects a page without an owning object. */
	if (object_page == NULL)
		return EINVAL;

	object = object_page->owner;
	if (object == NULL)
		return EINVAL;

	/* Revokes one mapping of the page at a time. */
	for (;;) {
		/* Takes the next mapping while the page is still BUSY. */
		vm_metadata_enter();

		irq = spin_lock_irqsave(&object->lock);

		if ((object_page->flags & VM_OBJECT_PAGE_BUSY) == 0) {
			spin_unlock_irqrestore(&object->lock, irq);
			vm_metadata_leave();
			return EBUSY;
		}

		/* Stops once no mapping of the page is left to revoke. */
		mapping = object_page->mappings;
		if (mapping == NULL) {
			spin_unlock_irqrestore(&object->lock, irq);
			vm_metadata_leave();
			break;
		}

		vm = mapping->vm;
		region = mapping->region;
		if (vm == NULL || region == NULL || !vmspace_tryref(vm))
			HAL_FATAL("invalid shared VM reverse mapping");

		spin_unlock_irqrestore(&object->lock, irq);

		/* Pins the mapping in metadata. */
		mutex_lock(&vm->lock);
		irq = spin_lock_irqsave(&object->lock);

		if (mapping->object_page != object_page ||
		    mapping->vm != vm ||
		    mapping->region != region ||
		    (mapping->flags & VM_MAPPING_BUSY) != 0)
			HAL_FATAL("shared VM reverse mapping changed while pinned");

		mapping->flags |= VM_MAPPING_BUSY;
		region->hold_count++;
		readonly = region->prot & ~HAL_SPACE_WRITE;
		was_mapped = (mapping->flags & VM_MAPPING_MAPPED) != 0;

		spin_unlock_irqrestore(&object->lock, irq);
		mutex_unlock(&vm->lock);
		vm_metadata_leave();

		if (vmspace_object_revoke_checkpoint != NULL)
			vmspace_object_revoke_checkpoint(vm, mapping->address);

		/* Revokes the hardware mapping with no lock held. */
		if (was_mapped) {
			flags = 0;
			if (readonly != 0) {
				error = hal_page_prot_query(vm->space,
							    (void *)mapping->address,
							    PAGE_SIZE,
							    readonly,
							    &flags);
				if (error != HAL_OK)
					HAL_FATAL("shared VM write revoke failed");
			} else {
				/* The synchronous unmap is the revoke operation. */
				flags |= HAL_PAGE_DIRTY;
			}

			if (hal_page_unmap(vm->space,
					   (void *)mapping->address,
					   PAGE_SIZE) != HAL_OK)
				HAL_FATAL("shared VM content unmap failed");

			observed |= flags;
		}

		/* Detaches the mapping from the region and the object page. */
		vm_metadata_enter();
		mutex_lock(&vm->lock);
		irq = spin_lock_irqsave(&object->lock);

		if (mapping->object_page != object_page ||
		    (mapping->flags & VM_MAPPING_BUSY) == 0)
			HAL_FATAL("shared VM revoke reservation lost");

		/*
		 * Walks the region's mapping list to the link that refers to
		 * this mapping, keeping the address of the link so it can be
		 * unlinked without a second search.
		 */
		link = &region->pages;
		while (*link != NULL && *link != mapping)
			link = &(*link)->next;
		if (*link != mapping)
			HAL_FATAL("shared VM mapping left region");
		*link = mapping->next;

		/* Retires the mapping and gives back the region hold it took. */
		mapping->flags &= ~(VM_MAPPING_MAPPED | VM_MAPPING_BUSY);
		vm_object_mapping_remove_locked(object_page, mapping);
		mapping->object_page = NULL;
		if (region->hold_count == 0)
			HAL_FATAL("shared VM revoke region hold underflow");
		region->hold_count--;

		vmspace_generation_advance_locked(vm);

		waitq_wake_all(&object->page_waitq);
		spin_unlock_irqrestore(&object->lock, irq);
		vmspace_fault_wake_locked(vm);
		mutex_unlock(&vm->lock);
		vm_metadata_leave();

		vm_page_free_metadata(mapping);
		vmspace_put(vm);
	}

	if (observed_flags != NULL)
		*observed_flags = observed;

	return 0;
}

/*
 * Reports the protection a page is mapped with, which excludes write for
 * a copy-on-write page.
 */
uint32_t
vm_page_effective_prot(
	const struct vm_page *page)
{
	uint32_t prot;

	prot = page->region->prot;
	if ((page->flags & VM_MAPPING_COW) != 0)
		prot &= ~HAL_SPACE_WRITE;

	return prot;
}

/*
 * Tests whether a page's private backing currently holds a physical
 * frame.
 */
int
vm_private_page_is_resident(
	const struct vm_page *page)
{
	struct vm_private_page *backing;
	unsigned long irq;
	int resident;

	/* A page without a private backing is never resident. */
	if (page == NULL)
		return 0;
	backing = page->private_page;
	if (backing == NULL)
		return 0;

	/* Samples the residency of the backing. */
	irq = spin_lock_irqsave(&backing->state_lock);

	resident = (backing->flags & VM_PAGE_RESIDENT) != 0;

	spin_unlock_irqrestore(&backing->state_lock, irq);

	/* Reports the sampled residency. */
	return resident;
}

/*
 * Reports the kernel virtual address of a resident private page, or
 * zero.
 */
uintptr_t
vm_private_page_vaddr(
	const struct vm_page *page)
{
	struct vm_private_page *backing;
	unsigned long irq;
	uintptr_t address;

	/* A page without a private backing has no address. */
	if (page == NULL)
		return 0;

	backing = page->private_page;
	if (backing == NULL)
		return 0;

	irq = spin_lock_irqsave(&backing->state_lock);

	/* Reads the address only while the backing is resident. */
	if ((backing->flags & VM_PAGE_RESIDENT) != 0)
		address = (uintptr_t)backing->pmem.vaddr;
	else
		address = 0;

	spin_unlock_irqrestore(&backing->state_lock, irq);

	/* Reports the resident address, or zero. */
	return address;
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
	vm->space = hal_mem_create_space();
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
 * Resolves a page fault by mapping the page an address belongs to.
 *
 * An existing private page is brought back from swap or copied for a
 * write to a copy-on-write mapping.  A missing page is filled from the
 * region's file, its shared object, or zeros.  Map-table pressure is
 * handled by reclaiming a small reserve and retrying the transaction.
 */
int
vmspace_fault(
	struct vmspace *vm,
	uintptr_t address,
	uint32_t required)
{
	struct vm_private_page *reserved_backing;
	struct vm_object_page *object_page;
	struct vm_region *region;
	struct vm_page *page;
	struct vm_page **link;
	hal_physaddr_t prepared_physical;
	uintptr_t page_address;
	uint64_t prepared_backing_generation;
	uint64_t reservation_generation;
	off_t object_offset;
	uint32_t new_reservation_prot;
	unsigned map_pressure_retries;
	int error;
	int mapped;
	int private_io_owned;
	int private_io_hold;
	uint64_t sequence;
	struct vm_private_page *fresh;
	uint32_t reservation_prot;
	unsigned long state_irq;
	int backing_owner;
	int need_cow;
	int was_mapped;
	int old_unmapped;
	int map_pressure;
	int retry_fault;
	int snapshot_cached;
	int swapped;
	uint64_t maximum_offset;
	unsigned long irq;

	/* Starts with nothing reserved, nothing mapped, and no error. */
	reserved_backing = NULL;
	object_page = NULL;
	prepared_physical = 0;
	page_address = address & ~(uintptr_t)(PAGE_SIZE - 1U);
	prepared_backing_generation = 0;
	object_offset = 0;
	new_reservation_prot = 0;
	map_pressure_retries = 0;
	error = 0;
	mapped = 0;
	private_io_owned = 0;
	private_io_hold = 0;

	/* Rejects a kernel space or an access the hardware cannot ask for. */
	if (vm == NULL ||
	    vm == &kernel_vmspace ||
	    (required != HAL_SPACE_READ &&
	    required != HAL_SPACE_WRITE &&
	    required != HAL_SPACE_EXEC))
		return EINVAL;

	/* Tells reclaim that a fault is in progress. */
	vm_reclaim_note_fault();

retry:
	/* Finds the region and any existing page, waiting out a BUSY one. */
	error = 0;
	vm_metadata_enter();
	mutex_lock(&vm->lock);

	/* Rejects an address no region covers with the access asked for. */
	region = find_region_locked(vm, address, 1);
	if (region == NULL || (region->prot & required) == 0) {
		mutex_unlock(&vm->lock);
		vm_metadata_leave();
		return EFAULT;
	}

	/* Waits out a mapping another fault already owns. */
	page = find_page(region, page_address);
	if (page != NULL && (page->flags & VM_MAPPING_BUSY) != 0) {
		sequence = waitq_sequence(&vm->fault_waitq);
		mutex_unlock(&vm->lock);
		vm_metadata_leave();
		vmspace_wait_fault_event(vm, sequence);
		goto retry;
	}

	/* An existing object mapping is served by the object cache. */
	if (page != NULL && page->object_page != NULL) {
		if (region->snapshot != NULL && (required == HAL_SPACE_WRITE ||
		    (page->flags & VM_MAPPING_MAPPED) == 0)) {
			page->flags |= VM_MAPPING_BUSY;
			region->hold_count++;
			mutex_unlock(&vm->lock);
			vm_metadata_leave();
			error = vmspace_exec_cache_fault(vm, region, page, required);
			if (error == EAGAIN)
				goto retry;
			return error;
		}

		mutex_unlock(&vm->lock);
		vm_metadata_leave();
		return 0;
	}

	/* An existing private page is revived, copied, or simply mapped. */
	if (page != NULL) {
		fresh = NULL;
		backing_owner = 0;
		need_cow = 0;
		was_mapped = 0;
		old_unmapped = 0;
		map_pressure = 0;
		retry_fault = 0;

		if (page->private_page == NULL) {
			mutex_unlock(&vm->lock);
			vm_metadata_leave();
			return EFAULT;
		}

		region->hold_count++;

		page->flags |= VM_MAPPING_BUSY;
		reserved_backing = page->private_page;

		vm_private_page_ref(reserved_backing);

		reservation_generation = vm->generation;
		reservation_prot = region->prot;

		mutex_unlock(&vm->lock);
		vm_metadata_leave();

		/* Takes exclusive I/O ownership of the backing. */
		error = vm_private_page_io_acquire(reserved_backing);
		if (error == 0)
			backing_owner = 1;

		/* Waiting for another vmspace's owner may have changed this backing. */
		vm_metadata_enter();
		mutex_lock(&vm->lock);

		if (error == 0) {
			if (find_region_locked(vm, address, 1) != region) {
				/* The region this reservation named is gone. */
				error = EAGAIN;
			} else if (find_page(region, page_address) != page) {
				/* The region no longer holds this mapping. */
				error = EAGAIN;
			} else if (page->region != region) {
				/* The mapping now belongs to another region. */
				error = EAGAIN;
			} else if (page->address != page_address) {
				/* The mapping now covers another address. */
				error = EAGAIN;
			} else if (page->private_page != reserved_backing) {
				/* The mapping took a different backing. */
				error = EAGAIN;
			} else if (region->prot != reservation_prot) {
				/* The region's protection changed under the wait. */
				error = EAGAIN;
			} else if ((region->prot & required) == 0) {
				/* The region no longer permits this access. */
				error = EAGAIN;
			}
		}

		if (error == 0) {
			/* Full validation acknowledges unrelated generation changes. */
			reservation_generation = vm->generation;
			need_cow = required == HAL_SPACE_WRITE && (page->flags & VM_MAPPING_COW) != 0;
			was_mapped = (page->flags & VM_MAPPING_MAPPED) != 0;
		}

		mutex_unlock(&vm->lock);
		vm_metadata_leave();

		/* Brings a swapped page back and prepares a COW copy. */
		if (error == 0) {

			/* Samples the backing the copy will map, while it is still owned. */
			state_irq = spin_lock_irqsave(&reserved_backing->state_lock);
			swapped = (reserved_backing->flags & VM_PAGE_SWAPPED) != 0;
			spin_unlock_irqrestore(&reserved_backing->state_lock, state_irq);
			if (swapped)
				error = vm_private_page_in_owned(reserved_backing, page);
		}

		if (error == 0 && need_cow)
			error = prepare_cow_copy(page, reserved_backing, &fresh);

		/* Samples what the copy must still match when it is installed. */
		if (error == 0) {
			state_irq = spin_lock_irqsave(&reserved_backing->state_lock);
			if ((reserved_backing->flags &
			    (VM_PAGE_BUSY | VM_PAGE_RESIDENT)) !=
			    (VM_PAGE_BUSY | VM_PAGE_RESIDENT)) {
				error = EIO;
			} else {
				prepared_backing_generation =
				    reserved_backing->generation;
				prepared_physical = reserved_backing->pmem.paddr;
			}

			spin_unlock_irqrestore(&reserved_backing->state_lock,
			    state_irq);
		}

		/* A replacement needs a shootdown, performed with no VM lock held. */
		if (error == 0 && fresh != NULL && was_mapped) {
			if (hal_page_unmap(vm->space, (void *)page_address,
			    PAGE_SIZE) != HAL_OK)
				error = EIO;
			else
				old_unmapped = 1;
		}

		/* Revalidates everything and installs the mapping. */
		vm_metadata_enter();
		mutex_lock(&vm->lock);

		if (old_unmapped)
			page->flags &= ~VM_MAPPING_MAPPED;

		/* Refuses the mapping if the backing moved since it was sampled. */
		if (error == 0) {
			state_irq = spin_lock_irqsave(&reserved_backing->state_lock);
			if ((reserved_backing->flags &
			    (VM_PAGE_BUSY | VM_PAGE_RESIDENT)) !=
			    (VM_PAGE_BUSY | VM_PAGE_RESIDENT) ||
			    reserved_backing->generation !=
			    prepared_backing_generation ||
			    reserved_backing->pmem.paddr != prepared_physical)
				error = EAGAIN;
			spin_unlock_irqrestore(&reserved_backing->state_lock,
			    state_irq);
		}

		/* Refuses the mapping if anything the reservation named has changed. */
		if (error == 0 &&
		    (vm->generation != reservation_generation ||
		    find_region_locked(vm, address, 1) != region ||
		    find_page(region, page_address) != page ||
		    page->region != region ||
		    page->address != page_address ||
		    page->private_page != reserved_backing ||
		    region->prot != reservation_prot ||
		    (region->prot & required) == 0))
			error = EAGAIN;

		/* Installs the copy, giving up on map pressure so a reclaim can run. */
		if (error == 0 && fresh != NULL) {
			if (hal_page_map(vm->space, (void *)page_address,
			    fresh->pmem.paddr, PAGE_SIZE, region->prot) != HAL_OK) {
				error = ENOMEM;
				map_pressure = 1;
			} else {
				/* The mapping owns the creator reference. */
				vm_page_replace_private(page, fresh);
				fresh = NULL;
				page->flags &= ~VM_MAPPING_COW;
				page->flags |= VM_MAPPING_MAPPED;
			}
		} else if (error == 0 &&
		    (page->flags & VM_MAPPING_MAPPED) == 0) {
			if (hal_page_map(vm->space, (void *)page_address,
			    prepared_physical, PAGE_SIZE,
			    vm_page_effective_prot(page)) != HAL_OK) {
				error = ENOMEM;
				map_pressure = 1;
			} else {
				page->flags |= VM_MAPPING_MAPPED;
			}
		}

		/* Map-table pressure reclaims a reserve and retries. */
		if (map_pressure) {
			mutex_unlock(&vm->lock);
			vm_metadata_leave();
			if (map_pressure_retries < VM_PAGE_MAP_RECLAIM_RETRIES &&
			    reclaim_page_map_reserve(page) != 0) {
				map_pressure_retries++;
				error = EAGAIN;
			}

			/* Retakes the locks and traps if the reservation did not survive. */
			vm_metadata_enter();
			mutex_lock(&vm->lock);
			if (find_region_locked(vm, address, 1) != region ||
			    find_page(region, page_address) != page ||
			    page->private_page != reserved_backing ||
			    (page->flags & VM_MAPPING_BUSY) == 0 ||
			    region->hold_count == 0)
				HAL_FATAL("VM map-pressure reservation lost");
		}

		/* Releases the reservation and the backing. */
		page->flags &= ~VM_MAPPING_BUSY;
		if (region->hold_count == 0)
			HAL_FATAL("VM fault region hold underflow");

		region->hold_count--;
		if (error == 0)
			vmspace_generation_advance_locked(vm);

		retry_fault = error == EAGAIN;
		vmspace_fault_wake_locked(vm);
		mutex_unlock(&vm->lock);
		vm_metadata_leave();

		if (backing_owner)
			vm_private_page_io_release(reserved_backing);

		vm_private_page_put(reserved_backing);

		if (fresh != NULL)
			vm_private_page_put(fresh);

		if (retry_fault)
			goto retry;

		return error;
	}

	/* Pin the region, then allocate a placeholder without holding the lock. */
	region->hold_count++;
	reservation_generation = vm->generation;

	mutex_unlock(&vm->lock);
	vm_metadata_leave();

	page = vm_page_alloc_metadata();
	if (page == NULL) {
		error = ENOMEM;
		goto release_region;
	}

	page->vm = vm;
	page->region = region;
	page->address = page_address;
	page->flags = VM_MAPPING_BUSY;

	/* Links the placeholder unless the page appeared meanwhile. */
	vm_metadata_enter();
	mutex_lock(&vm->lock);

	if (find_region_locked(vm, address, 1) != region ||
	    (region->prot & required) == 0) {
		error = EFAULT;
		goto unlink_locked;
	}

	/* Starts over when another fault published this page first. */
	if (find_page(region, page_address) != NULL) {
		region->hold_count--;
		vmspace_fault_wake_locked(vm);
		mutex_unlock(&vm->lock);
		vm_metadata_leave();
		vm_page_free_metadata(page);
		goto retry;
	}

	/* A generation change is allowed only after full region revalidation. */
	if (vm->generation != reservation_generation &&
	    find_region_locked(vm, address, 1) != region) {
		error = EFAULT;
		goto unlink_locked;
	}

	/* Publishes the placeholder and samples what it is measured against. */
	page->next = region->pages;
	region->pages = page;
	reservation_generation = vm->generation;
	new_reservation_prot = region->prot;

	mutex_unlock(&vm->lock);
	vm_metadata_leave();

	/* A shared object page is mapped straight from the object cache. */
	if (region->object != NULL || region->snapshot != NULL) {
		snapshot_cached = region->snapshot != NULL;
		if (sizeof(off_t) == 8)
			maximum_offset = (uint64_t)INT64_MAX;
		else
			maximum_offset = (uint64_t)INT32_MAX;

		/* Refuses an offset the file cannot address. */
		if (region->file_offset < 0 ||
		    (uint64_t)region->file_offset +
		    (uint64_t)(page_address - region->start) > maximum_offset) {
			error = EOVERFLOW;
			goto remove_placeholder;
		}

		/* Refuses an offset outside the snapshot this region maps. */
		object_offset = region->file_offset + (off_t)(page_address - region->start);
		if (region->snapshot != NULL) {
			if (object_offset < region->snapshot->offset ||
			    (uint64_t)(object_offset - region->snapshot->offset) >= region->snapshot->length) {
				error = EFAULT;
				goto remove_placeholder;
			}
		}

		/* Reads the page in through the object, or through the snapshot. */
		error = vm_object_fault(region->snapshot != NULL ?
					region->snapshot->input.read_object :
					region->object,
					object_offset,
					&object_page);
		if (error != 0)
			goto remove_placeholder;

		/* A snapshot mapping must get the page the snapshot pinned. */
		if (region->snapshot != NULL) {
			if (object_page != region->snapshot->pages[
			    (size_t)(object_offset - region->snapshot->offset) / PAGE_SIZE]) {
				error = EIO;
				goto remove_placeholder;
			}

			page->flags |= VM_MAPPING_COW;
		}

		/* Maps the object page, read-only when a snapshot owns it. */
		page->object_page = object_page;
		mapped = hal_page_map(vm->space,
				      (void *)page_address,
				      object_page->pmem.paddr,
				      PAGE_SIZE,
				      region->snapshot != NULL ?
				      region->prot & ~HAL_SPACE_WRITE :
				      region->prot) == HAL_OK;
		if (!mapped) {
			/*
			 * The fault hold keeps this cache page out of object
			 * reclaim, while the BUSY placeholder and region hold
			 * keep its VM metadata stable.  Reclaim other pages
			 * with no VM lock held, then retire the placeholder
			 * and retry the full transaction.
			 */
			if (map_pressure_retries < VM_PAGE_MAP_RECLAIM_RETRIES &&
			    reclaim_page_map_reserve(page) != 0) {
				map_pressure_retries++;
				error = EAGAIN;
			} else {
				error = ENOMEM;
			}

			goto remove_placeholder;
		}

		page->flags |= VM_PAGE_RESIDENT | VM_MAPPING_MAPPED;

		vm_metadata_enter();
		mutex_lock(&vm->lock);

		if (find_region_locked(vm, address, 1) != region ||
		    find_page(region, page_address) != page)
			HAL_FATAL("lost VM fault reservation");

		vm_object_mapping_add(object_page, page);

		object_page = NULL;
		page->flags &= ~VM_MAPPING_BUSY;
		region->hold_count--;

		vmspace_generation_advance_locked(vm);
		vmspace_fault_wake_locked(vm);
		mutex_unlock(&vm->lock);
		vm_metadata_leave();

		if (required == HAL_SPACE_WRITE && snapshot_cached)
			goto retry;

		return 0;
	}

	/* A private page gets a new backing, a frame, and its contents. */
	page->private_page = private_page_alloc();
	if (page->private_page == NULL) {
		error = ENOMEM;
		goto remove_placeholder;
	}

	private_page_attach_new(page, page->private_page);
	reserved_backing = page->private_page;
	vm_private_page_ref(reserved_backing);
	private_io_hold = 1;

	error = vm_private_page_io_acquire(reserved_backing);
	if (error != 0)
		goto remove_placeholder;

	private_io_owned = 1;
	if (allocate_page_frame(page->private_page, page) != 0) {
		error = ENOMEM;
		goto remove_placeholder;
	}

	/* Fills the fresh page from the file, or with zeroes. */
	if (region->backing == VM_BACKING_FILE) {
		error = fill_file_page(region, page);
	} else {
		memset((void *)page->private_page->pmem.vaddr, 0, PAGE_SIZE);
		error = 0;
	}

	/* Publishes the filled page as resident and samples what it became. */
	if (error == 0) {
		irq = spin_lock_irqsave(&page->private_page->state_lock);

		page->private_page->flags |= VM_PAGE_RESIDENT;

		page->private_page->generation++;
		if (page->private_page->generation == 0)
			page->private_page->generation++;

		prepared_backing_generation = page->private_page->generation;

		prepared_physical = page->private_page->pmem.paddr;

		spin_unlock_irqrestore(&page->private_page->state_lock, irq);
	}

	if (error != 0)
		goto remove_placeholder;

	/* Revalidates the reservation before mapping the frame. */
	vm_metadata_enter();
	mutex_lock(&vm->lock);

	if (error == 0) {
		irq = spin_lock_irqsave(&reserved_backing->state_lock);

		if ((reserved_backing->flags & (VM_PAGE_BUSY | VM_PAGE_RESIDENT)) != (VM_PAGE_BUSY | VM_PAGE_RESIDENT) ||
		    reserved_backing->generation != prepared_backing_generation ||
		    reserved_backing->pmem.paddr != prepared_physical)
			error = EAGAIN;

		spin_unlock_irqrestore(&reserved_backing->state_lock, irq);
	}

	if (error != 0)
		goto unlink_locked;

	/* Refuses the mapping if anything the reservation named has changed. */
	if (vm->generation != reservation_generation ||
	    find_region_locked(vm, address, 1) != region ||
	    find_page(region, page_address) != page ||
	    page->region != region ||
	    page->address != page_address ||
	    page->private_page != reserved_backing ||
	    region->prot != new_reservation_prot ||
	    (region->prot & required) == 0) {
		error = EAGAIN;
		goto unlink_locked;
	}

	if (hal_page_map(vm->space, (void *)page_address,
	    prepared_physical, PAGE_SIZE, region->prot) != HAL_OK) {
		/*
		 * The frame allocator already retries after reclaim, but the
		 * HAL can need another physical page for a new page-table
		 * level.  Drop the VM locks before reclaim, retire this
		 * incomplete fault reservation, and retry the complete
		 * transaction.  Releasing the prepared frame during rollback
		 * leaves both it and the reclaimed frame available to a HAL
		 * which needs separate allocation metadata and table storage.
		 */
		mutex_unlock(&vm->lock);
		vm_metadata_leave();
		if (map_pressure_retries < VM_PAGE_MAP_RECLAIM_RETRIES &&
		    reclaim_page_map_reserve(page) != 0) {
			map_pressure_retries++;
			error = EAGAIN;
		} else {
			error = ENOMEM;
		}

		goto remove_placeholder;
	}

	/* Publishes the mapped page and releases the backing. */
	mapped = 1;
	page->flags |= VM_MAPPING_MAPPED;
	vm_page_track(page);

	page->flags &= ~VM_MAPPING_BUSY;
	region->hold_count--;

	vmspace_generation_advance_locked(vm);
	vmspace_fault_wake_locked(vm);

	mutex_unlock(&vm->lock);
	vm_metadata_leave();

	vm_private_page_io_release(reserved_backing);
	private_io_owned = 0;
	vm_private_page_put(reserved_backing);
	private_io_hold = 0;

	return 0;

remove_placeholder:
	/* Undoes a partial mapping and drops an object fault hold. */
	if (mapped)
		(void)hal_page_unmap(vm->space, (void *)page_address, PAGE_SIZE);

	if (object_page != NULL) {
		vm_object_fault_release(object_page);
		object_page = NULL;
	}

	vm_metadata_enter();
	mutex_lock(&vm->lock);

unlink_locked:
	/* Unlinks the placeholder and releases the region hold. */
	link = &region->pages;
	while (*link != NULL && *link != page)
		link = &(*link)->next;
	if (*link == page)
		*link = page->next;

	if (region->hold_count == 0)
		HAL_FATAL("VM fault region hold underflow");
	region->hold_count--;

	vmspace_fault_wake_locked(vm);

	mutex_unlock(&vm->lock);
	vm_metadata_leave();

	if (page->private_page != NULL)
		vm_page_untrack(page);

	if (private_io_owned) {
		vm_private_page_io_release(reserved_backing);
		private_io_owned = 0;
	}

	if (private_io_hold) {
		vm_private_page_put(reserved_backing);
		private_io_hold = 0;
	}

	vm_page_free_metadata(page);

	if (error == EAGAIN)
		goto retry;

	return error;

release_region:
	/* Releases the region hold this fault took and wakes the waiters. */
	vm_metadata_enter();
	mutex_lock(&vm->lock);

	if (region->hold_count == 0)
		HAL_FATAL("VM fault region hold underflow");
	region->hold_count--;

	vmspace_fault_wake_locked(vm);

	mutex_unlock(&vm->lock);
	vm_metadata_leave();

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Releases the pins taken by vmspace_pin_user_pages(), last page first.
 */
void
vmspace_unpin_user_pages(
	struct vmspace_pinned_page *pages,
	size_t page_count)
{
	struct vmspace_pinned_page *page;

	/* Ignores a missing array. */
	if (pages == NULL)
		return;

	/* Releases every pin in reverse order and empties its entry. */
	while (page_count != 0) {
		page_count--;
		page = &pages[page_count];
		if (page->kind == VMSPACE_PINNED_PRIVATE)
			vm_private_page_unpin(page->owner.private_page);
		else if (page->kind == VMSPACE_PINNED_OBJECT)
			vm_object_page_unpin(page->owner.object_page);
		memset(page, 0, sizeof(*page));
	}
}

/*
 * Pins every page of a user range for kernel access.
 *
 * Each page is faulted in first with no VM lock held, then the whole
 * range is validated and pinned under both metadata locks so that no
 * unmap, remap, or fork can change page identity in between.  A page
 * that changed underneath is faulted again.
 */
int
vmspace_pin_user_pages(
	struct vmspace *vm,
	uintptr_t address,
	size_t size,
	uint32_t required,
	struct vmspace_pinned_page *pages,
	size_t page_count)
{
	struct vm_private_page *wait_backing;
	uintptr_t first;
	uintptr_t last;
	uintptr_t current;
	uint32_t fault_access;
	size_t expected;
	size_t index;
	size_t acquired;
	int error;
	int refault;
	int wait_fault;
	uint64_t fault_sequence;
	struct vm_region *region;
	struct vm_page *entry;
	int wait_error;

	/* Rejects a call that names no address space. */
	if (vm == NULL)
		return EFAULT;

	/* Rejects the kernel address space, which is never pinned this way. */
	if (vm == &kernel_vmspace)
		return EFAULT;

	/* Rejects a call that names nowhere to report the pages. */
	if (pages == NULL)
		return EFAULT;

	/* Rejects an empty range. */
	if (size == 0)
		return EFAULT;

	/* Rejects an access this kernel does not define. */
	if ((required & ~(HAL_SPACE_READ | HAL_SPACE_WRITE |
	    HAL_SPACE_EXEC)) != 0)
		return EFAULT;

	/* Rejects a request that asks for no access at all. */
	if (required == 0)
		return EFAULT;

	/* Rejects a range that does not lie inside the user address range. */
	if (!vmspace_user_range_valid(address, size))
		return EFAULT;

	/* Rejects a result array that does not match the range. */
	first = address & ~(uintptr_t)(PAGE_SIZE - 1U);
	last = (address + size - 1U) & ~(uintptr_t)(PAGE_SIZE - 1U);
	expected = (size_t)((last - first) / PAGE_SIZE) + 1U;
	if (page_count != expected)
		return EINVAL;
	memset(pages, 0, page_count * sizeof(*pages));

	/* Picks the one concrete access the fault path understands. */
	if ((required & HAL_SPACE_WRITE) != 0)
		fault_access = HAL_SPACE_WRITE;
	else if ((required & HAL_SPACE_READ) != 0)
		fault_access = HAL_SPACE_READ;
	else
		fault_access = HAL_SPACE_EXEC;

retry_faults:
	/* Faulting may sleep and may break COW, so it is never done under a VM lock. */
	for (current = first;; current += PAGE_SIZE) {
		if (!vmspace_pin_mapping_ready(vm, current, required)) {
			error = vmspace_fault(vm, current, fault_access);
			if (error != 0)
				return error;
		}

		if (current == last)
			break;
	}

	/*
	 * Validate the complete range before taking the first backing pin.
	 * Holding both metadata locks until the final pin closes the old
	 * wire/unlock/rewalk gap in which unmap, remap, or fork could
	 * change page identity.
	 */
	wait_backing = NULL;
	acquired = 0;
	refault = 0;
	wait_fault = 0;
	fault_sequence = 0;
	vm_metadata_enter();
	mutex_lock(&vm->lock);

	error = vmspace_check_locked(vm, address, size, required);
	for (index = 0, current = first; error == 0 && index < page_count;
	     index++, current += PAGE_SIZE) {

		/* Looks up the mapping the fault just published. */
		region = find_region_locked(vm, current, 1);
		if (region != NULL)
			entry = find_page(region, current);
		else
			entry = NULL;

		/* Faults again when the mapping went away under the walk. */
		if (entry == NULL) {
			refault = 1;
			error = EAGAIN;
			break;
		}

		/* Waits out a mapping another pass owns. */
		if ((entry->flags & VM_MAPPING_BUSY) != 0) {
			fault_sequence = waitq_sequence(&vm->fault_waitq);
			wait_fault = 1;
			error = EAGAIN;
			break;
		}

		/*
		 * A mapping is backed either privately or by an object, never
		 * by both and never by neither.
		 */
		if ((entry->private_page == NULL) ==
		    (entry->object_page == NULL)) {
			error = EFAULT;
			break;
		}

		if ((required & HAL_SPACE_WRITE) != 0 &&
		    (entry->flags & VM_MAPPING_COW) != 0) {
			/* Private cache pages also require COW before a writable pin. */
			refault = 1;
			error = EAGAIN;
			break;
		}

		/* A private mapping must be resident and past any copy-on-write. */
		if (entry->private_page != NULL) {
			if (!vm_private_page_is_resident(entry)) {
				refault = 1;
				error = EAGAIN;
				break;
			}

			if ((required & HAL_SPACE_WRITE) != 0 &&
			    (entry->flags & VM_MAPPING_COW) != 0) {
				/* A fork raced the fault phase; break the new COW generation. */
				refault = 1;
				error = EAGAIN;
				break;
			}
		}
	}

	/* Pins each page's backing. */
	for (index = 0, current = first; error == 0 && index < page_count;
	     index++, current += PAGE_SIZE) {
		region = find_region_locked(vm, current, 1);
		entry = find_page(region, current);
		if (entry->private_page != NULL) {
			error = vm_private_page_pin(entry->private_page,
			    &pages[index].memory);
			if (error == EBUSY) {
				/* Keep the wait target alive after dropping the VM locks. */
				wait_backing = entry->private_page;
				vm_private_page_ref(wait_backing);
			}

			/* Records which of the two backings this pin holds. */
			if (error == 0) {
				pages[index].kind = VMSPACE_PINNED_PRIVATE;
				pages[index].owner.private_page = entry->private_page;
			}
		} else {
			error = vm_object_page_pin(entry->object_page);
			if (error == 0) {
				pages[index].kind = VMSPACE_PINNED_OBJECT;
				pages[index].owner.object_page = entry->object_page;
				pages[index].memory = entry->object_page->pmem;
			}
		}

		if (error == 0)
			acquired++;
		if (error == 0 && vmspace_pin_page_checkpoint != NULL)
			vmspace_pin_page_checkpoint(vm, index, page_count);
	}

	mutex_unlock(&vm->lock);
	vm_metadata_leave();

	if (error == 0)
		return 0;

	/* Backing unpin may finish object teardown, so rollback outside VM locks. */
	vmspace_unpin_user_pages(pages, acquired);
	if (wait_backing != NULL) {
		wait_error = vm_private_page_wait_idle(wait_backing);
		vm_private_page_put(wait_backing);
		if (wait_error != 0)
			return wait_error;
		goto retry_faults;
	}

	if (wait_fault) {
		vmspace_wait_fault_event(vm, fault_sequence);
		goto retry_faults;
	}

	if (refault)
		goto retry_faults;

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Faults in and wires every page of a user range, undoing the wires on
 * failure.
 */
int
vmspace_wire_range(
	struct vmspace *vm,
	uintptr_t address,
	size_t size,
	uint32_t required)
{
	uintptr_t page;
	uintptr_t first;
	uintptr_t last;
	uint32_t fault_access;
	int error;
	struct vm_region *region;
	struct vm_page *entry;
	uint64_t sequence;

	error = vmspace_check(vm, address, size, required);
	if (error != 0)
		return error;

	/*
	 * vmspace_check() accepts a protection mask, while vmspace_fault()
	 * describes one concrete access.  A bidirectional uaccess pin asks
	 * for READ|WRITE and must not pass that mask through as though it
	 * were an individual fault access.
	 */
	if ((required & HAL_SPACE_WRITE) != 0)
		fault_access = HAL_SPACE_WRITE;
	else if ((required & HAL_SPACE_READ) != 0)
		fault_access = HAL_SPACE_READ;
	else
		fault_access = HAL_SPACE_EXEC;
	page = address & ~(uintptr_t)(PAGE_SIZE - 1U);
	first = page;
	last = (address + size - 1U) & ~(uintptr_t)(PAGE_SIZE - 1U);

	/* Faults and wires one page at a time, waiting out BUSY pages. */
	for (;;) {
		error = vmspace_fault(vm, page, fault_access);
		if (error != 0)
			break;

		/* Takes the locks the mapping below is inspected under. */
		vm_metadata_enter();
		mutex_lock(&vm->lock);

		/* Looks up the mapping the fault just published. */
		region = find_region_locked(vm, page, 1);
		if (region != NULL)
			entry = find_page(region, page);
		else
			entry = NULL;

		/* Waits out a mapping another pass owns. */
		if (entry != NULL && (entry->flags & VM_MAPPING_BUSY) != 0) {
			sequence = waitq_sequence(&vm->fault_waitq);
			mutex_unlock(&vm->lock);
			vm_metadata_leave();
			vmspace_wait_fault_event(vm, sequence);
			continue;
		}

		/* Refuses a page the fault did not leave resident. */
		if (entry == NULL || (entry->object_page == NULL &&
		    !vm_private_page_is_resident(entry))) {
			error = EFAULT;
			mutex_unlock(&vm->lock);
			vm_metadata_leave();
			break;
		}

		/* Wires the page and moves on to the next one. */
		entry->wire_count++;
		vmspace_generation_advance_locked(vm);
		mutex_unlock(&vm->lock);
		vm_metadata_leave();
		if (page == last)
			return 0;
		page += PAGE_SIZE;
	}

	/* Unwires the pages wired before the failure. */
	while (page != first) {
		page -= PAGE_SIZE;
		vm_metadata_enter();
		mutex_lock(&vm->lock);
		region = find_region_locked(vm, page, 1);
		if (region != NULL)
			entry = find_page(region, page);
		else
			entry = NULL;
		if (entry != NULL && entry->wire_count != 0)
			entry->wire_count--;
		mutex_unlock(&vm->lock);
		vm_metadata_leave();
	}

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Writes back the shared object pages of a user range.
 *
 * Exactly one of MS_ASYNC and MS_SYNC must be set; MS_INVALIDATE
 * additionally advances the generation so cached translations are
 * revalidated.
 */
int
vmspace_sync(
	struct vmspace *vm,
	uintptr_t start,
	size_t size,
	int flags)
{
	uintptr_t current;
	uintptr_t end;
	struct vm_object *object;
	struct vm_region *region;
	uintptr_t overlap_end;
	off_t offset;
	int error;

	/* Rejects a malformed range or an inconsistent flag combination. */
	if (vm == NULL ||
	    vm == &kernel_vmspace ||
	    !range_valid(start, size) ||
	    (flags & ~(MS_ASYNC | MS_INVALIDATE | MS_SYNC)) != 0 ||
	    (flags & (MS_ASYNC | MS_SYNC)) == 0 ||
	    (flags & (MS_ASYNC | MS_SYNC)) == (MS_ASYNC | MS_SYNC))
		return EINVAL;

	/* Syncs each region's object with no VM lock held. */
	current = start;
	end = start + size;
	while (current < end) {
		object = NULL;
		offset = 0;

		vm_metadata_enter();
		mutex_lock(&vm->lock);

		/* Refuses a range that reaches a hole in the address space. */
		region = find_region_locked(vm, current, 1);
		if (region == NULL) {
			mutex_unlock(&vm->lock);
			vm_metadata_leave();
			return ENOMEM;
		}

		/* Clamps the run to the region and takes the object it maps. */
		overlap_end = region->start + region->size;
		if (overlap_end > end)
			overlap_end = end;

		/* Takes a reference on the object so it survives the unlock. */
		if (region->object != NULL) {
			object = region->object;
			vm_object_ref(object);
			offset = region->file_offset +
			    (off_t)(current - region->start);
		}

		/* Syncs the object outside the locks, then releases it. */
		mutex_unlock(&vm->lock);
		vm_metadata_leave();

		if (object != NULL)
			error = vm_object_sync_range(object, offset,
			    overlap_end - current, flags);
		else
			error = 0;

		if (object != NULL)
			vm_object_put(object);

		if (error != 0)
			return error;

		current = overlap_end;
	}

	/* An invalidating sync forces every fault to look again. */
	if ((flags & MS_INVALIDATE) != 0) {
		vm_metadata_enter();
		mutex_lock(&vm->lock);
		vmspace_generation_advance_locked(vm);
		mutex_unlock(&vm->lock);
		vm_metadata_leave();
	}

	return 0;
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
 * Copies a vmspace for fork, sharing private pages copy-on-write.
 *
 * A page whose backing is busy is waited for outside the locks and the
 * whole copy is retried.
 */
int
vmspace_fork(
	struct vmspace *source,
	struct vmspace **result)
{
	struct vm_private_page *wait_backing;
	struct vmspace *failed_copy;
	int error;
	int wait_error;

	if (source == NULL || source == &kernel_vmspace || result == NULL)
		return EINVAL;

	*result = NULL;

retry:
	/* Copies under the source locks. */
	wait_backing = NULL;
	failed_copy = NULL;

	vm_metadata_enter();
	mutex_lock(&source->lock);

	error = vmspace_fork_locked(source, result, &wait_backing, &failed_copy);
	if (error == 0)
		vmspace_generation_advance_locked(source);

	mutex_unlock(&source->lock);
	vm_metadata_leave();

	/* Retires a failed copy and waits out a busy backing. */
	if (failed_copy != NULL)
		vmspace_put(failed_copy);
	if (error == EBUSY && wait_backing != NULL) {
		wait_error = vm_private_page_wait_idle(wait_backing);
		vm_private_page_put(wait_backing);
		if (wait_error == 0 || wait_error == EAGAIN)
			goto retry;
		return wait_error;
	}

	if (error == EAGAIN)
		goto retry;

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Maps anonymous memory at a fixed address that must be free.
 */
int
vmspace_map_anon(
	struct vmspace *vm,
	uintptr_t start,
	size_t size,
	uint32_t prot,
	struct vm_region **result)
{
	int error;

	if (vm == NULL || vm == &kernel_vmspace)
		return EINVAL;

	/* Runs the mapping under the metadata and space locks. */
	vm_metadata_enter();
	mutex_lock(&vm->lock);

	error = vmspace_map_anon_locked(vm, start, size, prot, result);
	if (error == 0)
		vmspace_generation_advance_locked(vm);

	mutex_unlock(&vm->lock);
	vm_metadata_leave();

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Maps anonymous memory at a fixed address, failing with EEXIST when the
 * range is in use.
 */
int
vmspace_map_anon_fixed_noreplace(
	struct vmspace *vm,
	uintptr_t start,
	size_t size,
	uint32_t prot,
	struct vm_region **result)
{
	int error;

	if (vm == NULL || vm == &kernel_vmspace)
		return EINVAL;

	/* Runs the mapping under the metadata and space locks. */
	vm_metadata_enter();
	mutex_lock(&vm->lock);

	error = vmspace_map_anon_fixed_noreplace_locked(vm, start, size, prot, result);
	if (error == 0)
		vmspace_generation_advance_locked(vm);

	mutex_unlock(&vm->lock);
	vm_metadata_leave();

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Maps anonymous memory at a fixed address, replacing whatever was there.
 */
int
vmspace_map_anon_fixed(
	struct vmspace *vm,
	uintptr_t start,
	size_t size,
	uint32_t prot,
	int shared,
	struct vm_region **result)
{
	struct vm_object *object;
	struct vm_region *prepared;
	unsigned flags;
	size_t commit_size;
	size_t data_size;
	int error;

	object = NULL;
	prepared = NULL;

	if (vm == NULL ||
	    vm == &kernel_vmspace ||
	    (shared != 0 && shared != 1))
		return EINVAL;

	/* A shared mapping is backed by an anonymous object; private memory is committed. */
	if (shared) {
		flags = VM_REGION_SHARED;
		commit_size = 0;
		data_size = size;
	} else {
		flags = 0;
		if (prot != 0)
			commit_size = size;
		else
			commit_size = 0;
		data_size = 0;
	}

	if (shared) {
		error = vm_object_create_anonymous(size, &object);
		if (error != 0)
			return error;
	}

	/* Prepares the region fully before replacing the old mappings. */
	error = prepare_region(start,
			       size,
			       prot,
			       VM_BACKING_ANON,
			       NULL,
			       0,
			       start,
			       data_size,
			       flags,
			       commit_size,
			       &prepared);
	if (error != 0) {
		if (object != NULL)
			vm_object_put(object);
		return error;
	}

	prepared->object = object;

	error = vmspace_replace_prepared(vm, prepared, result);
	if (error != 0)
		discard_prepared_region(prepared);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Maps a new shared anonymous object at a free address near a hint.
 */
int
vmspace_map_anon_shared_find(
	struct vmspace *vm,
	uintptr_t hint,
	size_t size,
	uint32_t prot,
	uintptr_t *mapped)
{
	struct vm_object *object;
	struct vm_region *region;
	uintptr_t start;
	size_t rounded;
	int error;

	region = NULL;

	/* Rejects a kernel space, a missing result, or an unusable size. */
	if (vm == NULL ||
	    vm == &kernel_vmspace ||
	    mapped == NULL ||
	    size == 0 ||
	    size > SIZE_MAX - (PAGE_SIZE - 1U))
		return EINVAL;

	/* Builds the anonymous object the mapping will share. */
	rounded = (size + PAGE_SIZE - 1U) & ~(PAGE_SIZE - 1U);
	error = vm_object_create_anonymous(rounded, &object);
	if (error != 0)
		return error;

	/* Finds a free range and maps the object there. */
	vm_metadata_enter();
	mutex_lock(&vm->lock);

	/* Places the mapping in the first free range that fits. */
	error = vmspace_find_free_range_locked(vm, hint, rounded, PAGE_SIZE, &start);
	if (error == 0) {
		error = map_region(vm,
				   start,
				   rounded,
				   prot,
				   VM_BACKING_ANON,
				   NULL,
				   0,
				   start,
				   rounded,
				   VM_REGION_SHARED,
				   0,
				   &region);
	}

	if (error == 0) {
		region->object = object;
		*mapped = start;
		vmspace_generation_advance_locked(vm);
	}

	mutex_unlock(&vm->lock);
	vm_metadata_leave();

	if (error != 0)
		vm_object_put(object);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Maps a private file region at a fixed address that must be free.
 */
int
vmspace_map_file(
	struct vmspace *vm,
	uintptr_t start,
	size_t size,
	uint32_t prot,
	struct file *file,
	off_t offset,
	uintptr_t data_start,
	size_t data_size,
	struct vm_region **result)
{
	int error;

	if (vm == NULL || vm == &kernel_vmspace)
		return EINVAL;
	/* Runs the mapping under the metadata and space locks. */
	vm_metadata_enter();
	mutex_lock(&vm->lock);

	error = vmspace_map_file_locked(vm, start, size, prot, file, offset,
	    data_start, data_size, result);
	if (error == 0)
		vmspace_generation_advance_locked(vm);

	mutex_unlock(&vm->lock);
	vm_metadata_leave();

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Maps a shared file region at a fixed address that must be free.
 */
int
vmspace_map_file_shared(
	struct vmspace *vm,
	uintptr_t start,
	size_t size,
	uint32_t prot,
	struct file *file,
	off_t offset,
	size_t data_size,
	struct vm_region **result)
{
	struct vm_object *object;
	int error;

	/* Rejects unsupported address spaces before acquiring a shared object. */
	if (vm == NULL || vm == &kernel_vmspace)
		return EINVAL;

	/* Resolves FAT identity before taking higher-ranked VM metadata locks. */
	error = vm_object_get_shared(file, &object);
	if (error != 0)
		return error;

	/* Publishes the region while the object makes formatting admission busy. */
	vm_metadata_enter();
	mutex_lock(&vm->lock);

	error = vmspace_map_file_shared_locked(
		vm, start, size, prot, file, offset, data_size, object, result);
	if (error == 0)
		vmspace_generation_advance_locked(vm);

	mutex_unlock(&vm->lock);
	vm_metadata_leave();

	/* Releases failed preparation after leaving address-space locks. */
	if (error != 0)
		vm_object_put(object);

	/* Reports publication or its exact failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Maps a file region at a fixed address, replacing whatever was there.
 */
int
vmspace_map_file_fixed(
	struct vmspace *vm,
	uintptr_t start,
	size_t size,
	uint32_t prot,
	struct file *file,
	off_t offset,
	size_t data_size,
	int shared,
	struct vm_region **result)
{
	struct vm_object *object;
	struct vm_region *prepared;
	unsigned flags;
	size_t commit_size;
	int error;

	object = NULL;
	prepared = NULL;

	if (vm == NULL ||
	    vm == &kernel_vmspace ||
	    file == NULL ||
	    (shared != 0 && shared != 1))
		return EINVAL;

	/* A shared mapping uses the file's object; a writable private one is committed. */
	if (shared) {
		flags = VM_REGION_SHARED;
		commit_size = 0;
	} else {
		flags = 0;
		if ((prot & HAL_SPACE_WRITE) != 0)
			commit_size = size;
		else
			commit_size = 0;
	}

	if (shared) {
		error = vm_object_get_shared(file, &object);
		if (error != 0)
			return error;
	}

	/* Prepares the region fully before replacing the old mappings. */
	error = prepare_region(start, size, prot, VM_BACKING_FILE, file, offset,
	    start, data_size, flags, commit_size, &prepared);
	if (error != 0) {
		if (object != NULL)
			vm_object_put(object);
		return error;
	}

	prepared->object = object;
	error = vmspace_replace_prepared(vm, prepared, result);
	if (error != 0)
		discard_prepared_region(prepared);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Maps the initial stack and its guard region below a top address.
 */
int
vmspace_map_stack(
	struct vmspace *vm,
	uintptr_t top,
	size_t size,
	size_t guard_size)
{
	int error;

	if (vm == NULL || vm == &kernel_vmspace)
		return EINVAL;
	/* Runs the mapping under the metadata and space locks. */
	vm_metadata_enter();
	mutex_lock(&vm->lock);

	error = vmspace_map_stack_locked(vm, top, size, guard_size);
	if (error == 0)
		vmspace_generation_advance_locked(vm);

	mutex_unlock(&vm->lock);
	vm_metadata_leave();

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Finds the region that contains a range, for inspection only.
 */
struct vm_region *
vmspace_find_region(
	struct vmspace *vm,
	uintptr_t address,
	size_t size)
{
	struct vm_region *region;

	/* Looks the region up under the metadata and space locks. */
	if (vm == NULL || vm == &kernel_vmspace)
		return NULL;
	vm_metadata_enter();
	mutex_lock(&vm->lock);

	region = find_region_locked(vm, address, size);

	mutex_unlock(&vm->lock);
	vm_metadata_leave();

	/* Compatibility inspection only; callers must not retain the pointer. */
	return region;
}

/*
 * Reports the shared object and offset that back a user range.
 */
int
vmspace_shared_mapping_key(
	struct vmspace *vm,
	uintptr_t address,
	size_t size,
	struct vm_object **object,
	uintptr_t *offset)
{
	struct vm_region *region;
	int error;

	error = EINVAL;

	if (vm == NULL ||
	    vm == &kernel_vmspace ||
	    object == NULL ||
	    offset == NULL)
		return EINVAL;
	/* Reports the object and offset of a shared file-backed region. */
	vm_metadata_enter();
	mutex_lock(&vm->lock);

	region = find_region_locked(vm, address, size);
	if (region != NULL &&
	    (region->flags & VM_REGION_SHARED) != 0 &&
	    region->object != NULL &&
	    region->file_offset >= 0 &&
	    (uintmax_t)region->file_offset <= UINTPTR_MAX -
	    (address - region->start)) {
		vm_object_ref(region->object);
		*object = region->object;
		*offset = (uintptr_t)region->file_offset + address - region->start;
		error = 0;
	}

	mutex_unlock(&vm->lock);
	vm_metadata_leave();

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Checks that a user range is mapped with the required protection.
 */
int
vmspace_check(
	struct vmspace *vm,
	uintptr_t address,
	size_t size,
	uint32_t required)
{
	int error;

	/* Checks the range under the metadata and space locks. */
	if (vm == NULL || vm == &kernel_vmspace)
		return EFAULT;
	vm_metadata_enter();
	mutex_lock(&vm->lock);

	error = vmspace_check_locked(vm, address, size, required);

	mutex_unlock(&vm->lock);
	vm_metadata_leave();

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Unwires every page of a user range.
 */
void
vmspace_unwire_range(
	struct vmspace *vm,
	uintptr_t address,
	size_t size)
{
	if (vm == NULL || vm == &kernel_vmspace)
		return;

	vm_metadata_enter();
	mutex_lock(&vm->lock);

	vmspace_unwire_range_locked(vm, address, size);
	vmspace_generation_advance_locked(vm);

	mutex_unlock(&vm->lock);
	vm_metadata_leave();
}

/*
 * Copies kernel memory into a user range.
 */
int
vmspace_copy_to(
	struct vmspace *vm,
	uintptr_t destination,
	const void *source,
	size_t size)
{
	int error;

	if (vm == NULL || vm == &kernel_vmspace)
		return EFAULT;

	/* Reports the failure. */
	error = vmspace_copy_to_locked(vm, destination, source, size);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Copies a user range into kernel memory.
 */
int
vmspace_copy_from(
	struct vmspace *vm,
	void *destination,
	uintptr_t source,
	size_t size)
{
	int error;

	if (vm == NULL || vm == &kernel_vmspace)
		return EFAULT;

	/* Reports the failure. */
	error = vmspace_copy_from_locked(vm, destination, source, size);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Finds a free aligned range between two bounds.
 */
int
vmspace_find_free_range_bounded(
	struct vmspace *vm,
	uintptr_t minimum,
	uintptr_t maximum,
	size_t size,
	size_t alignment,
	uintptr_t *mapped)
{
	int error;

	if (vm == NULL || vm == &kernel_vmspace)
		return EINVAL;

	/* Runs the mapping under the metadata and space locks. */
	vm_metadata_enter();
	mutex_lock(&vm->lock);

	error = vmspace_find_free_range_bounded_locked(vm, minimum, maximum, size, alignment, mapped);

	mutex_unlock(&vm->lock);
	vm_metadata_leave();

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Finds a free aligned range at or above a hint in the mmap area.
 */
int
vmspace_find_free_range(
	struct vmspace *vm,
	uintptr_t hint,
	size_t size,
	size_t alignment,
	uintptr_t *mapped)
{
	int error;

	if (vm == NULL || vm == &kernel_vmspace)
		return EINVAL;

	/* Runs the mapping under the metadata and space locks. */
	vm_metadata_enter();
	mutex_lock(&vm->lock);

	error = vmspace_find_free_range_locked(vm, hint, size, alignment, mapped);

	mutex_unlock(&vm->lock);
	vm_metadata_leave();

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Maps anonymous memory at a free address near a hint.
 */
int
vmspace_map_find(
	struct vmspace *vm,
	uintptr_t hint,
	size_t size,
	uint32_t prot,
	uintptr_t *mapped)
{
	int error;

	if (vm == NULL || vm == &kernel_vmspace)
		return EINVAL;

	/* Runs the mapping under the metadata and space locks. */
	vm_metadata_enter();
	mutex_lock(&vm->lock);

	error = vmspace_map_find_locked(vm, hint, size, prot, mapped);
	if (error == 0)
		vmspace_generation_advance_locked(vm);

	mutex_unlock(&vm->lock);
	vm_metadata_leave();

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Maps a private file region at a free address near a hint.
 */
int
vmspace_map_file_find(
	struct vmspace *vm,
	uintptr_t hint,
	size_t size,
	uint32_t prot,
	struct file *file,
	off_t offset,
	size_t data_size,
	uintptr_t *mapped)
{
	int error;

	if (vm == NULL || vm == &kernel_vmspace)
		return EINVAL;
	/* Runs the mapping under the metadata and space locks. */
	vm_metadata_enter();
	mutex_lock(&vm->lock);

	error = vmspace_map_file_find_locked(vm, hint, size, prot, file, offset, data_size, mapped);
	if (error == 0)
		vmspace_generation_advance_locked(vm);

	mutex_unlock(&vm->lock);
	vm_metadata_leave();

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Maps a shared file region at a free address near a hint.
 */
int
vmspace_map_file_shared_find(
	struct vmspace *vm,
	uintptr_t hint,
	size_t size,
	uint32_t prot,
	struct file *file,
	off_t offset,
	size_t data_size,
	uintptr_t *mapped)
{
	struct vm_object *object;
	int error;

	/* Rejects incomplete requests before taking a shared-object reference. */
	if (vm == NULL || vm == &kernel_vmspace || mapped == NULL)
		return EINVAL;

	/* Resolves FAT identity before taking higher-ranked VM metadata locks. */
	error = vm_object_get_shared(file, &object);
	if (error != 0)
		return error;

	/* Selects and publishes the region while the object excludes formatting. */
	vm_metadata_enter();
	mutex_lock(&vm->lock);

	error = vmspace_map_file_shared_find_locked(vm, hint, size, prot, file, offset, data_size, object, mapped);
	if (error == 0)
		vmspace_generation_advance_locked(vm);

	mutex_unlock(&vm->lock);
	vm_metadata_leave();

	/* Releases failed preparation outside address-space locks. */
	if (error != 0)
		vm_object_put(object);

	/* Reports publication or its exact failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Records where the program break starts and how much static data
 * counts against the data limit.
 */
int
vmspace_set_brk_start(
	struct vmspace *vm,
	uintptr_t start,
	uint64_t static_data_bytes)
{
	int error;

	if (vm == NULL || vm == &kernel_vmspace)
		return EINVAL;

	/* Runs the mapping under the metadata and space locks. */
	vm_metadata_enter();
	mutex_lock(&vm->lock);

	error = vmspace_set_brk_start_locked(vm, start, static_data_bytes);
	if (error == 0)
		vmspace_generation_advance_locked(vm);

	mutex_unlock(&vm->lock);
	vm_metadata_leave();

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Moves the program break, or reports it when the request is zero.
 */
int
vmspace_brk(
	struct vmspace *vm,
	uintptr_t requested,
	uintptr_t *result)
{
	int error;

	if (vm == NULL || vm == &kernel_vmspace)
		return EINVAL;

	/* Runs the mapping under the metadata and space locks. */
	vm_metadata_enter();
	mutex_lock(&vm->lock);

	error = vmspace_brk_locked(vm, requested, result);
	if (error == 0 && requested != 0)
		vmspace_generation_advance_locked(vm);

	mutex_unlock(&vm->lock);
	vm_metadata_leave();

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Unmaps a user range, retiring its regions outside the locks.
 */
int
vmspace_unmap(
	struct vmspace *vm,
	uintptr_t start,
	size_t size)
{
	struct vm_region *retired;
	int error;

	retired = NULL;

	if (vm == NULL || vm == &kernel_vmspace)
		return EINVAL;

	/* Runs the mapping under the metadata and space locks. */
	vm_metadata_enter();
	mutex_lock(&vm->lock);

	error = vmspace_unmap_locked(vm, start, size, &retired);
	if (error == 0)
		vmspace_generation_advance_locked(vm);

	mutex_unlock(&vm->lock);
	vm_metadata_leave();

	/*
	 * Test-only weak checkpoint: VA is free and old mappings are
	 * detached, while heavyweight backing/object retirement has not
	 * started yet.
	 */
	if (error == 0 && retired != NULL &&
	    vmspace_unmap_retire_checkpoint != NULL)
		vmspace_unmap_retire_checkpoint(vm, start, size);

	release_retired_regions(retired);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Changes the protection of a user range.
 */
int
vmspace_protect(
	struct vmspace *vm,
	uintptr_t start,
	size_t size,
	uint32_t prot)
{
	int error;

	if (vm == NULL || vm == &kernel_vmspace)
		return EINVAL;

	/* Runs the mapping under the metadata and space locks. */
	vm_metadata_enter();
	mutex_lock(&vm->lock);

	error = vmspace_protect_locked(vm, start, size, prot);
	if (error == 0)
		vmspace_generation_advance_locked(vm);

	mutex_unlock(&vm->lock);
	vm_metadata_leave();

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Sets the limit on mapped virtual bytes.
 */
int
vmspace_set_address_limit(
	struct vmspace *vm,
	uint64_t limit)
{
	int error;

	if (vm == NULL || vm == &kernel_vmspace)
		return EINVAL;

	/* Runs the mapping under the metadata and space locks. */
	vm_metadata_enter();
	mutex_lock(&vm->lock);

	error = vmspace_set_address_limit_locked(vm, limit);
	if (error == 0)
		vmspace_generation_advance_locked(vm);

	mutex_unlock(&vm->lock);
	vm_metadata_leave();

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Sets the limit on data segment growth.
 */
int
vmspace_set_data_limit(
	struct vmspace *vm,
	uint64_t limit)
{
	int error;

	if (vm == NULL || vm == &kernel_vmspace)
		return EINVAL;

	/* Runs the mapping under the metadata and space locks. */
	vm_metadata_enter();
	mutex_lock(&vm->lock);

	error = vmspace_set_data_limit_locked(vm, limit);
	if (error == 0)
		vmspace_generation_advance_locked(vm);

	mutex_unlock(&vm->lock);
	vm_metadata_leave();

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Sets the limit on stack size.
 */
void
vmspace_set_stack_limit(
	struct vmspace *vm,
	uint64_t limit)
{
	if (vm == NULL || vm == &kernel_vmspace)
		return;

	vm_metadata_enter();
	mutex_lock(&vm->lock);

	vmspace_set_stack_limit_locked(vm, limit);
	vmspace_generation_advance_locked(vm);

	mutex_unlock(&vm->lock);
	vm_metadata_leave();
}

/* Publishes captured cache pages as a private mapping, never MAP_SHARED. */
int
vmspace_map_exec_snapshot(
	struct vmspace *vm,
	uintptr_t start,
	uint32_t prot,
	struct file_exec_snapshot *snapshot)
{
	struct vm_region *region;
	int error;

	/* Requires a prepared read-only snapshot and a read-only mapping. */
	if (vm == NULL || vm == &kernel_vmspace || snapshot == NULL ||
	    !snapshot->input.active || !snapshot->input.shared_read ||
	    snapshot->input.read_object == NULL || snapshot->length == 0 ||
	    snapshot->page_count != snapshot->length / PAGE_SIZE ||
	    (prot & HAL_SPACE_WRITE) != 0)
		return EINVAL;

	/* Maps the snapshot's range, keeping the snapshot alive with the region. */
	vm_metadata_enter();
	mutex_lock(&vm->lock);

	error = map_region(vm, start, snapshot->length, prot, VM_BACKING_FILE,
	    snapshot->input.file, snapshot->offset, start, snapshot->length,
	    0, 0, &region);
	if (error == 0) {
		file_exec_snapshot_ref(snapshot);
		region->snapshot = snapshot;
		vmspace_generation_advance_locked(vm);
	}

	mutex_unlock(&vm->lock);
	vm_metadata_leave();

	/* Reports why the mapping failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Allocates one physical page for user memory. */
static int
alloc_vm_page(
	struct hal_pmem *memory)
{
	const struct hal_pmem_request request = {
		HAL_PMEM_PADDR_ANY, PAGE_SIZE, PAGE_SIZE,
		HAL_PMEM_TYPE_RAM, 0
	};
	int error;

	/* Reports the failure. */
	error = hal_pmem_alloc(&request, memory);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Allocates a metadata slab page, reclaiming once when not under the metadata lock. */
static int
alloc_vm_metadata_page(
	struct hal_pmem *memory)
{
	int error;

	/*
	 * A fault normally reaches allocate_page_frame(), which can reclaim
	 * a user page before retrying physical allocation.  A metadata slab
	 * refill happens first, however, so it needs the same bounded retry
	 * or memory pressure can fail a fault before reclaim is attempted at
	 * all.
	 *
	 * Never enter reclaim recursively while the caller owns the
	 * cross-VM metadata lock.  This also covers vmspace_fork(), whose
	 * metadata allocation intentionally remains a nonblocking ENOMEM
	 * path while its source VM is locked.  vm_reclaim_one() itself does
	 * not allocate mapping metadata, so one unlocked retry is sufficient
	 * and cannot recurse through this helper.
	 */
	error = alloc_vm_page(memory);

	if (error == HAL_OK)
		return HAL_OK;

	if (vm_metadata_owned() || vm_reclaim_one(NULL) != 0)
		return error;

	/* Reports the failure. */
	error = alloc_vm_page(memory);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Takes a zeroed page-metadata slot from any slab; the caller holds the slab lock. */
static struct vm_page *
vm_page_slab_take_locked(
	void)
{
	struct vm_page_slab *slab;
	unsigned slot;

	/* Takes the first free slot of the first slab that has one. */

	/* Takes the first free slot of the first slab that has one. */
	for (slab = vm_page_slabs; slab != NULL; slab = slab->next) {
		if (slab->free_mask == 0)
			continue;
		for (slot = 0; slot < VM_PAGE_SLAB_SLOTS; slot++) {
			if ((slab->free_mask & (1U << slot)) != 0)
				break;
		}

		/* Takes the slot the free mask named and hands it back cleared. */
		if (slot == VM_PAGE_SLAB_SLOTS)
			HAL_FATAL("invalid VM page slab bitmap");
		slab->free_mask &= ~(1U << slot);
		slab->used++;
		memset(&slab->slots[slot], 0, sizeof(slab->slots[slot]));
		return &slab->slots[slot];
	}

	/* Reports that every slab is full. */
	return NULL;
}

/* Takes a zeroed private-backing slot from any slab; the caller holds the slab lock. */
static struct vm_private_page *
vm_private_page_slab_take_locked(
	void)
{
	struct vm_private_page_slab *slab;
	unsigned slot;

	/* Takes the first free slot of the first slab that has one. */
	for (slab = vm_private_page_slabs; slab != NULL; slab = slab->next) {
		if (slab->free_mask == 0)
			continue;
		for (slot = 0; slot < VM_PRIVATE_PAGE_SLAB_SLOTS; slot++) {
			if ((slab->free_mask & (1U << slot)) != 0)
				break;
		}

		/* Takes the slot the free mask named and hands it back cleared. */
		if (slot == VM_PRIVATE_PAGE_SLAB_SLOTS)
			HAL_FATAL("invalid VM private metadata slab bitmap");
		slab->free_mask &= ~(1U << slot);
		slab->used++;
		memset(&slab->slots[slot], 0, sizeof(slab->slots[slot]));
		return &slab->slots[slot];
	}

	/* Reports that every slab is full. */
	return NULL;
}

/* Allocates a zeroed private-backing record, growing the slab pool when empty. */
static struct vm_private_page *
vm_private_page_alloc_metadata(
	void)
{
	struct hal_pmem memory;
	struct vm_private_page_slab *fresh;
	struct vm_private_page *backing;
	unsigned long irq;

	vmspace_layout_init();

	/* Takes a free slot from an existing slab. */
	irq = spin_lock_irqsave(&vm_private_page_slab_lock);

	backing = vm_private_page_slab_take_locked();

	spin_unlock_irqrestore(&vm_private_page_slab_lock, irq);

	if (backing != NULL)
		return backing;

	/* Reclaim or a concurrent free may have returned a slab slot. */
	if (alloc_vm_metadata_page(&memory) != HAL_OK) {
		irq = spin_lock_irqsave(&vm_private_page_slab_lock);
		backing = vm_private_page_slab_take_locked();
		spin_unlock_irqrestore(&vm_private_page_slab_lock, irq);
		return backing;
	}

	/* Publishes the new slab unless another one appeared meanwhile. */
	fresh = memory.vaddr;
	memset(fresh, 0, PAGE_SIZE);
	fresh->memory = memory;
	fresh->free_mask = VM_PRIVATE_PAGE_SLAB_FREE_MASK;
	irq = spin_lock_irqsave(&vm_private_page_slab_lock);

	backing = vm_private_page_slab_take_locked();
	if (backing == NULL) {
		fresh->next = vm_private_page_slabs;
		vm_private_page_slabs = fresh;
		backing = vm_private_page_slab_take_locked();
		fresh = NULL;
	}

	spin_unlock_irqrestore(&vm_private_page_slab_lock, irq);

	if (fresh != NULL && hal_pmem_free(&memory) != HAL_OK)
		HAL_FATAL("unused VM private metadata slab free failed");
	return backing;
}

/* Tests whether a page-aligned range lies inside the user address range. */
static int
range_valid(
	uintptr_t start,
	size_t size)
{
	vmspace_layout_init();

	if (size == 0)
		return 0;

	if ((start & (PAGE_SIZE - 1U)) != 0)
		return 0;

	if ((size & (PAGE_SIZE - 1U)) != 0)
		return 0;

	if (start < vm_layout.user_minimum)
		return 0;

	if (start >= vm_layout.user_limit)
		return 0;

	if (size > vm_layout.user_limit - start)
		return 0;

	return 1;
}

/* Tests whether a range intersects any region. */
static int
overlaps(
	struct vmspace *vm,
	uintptr_t start,
	size_t size)
{
	struct vm_region *region;
	uintptr_t end;

	/* Looks for a region that shares any byte with the range. */
	end = start + size;
	for (region = vm->regions; region != NULL; region = region->next) {
		if (start < region->start + region->size && region->start < end)
			return 1;
	}

	/* Reports a range no region touches. */
	return 0;
}

/* Links a region into the address-sorted list. */
static void
insert_region(
	struct vmspace *vm,
	struct vm_region *region)
{
	struct vm_region **link;

	link = &vm->regions;
	while (*link != NULL && (*link)->start < region->start)
		link = &(*link)->next;
	region->next = *link;
	*link = region;
}

/* Reclaims a small reserve of private pages for page-table growth. */
static unsigned
reclaim_page_map_reserve(
	struct vm_page *avoid)
{
	unsigned reclaimed;

	/*
	 * A page-table miss can require more than the one page returned by
	 * a normal frame-allocation retry.  In particular, once the fixed
	 * kernel heap is full, the i386 HAL needs one page-backed descriptor
	 * and one PTE page at a new PDE boundary.  Reclaim a small bounded
	 * reserve while the fault's own mapping is BUSY, then retry the
	 * complete fault transaction.
	 */
	for (reclaimed = 0; reclaimed < VM_PAGE_MAP_RESERVE_PAGES; reclaimed++) {
		if (vm_reclaim_private_one(avoid) != 0)
			break;
	}

	return reclaimed;
}

/* Allocates and initializes a private backing without a frame. */
static struct vm_private_page *
private_page_alloc(
	void)
{
	struct vm_private_page *backing;

	backing = vm_private_page_alloc_metadata();
	if (backing == NULL)
		return NULL;
	vm_private_page_init(backing);
	return backing;
}

/* Attaches a fresh private backing to its first mapping. */
static void
private_page_attach_new(
	struct vm_page *page,
	struct vm_private_page *backing)
{
	unsigned long irq;

	/*
	 * Makes the page the first and only mapping of a fresh backing.
	 */

	irq = spin_lock_irqsave(&backing->state_lock);

	if (backing->mapping_count != 0 || backing->mappings != NULL)
		HAL_FATAL("attaching initialized VM private backing twice");

	backing->mapping_count = 1;
	page->private_page = backing;
	page->private_next = backing->mappings;
	backing->mappings = page;

	spin_unlock_irqrestore(&backing->state_lock, irq);
}

/* Copies a vmspace's regions and shares its private pages; the caller holds the source locks. */
static int
vmspace_fork_locked(
	struct vmspace *source,
	struct vmspace **result,
	struct vm_private_page **wait_backing,
	struct vmspace **failed_copy)
{
	struct vmspace *copy;
	struct vm_region *source_region;
	int error;
	struct vm_region *copy_region;
	struct vm_page *source_page;
	struct vm_private_page *backing;
	struct vm_page *copy_page;
	hal_physaddr_t physical;
	uintptr_t page_address;
	uint64_t reservation_generation;
	uint32_t cow_prot;
	unsigned long state_irq;
	int resident;
	int child_mapped;
	int source_mapped;

	error = 0;

	/* Rejects a kernel source or a missing output. */
	if (source == NULL ||
	    source == &kernel_vmspace ||
	    result == NULL ||
	    wait_backing == NULL ||
	    failed_copy == NULL)
		return EINVAL;

	/* Starts with nothing to wait for and no failed copy. */
	*wait_backing = NULL;
	*failed_copy = NULL;

	/* Waits for every fault in the source to settle. */
	vmspace_wait_faults_locked(source);
	copy = vmspace_create();
	if (copy == NULL)
		return ENOMEM;

	copy->address_limit = source->address_limit;
	copy->data_limit = source->data_limit;
	copy->stack_limit = source->stack_limit;

	/* Copies each region, then shares each of its private pages. */
	for (source_region = source->regions; source_region != NULL;
	     source_region = source_region->next) {
		error = map_region(copy,
				   source_region->start,
				   source_region->size,
				   source_region->prot,
				   source_region->backing,
				   source_region->file,
				   source_region->file_offset,
				   source_region->data_start,
				   source_region->data_size,
				   source_region->flags,
				   source_region->commit_size,
				   &copy_region);
		if (error != 0)
			goto fail;

		copy_region->max_prot = source_region->max_prot;

		if (source_region->snapshot != NULL) {
			file_exec_snapshot_ref(source_region->snapshot);
			copy_region->snapshot = source_region->snapshot;
		}

		if (source_region->object != NULL) {
			vm_object_ref(source_region->object);
			copy_region->object = source_region->object;
			/* Shared object pages are mapped lazily in the child. */
			continue;
		}

		/* Gives the child a mapping descriptor for every one the parent has. */
		for (source_page = source_region->pages; source_page != NULL;
		     source_page = source_page->next) {
			physical = 0;
			child_mapped = 0;
			copy_page = vm_page_alloc_metadata();
			if (copy_page == NULL) {
				error = ENOMEM;
				goto fail;
			}

			copy_page->vm = copy;
			copy_page->region = copy_region;
			copy_page->address = source_page->address;
			if (source_page->object_page != NULL) {
				/* MAP_SHARED pages remain lazy in the child. */
				vm_page_free_metadata(copy_page);
				continue;
			}

			if (source_page->private_page == NULL) {
				vm_page_free_metadata(copy_page);
				error = EFAULT;
				goto fail;
			}

			/*
			 * Pin the source metadata before the backing share.
			 * The backing operation excludes reclaim/pins, while
			 * the local BUSY marker makes unmap/protect/fault wait
			 * when the VM locks are dropped for the source PTE
			 * shootdown.
			 */
			if (source_region->hold_count == (unsigned)-1)
				HAL_FATAL("VM fork region hold overflow");
			source_region->hold_count++;

			/* Takes the source mapping and samples what the child will share. */
			source_page->flags |= VM_MAPPING_BUSY;
			backing = source_page->private_page;
			page_address = source_page->address;
			cow_prot = source_region->prot & ~HAL_SPACE_WRITE;
			source_mapped = (source_page->flags & VM_MAPPING_MAPPED) != 0;
			reservation_generation = source->generation;

			/* Read-only private mappings also need COW for later mprotect. */
			error = vm_page_share_private(source_page, copy_page);
			if (error != 0) {
				if (error == EBUSY) {
					*wait_backing = backing;
					vm_private_page_ref(*wait_backing);
				}

				/* Releases what the failed page took before unwinding. */
				source_page->flags &= ~VM_MAPPING_BUSY;
				if (source_region->hold_count == 0)
					HAL_FATAL("VM fork region hold underflow");
				source_region->hold_count--;
				vmspace_fault_wake_locked(source);
				vm_page_free_metadata(copy_page);
				goto fail;
			}

			/* Links the copy in and samples the backing it will share. */
			copy_page->next = copy_region->pages;
			copy_region->pages = copy_page;
			state_irq = spin_lock_irqsave(&backing->state_lock);
			resident = (backing->flags & VM_PAGE_RESIDENT) != 0;
			if (resident)
				physical = backing->pmem.paddr;

			spin_unlock_irqrestore(&backing->state_lock, state_irq);

			/* Downgrades the source and maps the child with no VM lock held. */
			mutex_unlock(&source->lock);
			vm_metadata_leave();

			if (resident && source_mapped && hal_page_prot(source->space,
			    (void *)page_address, PAGE_SIZE, cow_prot) != HAL_OK)
				error = ENOMEM;

			/* Maps the child read-only so the first write faults. */
			if (error == 0 && resident && source_mapped &&
			    hal_page_map(copy->space, (void *)page_address, physical,
			    PAGE_SIZE, cow_prot) != HAL_OK)
				error = ENOMEM;
			else if (error == 0 && resident && source_mapped)
				child_mapped = 1;

			vm_metadata_enter();
			mutex_lock(&source->lock);

			/* Releases the pin after checking that nothing moved. */
			if (source_page->region != source_region ||
			    source_page->address != page_address ||
			    source_page->private_page != backing ||
			    find_page(source_region, page_address) != source_page)
				HAL_FATAL("VM fork lost pinned source mapping");

			if (child_mapped)
				copy_page->flags |= VM_MAPPING_MAPPED;

			if (error == 0 && source->generation != reservation_generation)
				error = EAGAIN;

			source_page->flags &= ~VM_MAPPING_BUSY;
			if (source_region->hold_count == 0)
				HAL_FATAL("VM fork region hold underflow");

			source_region->hold_count--;
			vm_private_page_operation_end(backing);
			vmspace_fault_wake_locked(source);

			if (error != 0)
				goto fail;

			vmspace_generation_advance_locked(source);
		}
	}

	/* Copies the layout bookkeeping. */
	copy->entry = source->entry;
	copy->brk_start = source->brk_start;
	copy->brk_current = source->brk_current;
	copy->static_data_bytes = source->static_data_bytes;
	copy->stack_guard_bottom = source->stack_guard_bottom;
	copy->stack_bottom = source->stack_bottom;
	copy->stack_top = source->stack_top;
	*result = copy;

	return 0;

fail:
	/* Child teardown may free pages or sync objects, so retire it locklessly. */
	*failed_copy = copy;

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Creates a region in a vmspace at a range that must be free; the caller holds the VM locks. */
static int
map_region(
	struct vmspace *vm,
	uintptr_t start,
	size_t size,
	uint32_t prot,
	enum vm_region_backing backing,
	struct file *file,
	off_t file_offset,
	uintptr_t data_start,
	size_t data_size,
	unsigned flags,
	size_t commit_size,
	struct vm_region **result)
{
	struct vm_region *region;
	uint32_t max_prot;
	uint64_t mapped_bytes;
	int error;

	max_prot = HAL_SPACE_READ | HAL_SPACE_WRITE | HAL_SPACE_EXEC;

	/* Validates the range, the address limit, and the file description. */
	if (vm == NULL ||
	    vm == &kernel_vmspace ||
	    !range_valid(start, size) ||
	    overlaps(vm, start, size) ||
	    (prot & ~(HAL_SPACE_READ | HAL_SPACE_WRITE | HAL_SPACE_EXEC)) != 0)
		return EINVAL;

	mapped_bytes = vm->mapped_virtual_bytes;
	if (mapped_bytes > vm->address_limit)
		mapped_bytes = vm->address_limit;

	if ((uint64_t)size > vm->address_limit - mapped_bytes)
		return ENOMEM;

	if (backing == VM_BACKING_FILE &&
	    (file == NULL || file_offset < 0 || data_start < start ||
	     data_start >= start + size || data_size > start + size - data_start))
		return EINVAL;

	/* A shared mapping of an unwritable file can never become writable. */
	if ((flags & VM_REGION_SHARED) != 0 && file != NULL &&
	    ((file_status_flags_get(file) & O_ACCMODE) == O_RDONLY ||
	     file->f_ops == NULL || file->f_ops->pwrite == NULL))
		max_prot &= ~HAL_SPACE_WRITE;
	if ((prot & ~max_prot) != 0)
		return EACCES;

	/* Reserves the commit charge and links the region. */
	region = kern_calloc(1, sizeof(*region));
	if (region == NULL)
		return ENOMEM;
	if (commit_size != 0) {
		error = vm_commit_reserve(commit_size);
		if (error != 0) {
			kern_free(region);
			return error;
		}
	}

	/* Describes the mapping and publishes it in the address space. */
	region->start = start;
	region->size = size;
	region->prot = prot;
	region->max_prot = max_prot;
	region->flags = flags;
	region->commit_size = commit_size;
	region->backing = backing;
	region->file = file;
	region->file_offset = file_offset;
	region->data_start = data_start;
	region->data_size = data_size;

	if (file != NULL)
		file_ref(file);

	insert_region(vm, region);
	vm->mapped_virtual_bytes += size;

	if (result != NULL)
		*result = region;

	return 0;
}

/* Builds a region that is not yet visible in any vmspace. */
static int
prepare_region(
	uintptr_t start,
	size_t size,
	uint32_t prot,
	enum vm_region_backing backing,
	struct file *file,
	off_t file_offset,
	uintptr_t data_start,
	size_t data_size,
	unsigned flags,
	size_t commit_size,
	struct vm_region **result)
{
	struct vm_region *region;
	uint32_t max_prot;
	int error;

	max_prot = HAL_SPACE_READ | HAL_SPACE_WRITE | HAL_SPACE_EXEC;

	/*
	 * MAP_FIXED uses this to reserve every fallible resource before it
	 * detaches an old mapping.
	 */
	if (result == NULL ||
	    !range_valid(start, size) ||
	    (prot & ~(HAL_SPACE_READ | HAL_SPACE_WRITE | HAL_SPACE_EXEC)) != 0)
		return EINVAL;
	if (backing == VM_BACKING_FILE &&
	    (file == NULL || file_offset < 0 || data_start < start ||
	    data_start >= start + size || data_size > start + size - data_start))
		return EINVAL;

	/* A shared mapping of an unwritable file can never become writable. */
	if ((flags & VM_REGION_SHARED) != 0 && file != NULL &&
	    ((file_status_flags_get(file) & O_ACCMODE) == O_RDONLY ||
	    file->f_ops == NULL || file->f_ops->pwrite == NULL))
		max_prot &= ~HAL_SPACE_WRITE;
	if ((prot & ~max_prot) != 0)
		return EACCES;

	/* Reserves the commit charge and fills the region in. */
	region = kern_calloc(1, sizeof(*region));
	if (region == NULL)
		return ENOMEM;
	if (commit_size != 0) {
		error = vm_commit_reserve(commit_size);
		if (error != 0) {
			kern_free(region);
			return error;
		}
	}

	/* Describes the mapping without publishing it yet. */
	region->start = start;
	region->size = size;
	region->prot = prot;
	region->max_prot = max_prot;
	region->flags = flags;
	region->commit_size = commit_size;
	region->backing = backing;
	region->file = file;
	region->file_offset = file_offset;
	region->data_start = data_start;
	region->data_size = data_size;
	if (file != NULL)
		file_ref(file);
	*result = region;
	return 0;
}

/* Releases a prepared region that was never published. */
static void
discard_prepared_region(
	struct vm_region *region)
{
	if (region == NULL)
		return;
	if (region->file != NULL)
		(void)file_close(region->file);
	if (region->object != NULL)
		vm_object_put(region->object);
	if (region->snapshot != NULL)
		file_exec_snapshot_put(region->snapshot);
	if (region->commit_size != 0)
		vm_commit_release(region->commit_size);
	kern_free(region);
}

/* Maps anonymous memory at a free range; the caller holds the VM locks. */
static int
vmspace_map_anon_locked(
	struct vmspace *vm,
	uintptr_t start,
	size_t size,
	uint32_t prot,
	struct vm_region **result)
{
	size_t commit_size;
	int error;

	/* Inaccessible memory is not committed until it becomes accessible. */
	if (prot != 0)
		commit_size = size;
	else
		commit_size = 0;

	/* Reports the failure. */
	error = map_region(vm, start, size, prot, VM_BACKING_ANON, NULL, 0, start, 0, 0, commit_size, result);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Maps anonymous memory at a range that must not be in use; the caller holds the VM locks. */
static int
vmspace_map_anon_fixed_noreplace_locked(
	struct vmspace *vm,
	uintptr_t start,
	size_t size,
	uint32_t prot,
	struct vm_region **result)
{
	int error;

	/* Refuses a malformed range, or one an existing region covers. */
	if (vm == NULL || !range_valid(start, size))
		return EINVAL;
	if (overlaps(vm, start, size))
		return EEXIST;

	/* Maps the range as ordinary anonymous memory. */

	/* Reports why the mapping failed. */
	error = vmspace_map_anon_locked(vm, start, size, prot, result);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Maps a private file region at a free range; the caller holds the VM locks. */
static int
vmspace_map_file_locked(
	struct vmspace *vm,
	uintptr_t start,
	size_t size,
	uint32_t prot,
	struct file *file,
	off_t file_offset,
	uintptr_t data_start,
	size_t data_size,
	struct vm_region **result)
{
	size_t commit_size;
	int error;

	/* Only a writable private file mapping needs commit. */
	if ((prot & HAL_SPACE_WRITE) != 0)
		commit_size = size;
	else
		commit_size = 0;

	/* Reports the failure. */
	error = map_region(vm, start, size, prot, VM_BACKING_FILE, file, file_offset, data_start, data_size, 0, commit_size, result);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Maps a shared file region at a free range; the caller holds the VM locks. */
static int
vmspace_map_file_shared_locked(
	struct vmspace *vm,
	uintptr_t start,
	size_t size,
	uint32_t prot,
	struct file *file,
	off_t file_offset,
	size_t data_size,
	struct vm_object *object,
	struct vm_region **result)
{
	struct vm_region *region;
	int error;

	/* Requires a prepared object and an aligned file offset. */
	if (object == NULL)
		return EINVAL;

	/* Rejects offsets that cannot describe shared pages. */
	if (file_offset < 0 || (file_offset & (PAGE_SIZE - 1U)) != 0)
		return EINVAL;

	/* Creates the region while its address-space metadata is locked. */
	error = map_region(vm,
			   start,
			   size,
			   prot,
			   VM_BACKING_FILE,
			   file,
			   file_offset,
			   start,
			   data_size,
			   VM_REGION_SHARED,
			   0,
			   &region);
	if (error != 0)
		return error;

	/* Transfers the prepared shared-object reference to the region. */
	region->object = object;

	/* Returns the published region when requested. */
	if (result != NULL)
		*result = region;

	/* Reports successful publication. */
	return 0;
}

/* Maps the stack and its guard below a top address; the caller holds the VM locks. */
static int
vmspace_map_stack_locked(
	struct vmspace *vm,
	uintptr_t top,
	size_t size,
	size_t guard_size)
{
	struct vm_region *guard;
	struct vm_region *stack;
	uintptr_t bottom;
	uintptr_t guard_bottom;
	uint64_t mapped_bytes;
	int error;

	/* Validates the geometry against the layout and the limits. */
	if (vm == NULL ||
	    vm == &kernel_vmspace ||
	    size == 0 ||
	    guard_size == 0 ||
	    (top & (PAGE_SIZE - 1U)) != 0 ||
	    (size & (PAGE_SIZE - 1U)) != 0 ||
	    (guard_size & (PAGE_SIZE - 1U)) != 0 ||
	    top > vm_layout.user_limit ||
	    size > top ||
	    guard_size > top - size)
		return EINVAL;

	mapped_bytes = vm->mapped_virtual_bytes;
	if (mapped_bytes > vm->address_limit)
		mapped_bytes = vm->address_limit;

	if ((uint64_t)size > vm->stack_limit ||
	    (uint64_t)size + guard_size > vm->address_limit - mapped_bytes)
		return ENOMEM;

	bottom = top - size;
	guard_bottom = bottom - guard_size;
	if (!range_valid(guard_bottom, size + guard_size) ||
	    overlaps(vm, guard_bottom, size + guard_size))
		return EINVAL;

	/* Allocates both regions and the stack's commit charge. */
	guard = kern_calloc(1, sizeof(*guard));
	if (guard == NULL)
		return ENOMEM;

	stack = kern_calloc(1, sizeof(*stack));
	if (stack == NULL) {
		kern_free(guard);
		return ENOMEM;
	}

	/* Reserves the commitment the stack will use. */
	error = vm_commit_reserve(size);
	if (error != 0) {
		kern_free(stack);
		kern_free(guard);
		return error;
	}

	/* The guard is inaccessible and immutable; the stack is read-write. */
	guard->start = guard_bottom;
	guard->size = guard_size;
	guard->backing = VM_BACKING_ANON;
	guard->flags = VM_REGION_GUARD | VM_REGION_IMMUTABLE;
	guard->max_prot = 0;
	guard->data_start = guard_bottom;
	stack->start = bottom;
	stack->size = size;
	stack->prot = HAL_SPACE_READ | HAL_SPACE_WRITE;
	stack->max_prot = stack->prot;
	stack->flags = VM_REGION_STACK | VM_REGION_IMMUTABLE;
	stack->commit_size = size;
	stack->backing = VM_BACKING_ANON;
	stack->data_start = bottom;

	insert_region(vm, guard);
	insert_region(vm, stack);

	vm->mapped_virtual_bytes += size + guard_size;
	vm->stack_guard_bottom = guard_bottom;
	vm->stack_bottom = bottom;
	vm->stack_top = top;

	return 0;
}

/* Finds the region that contains a whole range; the caller holds the VM lock. */
static struct vm_region *
find_region_locked(
	struct vmspace *vm,
	uintptr_t address,
	size_t size)
{
	struct vm_region *region;

	/* Rejects a malformed query. */
	if (vm == NULL || size == 0 || address > UINTPTR_MAX - size)
		return NULL;

	/* Looks for the region that contains the whole range. */
	for (region = vm->regions; region != NULL; region = region->next) {
		if (address >= region->start &&
		    address + size <= region->start + region->size)
			return region;
	}

	/* Reports a range no single region covers. */
	return NULL;
}

/* Finds the page record of an address within a region. */
static struct vm_page *
find_page(
	struct vm_region *region,
	uintptr_t address)
{
	struct vm_page *page;

	/* Looks for the page that starts at the containing page address. */
	address &= ~(uintptr_t)(PAGE_SIZE - 1U);
	for (page = region->pages; page != NULL; page = page->next) {
		if (page->address == address)
			return page;
	}

	return NULL;
}

/* Sleeps until the fault wait queue advances past an observed sequence. */
static void
vmspace_wait_fault_event(
	struct vmspace *vm,
	uint64_t sequence)
{
	unsigned long irq;

	irq = spin_lock_irqsave(&vm->lock.guard);

	(void)waitq_sleep(&vm->fault_waitq, &vm->lock.guard, sequence, 0, 0);

	spin_unlock_irqrestore(&vm->lock.guard, irq);
}

/* Waits until no region is held and no page is BUSY; the caller holds the VM locks. */
static void
vmspace_wait_faults_locked(
	struct vmspace *vm)
{
	struct vm_region *region;
	int busy;
	struct vm_page *page;
	uint64_t sequence;

	for (;;) {
		/* Scans for any fault in progress. */
		busy = 0;
		for (region = vm->regions; region != NULL; region = region->next) {
			if (region->hold_count != 0) {
				busy = 1;
				break;
			}

			/* Looks for a mapping of this region that a fault still owns. */
			for (page = region->pages; page != NULL; page = page->next) {
				if ((page->flags & VM_MAPPING_BUSY) != 0) {
					busy = 1;
					break;
				}
			}

			if (busy)
				break;
		}

		if (!busy)
			return;

		/* Fault completion needs the outer cross-vm metadata lock. */
		sequence = waitq_sequence(&vm->fault_waitq);
		mutex_unlock(&vm->lock);
		vm_metadata_leave();
		vmspace_wait_fault_event(vm, sequence);
		vm_metadata_enter();
		mutex_lock(&vm->lock);
	}
}

/* Allocates a frame for a private backing, reclaiming one page on failure. */
static int
allocate_page_frame(
	struct vm_private_page *backing,
	struct vm_page *avoid)
{
	if (alloc_vm_page(&backing->pmem) == HAL_OK)
		return 0;
	if (vm_reclaim_one(avoid) != 0)
		return ENOMEM;
	if (alloc_vm_page(&backing->pmem) != HAL_OK)
		return ENOMEM;
	return 0;
}

/* Fills a private page from the region's file, zeroing the rest. */
static int
fill_file_page(
	struct vm_region *region,
	struct vm_page *page)
{
	uintptr_t page_end;
	uintptr_t data_end;
	uintptr_t read_start;
	uintptr_t read_end;
	uint64_t offset_max;
	size_t length;
	off_t offset;
	ssize_t count;

	/* Intersects the page with the region's data range. */
	page_end = page->address + PAGE_SIZE;
	data_end = region->data_start + region->data_size;
	if (page->address > region->data_start)
		read_start = page->address;
	else
		read_start = region->data_start;
	if (page_end < data_end)
		read_end = page_end;
	else
		read_end = data_end;

	/* A page outside the data is zero only for an ELF zero tail. */
	memset((void *)page->private_page->pmem.vaddr, 0, PAGE_SIZE);
	if (read_start >= read_end) {
		if ((region->flags & VM_REGION_ELF_ZERO_TAIL) != 0)
			return 0;
		return ENXIO;
	}

	/* Reads the overlapping bytes at their file offset. */
	length = read_end - read_start;
	if (sizeof(off_t) == sizeof(int64_t))
		offset_max = (uint64_t)INT64_MAX;
	else
		offset_max = (uint64_t)INT32_MAX;

	if (region->file_offset < 0 ||
	    (uint64_t)(read_start - region->data_start) >
	    offset_max - (uint64_t)region->file_offset)
		return EOVERFLOW;

	offset = region->file_offset +
		(off_t)(read_start - region->data_start);

	/* Reports a short read as a device error. */
	count = file_pread(region->file,
		(void *)(page->private_page->pmem.vaddr +
		    read_start - page->address),
		length, offset);
	if (count == (ssize_t)length)
		return 0;
	return EIO;
}

/* Builds an unlinked copy of a private page while the fault owns the old one. */
static int
prepare_cow_copy(
	struct vm_page *page,
	struct vm_private_page *old,
	struct vm_private_page **result)
{
	struct vm_private_page *fresh;
	unsigned long irq;

	if (page == NULL || old == NULL || result == NULL)
		return EINVAL;
	/* Allocates a backing and a frame for the private copy. */
	fresh = private_page_alloc();
	if (fresh == NULL)
		return ENOMEM;

	if (allocate_page_frame(fresh, page) != 0) {
		vm_private_page_put(fresh);
		return ENOMEM;
	}

	/* Copies the shared page and publishes the copy as resident and dirty. */
	memcpy((void *)fresh->pmem.vaddr, (const void *)old->pmem.vaddr,
	    PAGE_SIZE);
	irq = spin_lock_irqsave(&fresh->state_lock);

	fresh->flags = VM_PAGE_RESIDENT | VM_PAGE_DIRTY;
	fresh->generation++;
	if (fresh->generation == 0)
		fresh->generation++;

	spin_unlock_irqrestore(&fresh->state_lock, irq);

	*result = fresh;

	/* Reports the prepared copy. */
	return 0;
}

/* Wakes every waiter on the fault queue; the caller holds the VM lock. */
static void
vmspace_fault_wake_locked(
	struct vmspace *vm)
{
	unsigned long irq;

	irq = spin_lock_irqsave(&vm->lock.guard);

	waitq_wake_all(&vm->fault_waitq);

	spin_unlock_irqrestore(&vm->lock.guard, irq);
}

/* Checks that a range is covered by regions with the required protection; the caller holds the VM lock. */
static int
vmspace_check_locked(
	struct vmspace *vm,
	uintptr_t address,
	size_t size,
	uint32_t required)
{
	struct vm_region *region;
	uintptr_t current;
	uintptr_t end;
	uintptr_t region_end;

	if (vm == NULL || !vmspace_user_range_valid(address, size))
		return EFAULT;

	/* Walks the sorted regions, refusing any gap. */
	current = address;
	end = address + size;
	for (region = vm->regions; region != NULL && current < end;
	     region = region->next) {
		region_end = region->start + region->size;
		if (current < region->start)
			return EFAULT;
		if (current >= region_end)
			continue;
		if ((region->prot & required) != required)
			return EFAULT;
		if (region_end < end)
			current = region_end;
		else
			current = end;
	}

	if (current == end)
		return 0;
	return EFAULT;
}

/* Tests whether a page can be pinned without entering the fault path. */
static int
vmspace_pin_mapping_ready(
	struct vmspace *vm,
	uintptr_t address,
	uint32_t required)
{
	struct vm_region *region;
	struct vm_page *entry;
	int ready;
	struct vm_object_page *page;
	struct vm_object *object;
	unsigned long irq;

	ready = 0;

	/*
	 * A resident mapping does not need to enter the fault path again
	 * merely to acquire another uaccess pin.  This matters when one
	 * syscall pins multiple ranges which share a user page: the fault
	 * path takes exclusive backing I/O ownership and would otherwise
	 * wait for the syscall's first pin.
	 */
	vm_metadata_enter();
	mutex_lock(&vm->lock);

	region = find_region_locked(vm, address, 1);
	if (region != NULL)
		entry = find_page(region, address);
	else
		entry = NULL;

	/* A mapping is ready when it is present, permitted, and past any copy-on-write. */
	if (entry != NULL &&
	    (region->prot & required) == required &&
	    (entry->flags & (VM_MAPPING_BUSY | VM_MAPPING_MAPPED)) ==
	    VM_MAPPING_MAPPED) {
		if (entry->private_page != NULL &&
		    vm_private_page_is_resident(entry) &&
		    ((required & HAL_SPACE_WRITE) == 0 ||
		    (entry->flags & VM_MAPPING_COW) == 0)) {
			ready = 1;
		} else if (entry->object_page != NULL &&
		    ((required & HAL_SPACE_WRITE) == 0 ||
		    (entry->flags & VM_MAPPING_COW) == 0)) {
			/* A cached object page must be complete and not in error. */
			page = entry->object_page;
			object = page->owner;
			if (object != NULL) {
				irq = spin_lock_irqsave(&object->lock);
				ready = 0;
				if (page->owner == object &&
				    page->pmem.size != 0 &&
				    (page->flags & (VM_OBJECT_PAGE_BUSY |
				    VM_OBJECT_PAGE_ERROR)) == 0)
					ready = 1;
				spin_unlock_irqrestore(&object->lock, irq);
			}
		}
	}

	mutex_unlock(&vm->lock);
	vm_metadata_leave();

	return ready;
}

/* Unwires every page of a range; the caller holds the VM lock. */
static void
vmspace_unwire_range_locked(
	struct vmspace *vm,
	uintptr_t address,
	size_t size)
{
	uintptr_t page;
	uintptr_t last;
	struct vm_region *region;
	struct vm_page *entry;

	if (vm == NULL || size == 0 || address > UINTPTR_MAX - size)
		return;

	/* Drops one wire from every mapped page of the range. */
	page = address & ~(uintptr_t)(PAGE_SIZE - 1U);
	last = (address + size - 1U) & ~(uintptr_t)(PAGE_SIZE - 1U);
	for (;;) {
		region = find_region_locked(vm, page, 1);
		if (region != NULL)
			entry = find_page(region, page);
		else
			entry = NULL;
		if (entry != NULL && entry->wire_count != 0)
			entry->wire_count--;
		if (page == last)
			break;
		page += PAGE_SIZE;
	}
}

/* Copies between kernel memory and a pinned user range in either direction. */
static int
copy_backing(
	struct vmspace *vm,
	uintptr_t user_address,
	void *kernel,
	size_t size,
	uint32_t required,
	int to_user)
{
	struct vmspace_pinned_page *pages;
	uint8_t *bytes;
	uintptr_t first;
	uintptr_t last;
	size_t page_count;
	size_t position;
	int error;
	struct vmspace_pinned_page *page;
	size_t offset;
	size_t chunk;
	void *mapped;

	bytes = kernel;

	/* Pins every page of the range. */
	if (size == 0)
		return 0;

	if (kernel == NULL)
		return EINVAL;

	if (!vmspace_user_range_valid(user_address, size))
		return EFAULT;

	first = user_address & ~(uintptr_t)(PAGE_SIZE - 1U);
	last = (user_address + size - 1U) & ~(uintptr_t)(PAGE_SIZE - 1U);
	page_count = (size_t)((last - first) / PAGE_SIZE) + 1U;
	if (page_count > SIZE_MAX / sizeof(*pages))
		return ENOMEM;

	pages = kern_calloc(page_count, sizeof(*pages));
	if (pages == NULL)
		return ENOMEM;

	/* Pins every page of the range before any byte moves. */
	error = vmspace_pin_user_pages(vm, user_address, size, required, pages, page_count);
	if (error != 0) {
		kern_free(pages);
		if (error == ENOMEM)
			return ENOMEM;
		return EFAULT;
	}

	/* Copies one page-bounded chunk at a time. */
	position = user_address - first;
	while (size != 0) {
		page = &pages[position / PAGE_SIZE];
		offset = position & (PAGE_SIZE - 1U);
		chunk = PAGE_SIZE - offset;
		if (chunk > size)
			chunk = size;
		if (page->kind == VMSPACE_PINNED_PRIVATE) {
			mapped = (uint8_t *)page->memory.vaddr + offset;
			if (to_user) {
				memcpy(mapped, bytes, chunk);
				vm_private_page_mark_dirty(
				    page->owner.private_page);
			} else {
				memcpy(bytes, mapped, chunk);
			}

			/* Copies through whichever backing the pin holds. */
			error = 0;
		} else if (page->kind == VMSPACE_PINNED_OBJECT) {
			if (to_user)
				error = vm_object_page_pin_write(
				    page->owner.object_page, offset, bytes, chunk);
			else
				error = vm_object_page_pin_read(
				    page->owner.object_page, offset, bytes, chunk);
		} else {
			error = EFAULT;
		}

		if (error != 0)
			break;

		/* Advances past the bytes this pin carried. */
		bytes += chunk;
		position += chunk;
		size -= chunk;
	}

	vmspace_unpin_user_pages(pages, page_count);
	kern_free(pages);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Copies kernel memory into a user range. */
static int
vmspace_copy_to_locked(
	struct vmspace *vm,
	uintptr_t destination,
	const void *source,
	size_t size)
{
	int error;

	/* Reports the failure. */
	error = copy_backing(vm, destination, (void *)source, size, HAL_SPACE_WRITE, 1);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Copies a user range into kernel memory. */
static int
vmspace_copy_from_locked(
	struct vmspace *vm,
	void *destination,
	uintptr_t source,
	size_t size)
{
	int error;

	/* Reports the failure. */
	error = copy_backing(vm, source, destination, size,
	    HAL_SPACE_READ, 0);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Finds a free aligned range between two bounds; the caller holds the VM lock. */
static int
vmspace_find_free_range_bounded_locked(
	struct vmspace *vm,
	uintptr_t minimum,
	uintptr_t maximum,
	size_t size,
	size_t alignment,
	uintptr_t *mapped)
{
	uintptr_t start;
	uintptr_t limit;
	struct vm_region *region;
	int moved;

	vmspace_layout_init();

	/* Validates the bounds and rounds the size up to pages. */
	if (vm == NULL ||
	    mapped == NULL ||
	    size == 0 ||
	    alignment < PAGE_SIZE ||
	    (alignment & (alignment - 1U)) != 0 ||
	    minimum < vm_layout.user_minimum ||
	    maximum > vm_layout.user_limit ||
	    minimum >= maximum ||
	    size > (size_t)(maximum - minimum))
		return EINVAL;

	if (size > SIZE_MAX - (PAGE_SIZE - 1U))
		return EOVERFLOW;

	size = (size + PAGE_SIZE - 1U) & ~(PAGE_SIZE - 1U);
	limit = maximum;
	if (vm->stack_guard_bottom != 0 && vm->stack_guard_bottom < limit)
		limit = vm->stack_guard_bottom;

	start = minimum;
	if (start > UINTPTR_MAX - (alignment - 1U))
		return ENOMEM;

	start = (start + alignment - 1U) & ~(uintptr_t)(alignment - 1U);

	/* Slides the candidate past every region it collides with. */
	for (;;) {
		moved = 0;

		if (start >= limit || size > limit - start)
			return ENOMEM;

		for (region = vm->regions; region != NULL; region = region->next) {
			if (start + size <= region->start)
				break;

			/* Moves the candidate past a region it overlaps. */
			if (start < region->start + region->size &&
			    region->start < start + size) {
				if (region->start > UINTPTR_MAX - region->size ||
				    region->start + region->size >
				    UINTPTR_MAX - (alignment - 1U))
					return ENOMEM;
				start = (region->start + region->size + alignment - 1U) &
					~(uintptr_t)(alignment - 1U);
				moved = 1;
				break;
			}
		}

		if (!moved)
			break;
	}

	*mapped = start;
	return 0;
}

/* Finds a free aligned range at or above a hint in the mmap area; the caller holds the VM lock. */
static int
vmspace_find_free_range_locked(
	struct vmspace *vm,
	uintptr_t hint,
	size_t size,
	size_t alignment,
	uintptr_t *mapped)
{
	int error;

	/* Clamps the hint into the mapping area of the layout. */
	vmspace_layout_init();
	if (hint < vm_layout.mmap_base)
		hint = vm_layout.mmap_base;
	if (hint >= vm_layout.user_limit)
		return ENOMEM;

	/* Searches from the hint up to the end of the user area. */

	/* Reports why the search failed. */
	error = vmspace_find_free_range_bounded_locked(vm, hint, vm_layout.user_limit, size, alignment, mapped);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Maps anonymous memory at a free range near a hint; the caller holds the VM locks. */
static int
vmspace_map_find_locked(
	struct vmspace *vm,
	uintptr_t hint,
	size_t size,
	uint32_t prot,
	uintptr_t *mapped)
{
	uintptr_t start;
	int error;

	error = vmspace_find_free_range_locked(vm, hint, size, PAGE_SIZE, &start);
	if (error == 0)
		error = vmspace_map_anon_locked(vm, start,
		    (size + PAGE_SIZE - 1U) & ~(PAGE_SIZE - 1U), prot, NULL);
	if (error == 0)
		*mapped = start;

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Maps a private file region at a free range near a hint; the caller holds the VM locks. */
static int
vmspace_map_file_find_locked(
	struct vmspace *vm,
	uintptr_t hint,
	size_t size,
	uint32_t prot,
	struct file *file,
	off_t offset,
	size_t data_size,
	uintptr_t *mapped)
{
	uintptr_t start;
	size_t rounded;
	int error;

	/* Finds a free range large enough for the mapping. */
	error = vmspace_find_free_range_locked(vm, hint, size, PAGE_SIZE, &start);
	if (error != 0)
		return error;

	/* Maps the file there, clamping the data to the mapping. */
	rounded = (size + PAGE_SIZE - 1U) & ~(PAGE_SIZE - 1U);

	if (data_size > size)
		data_size = size;

	error = vmspace_map_file_locked(vm, start, rounded, prot, file, offset, start, data_size, NULL);
	if (error == 0)
		*mapped = start;

	/* Reports why the mapping failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Maps a shared file region at a free range near a hint; the caller holds the VM locks. */
static int
vmspace_map_file_shared_find_locked(
	struct vmspace *vm,
	uintptr_t hint,
	size_t size,
	uint32_t prot,
	struct file *file,
	off_t offset,
	size_t data_size,
	struct vm_object *object,
	uintptr_t *mapped)
{
	uintptr_t start;
	size_t rounded;
	int error;

	/* Finds a valid page-aligned destination before publishing a region. */
	error = vmspace_find_free_range_locked(vm, hint, size, PAGE_SIZE, &start);
	if (error != 0)
		return error;

	/* Bounds the file portion to the requested mapping length. */
	rounded = (size + PAGE_SIZE - 1U) & ~(PAGE_SIZE - 1U);
	if (data_size > size)
		data_size = size;

	/* Publishes the prepared object only after address selection succeeds. */
	error = vmspace_map_file_shared_locked(
		vm, start, rounded, prot, file, offset, data_size, object, NULL);
	if (error == 0)
		*mapped = start;

	/* Reports the mapping outcome to the prepared-object owner. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Unmaps and frees one page record during teardown. */
static void
free_vm_page(
	struct vmspace *vm,
	struct vm_page *page)
{
	/* An object page only drops its reverse mapping. */
	if (page->object_page != NULL) {
		if (page->flags & VM_MAPPING_MAPPED)
			(void)hal_page_unmap(vm->space, (void *)page->address, PAGE_SIZE);
		vm_object_mapping_remove(page->object_page, page);
		vm_page_free_metadata(page);
		return;
	}

	if (page->flags & VM_MAPPING_MAPPED)
		(void)hal_page_unmap(vm->space, (void *)page->address, PAGE_SIZE);
	vm_page_untrack(page);
	vm_page_free_metadata(page);
}

/* Frees every page of a region during teardown. */
static void
free_region_pages(
	struct vmspace *vm,
	struct vm_region *region)
{
	struct vm_page *page;

	/* Frees every page the region still holds. */
	page = region->pages;
	while (page != NULL) {
		region->pages = page->next;
		free_vm_page(vm, page);
		page = region->pages;
	}
}

/* Removes a page's hardware and reverse mappings for unmap; the caller holds the VM locks. */
static void
detach_vm_page_for_unmap(
	struct vmspace *vm,
	struct vm_page *page)
{
	struct vm_private_page *backing;

	/*
	 * vmspace_unmap() has two teardown phases.  This first phase runs
	 * while both the global VM metadata lock and the vmspace lock are
	 * held.  It must remove every hardware and reverse mapping before
	 * the region list publishes the virtual address as free.
	 */
	if ((page->flags & VM_MAPPING_MAPPED) != 0) {
		if (hal_page_unmap(vm->space, (void *)page->address,
		    PAGE_SIZE) != HAL_OK)
			HAL_FATAL("VM unmap commit failed");
		page->flags &= ~VM_MAPPING_MAPPED;
	}

	/* An object mapping only leaves the object's reverse mapping list. */
	if (page->object_page != NULL) {
		vm_object_mapping_remove(page->object_page, page);
		page->object_page = NULL;
		page->object_next = NULL;
		return;
	}

	/*
	 * A private backing can own physical memory or a swap slot.  Take a
	 * temporary reference before vm_page_untrack() so that the
	 * reverse-map detach cannot be the operation which frees those
	 * resources.  The detached vm_page retains that reference solely as
	 * a retire token; release_detached_region_pages() drops it after
	 * the address has been unpublished and both locks are gone.
	 */
	backing = page->private_page;
	if (backing == NULL)
		return;
	vm_private_page_ref(backing);
	vm_page_untrack(page);
	page->private_page = backing;
	page->private_next = NULL;
}

/* Detaches every page of a region for unmap; the caller holds the VM locks. */
static void
detach_region_pages_for_unmap(
	struct vmspace *vm,
	struct vm_region *region)
{
	struct vm_page *page;

	for (page = region->pages; page != NULL; page = page->next)
		detach_vm_page_for_unmap(vm, page);
}

/* Frees the detached pages of a retired region outside the locks. */
static void
release_detached_region_pages(
	struct vm_region *region)
{
	struct vm_page *page;

	page = region->pages;
	while (page != NULL) {
		region->pages = page->next;
		/* Drops the private-backing retire token, if there is one. */
		if (page->private_page != NULL)
			vm_private_page_put(page->private_page);
		vm_page_free_metadata(page);
		page = region->pages;
	}
}

/* Splits a region at an address into itself and a preallocated right half. */
static int
split_region_prepared(
	struct vm_region *region,
	uintptr_t address,
	struct vm_region *right)
{
	struct vm_page **link;
	size_t left_size;
	size_t right_size;
	uintptr_t original_data_start;
	uintptr_t original_data_end;
	uintptr_t left_data_start;
	uintptr_t left_data_end;
	uintptr_t right_data_start;
	uintptr_t right_data_end;
	uintptr_t region_end;
	struct vm_page *page;

	/* Rejects a split outside the region or of a busy or immutable one. */
	if (region == NULL ||
	    right == NULL ||
	    address <= region->start ||
	    address >= region->start + region->size ||
	    (address & (PAGE_SIZE - 1U)) != 0)
		return EINVAL;
	if (region->hold_count != 0)
		return EBUSY;
	if (region->flags & VM_REGION_IMMUTABLE)
		return EACCES;

	/* The right half inherits the region and clips the data window. */
	left_size = address - region->start;
	right_size = region->size - left_size;
	original_data_start = region->data_start;
	original_data_end = region->data_start + region->data_size;
	region_end = region->start + region->size;
	*right = *region;
	right->start = address;
	right->size = right_size;

	if (original_data_start > address)
		right_data_start = original_data_start;
	else
		right_data_start = address;

	if (original_data_end < region_end)
		right_data_end = original_data_end;
	else
		right_data_end = region_end;

	if (right_data_end < right_data_start)
		right_data_end = right_data_start;

	right->data_start = right_data_start;
	right->data_size = right_data_end - right_data_start;

	if (region->backing == VM_BACKING_FILE &&
	    right_data_start > original_data_start)
		right->file_offset += (off_t)(right_data_start - original_data_start);

	if (region->commit_size != 0)
		right->commit_size = right_size;
	else
		right->commit_size = 0;

	right->pages = NULL;

	if (right->file != NULL)
		file_ref(right->file);

	if (right->object != NULL)
		vm_object_ref(right->object);

	if (right->snapshot != NULL)
		file_exec_snapshot_ref(right->snapshot);

	/* The left half keeps whatever data lies before the split. */
	region->size = left_size;
	if (original_data_start < address &&
	    original_data_end > region->start) {
		left_data_start = original_data_start;
		if (original_data_end < address)
			left_data_end = original_data_end;
		else
			left_data_end = address;
	} else {
		left_data_start = region->start;
		left_data_end = left_data_start;
	}

	/* Gives the left half its share of the data range and the commitment. */
	region->data_start = left_data_start;
	region->data_size = left_data_end - left_data_start;

	if (region->commit_size != 0)
		region->commit_size = left_size;
	else
		region->commit_size = 0;

	/* Moves the pages at or above the split to the right half. */
	link = &region->pages;
	while (*link != NULL) {
		page = *link;
		if (page->address < address) {
			link = &page->next;
			continue;
		}

		*link = page->next;
		page->region = right;
		page->next = right->pages;
		right->pages = page;
	}

	right->next = region->next;
	region->next = right;

	return 0;
}

/* Splits a region at an address, allocating the right half. */
static int
split_region(
	struct vm_region *region,
	uintptr_t address)
{
	struct vm_region *right;
	int error;

	/* Allocates the record the right half of the split will use. */
	right = kern_calloc(1, sizeof(*right));
	if (right == NULL)
		return ENOMEM;

	/* Splits the region, returning the record a failure did not take. */
	error = split_region_prepared(region, address, right);
	if (error != 0)
		kern_free(right);

	/* Reports why the split failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Frees a list of retired regions and their resources outside the locks. */
static void
release_retired_regions(
	struct vm_region *retired)
{
	struct vm_region *region;

	/* Frees each detached region with everything it owned. */
	region = retired;
	while (region != NULL) {
		retired = region->next;
		release_detached_region_pages(region);
		if (region->file != NULL)
			(void)file_close(region->file);
		if (region->object != NULL)
			vm_object_put(region->object);
		if (region->snapshot != NULL)
			file_exec_snapshot_put(region->snapshot);
		if (region->commit_size != 0)
			vm_commit_release(region->commit_size);
		kern_free(region);
		region = retired;
	}
}

/* Publishes a prepared region at an exact address, retiring what it overlaps. */
static int
vmspace_replace_prepared(
	struct vmspace *vm,
	struct vm_region *prepared,
	struct vm_region **result)
{
	struct vm_region *split_first;
	struct vm_region *split_second;
	struct vm_region *retired;
	struct vm_region *region;
	struct vm_region **link;
	uintptr_t start;
	uintptr_t end;
	size_t removed;
	int error;
	struct vm_page *page;
	uintptr_t overlap_start;
	uintptr_t overlap_end;
	struct vm_region *split;
	uint64_t mapped_bytes;

	retired = NULL;
	removed = 0;
	error = 0;

	/*
	 * Two preallocated split records make the metadata commit
	 * failure-free after the first old region is changed.
	 */
	if (vm == NULL ||
	    vm == &kernel_vmspace ||
	    prepared == NULL ||
	    !range_valid(prepared->start, prepared->size))
		return EINVAL;

	/* Preallocates both possible splits, because the commit may not fail. */
	split_first = kern_calloc(1, sizeof(*split_first));
	split_second = kern_calloc(1, sizeof(*split_second));
	if (split_first == NULL || split_second == NULL) {
		kern_free(split_first);
		kern_free(split_second);
		return ENOMEM;
	}

	start = prepared->start;
	end = start + prepared->size;
	vm_metadata_enter();
	mutex_lock(&vm->lock);

	vmspace_wait_faults_locked(vm);

	/* Validates every overlapped region and sums the bytes removed. */
	for (region = vm->regions; region != NULL; region = region->next) {
		if (region->start >= end || region->start + region->size <= start)
			continue;
		if ((region->flags & VM_REGION_IMMUTABLE) != 0) {
			error = EINVAL;
			goto out_locked;
		}

		if (region->hold_count != 0) {
			error = EAGAIN;
			goto out_locked;
		}

		/* Refuses the replacement while any page of the range is wired. */
		for (page = region->pages; page != NULL; page = page->next) {
			if (page->address < end &&
			    page->address + PAGE_SIZE > start &&
			    page->wire_count != 0) {
				error = EAGAIN;
				goto out_locked;
			}
		}

		/* Counts how much mapped address space the replacement frees. */
		if (region->start > start)
			overlap_start = region->start;
		else
			overlap_start = start;
		if (region->start + region->size < end)
			overlap_end = region->start + region->size;
		else
			overlap_end = end;
		removed += overlap_end - overlap_start;
	}

	if (removed > vm->mapped_virtual_bytes) {
		error = ENOMEM;
		goto out_locked;
	}

	/* Refuses a replacement that would exceed the address-space limit. */
	mapped_bytes = vm->mapped_virtual_bytes - removed;
	if (mapped_bytes > vm->address_limit)
		mapped_bytes = vm->address_limit;
	if ((uint64_t)prepared->size > vm->address_limit - mapped_bytes) {
		error = ENOMEM;
		goto out_locked;
	}

	/* Splits the regions that straddle either end of the range. */
	region = find_region_locked(vm, end - 1U, 1);
	if (region != NULL && end < region->start + region->size) {
		error = split_region_prepared(region, end, split_first);
		if (error != 0)
			goto out_locked;
		split_first = NULL;
	}

	/* Splits the region that straddles the start of the range. */
	region = find_region_locked(vm, start, 1);
	if (region != NULL && start > region->start) {
		if (split_first != NULL)
			split = split_first;
		else
			split = split_second;
		error = split_region_prepared(region, start, split);
		if (error != 0)
			HAL_FATAL("prepared VM split commit failed");
		if (split == split_first)
			split_first = NULL;
		else
			split_second = NULL;
	}

	/* Detaches the old mappings, unlinks the old regions, and links the new one. */
	for (region = vm->regions; region != NULL && region->start < end;
	     region = region->next) {
		if (region->start >= start)
			detach_region_pages_for_unmap(vm, region);
	}

	for (link = &vm->regions; *link != NULL; link = &(*link)->next) {
		if ((*link)->start >= start)
			break;
	}

	region = *link;
	while (region != NULL && region->start < end) {
		*link = region->next;
		vm->mapped_virtual_bytes -= region->size;
		region->next = retired;
		retired = region;
		region = *link;
	}

	insert_region(vm, prepared);
	vm->mapped_virtual_bytes += prepared->size;
	vmspace_generation_advance_locked(vm);
	if (result != NULL)
		*result = prepared;

out_locked:
	/* Leaves the locks and frees the split records the pass reserved. */

	mutex_unlock(&vm->lock);
	vm_metadata_leave();

	kern_free(split_first);
	kern_free(split_second);

	/* Retires the replaced regions outside every lock. */
	if (error == 0 && retired != NULL &&
	    vmspace_unmap_retire_checkpoint != NULL)
		vmspace_unmap_retire_checkpoint(vm, start, prepared->size);
	release_retired_regions(retired);

	/* Reports why the replacement failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Records the program break start; the caller holds the VM locks. */
static int
vmspace_set_brk_start_locked(
	struct vmspace *vm,
	uintptr_t start,
	uint64_t static_data_bytes)
{
	if (vm == NULL ||
	    vm == &kernel_vmspace ||
	    vm->brk_start != 0 ||
	    (start & (PAGE_SIZE - 1U)) != 0 ||
	    start < vm_layout.user_minimum ||
	    start >= vm_layout.brk_limit ||
	    overlaps(vm, start, PAGE_SIZE))
		return EINVAL;

	if (static_data_bytes > vm->data_limit)
		return ENOMEM;

	vm->brk_start = start;
	vm->brk_current = start;
	vm->static_data_bytes = static_data_bytes;

	return 0;
}

/* Grows or shrinks the program break region; the caller holds the VM locks. */
static int
vmspace_brk_locked(
	struct vmspace *vm,
	uintptr_t requested,
	uintptr_t *result)
{
	struct vm_region **link;
	struct vm_region *region;
	uintptr_t old_end;
	uintptr_t new_end;
	uint64_t mapped_bytes;
	size_t difference;
	int error;
	struct vm_page **page_link;
	struct vm_page *page;

	region = NULL;

	/* Requires a user space that has a program break. */
	if (vm == NULL ||
	    vm == &kernel_vmspace ||
	    result == NULL ||
	    vm->brk_start == 0)
		return EINVAL;

	/* Waits for the faults in flight before changing the layout. */
	vmspace_wait_faults_locked(vm);
	if (requested == 0) {
		*result = vm->brk_current;
		return 0;
	}

	/* The request must stay within the break area and the data limit. */
	if (requested < vm->brk_start || requested >= vm_layout.brk_limit)
		return ENOMEM;

	if (vm->static_data_bytes > vm->data_limit ||
	    (uint64_t)(requested - vm->brk_start) >
	    vm->data_limit - vm->static_data_bytes)
		return ENOMEM;

	old_end = (vm->brk_current + PAGE_SIZE - 1U) & ~(uintptr_t)(PAGE_SIZE - 1U);
	new_end = (requested + PAGE_SIZE - 1U) & ~(uintptr_t)(PAGE_SIZE - 1U);

	/* Finds the break region, if one has been created. */
	for (link = &vm->regions; *link != NULL; link = &(*link)->next) {
		if (((*link)->flags & VM_REGION_BRK) != 0) {
			region = *link;
			break;
		}
	}

	/* Growth extends or creates the break region; shrinking frees pages. */
	if (new_end > old_end) {
		difference = new_end - old_end;
		if (region == NULL) {
			error = map_region(vm, vm->brk_start,
				new_end - vm->brk_start,
				HAL_SPACE_READ | HAL_SPACE_WRITE, VM_BACKING_ANON,
				NULL, 0, vm->brk_start, 0,
				VM_REGION_BRK | VM_REGION_IMMUTABLE,
				new_end - vm->brk_start, &region);
			if (error != 0)
				return error;
		} else {
			if (region->start + region->size != old_end ||
			    overlaps(vm, old_end, difference))
				return ENOMEM;
			mapped_bytes = vm->mapped_virtual_bytes;
			if (mapped_bytes > vm->address_limit)
				mapped_bytes = vm->address_limit;
			if ((uint64_t)difference > vm->address_limit - mapped_bytes)
				return ENOMEM;
			error = vm_commit_reserve(difference);
			if (error != 0)
				return error;
			region->size += difference;
			region->commit_size += difference;
			vm->mapped_virtual_bytes += difference;
		}
	} else if (new_end < old_end) {
		if (region == NULL || region->start + region->size != old_end)
			return EINVAL;
		page_link = &region->pages;
		while (*page_link != NULL) {
			page = *page_link;
			if (page->address < new_end) {
				page_link = &page->next;
				continue;
			}

			*page_link = page->next;
			free_vm_page(vm, page);
		}

		/* Gives back the commitment and the address space the break released. */
		difference = old_end - new_end;
		vm_commit_release(difference);
		region->size -= difference;
		region->commit_size -= difference;
		vm->mapped_virtual_bytes -= difference;
		if (region->size == 0) {
			*link = region->next;
			kern_free(region);
		}
	}

	vm->brk_current = requested;
	*result = requested;

	return 0;
}

/* Detaches and unlinks the regions of a range; the caller holds the VM locks. */
static int
vmspace_unmap_locked(
	struct vmspace *vm,
	uintptr_t start,
	size_t size,
	struct vm_region **retired)
{
	struct vm_region **link;
	struct vm_region *region;
	uintptr_t end;
	int error;
	struct vm_page *page;

	/* Rejects a kernel space, a missing output, or a malformed range. */
	if (vm == NULL ||
	    vm == &kernel_vmspace ||
	    retired == NULL ||
	    !range_valid(start, size))
		return EINVAL;

	/* Waits for the faults in flight before changing the layout. */
	vmspace_wait_faults_locked(vm);
	end = start + size;

	/* Validate the complete transaction before split_region mutates metadata. */
	for (region = vm->regions; region != NULL; region = region->next) {
		if (region->start >= end || region->start + region->size <= start)
			continue;
		if ((region->flags & VM_REGION_IMMUTABLE) != 0)
			return EACCES;
		for (page = region->pages; page != NULL; page = page->next) {
			if (page->address < end &&
			    page->address + PAGE_SIZE > start &&
			    page->wire_count != 0)
				return EBUSY;
		}
	}

	/* Splits the region that straddles the end of the range. */
	region = find_region_locked(vm, end - 1U, 1);
	if (region != NULL && end < region->start + region->size) {
		error = split_region(region, end);
		if (error != 0)
			return error;
	}

	/* Splits the region that straddles the start of the range. */
	region = find_region_locked(vm, start, 1);
	if (region != NULL && start > region->start) {
		error = split_region(region, start);
		if (error != 0)
			return error;
	}

	/*
	 * All validation, allocation, and splitting is complete.  From this
	 * point the commit cannot report failure: a HAL failure would leave
	 * only part of the old range detached, so it is a kernel invariant
	 * violation.  Detach every old PTE/reverse map before unlinking any
	 * region.
	 */
	for (region = vm->regions; region != NULL && region->start < end;
	     region = region->next) {
		if (region->start >= start)
			detach_region_pages_for_unmap(vm, region);
	}

	for (link = &vm->regions; *link != NULL; link = &(*link)->next) {
		if ((*link)->start >= start)
			break;
	}

	region = *link;
	while (region != NULL && region->start < end) {
		*link = region->next;
		vm->mapped_virtual_bytes -= region->size;
		region->next = *retired;
		*retired = region;
		region = *link;
	}

	return 0;
}

/* Changes the protection of a range, rolling the HAL back on failure; the caller holds the VM locks. */
static int
vmspace_protect_locked(
	struct vmspace *vm,
	uintptr_t start,
	size_t size,
	uint32_t prot)
{
	struct vm_region *region;
	struct vm_region *first;
	struct vm_region *end_region;
	struct vm_page *page;
	struct vm_page *failed_page;
	struct vm_region *failed_region;
	uintptr_t end;
	uintptr_t covered;
	size_t new_commit;
	int error;
	uint32_t page_prot;
	int hal_error;
	struct vm_page *rollback;
	hal_physaddr_t physical;

	failed_page = NULL;
	failed_region = NULL;
	new_commit = 0;

	/* Rejects a malformed range or a protection the hardware has no bit for. */
	if (vm == NULL ||
	    vm == &kernel_vmspace ||
	    !range_valid(start, size) ||
	    (prot & ~(HAL_SPACE_READ | HAL_SPACE_WRITE | HAL_SPACE_EXEC)) != 0)
		return EINVAL;

	/* Waits for the faults in flight before changing the protections. */
	vmspace_wait_faults_locked(vm);

	/* The range must be fully mapped by mutable regions that allow prot. */
	end = start + size;
	covered = start;
	for (region = vm->regions; region != NULL && covered < end;
	     region = region->next) {
		if (covered < region->start)
			return EINVAL;
		if (covered >= region->start + region->size)
			continue;
		if (region->flags & VM_REGION_IMMUTABLE)
			return EACCES;
		if ((prot & ~region->max_prot) != 0)
			return EACCES;
		covered = region->start + region->size;
		if (covered > end)
			covered = end;
	}

	if (covered != end)
		return EINVAL;

	/* Splits the regions that straddle either end of the range. */
	end_region = find_region_locked(vm, end - 1U, 1);
	if (end_region != NULL && end < end_region->start + end_region->size) {
		error = split_region(end_region, end);
		if (error != 0)
			return error;
	}

	/* Splits the region that straddles the start of the range. */
	region = find_region_locked(vm, start, 1);
	if (start > region->start) {
		error = split_region(region, start);
		if (error != 0)
			return error;
		region = region->next;
	}

	first = region;

	/* A region becoming accessible or writable needs commit first. */
	for (; region != NULL && region->start < end; region = region->next) {
		if (region->commit_size == 0 && region->object == NULL &&
		    ((region->backing == VM_BACKING_ANON && prot != 0) ||
		     (region->backing == VM_BACKING_FILE &&
		      (prot & HAL_SPACE_WRITE) != 0))) {
			if (new_commit > SIZE_MAX - region->size)
				return EOVERFLOW;
			new_commit += region->size;
		}
	}

	if (new_commit != 0) {
		error = vm_commit_reserve(new_commit);
		if (error != 0)
			return error;
	}

	/* Applies the new protection to every resident page. */
	for (region = first; region != NULL && region->start < end;
	     region = region->next) {
		for (page = region->pages; page != NULL; page = page->next) {
			if (page->flags & VM_MAPPING_COW)
				page_prot = prot & ~HAL_SPACE_WRITE;
			else
				page_prot = prot;
			hal_error = HAL_OK;
			if (prot == 0 && (page->flags & VM_MAPPING_MAPPED)) {
				hal_error = hal_page_unmap(vm->space,
				    (void *)page->address, PAGE_SIZE);
				if (hal_error == HAL_OK) {
					page->flags &= ~VM_MAPPING_MAPPED;
					page->flags |= VM_MAPPING_PROTECT_REMOVED;
				}
			} else if (prot != 0 &&
			    (page->flags & VM_MAPPING_MAPPED)) {
				hal_error = hal_page_prot(vm->space,
				    (void *)page->address, PAGE_SIZE, page_prot);
			} else if (prot != 0 && page->object_page != NULL) {
				hal_error = hal_page_map(vm->space,
				    (void *)page->address,
				    page->object_page->pmem.paddr, PAGE_SIZE, page_prot);
				if (hal_error == HAL_OK)
					page->flags |= VM_MAPPING_MAPPED |
					    VM_MAPPING_PROTECT_ADDED;
			} else if (prot != 0 &&
			    vm_private_page_is_resident(page)) {
				hal_error = hal_page_map(vm->space,
				    (void *)page->address,
				    page->private_page->pmem.paddr, PAGE_SIZE, page_prot);
				if (hal_error == HAL_OK)
					page->flags |= VM_MAPPING_MAPPED |
					    VM_MAPPING_PROTECT_ADDED;
			}

			/* Remembers where the failure happened so it can be rolled back. */
			if (hal_error != HAL_OK) {
				failed_region = region;
				failed_page = page;
				goto rollback;
			}
		}
	}

	/* Commits the new protection and the commit charge. */
	for (region = first; region != NULL && region->start < end;
	     region = region->next) {
		region->prot = prot;
		for (page = region->pages; page != NULL; page = page->next)
			page->flags &= ~(VM_MAPPING_PROTECT_REMOVED |
			    VM_MAPPING_PROTECT_ADDED);
		if (region->commit_size == 0 && region->object == NULL &&
		    ((region->backing == VM_BACKING_ANON && prot != 0) ||
		     (region->backing == VM_BACKING_FILE &&
		      (prot & HAL_SPACE_WRITE) != 0)))
			region->commit_size = region->size;
	}

	return 0;

rollback:
	/* Restores every page changed before the failure. */
	for (region = first; region != NULL && region->start < end;
	     region = region->next) {
		for (rollback = region->pages; rollback != NULL;
		     rollback = rollback->next) {
			if (region == failed_region && rollback == failed_page)
				break;
			if (rollback->flags & VM_MAPPING_PROTECT_ADDED) {
				if (hal_page_unmap(vm->space,
				    (void *)rollback->address, PAGE_SIZE) != HAL_OK)
					HAL_FATAL("VM protection rollback unmap failed");
				rollback->flags &= ~(VM_MAPPING_MAPPED |
				    VM_MAPPING_PROTECT_ADDED);
			} else if (rollback->flags & VM_MAPPING_PROTECT_REMOVED) {
				if (rollback->object_page != NULL)
					physical = rollback->object_page->pmem.paddr;
				else
					physical = rollback->private_page->pmem.paddr;
				if (hal_page_map(vm->space,
				    (void *)rollback->address, physical, PAGE_SIZE,
				    vm_page_effective_prot(rollback)) != HAL_OK)
					HAL_FATAL("VM protection rollback map failed");
				rollback->flags &= ~VM_MAPPING_PROTECT_REMOVED;
				rollback->flags |= VM_MAPPING_MAPPED;
			} else if ((rollback->flags & VM_MAPPING_MAPPED) &&
			    hal_page_prot(vm->space, (void *)rollback->address,
				PAGE_SIZE, vm_page_effective_prot(rollback)) != HAL_OK) {
				HAL_FATAL("VM protection rollback failed");
			}
		}

		if (region == failed_region)
			break;
	}

	if (new_commit != 0)
		vm_commit_release(new_commit);
	return EINVAL;
}

/* Tears down a vmspace whose last reference is gone. */
static void
vmspace_destroy(
	struct vmspace *vm)
{
	struct vm_region *region;

	/* Frees each region's pages under the lock and its resources outside it. */
	vm_metadata_enter();

	region = vm->regions;
	while (region != NULL) {
		vm->regions = region->next;
		free_region_pages(vm, region);
		vm_metadata_leave();
		if (region->file != NULL)
			(void)file_close(region->file);
		if (region->object != NULL)
			vm_object_put(region->object);
		if (region->snapshot != NULL)
			file_exec_snapshot_put(region->snapshot);
		if (region->commit_size != 0)
			vm_commit_release(region->commit_size);
		kern_free(region);
		vm_metadata_enter();
		region = vm->regions;
	}

	hal_page_destroy_space(vm->space);
	(void)atomic_raw_fetch_add_relaxed(&vmspace_live.value, (unsigned)-1);

	vm_metadata_leave();

	kern_free(vm);
}

/* Sets the limit on mapped virtual bytes; the caller holds the VM locks. */
static int
vmspace_set_address_limit_locked(
	struct vmspace *vm,
	uint64_t limit)
{
	if (vm == NULL || vm == &kernel_vmspace || limit > vmspace_address_cap())
		return EINVAL;
	vm->address_limit = limit;
	return 0;
}

/* Sets the stack size limit; the caller holds the VM locks. */
static void
vmspace_set_stack_limit_locked(
	struct vmspace *vm,
	uint64_t limit)
{
	if (vm != NULL && vm != &kernel_vmspace)
		vm->stack_limit = limit;
}

/* Sets the data size limit; the caller holds the VM locks. */
static int
vmspace_set_data_limit_locked(
	struct vmspace *vm,
	uint64_t limit)
{
	uint64_t cap;

	cap = vmspace_address_cap();
	if (vm == NULL || vm == &kernel_vmspace)
		return EINVAL;

	/*
	 * RLIM_INFINITY is the public resource-limit representation.  The
	 * VM still needs a concrete bound for overflow-safe brk arithmetic,
	 * so map it to the largest addressable user range at this boundary.
	 */
	if (limit == UINT64_MAX)
		limit = cap;
	if (limit > cap)
		return EINVAL;
	vm->data_limit = limit;
	return 0;
}

/* Advances the generation, skipping zero; the caller holds the VM lock. */
static void
vmspace_generation_advance_locked(
	struct vmspace *vm)
{
	vm->generation++;
	if (vm->generation == 0)
		vm->generation++;
}

/* Caller reserved BUSY page/region ownership and released both VM locks. */
static int
vmspace_exec_cache_fault(
	struct vmspace *vm,
	struct vm_region *region,
	struct vm_page *page,
	uint32_t required)
{
	struct vm_object_page *source;
	struct vm_private_page *fresh;
	uintptr_t address;
	uint32_t prot;
	unsigned long irq;
	int error, writing, unmapped;

	/* The region's snapshot pins the immutable source during allocation and copy. */
	source = page->object_page;
	address = page->address;
	prot = region->prot;
	writing = required == HAL_SPACE_WRITE;
	fresh = NULL;
	error = 0;
	unmapped = 0;
	if (writing) {
		fresh = private_page_alloc();
		if (fresh == NULL)
			error = ENOMEM;
		if (error == 0)
			error = allocate_page_frame(fresh, page);
		if (error == 0) {
			irq = spin_lock_irqsave(&fresh->state_lock);
			fresh->flags = VM_PAGE_RESIDENT;
			fresh->generation++;
			if (fresh->generation == 0)
				fresh->generation++;
			spin_unlock_irqrestore(&fresh->state_lock, irq);
			error = vm_object_page_pin_read(source, 0, fresh->pmem.vaddr, PAGE_SIZE);
			if (error == 0)
				vm_private_page_mark_dirty(fresh);
		}
	}

	/* Replaces a PTE only after the complete private copy is ready. */
	if (error == 0 && writing && (page->flags & VM_MAPPING_MAPPED) != 0) {
		if (hal_page_unmap(vm->space, (void *)address, PAGE_SIZE) != HAL_OK)
			error = EIO;
		else
			unmapped = 1;
	}

	/* Revalidates everything and installs the mapping. */
	vm_metadata_enter();
	mutex_lock(&vm->lock);

	if ((page->flags & VM_MAPPING_BUSY) == 0 || region->hold_count == 0)
		HAL_FATAL("exec cache fault lost reservation");

	if (unmapped)
		page->flags &= ~VM_MAPPING_MAPPED;

	if (error == 0 && (find_region_locked(vm, address, 1) != region ||
	    find_page(region, address) != page || page->object_page != source ||
	    region->snapshot == NULL || region->prot != prot || (prot & required) == 0))
		error = EAGAIN;

	if (error == 0 && hal_page_map(vm->space, (void *)address,
	    writing ? fresh->pmem.paddr : source->pmem.paddr, PAGE_SIZE,
	    writing ? prot : prot & ~HAL_SPACE_WRITE) != HAL_OK)
		error = ENOMEM;

	/* Publishes the mapping, taking a private copy when this was a write. */
	if (error == 0) {
		if (writing) {
			vm_object_mapping_remove(source, page);
			page->object_page = NULL;
			page->object_next = NULL;
			private_page_attach_new(page, fresh);
			fresh = NULL;
			vm_page_track(page);
			page->flags &= ~VM_MAPPING_COW;
		}

		page->flags |= VM_MAPPING_MAPPED;
		vmspace_generation_advance_locked(vm);
	}

	/* Releases the reservation and wakes the faults that waited. */
	page->flags &= ~VM_MAPPING_BUSY;
	region->hold_count--;
	vmspace_fault_wake_locked(vm);

	mutex_unlock(&vm->lock);
	vm_metadata_leave();

	if (fresh != NULL)
		vm_private_page_put(fresh);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}
