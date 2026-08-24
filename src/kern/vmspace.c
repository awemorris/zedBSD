/*
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 *
 * Process virtual-memory ownership and demand paging.
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

#define PAGE_SIZE ZEDBSD_PAGE_SIZE
#define VM_PAGE_SLAB_SLOTS 32U

extern void vmspace_unmap_retire_checkpoint(struct vmspace *, uintptr_t,
	size_t) __attribute__((weak));
extern void vmspace_pin_page_checkpoint(struct vmspace *, size_t, size_t)
	__attribute__((weak));
extern void vmspace_object_revoke_checkpoint(struct vmspace *, uintptr_t)
	__attribute__((weak));

struct vm_page_slab {
	struct hal_pmem memory;
	struct vm_page_slab *next;
	uint32_t free_mask;
	unsigned used;
	struct vm_page slots[VM_PAGE_SLAB_SLOTS];
};

_Static_assert(sizeof(struct vm_page_slab) <= PAGE_SIZE,
	"VM page metadata slab exceeds one physical page");

static struct spinlock vm_page_slab_lock;
static struct vm_page_slab *vm_page_slabs;
static struct spinlock vmspace_reap_lock;
static struct vmspace *vmspace_reap_head;
static struct vmspace *vmspace_reap_tail;
static void (*vmspace_reap_notify)(void *);
static void *vmspace_reap_notify_argument;
struct vmspace kernel_vmspace = {
	.space = HAL_SPACE_SYS,
	.refs = { 1 },
};

struct vm_layout vm_layout;
static int vm_layout_initialized;
static atomic_uint_t vmspace_live;

static int
alloc_vm_page(struct hal_pmem *memory)
{
	const struct hal_pmem_request request = {
		HAL_PMEM_PADDR_ANY, PAGE_SIZE, PAGE_SIZE,
		HAL_PMEM_TYPE_RAM, 0
	};
	return hal_pmem_alloc(&request, memory);
}

void
vmspace_layout_init(void)
{
	uintptr_t minimum, limit;
	if (vm_layout_initialized)
		return;
	spin_init(&vm_page_slab_lock, LOCK_RANK_VM_OBJECT,
	    "VM page metadata");
	spin_init(&vmspace_reap_lock, LOCK_RANK_VMSPACE,
	    "VM space reaper");
	vm_page_slabs = NULL;
	vmspace_reap_head = vmspace_reap_tail = NULL;
	vmspace_reap_notify = NULL;
	vmspace_reap_notify_argument = NULL;
	vm_metadata_init();
	hal_page_get_user_range(&minimum, &limit);
	if (minimum < PAGE_SIZE || (minimum & (PAGE_SIZE - 1U)) != 0 ||
	    limit <= minimum || (limit & (PAGE_SIZE - 1U)) != 0)
		HAL_FATAL("invalid HAL user address range");
	vm_layout.user_minimum = minimum;
	vm_layout.user_limit = limit;
#ifdef ZEDBSD_USER_ABI_LP64
	vm_layout.brk_limit = 0x0000000100000000ULL;
	vm_layout.mmap_base = 0x0000000100000000ULL;
#else
	vm_layout.brk_limit = 0x10000000U;
	vm_layout.mmap_base = 0x10000000U;
#endif
	if (vm_layout.brk_limit >= limit || vm_layout.mmap_base >= limit ||
	    limit - minimum <= PAGE_SIZE)
		HAL_FATAL("user address range too small");
	vm_layout.stack_top = limit - PAGE_SIZE;
	vm_layout_initialized = 1;
}

static struct vm_page *
vm_page_slab_take_locked(void)
{
	struct vm_page_slab *slab;
	unsigned slot;

	for (slab = vm_page_slabs; slab != NULL; slab = slab->next) {
		if (slab->free_mask == 0)
			continue;
		for (slot = 0; slot < VM_PAGE_SLAB_SLOTS; slot++)
			if ((slab->free_mask & (1U << slot)) != 0)
				break;
		if (slot == VM_PAGE_SLAB_SLOTS)
			HAL_FATAL("invalid VM page slab bitmap");
		slab->free_mask &= ~(1U << slot);
		slab->used++;
		memset(&slab->slots[slot], 0, sizeof(slab->slots[slot]));
		return &slab->slots[slot];
	}
	return NULL;
}

struct vm_page *
vm_page_alloc_metadata(void)
{
	const struct hal_pmem_request request = {
		HAL_PMEM_PADDR_ANY, PAGE_SIZE, PAGE_SIZE,
		HAL_PMEM_TYPE_RAM, 0
	};
	struct hal_pmem memory;
	struct vm_page_slab *fresh;
	struct vm_page *page;
	unsigned long irq;

	vmspace_layout_init();
	irq = spin_lock_irqsave(&vm_page_slab_lock);
	page = vm_page_slab_take_locked();
	spin_unlock_irqrestore(&vm_page_slab_lock, irq);
	if (page != NULL)
		return page;
	if (hal_pmem_alloc(&request, &memory) != HAL_OK)
		return NULL;
	fresh = memory.vaddr;
	memset(fresh, 0, PAGE_SIZE);
	fresh->memory = memory;
	fresh->free_mask = UINT32_MAX;
	irq = spin_lock_irqsave(&vm_page_slab_lock);
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

void
vm_page_free_metadata(struct vm_page *page)
{
	struct vm_page_slab *slab;
	uintptr_t address;
	unsigned long irq;

	if (page == NULL)
		return;
	address = (uintptr_t)page;
	irq = spin_lock_irqsave(&vm_page_slab_lock);
	for (slab = vm_page_slabs; slab != NULL; slab = slab->next) {
		uintptr_t first = (uintptr_t)&slab->slots[0];
		uintptr_t end = (uintptr_t)&slab->slots[VM_PAGE_SLAB_SLOTS];
		unsigned slot;
		if (address < first || address >= end ||
		    (address - first) % sizeof(struct vm_page) != 0)
			continue;
		slot = (unsigned)((address - first) / sizeof(struct vm_page));
		if ((slab->free_mask & (1U << slot)) != 0 || slab->used == 0)
			HAL_FATAL("invalid VM page metadata free");
		memset(page, 0, sizeof(*page));
		slab->free_mask |= 1U << slot;
		slab->used--;
		spin_unlock_irqrestore(&vm_page_slab_lock, irq);
		return;
	}
	spin_unlock_irqrestore(&vm_page_slab_lock, irq);
	HAL_FATAL("foreign VM page metadata free");
}

static int
range_valid(uintptr_t start, size_t size)
{
	vmspace_layout_init();
	return size != 0 && (start & (PAGE_SIZE - 1U)) == 0 &&
		(size & (PAGE_SIZE - 1U)) == 0 &&
		start >= vm_layout.user_minimum && start < vm_layout.user_limit &&
		size <= vm_layout.user_limit - start;
}

int vmspace_user_range_valid(uintptr_t start, size_t size)
{
	vmspace_layout_init();
	return size != 0 && start >= vm_layout.user_minimum &&
	    start < vm_layout.user_limit && size <= vm_layout.user_limit - start;
}

static int
overlaps(struct vmspace *vm, uintptr_t start, size_t size)
{
	struct vm_region *region;
	uintptr_t end = start + size;

	for (region = vm->regions; region != NULL; region = region->next)
		if (start < region->start + region->size && region->start < end)
			return 1;
	return 0;
}

static void
insert_region(struct vmspace *vm, struct vm_region *region)
{
	struct vm_region **link = &vm->regions;
	while (*link != NULL && (*link)->start < region->start)
		link = &(*link)->next;
	region->next = *link;
	*link = region;
}

static int map_region(struct vmspace *, uintptr_t, size_t, uint32_t,
		      enum vm_region_backing, struct file *, off_t, uintptr_t,
		      size_t, unsigned, size_t, struct vm_region **);
static int prepare_region(uintptr_t, size_t, uint32_t,
		      enum vm_region_backing, struct file *, off_t, uintptr_t,
		      size_t, unsigned, size_t, struct vm_region **);
static int allocate_page_frame(struct vm_private_page *, struct vm_page *);
static struct vm_page *find_page(struct vm_region *, uintptr_t);
static void vmspace_generation_advance_locked(struct vmspace *);
static void vmspace_wait_faults_locked(struct vmspace *);
static void vmspace_fault_wake_locked(struct vmspace *);

/*
 * A shared-file content writer first makes its object page BUSY.  Existing
 * fault holds drain before this routine is called, so no new reverse mapping
 * can appear until the writer publishes the new cache bytes.  Each mapping is
 * then pinned in metadata, revoked with no VM/object lock held, and finally
 * detached.  hal_page_prot_query() returns A/D state only after the remote TLB
 * acknowledgement, closing the late-store window between a dirty snapshot and
 * unmap.  A write-only mapping cannot be represented read-only by the current
 * HAL contract; conservatively treating it as dirty is safe.
 */
int
vmspace_object_page_revoke(struct vm_object_page *object_page,
	uint32_t *observed_flags)
{
	struct vm_object *object;
	uint32_t observed = 0;

	if (object_page == NULL || (object = object_page->owner) == NULL)
		return EINVAL;
	for (;;) {
		struct vm_page *mapping;
		struct vm_region *region;
		struct vmspace *vm;
		uint32_t readonly;
		unsigned long irq;
		int error, was_mapped;

		vm_metadata_enter();
		irq = spin_lock_irqsave(&object->lock);
		if ((object_page->flags & VM_OBJECT_PAGE_BUSY) == 0) {
			spin_unlock_irqrestore(&object->lock, irq);
			vm_metadata_leave();
			return EBUSY;
		}
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

		mutex_lock(&vm->lock);
		irq = spin_lock_irqsave(&object->lock);
		if (mapping->object_page != object_page || mapping->vm != vm ||
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

		if (was_mapped) {
			uint32_t flags = 0;

			if (readonly != 0) {
				error = hal_page_prot_query(vm->space,
				    (void *)mapping->address, PAGE_SIZE, readonly,
				    &flags);
				if (error != HAL_OK)
					HAL_FATAL("shared VM write revoke failed");
			} else {
				/* The synchronous unmap is the revoke operation. */
				flags |= HAL_PAGE_DIRTY;
			}
			if (hal_page_unmap(vm->space, (void *)mapping->address,
			    PAGE_SIZE) != HAL_OK)
				HAL_FATAL("shared VM content unmap failed");
			observed |= flags;
		}

		vm_metadata_enter();
		mutex_lock(&vm->lock);
		irq = spin_lock_irqsave(&object->lock);
		if (mapping->object_page != object_page ||
		    (mapping->flags & VM_MAPPING_BUSY) == 0)
			HAL_FATAL("shared VM revoke reservation lost");
		{
			struct vm_page **link;

			for (link = &region->pages; *link != NULL && *link != mapping;
			     link = &(*link)->next)
				;
			if (*link != mapping)
				HAL_FATAL("shared VM mapping left region");
			*link = mapping->next;
		}
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

static struct vm_private_page *
private_page_alloc(void)
{
	struct vm_private_page *backing = kern_calloc(1, sizeof(*backing));
	if (backing == NULL)
		return NULL;
	vm_private_page_init(backing);
	return backing;
}

static void
private_page_attach_new(struct vm_page *page,
	struct vm_private_page *backing)
{
	unsigned long irq = spin_lock_irqsave(&backing->state_lock);

	if (backing->mapping_count != 0 || backing->mappings != NULL)
		HAL_FATAL("attaching initialized VM private backing twice");
	backing->mapping_count = 1;
	page->private_page = backing;
	page->private_next = backing->mappings;
	backing->mappings = page;
	spin_unlock_irqrestore(&backing->state_lock, irq);
}

uint32_t
vm_page_effective_prot(const struct vm_page *page)
{
	uint32_t prot = page->region->prot;
	if ((page->flags & VM_MAPPING_COW) != 0)
		prot &= ~HAL_SPACE_WRITE;
	return prot;
}

int
vm_private_page_is_resident(const struct vm_page *page)
{
	struct vm_private_page *backing;
	unsigned long irq;
	int resident;

	if (page == NULL || (backing = page->private_page) == NULL)
		return 0;
	irq = spin_lock_irqsave(&backing->state_lock);
	resident = (backing->flags & VM_PAGE_RESIDENT) != 0;
	spin_unlock_irqrestore(&backing->state_lock, irq);
	return resident;
}

uintptr_t
vm_private_page_vaddr(const struct vm_page *page)
{
	struct vm_private_page *backing;
	unsigned long irq;
	uintptr_t address;

	if (page == NULL || (backing = page->private_page) == NULL)
		return 0;
	irq = spin_lock_irqsave(&backing->state_lock);
	address = (backing->flags & VM_PAGE_RESIDENT) != 0 ?
	    (uintptr_t)backing->pmem.vaddr : 0;
	spin_unlock_irqrestore(&backing->state_lock, irq);
	return address;
}

struct vmspace *
vmspace_create(void)
{
	struct vmspace *vm = kern_calloc(1, sizeof(*vm));
	vmspace_layout_init();

	if (vm == NULL)
		return NULL;
	vm->space = hal_mem_create_space();
	if (vm->space == NULL) {
		kern_free(vm);
		return NULL;
	}
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

int
vmspace_tryref(struct vmspace *vm)
{
	return vm != NULL && refcount_tryget(&vm->refs);
}

void
vmspace_ref(struct vmspace *vm)
{
	if (vm != NULL)
		refcount_get(&vm->refs);
}

static int
vmspace_fork_locked(struct vmspace *source, struct vmspace **result,
	struct vm_private_page **wait_backing, struct vmspace **failed_copy)
{
	struct vmspace *copy;
	struct vm_region *source_region;
	int error = 0;

	if (source == NULL || source == &kernel_vmspace || result == NULL ||
	    wait_backing == NULL || failed_copy == NULL)
		return EINVAL;
	*wait_backing = NULL;
	*failed_copy = NULL;
	vmspace_wait_faults_locked(source);
	copy = vmspace_create();
	if (copy == NULL)
		return ENOMEM;
	copy->address_limit = source->address_limit;
	copy->data_limit = source->data_limit;
	copy->stack_limit = source->stack_limit;
	for (source_region = source->regions; source_region != NULL;
	     source_region = source_region->next) {
		struct vm_region *copy_region;
		struct vm_page *source_page;

		error = map_region(copy, source_region->start, source_region->size,
		    source_region->prot, source_region->backing,
		    source_region->file, source_region->file_offset,
		    source_region->data_start, source_region->data_size,
		    source_region->flags, source_region->commit_size,
		    &copy_region);
		if (error != 0)
			goto fail;
		copy_region->max_prot = source_region->max_prot;
		if (source_region->object != NULL) {
			vm_object_ref(source_region->object);
			copy_region->object = source_region->object;
			/* Shared object pages are mapped lazily in the child. */
			continue;
		}
		for (source_page = source_region->pages; source_page != NULL;
		     source_page = source_page->next) {
			struct vm_private_page *backing;
			struct vm_page *copy_page;
			hal_physaddr_t physical = 0;
			uintptr_t page_address;
			uint64_t reservation_generation;
			uint32_t cow_prot;
			unsigned long state_irq;
			int resident;
			int child_mapped = 0;
			int source_mapped;

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
			 * Pin the source metadata before the backing share.  The backing
			 * operation excludes reclaim/pins, while the local BUSY marker
			 * makes unmap/protect/fault wait when the VM locks are dropped for
			 * the source PTE shootdown.
			 */
			if (source_region->hold_count == (unsigned)-1)
				HAL_FATAL("VM fork region hold overflow");
			source_region->hold_count++;
			source_page->flags |= VM_MAPPING_BUSY;
			backing = source_page->private_page;
			page_address = source_page->address;
			cow_prot = source_region->prot & ~HAL_SPACE_WRITE;
			source_mapped =
			    (source_page->flags & VM_MAPPING_MAPPED) != 0;
			reservation_generation = source->generation;
			/* Read-only private mappings also need COW for later mprotect. */
			error = vm_page_share_private(source_page, copy_page);
			if (error != 0) {
				if (error == EBUSY) {
					*wait_backing = backing;
					vm_private_page_ref(*wait_backing);
				}
				source_page->flags &= ~VM_MAPPING_BUSY;
				if (source_region->hold_count == 0)
					HAL_FATAL("VM fork region hold underflow");
				source_region->hold_count--;
				vmspace_fault_wake_locked(source);
				vm_page_free_metadata(copy_page);
				goto fail;
			}
			copy_page->next = copy_region->pages;
			copy_region->pages = copy_page;
			state_irq = spin_lock_irqsave(&backing->state_lock);
			resident = (backing->flags & VM_PAGE_RESIDENT) != 0;
			if (resident)
				physical = backing->pmem.paddr;
			spin_unlock_irqrestore(&backing->state_lock, state_irq);

			mutex_unlock(&source->lock);
			vm_metadata_leave();
			if (resident && source_mapped && hal_page_prot(source->space,
			    (void *)page_address, PAGE_SIZE, cow_prot) != HAL_OK)
				error = ENOMEM;
			if (error == 0 && resident && source_mapped &&
			    hal_page_map(copy->space, (void *)page_address, physical,
			    PAGE_SIZE, cow_prot) != HAL_OK)
				error = ENOMEM;
			else if (error == 0 && resident && source_mapped)
				child_mapped = 1;
			vm_metadata_enter();
			mutex_lock(&source->lock);

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
	return error;
}

static int
map_region(struct vmspace *vm, uintptr_t start, size_t size, uint32_t prot,
	   enum vm_region_backing backing, struct file *file,
	   off_t file_offset, uintptr_t data_start, size_t data_size,
	   unsigned flags, size_t commit_size, struct vm_region **result)
{
	struct vm_region *region;
	uint32_t max_prot = HAL_SPACE_READ | HAL_SPACE_WRITE | HAL_SPACE_EXEC;
	int error;

	if (vm == NULL || vm == &kernel_vmspace || !range_valid(start, size) ||
	    overlaps(vm, start, size) ||
	    (prot & ~(HAL_SPACE_READ | HAL_SPACE_WRITE | HAL_SPACE_EXEC)) != 0)
		return EINVAL;
	if ((uint64_t)size > vm->address_limit -
	    (vm->mapped_virtual_bytes > vm->address_limit ?
	    vm->address_limit : vm->mapped_virtual_bytes))
		return ENOMEM;
	if (backing == VM_BACKING_FILE &&
	    (file == NULL || file_offset < 0 || data_start < start ||
	     data_start >= start + size || data_size > start + size - data_start))
		return EINVAL;
	if ((flags & VM_REGION_SHARED) != 0 && file != NULL &&
	    ((file_status_flags_get(file) & O_ACCMODE) == O_RDONLY ||
	     file->f_ops == NULL || file->f_ops->pwrite == NULL))
		max_prot &= ~HAL_SPACE_WRITE;
	if ((prot & ~max_prot) != 0)
		return EACCES;
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

/*
 * Build a region which is not yet visible in a vmspace.  MAP_FIXED uses this
 * to reserve every fallible resource before it detaches an old mapping.
 */
static int
prepare_region(uintptr_t start, size_t size, uint32_t prot,
	enum vm_region_backing backing, struct file *file, off_t file_offset,
	uintptr_t data_start, size_t data_size, unsigned flags,
	size_t commit_size, struct vm_region **result)
{
	struct vm_region *region;
	uint32_t max_prot = HAL_SPACE_READ | HAL_SPACE_WRITE | HAL_SPACE_EXEC;
	int error;

	if (result == NULL || !range_valid(start, size) ||
	    (prot & ~(HAL_SPACE_READ | HAL_SPACE_WRITE | HAL_SPACE_EXEC)) != 0)
		return EINVAL;
	if (backing == VM_BACKING_FILE &&
	    (file == NULL || file_offset < 0 || data_start < start ||
	    data_start >= start + size || data_size > start + size - data_start))
		return EINVAL;
	if ((flags & VM_REGION_SHARED) != 0 && file != NULL &&
	    ((file_status_flags_get(file) & O_ACCMODE) == O_RDONLY ||
	    file->f_ops == NULL || file->f_ops->pwrite == NULL))
		max_prot &= ~HAL_SPACE_WRITE;
	if ((prot & ~max_prot) != 0)
		return EACCES;
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

static void
discard_prepared_region(struct vm_region *region)
{
	if (region == NULL)
		return;
	if (region->file != NULL)
		(void)file_close(region->file);
	if (region->object != NULL)
		vm_object_put(region->object);
	if (region->commit_size != 0)
		vm_commit_release(region->commit_size);
	kern_free(region);
}

static int
vmspace_map_anon_locked(struct vmspace *vm, uintptr_t start, size_t size,
		 uint32_t prot, struct vm_region **result)
{
	return map_region(vm, start, size, prot, VM_BACKING_ANON, NULL,
			  0, start, 0, 0, prot != 0 ? size : 0, result);
}

static int
vmspace_map_anon_fixed_noreplace_locked(struct vmspace *vm, uintptr_t start,
				 size_t size, uint32_t prot,
				 struct vm_region **result)
{
	if (vm == NULL || !range_valid(start, size))
		return EINVAL;
	if (overlaps(vm, start, size))
		return EEXIST;
	return vmspace_map_anon_locked(vm, start, size, prot, result);
}

static int
vmspace_map_file_locked(struct vmspace *vm, uintptr_t start, size_t size,
		 uint32_t prot, struct file *file, off_t file_offset,
		 uintptr_t data_start, size_t data_size,
		 struct vm_region **result)
{
	return map_region(vm, start, size, prot, VM_BACKING_FILE, file,
			  file_offset, data_start, data_size, 0,
			  (prot & HAL_SPACE_WRITE) != 0 ? size : 0, result);
}

static int
vmspace_map_file_shared_locked(struct vmspace *vm, uintptr_t start, size_t size,
			uint32_t prot, struct file *file, off_t file_offset,
			size_t data_size, struct vm_region **result)
{
	struct vm_object *object;
	struct vm_region *region;
	int error;

	if (file_offset < 0 || (file_offset & (PAGE_SIZE - 1U)) != 0)
		return EINVAL;
	error = vm_object_get_shared(file, &object);
	if (error != 0)
		return error;
	error = map_region(vm, start, size, prot, VM_BACKING_FILE, file,
	    file_offset, start, data_size, VM_REGION_SHARED, 0, &region);
	if (error != 0) {
		vm_object_put(object);
		return error;
	}
	region->object = object;
	if (result != NULL)
		*result = region;
	return 0;
}

static int
vmspace_map_stack_locked(struct vmspace *vm, uintptr_t top, size_t size,
		  size_t guard_size)
{
	struct vm_region *guard, *stack;
	uintptr_t bottom, guard_bottom;
	int error;

	if (vm == NULL || vm == &kernel_vmspace || size == 0 ||
	    guard_size == 0 || (top & (PAGE_SIZE - 1U)) != 0 ||
	    (size & (PAGE_SIZE - 1U)) != 0 ||
	    (guard_size & (PAGE_SIZE - 1U)) != 0 || top > vm_layout.user_limit ||
	    size > top || guard_size > top - size)
		return EINVAL;
	if ((uint64_t)size > vm->stack_limit ||
	    (uint64_t)size + guard_size > vm->address_limit -
	    (vm->mapped_virtual_bytes > vm->address_limit ?
	    vm->address_limit : vm->mapped_virtual_bytes))
		return ENOMEM;
	bottom = top - size;
	guard_bottom = bottom - guard_size;
	if (!range_valid(guard_bottom, size + guard_size) ||
	    overlaps(vm, guard_bottom, size + guard_size))
		return EINVAL;
	guard = kern_calloc(1, sizeof(*guard));
	if (guard == NULL)
		return ENOMEM;
	stack = kern_calloc(1, sizeof(*stack));
	if (stack == NULL) {
		kern_free(guard);
		return ENOMEM;
	}
	error = vm_commit_reserve(size);
	if (error != 0) {
		kern_free(stack);
		kern_free(guard);
		return error;
	}
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

static struct vm_region *
find_region_locked(struct vmspace *vm, uintptr_t address, size_t size)
{
	struct vm_region *region;

	if (vm == NULL || size == 0 || address > UINTPTR_MAX - size)
		return NULL;
	for (region = vm->regions; region != NULL; region = region->next)
		if (address >= region->start &&
		    address + size <= region->start + region->size)
			return region;
	return NULL;
}

static struct vm_page *
find_page(struct vm_region *region, uintptr_t address)
{
	struct vm_page *page;
	address &= ~(uintptr_t)(PAGE_SIZE - 1U);
	for (page = region->pages; page != NULL; page = page->next)
		if (page->address == address)
			return page;
	return NULL;
}

static void
vmspace_wait_fault_event(struct vmspace *vm, uint64_t sequence)
{
	unsigned long irq = spin_lock_irqsave(&vm->lock.guard);
	(void)waitq_sleep(&vm->fault_waitq, &vm->lock.guard, sequence, 0, 0);
	spin_unlock_irqrestore(&vm->lock.guard, irq);
}

static void
vmspace_wait_faults_locked(struct vmspace *vm)
{
	for (;;) {
		struct vm_region *region;
		int busy = 0;
		for (region = vm->regions; region != NULL; region = region->next) {
			struct vm_page *page;
			if (region->hold_count != 0) {
				busy = 1;
				break;
			}
			for (page = region->pages; page != NULL; page = page->next)
				if ((page->flags & VM_MAPPING_BUSY) != 0) {
					busy = 1;
					break;
				}
			if (busy)
				break;
		}
		if (!busy)
			return;
		{
			uint64_t sequence = waitq_sequence(&vm->fault_waitq);
			/* Fault completion needs the outer cross-vm metadata lock. */
			mutex_unlock(&vm->lock);
			vm_metadata_leave();
			vmspace_wait_fault_event(vm, sequence);
			vm_metadata_enter();
			mutex_lock(&vm->lock);
		}
	}
}

static int
allocate_page_frame(struct vm_private_page *backing, struct vm_page *avoid)
{
	if (alloc_vm_page(&backing->pmem) == HAL_OK)
		return 0;
	if (vm_reclaim_one(avoid) != 0 ||
	    alloc_vm_page(&backing->pmem) != HAL_OK)
		return ENOMEM;
	return 0;
}

/* Caller owns backing I/O and keeps accounting_page metadata pinned. */
static int
page_in_owned(struct vm_private_page *backing,
	struct vm_page *accounting_page)
{
	struct swap_backend *backend = swap_system_backend();
	uint32_t slot;
	unsigned long irq;
	int error;

	if (backing == NULL || backend == NULL)
		return EIO;
	irq = spin_lock_irqsave(&backing->state_lock);
	if ((backing->flags & (VM_PAGE_BUSY | VM_PAGE_SWAPPED)) !=
	    (VM_PAGE_BUSY | VM_PAGE_SWAPPED)) {
		spin_unlock_irqrestore(&backing->state_lock, irq);
		return EIO;
	}
	slot = backing->swap_slot;
	spin_unlock_irqrestore(&backing->state_lock, irq);
	error = allocate_page_frame(backing, accounting_page);
	if (error == 0)
		error = swap_read_page(backend, slot,
				       (void *)backing->pmem.vaddr);
	if (error != 0) {
		if (backing->pmem.size != 0)
			(void)hal_pmem_free(&backing->pmem);
		return error;
	}
	swap_free_slot(backend, slot);
	irq = spin_lock_irqsave(&backing->state_lock);
	if ((backing->flags & (VM_PAGE_BUSY | VM_PAGE_SWAPPED)) !=
	    (VM_PAGE_BUSY | VM_PAGE_SWAPPED) || backing->swap_slot != slot)
		HAL_FATAL("VM private page-in state changed under I/O owner");
	backing->swap_slot = SWAP_SLOT_NONE;
	backing->flags &= ~VM_PAGE_SWAPPED;
	/*
	 * The swap slot was the only backing store for this page.  Once it is
	 * released, a clean-looking resident page must not be discarded: CPU
	 * reads do not set the PTE dirty bit and a later reclaim would recreate
	 * anonymous memory as zero-filled data.  Treat the page as dirty until
	 * it has been written to swap again.
	 */
	backing->flags |= VM_PAGE_RESIDENT | VM_PAGE_DIRTY;
	if (++backing->generation == 0)
		backing->generation++;
	spin_unlock_irqrestore(&backing->state_lock, irq);
	vm_page_note_in(accounting_page);
	return 0;
}

static int
fill_file_page(struct vm_region *region, struct vm_page *page)
{
	uintptr_t page_end = page->address + PAGE_SIZE;
	uintptr_t data_end = region->data_start + region->data_size;
	uintptr_t read_start = page->address > region->data_start ?
		page->address : region->data_start;
	uintptr_t read_end = page_end < data_end ? page_end : data_end;
	size_t length;
	off_t offset;
	ssize_t count;

	memset((void *)page->private_page->pmem.vaddr, 0, PAGE_SIZE);
	if (read_start >= read_end)
		return (region->flags & VM_REGION_ELF_ZERO_TAIL) != 0 ? 0 : ENXIO;
	length = read_end - read_start;
	if (region->file_offset < 0 ||
	    (uint64_t)(read_start - region->data_start) >
	    (sizeof(off_t) == sizeof(int64_t) ? (uint64_t)INT64_MAX :
	    (uint64_t)INT32_MAX) - (uint64_t)region->file_offset)
		return EOVERFLOW;
	offset = region->file_offset +
		(off_t)(read_start - region->data_start);
	count = file_pread(region->file,
		(void *)(page->private_page->pmem.vaddr +
		    read_start - page->address),
		length, offset);
	return count == (ssize_t)length ? 0 : EIO;
}

/* Build an unlinked replacement while old is exclusively owned by the fault. */
static int
prepare_cow_copy(struct vm_page *page, struct vm_private_page *old,
	struct vm_private_page **result)
{
	struct vm_private_page *fresh;
	unsigned long irq;

	if (page == NULL || old == NULL || result == NULL)
		return EINVAL;
	fresh = private_page_alloc();
	if (fresh == NULL)
		return ENOMEM;
	if (allocate_page_frame(fresh, page) != 0) {
		vm_private_page_put(fresh);
		return ENOMEM;
	}
	memcpy((void *)fresh->pmem.vaddr, (const void *)old->pmem.vaddr,
	    PAGE_SIZE);
	irq = spin_lock_irqsave(&fresh->state_lock);
	fresh->flags = VM_PAGE_RESIDENT | VM_PAGE_DIRTY;
	if (++fresh->generation == 0)
		fresh->generation++;
	spin_unlock_irqrestore(&fresh->state_lock, irq);
	*result = fresh;
	return 0;
}

static void
vmspace_fault_wake_locked(struct vmspace *vm)
{
	unsigned long irq = spin_lock_irqsave(&vm->lock.guard);
	waitq_wake_all(&vm->fault_waitq);
	spin_unlock_irqrestore(&vm->lock.guard, irq);
}

int
vmspace_fault(struct vmspace *vm, uintptr_t address, uint32_t required)
{
	struct vm_private_page *reserved_backing = NULL;
	struct vm_object_page *object_page = NULL;
	struct vm_region *region;
	struct vm_page *page;
	struct vm_page **link;
	hal_physaddr_t prepared_physical = 0;
	uintptr_t page_address = address & ~(uintptr_t)(PAGE_SIZE - 1U);
	uint64_t prepared_backing_generation = 0;
	uint64_t reservation_generation;
	off_t object_offset = 0;
	uint32_t new_reservation_prot = 0;
	int error = 0;
	int mapped = 0;
	int private_io_owned = 0;
	int private_io_hold = 0;

	if (vm == NULL || vm == &kernel_vmspace ||
	    (required != HAL_SPACE_READ && required != HAL_SPACE_WRITE &&
	    required != HAL_SPACE_EXEC))
		return EINVAL;
	vm_reclaim_note_fault();

retry:
	error = 0;
	vm_metadata_enter();
	mutex_lock(&vm->lock);
	region = find_region_locked(vm, address, 1);
	if (region == NULL || (region->prot & required) == 0) {
		mutex_unlock(&vm->lock);
		vm_metadata_leave();
		return EFAULT;
	}
	page = find_page(region, page_address);
	if (page != NULL && (page->flags & VM_MAPPING_BUSY) != 0) {
		uint64_t sequence = waitq_sequence(&vm->fault_waitq);
		mutex_unlock(&vm->lock);
		vm_metadata_leave();
		vmspace_wait_fault_event(vm, sequence);
		goto retry;
	}
	if (page != NULL && page->object_page != NULL) {
		mutex_unlock(&vm->lock);
		vm_metadata_leave();
		return 0;
	}
	if (page != NULL) {
		struct vm_private_page *fresh = NULL;
		uint32_t reservation_prot;
		unsigned long state_irq;
		int backing_owner = 0;
		int need_cow = 0;
		int was_mapped = 0;
		int old_unmapped = 0;
		int retry_fault = 0;

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

		error = vm_private_page_io_acquire(reserved_backing);
		if (error == 0)
			backing_owner = 1;

		/* Waiting for another vmspace's owner may have changed this backing. */
		vm_metadata_enter();
		mutex_lock(&vm->lock);
		if (error == 0 &&
		    (find_region_locked(vm, address, 1) != region ||
		    find_page(region, page_address) != page ||
		    page->region != region || page->address != page_address ||
		    page->private_page != reserved_backing ||
		    region->prot != reservation_prot ||
		    (region->prot & required) == 0))
			error = EAGAIN;
		if (error == 0) {
			/* Full validation acknowledges unrelated generation changes. */
			reservation_generation = vm->generation;
			need_cow = required == HAL_SPACE_WRITE &&
			    (page->flags & VM_MAPPING_COW) != 0;
			was_mapped = (page->flags & VM_MAPPING_MAPPED) != 0;
		}
		mutex_unlock(&vm->lock);
		vm_metadata_leave();

		if (error == 0) {
			int swapped;
			state_irq = spin_lock_irqsave(&reserved_backing->state_lock);
			swapped = (reserved_backing->flags & VM_PAGE_SWAPPED) != 0;
			spin_unlock_irqrestore(&reserved_backing->state_lock, state_irq);
			if (swapped)
				error = page_in_owned(reserved_backing, page);
		}
		if (error == 0 && need_cow)
			error = prepare_cow_copy(page, reserved_backing, &fresh);
		if (error == 0) {
			state_irq = spin_lock_irqsave(&reserved_backing->state_lock);
			if ((reserved_backing->flags &
			    (VM_PAGE_BUSY | VM_PAGE_RESIDENT)) !=
			    (VM_PAGE_BUSY | VM_PAGE_RESIDENT))
				error = EIO;
			else {
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

		vm_metadata_enter();
		mutex_lock(&vm->lock);
		if (old_unmapped)
			page->flags &= ~VM_MAPPING_MAPPED;
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
		if (error == 0 &&
		    (vm->generation != reservation_generation ||
		    find_region_locked(vm, address, 1) != region ||
		    find_page(region, page_address) != page ||
		    page->region != region || page->address != page_address ||
		    page->private_page != reserved_backing ||
		    region->prot != reservation_prot ||
		    (region->prot & required) == 0))
			error = EAGAIN;
		if (error == 0 && fresh != NULL) {
			if (hal_page_map(vm->space, (void *)page_address,
			    fresh->pmem.paddr, PAGE_SIZE, region->prot) != HAL_OK)
				error = ENOMEM;
			else {
				vm_page_replace_private(page, fresh);
				fresh = NULL; /* The mapping owns the creator reference. */
				page->flags &= ~VM_MAPPING_COW;
				page->flags |= VM_MAPPING_MAPPED;
			}
		} else if (error == 0 &&
		    (page->flags & VM_MAPPING_MAPPED) == 0) {
			if (hal_page_map(vm->space, (void *)page_address,
			    prepared_physical, PAGE_SIZE,
			    vm_page_effective_prot(page)) != HAL_OK)
				error = ENOMEM;
			else
				page->flags |= VM_MAPPING_MAPPED;
		}
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

	vm_metadata_enter();
	mutex_lock(&vm->lock);
	if (find_region_locked(vm, address, 1) != region ||
	    (region->prot & required) == 0) {
		error = EFAULT;
		goto unlink_locked;
	}
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
	page->next = region->pages;
	region->pages = page;
	reservation_generation = vm->generation;
	new_reservation_prot = region->prot;
	mutex_unlock(&vm->lock);
	vm_metadata_leave();

	if (region->object != NULL) {
		uint64_t maximum_offset = sizeof(off_t) == 8 ?
		    (uint64_t)INT64_MAX : (uint64_t)INT32_MAX;
		if (region->file_offset < 0 ||
		    (uint64_t)region->file_offset +
		    (uint64_t)(page_address - region->start) > maximum_offset) {
			error = EOVERFLOW;
			goto remove_placeholder;
		}
		object_offset = region->file_offset +
		    (off_t)(page_address - region->start);
		error = vm_object_fault(region->object, object_offset, &object_page);
		if (error != 0)
			goto remove_placeholder;
		page->object_page = object_page;
		mapped = hal_page_map(vm->space, (void *)page_address,
		    object_page->pmem.paddr, PAGE_SIZE, region->prot) == HAL_OK;
		if (!mapped) {
			error = ENOMEM;
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
		return 0;
	}

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
	if (region->backing == VM_BACKING_FILE)
		error = fill_file_page(region, page);
	else {
		memset((void *)page->private_page->pmem.vaddr, 0, PAGE_SIZE);
		error = 0;
	}
	if (error == 0) {
		unsigned long irq = spin_lock_irqsave(
		    &page->private_page->state_lock);
		page->private_page->flags |= VM_PAGE_RESIDENT;
		if (++page->private_page->generation == 0)
			page->private_page->generation++;
		prepared_backing_generation = page->private_page->generation;
		prepared_physical = page->private_page->pmem.paddr;
		spin_unlock_irqrestore(&page->private_page->state_lock, irq);
	}
	if (error != 0)
		goto remove_placeholder;
	vm_metadata_enter();
	mutex_lock(&vm->lock);
	if (error == 0) {
		unsigned long irq = spin_lock_irqsave(
		    &reserved_backing->state_lock);

		if ((reserved_backing->flags &
		    (VM_PAGE_BUSY | VM_PAGE_RESIDENT)) !=
		    (VM_PAGE_BUSY | VM_PAGE_RESIDENT) ||
		    reserved_backing->generation != prepared_backing_generation ||
		    reserved_backing->pmem.paddr != prepared_physical)
			error = EAGAIN;
		spin_unlock_irqrestore(&reserved_backing->state_lock, irq);
	}
	if (error != 0)
		goto unlink_locked;
	if (vm->generation != reservation_generation ||
	    find_region_locked(vm, address, 1) != region ||
	    find_page(region, page_address) != page ||
	    page->region != region || page->address != page_address ||
	    page->private_page != reserved_backing ||
	    region->prot != new_reservation_prot ||
	    (region->prot & required) == 0) {
		error = EAGAIN;
		goto unlink_locked;
	}
	if (hal_page_map(vm->space, (void *)page_address,
	    prepared_physical, PAGE_SIZE, region->prot) != HAL_OK) {
		error = ENOMEM;
		goto unlink_locked;
	}
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
	if (mapped)
		(void)hal_page_unmap(vm->space, (void *)page_address, PAGE_SIZE);
	if (object_page != NULL) {
		vm_object_fault_release(object_page);
		object_page = NULL;
	}
	vm_metadata_enter();
	mutex_lock(&vm->lock);
unlink_locked:
	for (link = &region->pages; *link != NULL && *link != page;
	    link = &(*link)->next)
		;
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
	vm_metadata_enter();
	mutex_lock(&vm->lock);
	if (region->hold_count == 0)
		HAL_FATAL("VM fault region hold underflow");
	region->hold_count--;
	vmspace_fault_wake_locked(vm);
	mutex_unlock(&vm->lock);
	vm_metadata_leave();
	return error;
}

static int
vmspace_check_locked(struct vmspace *vm, uintptr_t address, size_t size,
	      uint32_t required)
{
	struct vm_region *region;
	uintptr_t current, end;

	if (vm == NULL || !vmspace_user_range_valid(address, size))
		return EFAULT;
	current = address;
	end = address + size;
	for (region = vm->regions; region != NULL && current < end;
	     region = region->next) {
		uintptr_t region_end = region->start + region->size;
		if (current < region->start)
			return EFAULT;
		if (current >= region_end)
			continue;
		if ((region->prot & required) != required)
			return EFAULT;
		current = region_end < end ? region_end : end;
	}
	return current == end ? 0 : EFAULT;
}

void
vmspace_unpin_user_pages(struct vmspace_pinned_page *pages, size_t page_count)
{
	if (pages == NULL)
		return;
	while (page_count != 0) {
		struct vmspace_pinned_page *page = &pages[--page_count];

		if (page->kind == VMSPACE_PINNED_PRIVATE)
			vm_private_page_unpin(page->owner.private_page);
		else if (page->kind == VMSPACE_PINNED_OBJECT)
			vm_object_page_unpin(page->owner.object_page);
		memset(page, 0, sizeof(*page));
	}
}

int
vmspace_pin_user_pages(struct vmspace *vm, uintptr_t address, size_t size,
	uint32_t required, struct vmspace_pinned_page *pages, size_t page_count)
{
	struct vm_private_page *wait_backing;
	uintptr_t first, last, current;
	uint32_t fault_access;
	size_t expected, index, acquired;
	int error, refault, wait_fault;
	uint64_t fault_sequence;

	if (vm == NULL || vm == &kernel_vmspace || pages == NULL || size == 0 ||
	    (required & ~(HAL_SPACE_READ | HAL_SPACE_WRITE |
	    HAL_SPACE_EXEC)) != 0 || required == 0 ||
	    !vmspace_user_range_valid(address, size))
		return EFAULT;
	first = address & ~(uintptr_t)(PAGE_SIZE - 1U);
	last = (address + size - 1U) & ~(uintptr_t)(PAGE_SIZE - 1U);
	expected = (size_t)((last - first) / PAGE_SIZE) + 1U;
	if (page_count != expected)
		return EINVAL;
	memset(pages, 0, page_count * sizeof(*pages));
	if ((required & HAL_SPACE_WRITE) != 0)
		fault_access = HAL_SPACE_WRITE;
	else if ((required & HAL_SPACE_READ) != 0)
		fault_access = HAL_SPACE_READ;
	else
		fault_access = HAL_SPACE_EXEC;

retry_faults:
	/* Faulting may sleep and may break COW, so it is never done under a VM lock. */
	for (current = first;; current += PAGE_SIZE) {
		error = vmspace_fault(vm, current, fault_access);
		if (error != 0)
			return error;
		if (current == last)
			break;
	}

	wait_backing = NULL;
	acquired = 0;
	refault = 0;
	wait_fault = 0;
	fault_sequence = 0;
	vm_metadata_enter();
	mutex_lock(&vm->lock);
	error = vmspace_check_locked(vm, address, size, required);
	/*
	 * Validate the complete range before taking the first backing pin.  Holding
	 * both metadata locks until the final pin closes the old wire/unlock/rewalk
	 * gap in which unmap, remap, or fork could change page identity.
	 */
	for (index = 0, current = first; error == 0 && index < page_count;
	     index++, current += PAGE_SIZE) {
		struct vm_region *region = find_region_locked(vm, current, 1);
		struct vm_page *entry = region != NULL ? find_page(region, current) : NULL;

		if (entry == NULL) {
			refault = 1;
			error = EAGAIN;
			break;
		}
		if ((entry->flags & VM_MAPPING_BUSY) != 0) {
			fault_sequence = waitq_sequence(&vm->fault_waitq);
			wait_fault = 1;
			error = EAGAIN;
			break;
		}
		if ((entry->private_page == NULL) ==
		    (entry->object_page == NULL)) {
			error = EFAULT;
			break;
		}
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
	for (index = 0, current = first; error == 0 && index < page_count;
	     index++, current += PAGE_SIZE) {
		struct vm_region *region = find_region_locked(vm, current, 1);
		struct vm_page *entry = find_page(region, current);

		if (entry->private_page != NULL) {
			error = vm_private_page_pin(entry->private_page,
			    &pages[index].memory);
			if (error == EBUSY) {
				/* Keep the wait target alive after dropping the VM locks. */
				wait_backing = entry->private_page;
				vm_private_page_ref(wait_backing);
			}
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
		int wait_error = vm_private_page_wait_idle(wait_backing);

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
	return error;
}

int
vmspace_wire_range(struct vmspace *vm, uintptr_t address, size_t size,
		   uint32_t required)
{
	uintptr_t page, first, last;
	uint32_t fault_access;
	int error = vmspace_check(vm, address, size, required);

	if (error != 0)
		return error;
	/*
	 * vmspace_check() accepts a protection mask, while vmspace_fault()
	 * describes one concrete access.  A bidirectional uaccess pin asks for
	 * READ|WRITE and must not pass that mask through as though it were an
	 * individual fault access.
	 */
	if ((required & HAL_SPACE_WRITE) != 0)
		fault_access = HAL_SPACE_WRITE;
	else if ((required & HAL_SPACE_READ) != 0)
		fault_access = HAL_SPACE_READ;
	else
		fault_access = HAL_SPACE_EXEC;
	first = page = address & ~(uintptr_t)(PAGE_SIZE - 1U);
	last = (address + size - 1U) & ~(uintptr_t)(PAGE_SIZE - 1U);
	for (;;) {
		struct vm_region *region;
		struct vm_page *entry;

		error = vmspace_fault(vm, page, fault_access);
		if (error != 0)
			break;
		vm_metadata_enter();
		mutex_lock(&vm->lock);
		region = find_region_locked(vm, page, 1);
		entry = region != NULL ? find_page(region, page) : NULL;
		if (entry != NULL && (entry->flags & VM_MAPPING_BUSY) != 0) {
			uint64_t sequence = waitq_sequence(&vm->fault_waitq);

			mutex_unlock(&vm->lock);
			vm_metadata_leave();
			vmspace_wait_fault_event(vm, sequence);
			continue;
		}
		if (entry == NULL || (entry->object_page == NULL &&
		    !vm_private_page_is_resident(entry))) {
			error = EFAULT;
			mutex_unlock(&vm->lock);
			vm_metadata_leave();
			break;
		}
		entry->wire_count++;
		vmspace_generation_advance_locked(vm);
		mutex_unlock(&vm->lock);
		vm_metadata_leave();
		if (page == last)
			return 0;
		page += PAGE_SIZE;
	}
	while (page != first) {
		struct vm_region *region;
		struct vm_page *entry;
		page -= PAGE_SIZE;
		vm_metadata_enter();
		mutex_lock(&vm->lock);
		region = find_region_locked(vm, page, 1);
		entry = region != NULL ? find_page(region, page) : NULL;
		if (entry != NULL && entry->wire_count != 0)
			entry->wire_count--;
		mutex_unlock(&vm->lock);
		vm_metadata_leave();
	}
	return error;
}

static void
vmspace_unwire_range_locked(struct vmspace *vm, uintptr_t address, size_t size)
{
	uintptr_t page, last;

	if (vm == NULL || size == 0 || address > UINTPTR_MAX - size)
		return;
	page = address & ~(uintptr_t)(PAGE_SIZE - 1U);
	last = (address + size - 1U) & ~(uintptr_t)(PAGE_SIZE - 1U);
	for (;;) {
		struct vm_region *region = find_region_locked(vm, page, 1);
		struct vm_page *entry = region != NULL ? find_page(region, page) : NULL;
		if (entry != NULL && entry->wire_count != 0)
			entry->wire_count--;
		if (page == last)
			break;
		page += PAGE_SIZE;
	}
}

static int
copy_backing(struct vmspace *vm, uintptr_t user_address, void *kernel,
	     size_t size, uint32_t required, int to_user)
{
	struct vmspace_pinned_page *pages;
	uint8_t *bytes = kernel;
	uintptr_t first, last;
	size_t page_count, position;
	int error;

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
	error = vmspace_pin_user_pages(vm, user_address, size, required, pages,
	    page_count);
	if (error != 0) {
		kern_free(pages);
		return error == ENOMEM ? ENOMEM : EFAULT;
	}
	position = user_address - first;
	while (size != 0) {
		struct vmspace_pinned_page *page =
		    &pages[position / PAGE_SIZE];
		size_t offset = position & (PAGE_SIZE - 1U);
		size_t chunk = PAGE_SIZE - offset;

		if (chunk > size)
			chunk = size;
		if (page->kind == VMSPACE_PINNED_PRIVATE) {
			void *mapped = (uint8_t *)page->memory.vaddr + offset;

			if (to_user) {
				memcpy(mapped, bytes, chunk);
				vm_private_page_mark_dirty(
				    page->owner.private_page);
			} else {
				memcpy(bytes, mapped, chunk);
			}
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
		bytes += chunk;
		position += chunk;
		size -= chunk;
	}
	vmspace_unpin_user_pages(pages, page_count);
	kern_free(pages);
	return error;
}

static int
vmspace_copy_to_locked(struct vmspace *vm, uintptr_t destination,
		const void *source, size_t size)
{
	return copy_backing(vm, destination, (void *)source, size,
			    HAL_SPACE_WRITE, 1);
}

static int
vmspace_copy_from_locked(struct vmspace *vm, void *destination,
		  uintptr_t source, size_t size)
{
	return copy_backing(vm, source, destination, size,
			    HAL_SPACE_READ, 0);
}

static int
vmspace_find_free_range_bounded_locked(struct vmspace *vm, uintptr_t minimum,
	uintptr_t maximum, size_t size, size_t alignment, uintptr_t *mapped)
{
	uintptr_t start, limit;
	struct vm_region *region;

	vmspace_layout_init();
	if (vm == NULL || mapped == NULL || size == 0 ||
	    alignment < PAGE_SIZE || (alignment & (alignment - 1U)) != 0 ||
	    minimum < vm_layout.user_minimum || maximum > vm_layout.user_limit ||
	    minimum >= maximum || size > (size_t)(maximum - minimum))
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
	for (;;) {
		int moved = 0;
		if (start >= limit || size > limit - start)
			return ENOMEM;
		for (region = vm->regions; region != NULL; region = region->next) {
			if (start + size <= region->start)
				break;
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

static int
vmspace_find_free_range_locked(struct vmspace *vm, uintptr_t hint, size_t size,
			 size_t alignment, uintptr_t *mapped)
{
	vmspace_layout_init();
	if (hint < vm_layout.mmap_base)
		hint = vm_layout.mmap_base;
	if (hint >= vm_layout.user_limit)
		return ENOMEM;
	return vmspace_find_free_range_bounded_locked(vm, hint, vm_layout.user_limit,
	    size, alignment, mapped);
}

static int
vmspace_map_find_locked(struct vmspace *vm, uintptr_t hint, size_t size,
		 uint32_t prot, uintptr_t *mapped)
{
	uintptr_t start;
	int error = vmspace_find_free_range_locked(vm, hint, size, PAGE_SIZE, &start);
	if (error == 0)
		error = vmspace_map_anon_locked(vm, start,
		    (size + PAGE_SIZE - 1U) & ~(PAGE_SIZE - 1U), prot, NULL);
	if (error == 0)
		*mapped = start;
	return error;
}

static int
vmspace_map_file_find_locked(struct vmspace *vm, uintptr_t hint, size_t size,
		      uint32_t prot, struct file *file, off_t offset,
		      size_t data_size, uintptr_t *mapped)
{
	uintptr_t start;
	size_t rounded;
	int error = vmspace_find_free_range_locked(vm, hint, size, PAGE_SIZE, &start);
	if (error != 0)
		return error;
	rounded = (size + PAGE_SIZE - 1U) & ~(PAGE_SIZE - 1U);
	if (data_size > size)
		data_size = size;
	error = vmspace_map_file_locked(vm, start, rounded, prot, file, offset,
	    start, data_size, NULL);
	if (error == 0)
		*mapped = start;
	return error;
}

static int
vmspace_map_file_shared_find_locked(struct vmspace *vm, uintptr_t hint,
				    size_t size,
			     uint32_t prot, struct file *file, off_t offset,
			     size_t data_size, uintptr_t *mapped)
{
	uintptr_t start;
	size_t rounded;
	int error = vmspace_find_free_range_locked(vm, hint, size, PAGE_SIZE,
	    &start);
	if (error != 0)
		return error;
	rounded = (size + PAGE_SIZE - 1U) & ~(PAGE_SIZE - 1U);
	if (data_size > size)
		data_size = size;
	error = vmspace_map_file_shared_locked(vm, start, rounded, prot, file,
	    offset,
	    data_size, NULL);
	if (error == 0)
		*mapped = start;
	return error;
}

static void
free_vm_page(struct vmspace *vm, struct vm_page *page)
{
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

static void
free_region_pages(struct vmspace *vm, struct vm_region *region)
{
	struct vm_page *page;

	while ((page = region->pages) != NULL) {
		region->pages = page->next;
		free_vm_page(vm, page);
	}
}

/*
 * vmspace_unmap() has two teardown phases.  This first phase runs while both
 * the global VM metadata lock and the vmspace lock are held.  It must remove
 * every hardware and reverse mapping before the region list publishes the
 * virtual address as free.
 *
 * A private backing can own physical memory or a swap slot.  Take a temporary
 * reference before vm_page_untrack() so that the reverse-map detach cannot be
 * the operation which frees those resources.  The detached vm_page retains
 * that reference solely as a retire token; release_detached_region_pages()
 * drops it after the address has been unpublished and both locks are gone.
 */
static void
detach_vm_page_for_unmap(struct vmspace *vm, struct vm_page *page)
{
	struct vm_private_page *backing;

	if ((page->flags & VM_MAPPING_MAPPED) != 0) {
		if (hal_page_unmap(vm->space, (void *)page->address,
		    PAGE_SIZE) != HAL_OK)
			HAL_FATAL("VM unmap commit failed");
		page->flags &= ~VM_MAPPING_MAPPED;
	}
	if (page->object_page != NULL) {
		vm_object_mapping_remove(page->object_page, page);
		page->object_page = NULL;
		page->object_next = NULL;
		return;
	}
	backing = page->private_page;
	if (backing == NULL)
		return;
	vm_private_page_ref(backing);
	vm_page_untrack(page);
	/* This pointer now owns only the temporary retirement reference. */
	page->private_page = backing;
	page->private_next = NULL;
}

static void
detach_region_pages_for_unmap(struct vmspace *vm, struct vm_region *region)
{
	struct vm_page *page;

	for (page = region->pages; page != NULL; page = page->next)
		detach_vm_page_for_unmap(vm, page);
}

static void
release_detached_region_pages(struct vm_region *region)
{
	struct vm_page *page;

	while ((page = region->pages) != NULL) {
		region->pages = page->next;
		/* Drops the private-backing retire token, if there is one. */
		if (page->private_page != NULL)
			vm_private_page_put(page->private_page);
		vm_page_free_metadata(page);
	}
}

static int
split_region_prepared(struct vm_region *region, uintptr_t address,
	struct vm_region *right)
{
	struct vm_page **link;
	size_t left_size, right_size;
	uintptr_t original_data_start, original_data_end;
	uintptr_t left_data_start, left_data_end;
	uintptr_t right_data_start, right_data_end;

	if (region == NULL || right == NULL || address <= region->start ||
	    address >= region->start + region->size ||
	    (address & (PAGE_SIZE - 1U)) != 0)
		return EINVAL;
	if (region->hold_count != 0)
		return EBUSY;
	if (region->flags & VM_REGION_IMMUTABLE)
		return EACCES;
	left_size = address - region->start;
	right_size = region->size - left_size;
	original_data_start = region->data_start;
	original_data_end = region->data_start + region->data_size;
	*right = *region;
	right->start = address;
	right->size = right_size;
	right_data_start = original_data_start > address ?
	    original_data_start : address;
	right_data_end = original_data_end < region->start + region->size ?
	    original_data_end : region->start + region->size;
	if (right_data_end < right_data_start)
		right_data_end = right_data_start;
	right->data_start = right_data_start;
	right->data_size = right_data_end - right_data_start;
	if (region->backing == VM_BACKING_FILE &&
	    right_data_start > original_data_start)
		right->file_offset +=
		    (off_t)(right_data_start - original_data_start);
	right->commit_size = region->commit_size != 0 ? right_size : 0;
	right->pages = NULL;
	if (right->file != NULL)
		file_ref(right->file);
	if (right->object != NULL)
		vm_object_ref(right->object);
	region->size = left_size;
	if (original_data_start < address &&
	    original_data_end > region->start) {
		left_data_start = original_data_start;
		left_data_end = original_data_end < address ?
		    original_data_end : address;
	} else {
		left_data_start = region->start;
		left_data_end = left_data_start;
	}
	region->data_start = left_data_start;
	region->data_size = left_data_end - left_data_start;
	region->commit_size = region->commit_size != 0 ? left_size : 0;
	for (link = &region->pages; *link != NULL; ) {
		struct vm_page *page = *link;
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

static int
split_region(struct vm_region *region, uintptr_t address)
{
	struct vm_region *right = kern_calloc(1, sizeof(*right));
	int error;

	if (right == NULL)
		return ENOMEM;
	error = split_region_prepared(region, address, right);
	if (error != 0)
		kern_free(right);
	return error;
}

static void
release_retired_regions(struct vm_region *retired)
{
	struct vm_region *region;

	while ((region = retired) != NULL) {
		retired = region->next;
		release_detached_region_pages(region);
		if (region->file != NULL)
			(void)file_close(region->file);
		if (region->object != NULL)
			vm_object_put(region->object);
		if (region->commit_size != 0)
			vm_commit_release(region->commit_size);
		kern_free(region);
	}
}

/*
 * Publish a prepared mapping at an exact address.  Two preallocated split
 * records make the metadata commit failure-free after the first old region
 * is changed.
 */
static int
vmspace_replace_prepared(struct vmspace *vm, struct vm_region *prepared,
	struct vm_region **result)
{
	struct vm_region *split_first, *split_second;
	struct vm_region *retired = NULL;
	struct vm_region *region;
	struct vm_region **link;
	uintptr_t start, end;
	size_t removed = 0;
	int error = 0;

	if (vm == NULL || vm == &kernel_vmspace || prepared == NULL ||
	    !range_valid(prepared->start, prepared->size))
		return EINVAL;
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
	for (region = vm->regions; region != NULL; region = region->next) {
		struct vm_page *page;
		uintptr_t overlap_start, overlap_end;

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
		for (page = region->pages; page != NULL; page = page->next)
			if (page->address < end &&
			    page->address + PAGE_SIZE > start &&
			    page->wire_count != 0) {
				error = EAGAIN;
				goto out_locked;
			}
		overlap_start = region->start > start ? region->start : start;
		overlap_end = region->start + region->size < end ?
		    region->start + region->size : end;
		removed += overlap_end - overlap_start;
	}
	if (removed > vm->mapped_virtual_bytes ||
	    (uint64_t)prepared->size > vm->address_limit -
	    (vm->mapped_virtual_bytes - removed > vm->address_limit ?
	    vm->address_limit : vm->mapped_virtual_bytes - removed)) {
		error = ENOMEM;
		goto out_locked;
	}
	region = find_region_locked(vm, end - 1U, 1);
	if (region != NULL && end < region->start + region->size) {
		error = split_region_prepared(region, end, split_first);
		if (error != 0)
			goto out_locked;
		split_first = NULL;
	}
	region = find_region_locked(vm, start, 1);
	if (region != NULL && start > region->start) {
		struct vm_region *split = split_first != NULL ?
		    split_first : split_second;

		error = split_region_prepared(region, start, split);
		if (error != 0)
			HAL_FATAL("prepared VM split commit failed");
		if (split == split_first)
			split_first = NULL;
		else
			split_second = NULL;
	}
	for (region = vm->regions; region != NULL && region->start < end;
	     region = region->next)
		if (region->start >= start)
			detach_region_pages_for_unmap(vm, region);
	for (link = &vm->regions; *link != NULL; link = &(*link)->next)
		if ((*link)->start >= start)
			break;
	while ((region = *link) != NULL && region->start < end) {
		*link = region->next;
		vm->mapped_virtual_bytes -= region->size;
		region->next = retired;
		retired = region;
	}
	insert_region(vm, prepared);
	vm->mapped_virtual_bytes += prepared->size;
	vmspace_generation_advance_locked(vm);
	if (result != NULL)
		*result = prepared;

out_locked:
	mutex_unlock(&vm->lock);
	vm_metadata_leave();
	kern_free(split_first);
	kern_free(split_second);
	if (error == 0 && retired != NULL &&
	    vmspace_unmap_retire_checkpoint != NULL)
		vmspace_unmap_retire_checkpoint(vm, start, prepared->size);
	release_retired_regions(retired);
	return error;
}

static int
vmspace_set_brk_start_locked(struct vmspace *vm, uintptr_t start,
	uint64_t static_data_bytes)
{
	if (vm == NULL || vm == &kernel_vmspace || vm->brk_start != 0 ||
	    (start & (PAGE_SIZE - 1U)) != 0 ||
	    start < vm_layout.user_minimum || start >= vm_layout.brk_limit ||
	    overlaps(vm, start, PAGE_SIZE))
		return EINVAL;
	if (static_data_bytes > vm->data_limit)
		return ENOMEM;
	vm->brk_start = start;
	vm->brk_current = start;
	vm->static_data_bytes = static_data_bytes;
	return 0;
}

static int
vmspace_brk_locked(struct vmspace *vm, uintptr_t requested, uintptr_t *result)
{
	struct vm_region **link, *region = NULL;
	uintptr_t old_end, new_end;
	size_t difference;
	int error;

	if (vm == NULL || vm == &kernel_vmspace || result == NULL ||
	    vm->brk_start == 0)
		return EINVAL;
	vmspace_wait_faults_locked(vm);
	if (requested == 0) {
		*result = vm->brk_current;
		return 0;
	}
	if (requested < vm->brk_start || requested >= vm_layout.brk_limit)
		return ENOMEM;
	if (vm->static_data_bytes > vm->data_limit ||
	    (uint64_t)(requested - vm->brk_start) >
	    vm->data_limit - vm->static_data_bytes)
		return ENOMEM;
	old_end = (vm->brk_current + PAGE_SIZE - 1U) &
		~(uintptr_t)(PAGE_SIZE - 1U);
	new_end = (requested + PAGE_SIZE - 1U) &
		~(uintptr_t)(PAGE_SIZE - 1U);
	for (link = &vm->regions; *link != NULL; link = &(*link)->next)
		if (((*link)->flags & VM_REGION_BRK) != 0) {
			region = *link;
			break;
		}
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
			if ((uint64_t)difference > vm->address_limit -
			    (vm->mapped_virtual_bytes > vm->address_limit ?
			    vm->address_limit : vm->mapped_virtual_bytes))
				return ENOMEM;
			error = vm_commit_reserve(difference);
			if (error != 0)
				return error;
			region->size += difference;
			region->commit_size += difference;
			vm->mapped_virtual_bytes += difference;
		}
	} else if (new_end < old_end) {
		struct vm_page **page_link;
		if (region == NULL || region->start + region->size != old_end)
			return EINVAL;
		for (page_link = &region->pages; *page_link != NULL; ) {
			struct vm_page *page = *page_link;
			if (page->address < new_end) {
				page_link = &page->next;
				continue;
			}
			*page_link = page->next;
			free_vm_page(vm, page);
		}
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

static int
vmspace_unmap_locked(struct vmspace *vm, uintptr_t start, size_t size,
	struct vm_region **retired)
{
	struct vm_region **link, *region;
	uintptr_t end;
	int error;

	if (vm == NULL || vm == &kernel_vmspace || retired == NULL ||
	    !range_valid(start, size))
		return EINVAL;
	vmspace_wait_faults_locked(vm);
	end = start + size;
	/* Validate the complete transaction before split_region mutates metadata. */
	for (region = vm->regions; region != NULL; region = region->next) {
		struct vm_page *page;
		if (region->start >= end || region->start + region->size <= start)
			continue;
		if ((region->flags & VM_REGION_IMMUTABLE) != 0)
			return EACCES;
		for (page = region->pages; page != NULL; page = page->next)
			if (page->address < end && page->address + PAGE_SIZE > start &&
			    page->wire_count != 0)
				return EBUSY;
	}
	region = find_region_locked(vm, end - 1U, 1);
	if (region != NULL && end < region->start + region->size) {
		error = split_region(region, end);
		if (error != 0)
			return error;
	}
	region = find_region_locked(vm, start, 1);
	if (region != NULL && start > region->start) {
		error = split_region(region, start);
		if (error != 0)
			return error;
	}
	/*
	 * All validation, allocation, and splitting is complete.  From this
	 * point the commit cannot report failure: a HAL failure would leave only
	 * part of the old range detached, so it is a kernel invariant violation.
	 * Detach every old PTE/reverse map before unlinking any region.
	 */
	for (region = vm->regions; region != NULL && region->start < end;
	     region = region->next)
		if (region->start >= start)
			detach_region_pages_for_unmap(vm, region);
	for (link = &vm->regions; *link != NULL; link = &(*link)->next)
		if ((*link)->start >= start)
			break;
	while ((region = *link) != NULL && region->start < end) {
		*link = region->next;
		vm->mapped_virtual_bytes -= region->size;
		region->next = *retired;
		*retired = region;
	}
	return 0;
}

static int
vmspace_protect_locked(struct vmspace *vm, uintptr_t start, size_t size,
		uint32_t prot)
{
	struct vm_region *region, *first, *end_region;
	struct vm_page *page, *failed_page = NULL;
	struct vm_region *failed_region = NULL;
	uintptr_t end, covered;
	size_t new_commit = 0;
	int error;

	if (vm == NULL || vm == &kernel_vmspace || !range_valid(start, size) ||
	    (prot & ~(HAL_SPACE_READ | HAL_SPACE_WRITE | HAL_SPACE_EXEC)) != 0)
		return EINVAL;
	vmspace_wait_faults_locked(vm);
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
	end_region = find_region_locked(vm, end - 1U, 1);
	if (end_region != NULL && end < end_region->start + end_region->size) {
		error = split_region(end_region, end);
		if (error != 0)
			return error;
	}
	region = find_region_locked(vm, start, 1);
	if (start > region->start) {
		error = split_region(region, start);
		if (error != 0)
			return error;
		region = region->next;
	}
	first = region;
	for (; region != NULL && region->start < end; region = region->next)
		if (region->commit_size == 0 && region->object == NULL &&
		    ((region->backing == VM_BACKING_ANON && prot != 0) ||
		     (region->backing == VM_BACKING_FILE &&
		      (prot & HAL_SPACE_WRITE) != 0))) {
			if (new_commit > SIZE_MAX - region->size)
				return EOVERFLOW;
			new_commit += region->size;
		}
	if (new_commit != 0) {
		error = vm_commit_reserve(new_commit);
		if (error != 0)
			return error;
	}
	for (region = first; region != NULL && region->start < end;
	     region = region->next) {
		for (page = region->pages; page != NULL; page = page->next) {
			uint32_t page_prot = (page->flags & VM_MAPPING_COW) ?
			    (prot & ~HAL_SPACE_WRITE) : prot;
			int hal_error = HAL_OK;

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
			if (hal_error != HAL_OK) {
				failed_region = region;
				failed_page = page;
				goto rollback;
			}
		}
	}
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
	for (region = first; region != NULL && region->start < end;
	     region = region->next) {
		struct vm_page *rollback;
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
				hal_physaddr_t physical = rollback->object_page != NULL ?
				    rollback->object_page->pmem.paddr :
				    rollback->private_page->pmem.paddr;
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

int
vmspace_sync(struct vmspace *vm, uintptr_t start, size_t size, int flags)
{
	uintptr_t current, end;
	if (vm == NULL || vm == &kernel_vmspace || !range_valid(start, size) ||
	    (flags & ~(MS_ASYNC | MS_INVALIDATE | MS_SYNC)) != 0 ||
	    (flags & (MS_ASYNC | MS_SYNC)) == 0 ||
	    (flags & (MS_ASYNC | MS_SYNC)) == (MS_ASYNC | MS_SYNC))
		return EINVAL;
	current = start;
	end = start + size;
	while (current < end) {
		struct vm_object *object = NULL;
		struct vm_region *region;
		uintptr_t overlap_end;
		off_t offset = 0;
		int error;

		vm_metadata_enter();
		mutex_lock(&vm->lock);
		region = find_region_locked(vm, current, 1);
		if (region == NULL) {
			mutex_unlock(&vm->lock);
			vm_metadata_leave();
			return ENOMEM;
		}
		overlap_end = region->start + region->size;
		if (overlap_end > end)
			overlap_end = end;
		if (region->object != NULL) {
			object = region->object;
			vm_object_ref(object);
			offset = region->file_offset +
			    (off_t)(current - region->start);
		}
		mutex_unlock(&vm->lock);
		vm_metadata_leave();

		error = object != NULL ? vm_object_sync_range(object, offset,
		    overlap_end - current, flags) : 0;
		if (object != NULL)
			vm_object_put(object);
		if (error != 0)
			return error;
		current = overlap_end;
	}
	if ((flags & MS_INVALIDATE) != 0) {
		vm_metadata_enter();
		mutex_lock(&vm->lock);
		vmspace_generation_advance_locked(vm);
		mutex_unlock(&vm->lock);
		vm_metadata_leave();
	}
	return 0;
}

static void
vmspace_destroy(struct vmspace *vm)
{
	struct vm_region *region;

	vm_metadata_enter();
	while ((region = vm->regions) != NULL) {
		vm->regions = region->next;
		free_region_pages(vm, region);
		vm_metadata_leave();
		if (region->file != NULL)
			(void)file_close(region->file);
		if (region->object != NULL)
			vm_object_put(region->object);
		if (region->commit_size != 0)
			vm_commit_release(region->commit_size);
		kern_free(region);
		vm_metadata_enter();
	}
	hal_page_destroy_space(vm->space);
	(void)atomic_raw_fetch_add_relaxed(&vmspace_live.value, (unsigned)-1);
	vm_metadata_leave();
	kern_free(vm);
}

void
vmspace_put(struct vmspace *vm)
{
	if (vm == NULL || vm == &kernel_vmspace)
		return;
	if (refcount_put(&vm->refs))
		vmspace_destroy(vm);
}

void
vmspace_put_deferred(struct vmspace *vm)
{
	void (*notify)(void *);
	void *notify_argument;
	unsigned long irq;

	if (vm == NULL || vm == &kernel_vmspace)
		return;
	if (!refcount_put(&vm->refs))
		return;
	/*
	 * Scheduler retirement is non-sleeping.  Transfer final ownership to
	 * the process reaper instead of entering VFS/object teardown here.
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
	/* Enqueue owns the wakeup.  The callback is deliberately invoked after
	 * dropping the queue lock and must only retain a scheduler notification;
	 * final puts can arrive while higher-ranked VM/reclaim locks are held. */
	if (notify != NULL)
		notify(notify_argument);
}

void
vmspace_set_reaper_notify(void (*notify)(void *), void *argument)
{
	int pending;
	unsigned long irq;

	vmspace_layout_init();
	irq = spin_lock_irqsave(&vmspace_reap_lock);
	vmspace_reap_notify = notify;
	vmspace_reap_notify_argument = argument;
	pending = vmspace_reap_head != NULL;
	spin_unlock_irqrestore(&vmspace_reap_lock, irq);
	/* A queue item may predate registration.  Publish the retained wake only
	 * after the callback and argument have been installed atomically. */
	if (pending && notify != NULL)
		notify(argument);
}

unsigned
vmspace_reap_pending(void)
{
	struct vmspace *list;
	unsigned count = 0;
	unsigned long irq;

	irq = spin_lock_irqsave(&vmspace_reap_lock);
	list = vmspace_reap_head;
	vmspace_reap_head = vmspace_reap_tail = NULL;
	spin_unlock_irqrestore(&vmspace_reap_lock, irq);
	while (list != NULL) {
		struct vmspace *next = list->reap_next;
		list->reap_next = NULL;
		vmspace_destroy(list);
		list = next;
		count++;
	}
	return count;
}

unsigned vmspace_count(void)
{ return atomic_load_acquire(&vmspace_live); }

uint64_t
vmspace_address_cap(void)
{
	vmspace_layout_init();
	return (uint64_t)(vm_layout.user_limit - vm_layout.user_minimum);
}

static int
vmspace_set_address_limit_locked(struct vmspace *vm, uint64_t limit)
{
	if (vm == NULL || vm == &kernel_vmspace || limit > vmspace_address_cap())
		return EINVAL;
	vm->address_limit = limit;
	return 0;
}

static void
vmspace_set_stack_limit_locked(struct vmspace *vm, uint64_t limit)
{
	if (vm != NULL && vm != &kernel_vmspace)
		vm->stack_limit = limit;
}

static int
vmspace_set_data_limit_locked(struct vmspace *vm, uint64_t limit)
{
	uint64_t cap = vmspace_address_cap();

	if (vm == NULL || vm == &kernel_vmspace)
		return EINVAL;
	/* RLIM_INFINITY is the public resource-limit representation.  The VM
	 * still needs a concrete bound for overflow-safe brk arithmetic, so map
	 * it to the largest addressable user range at this boundary. */
	if (limit == UINT64_MAX)
		limit = cap;
	if (limit > cap)
		return EINVAL;
	vm->data_limit = limit;
	return 0;
}

static void
vmspace_generation_advance_locked(struct vmspace *vm)
{
	if (++vm->generation == 0)
		vm->generation++;
}

int
vmspace_fork(struct vmspace *source, struct vmspace **result)
{
	struct vm_private_page *wait_backing;
	struct vmspace *failed_copy;
	int error, wait_error;
	if (source == NULL || source == &kernel_vmspace || result == NULL)
		return EINVAL;
	*result = NULL;
retry:
	wait_backing = NULL;
	failed_copy = NULL;
	vm_metadata_enter();
	mutex_lock(&source->lock);
	error = vmspace_fork_locked(source, result, &wait_backing,
	    &failed_copy);
	if (error == 0)
		vmspace_generation_advance_locked(source);
	mutex_unlock(&source->lock);
	vm_metadata_leave();
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
	return error;
}

int
vmspace_map_anon(struct vmspace *vm, uintptr_t start, size_t size,
	uint32_t prot, struct vm_region **result)
{
	int error;
	if (vm == NULL || vm == &kernel_vmspace)
		return EINVAL;
	vm_metadata_enter();
	mutex_lock(&vm->lock);
	error = vmspace_map_anon_locked(vm, start, size, prot, result);
	if (error == 0)
		vmspace_generation_advance_locked(vm);
	mutex_unlock(&vm->lock);
	vm_metadata_leave();
	return error;
}

int
vmspace_map_anon_fixed_noreplace(struct vmspace *vm, uintptr_t start,
	size_t size, uint32_t prot, struct vm_region **result)
{
	int error;
	if (vm == NULL || vm == &kernel_vmspace)
		return EINVAL;
	vm_metadata_enter();
	mutex_lock(&vm->lock);
	error = vmspace_map_anon_fixed_noreplace_locked(vm, start, size, prot,
	    result);
	if (error == 0)
		vmspace_generation_advance_locked(vm);
	mutex_unlock(&vm->lock);
	vm_metadata_leave();
	return error;
}

int
vmspace_map_anon_fixed(struct vmspace *vm, uintptr_t start, size_t size,
	uint32_t prot, int shared, struct vm_region **result)
{
	struct vm_object *object = NULL;
	struct vm_region *prepared = NULL;
	unsigned flags = shared ? VM_REGION_SHARED : 0;
	size_t commit_size = !shared && prot != 0 ? size : 0;
	int error;

	if (vm == NULL || vm == &kernel_vmspace ||
	    (shared != 0 && shared != 1))
		return EINVAL;
	if (shared) {
		error = vm_object_create_anonymous(size, &object);
		if (error != 0)
			return error;
	}
	error = prepare_region(start, size, prot, VM_BACKING_ANON, NULL, 0,
	    start, shared ? size : 0, flags, commit_size, &prepared);
	if (error != 0) {
		if (object != NULL)
			vm_object_put(object);
		return error;
	}
	prepared->object = object;
	error = vmspace_replace_prepared(vm, prepared, result);
	if (error != 0)
		discard_prepared_region(prepared);
	return error;
}

int
vmspace_map_anon_shared_find(struct vmspace *vm, uintptr_t hint, size_t size,
	uint32_t prot, uintptr_t *mapped)
{
	struct vm_object *object;
	struct vm_region *region = NULL;
	uintptr_t start;
	size_t rounded;
	int error;

	if (vm == NULL || vm == &kernel_vmspace || mapped == NULL || size == 0 ||
	    size > SIZE_MAX - (PAGE_SIZE - 1U))
		return EINVAL;
	rounded = (size + PAGE_SIZE - 1U) & ~(PAGE_SIZE - 1U);
	error = vm_object_create_anonymous(rounded, &object);
	if (error != 0)
		return error;
	vm_metadata_enter();
	mutex_lock(&vm->lock);
	error = vmspace_find_free_range_locked(vm, hint, rounded, PAGE_SIZE,
	    &start);
	if (error == 0)
		error = map_region(vm, start, rounded, prot, VM_BACKING_ANON,
		    NULL, 0, start, rounded, VM_REGION_SHARED, 0, &region);
	if (error == 0) {
		region->object = object;
		*mapped = start;
		vmspace_generation_advance_locked(vm);
	}
	mutex_unlock(&vm->lock);
	vm_metadata_leave();
	if (error != 0)
		vm_object_put(object);
	return error;
}

int
vmspace_map_file(struct vmspace *vm, uintptr_t start, size_t size,
	uint32_t prot, struct file *file, off_t offset, uintptr_t data_start,
	size_t data_size, struct vm_region **result)
{
	int error;
	if (vm == NULL || vm == &kernel_vmspace)
		return EINVAL;
	vm_metadata_enter();
	mutex_lock(&vm->lock);
	error = vmspace_map_file_locked(vm, start, size, prot, file, offset,
	    data_start, data_size, result);
	if (error == 0)
		vmspace_generation_advance_locked(vm);
	mutex_unlock(&vm->lock);
	vm_metadata_leave();
	return error;
}

int
vmspace_map_file_shared(struct vmspace *vm, uintptr_t start, size_t size,
	uint32_t prot, struct file *file, off_t offset, size_t data_size,
	struct vm_region **result)
{
	int error;
	if (vm == NULL || vm == &kernel_vmspace)
		return EINVAL;
	vm_metadata_enter();
	mutex_lock(&vm->lock);
	error = vmspace_map_file_shared_locked(vm, start, size, prot, file,
	    offset, data_size, result);
	if (error == 0)
		vmspace_generation_advance_locked(vm);
	mutex_unlock(&vm->lock);
	vm_metadata_leave();
	return error;
}

int
vmspace_map_file_fixed(struct vmspace *vm, uintptr_t start, size_t size,
	uint32_t prot, struct file *file, off_t offset, size_t data_size,
	int shared, struct vm_region **result)
{
	struct vm_object *object = NULL;
	struct vm_region *prepared = NULL;
	unsigned flags = shared ? VM_REGION_SHARED : 0;
	size_t commit_size = !shared && (prot & HAL_SPACE_WRITE) != 0 ?
	    size : 0;
	int error;

	if (vm == NULL || vm == &kernel_vmspace || file == NULL ||
	    (shared != 0 && shared != 1))
		return EINVAL;
	if (shared) {
		error = vm_object_get_shared(file, &object);
		if (error != 0)
			return error;
	}
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
	return error;
}

int
vmspace_map_stack(struct vmspace *vm, uintptr_t top, size_t size,
	size_t guard_size)
{
	int error;
	if (vm == NULL || vm == &kernel_vmspace)
		return EINVAL;
	vm_metadata_enter();
	mutex_lock(&vm->lock);
	error = vmspace_map_stack_locked(vm, top, size, guard_size);
	if (error == 0)
		vmspace_generation_advance_locked(vm);
	mutex_unlock(&vm->lock);
	vm_metadata_leave();
	return error;
}

struct vm_region *
vmspace_find_region(struct vmspace *vm, uintptr_t address, size_t size)
{
	struct vm_region *region;
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

int
vmspace_shared_mapping_key(struct vmspace *vm, uintptr_t address, size_t size,
	struct vm_object **object, uintptr_t *offset)
{
	struct vm_region *region;
	int error = EINVAL;
	if (vm == NULL || vm == &kernel_vmspace || object == NULL ||
	    offset == NULL)
		return EINVAL;
	vm_metadata_enter();
	mutex_lock(&vm->lock);
	region = find_region_locked(vm, address, size);
	if (region != NULL && (region->flags & VM_REGION_SHARED) != 0 &&
	    region->object != NULL && region->file_offset >= 0 &&
	    (uintmax_t)region->file_offset <= UINTPTR_MAX -
	    (address - region->start)) {
		vm_object_ref(region->object);
		*object = region->object;
		*offset = (uintptr_t)region->file_offset + address - region->start;
		error = 0;
	}
	mutex_unlock(&vm->lock);
	vm_metadata_leave();
	return error;
}

int
vmspace_check(struct vmspace *vm, uintptr_t address, size_t size,
	uint32_t required)
{
	int error;
	if (vm == NULL || vm == &kernel_vmspace)
		return EFAULT;
	vm_metadata_enter();
	mutex_lock(&vm->lock);
	error = vmspace_check_locked(vm, address, size, required);
	mutex_unlock(&vm->lock);
	vm_metadata_leave();
	return error;
}

void
vmspace_unwire_range(struct vmspace *vm, uintptr_t address, size_t size)
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

int
vmspace_copy_to(struct vmspace *vm, uintptr_t destination,
	const void *source, size_t size)
{
	int error;
	if (vm == NULL || vm == &kernel_vmspace)
		return EFAULT;
	error = vmspace_copy_to_locked(vm, destination, source, size);
	return error;
}

int
vmspace_copy_from(struct vmspace *vm, void *destination,
	uintptr_t source, size_t size)
{
	int error;
	if (vm == NULL || vm == &kernel_vmspace)
		return EFAULT;
	error = vmspace_copy_from_locked(vm, destination, source, size);
	return error;
}

int
vmspace_find_free_range_bounded(struct vmspace *vm, uintptr_t minimum,
	uintptr_t maximum, size_t size, size_t alignment, uintptr_t *mapped)
{
	int error;
	if (vm == NULL || vm == &kernel_vmspace)
		return EINVAL;
	vm_metadata_enter();
	mutex_lock(&vm->lock);
	error = vmspace_find_free_range_bounded_locked(vm, minimum, maximum,
	    size, alignment, mapped);
	mutex_unlock(&vm->lock);
	vm_metadata_leave();
	return error;
}

int
vmspace_find_free_range(struct vmspace *vm, uintptr_t hint, size_t size,
	size_t alignment, uintptr_t *mapped)
{
	int error;
	if (vm == NULL || vm == &kernel_vmspace)
		return EINVAL;
	vm_metadata_enter();
	mutex_lock(&vm->lock);
	error = vmspace_find_free_range_locked(vm, hint, size, alignment,
	    mapped);
	mutex_unlock(&vm->lock);
	vm_metadata_leave();
	return error;
}

int
vmspace_map_find(struct vmspace *vm, uintptr_t hint, size_t size,
	uint32_t prot, uintptr_t *mapped)
{
	int error;
	if (vm == NULL || vm == &kernel_vmspace)
		return EINVAL;
	vm_metadata_enter();
	mutex_lock(&vm->lock);
	error = vmspace_map_find_locked(vm, hint, size, prot, mapped);
	if (error == 0)
		vmspace_generation_advance_locked(vm);
	mutex_unlock(&vm->lock);
	vm_metadata_leave();
	return error;
}

int
vmspace_map_file_find(struct vmspace *vm, uintptr_t hint, size_t size,
	uint32_t prot, struct file *file, off_t offset, size_t data_size,
	uintptr_t *mapped)
{
	int error;
	if (vm == NULL || vm == &kernel_vmspace)
		return EINVAL;
	vm_metadata_enter();
	mutex_lock(&vm->lock);
	error = vmspace_map_file_find_locked(vm, hint, size, prot, file,
	    offset, data_size, mapped);
	if (error == 0)
		vmspace_generation_advance_locked(vm);
	mutex_unlock(&vm->lock);
	vm_metadata_leave();
	return error;
}

int
vmspace_map_file_shared_find(struct vmspace *vm, uintptr_t hint, size_t size,
	uint32_t prot, struct file *file, off_t offset, size_t data_size,
	uintptr_t *mapped)
{
	int error;
	if (vm == NULL || vm == &kernel_vmspace)
		return EINVAL;
	vm_metadata_enter();
	mutex_lock(&vm->lock);
	error = vmspace_map_file_shared_find_locked(vm, hint, size, prot, file,
	    offset, data_size, mapped);
	if (error == 0)
		vmspace_generation_advance_locked(vm);
	mutex_unlock(&vm->lock);
	vm_metadata_leave();
	return error;
}

int
vmspace_set_brk_start(struct vmspace *vm, uintptr_t start,
	uint64_t static_data_bytes)
{
	int error;
	if (vm == NULL || vm == &kernel_vmspace)
		return EINVAL;
	vm_metadata_enter();
	mutex_lock(&vm->lock);
	error = vmspace_set_brk_start_locked(vm, start, static_data_bytes);
	if (error == 0)
		vmspace_generation_advance_locked(vm);
	mutex_unlock(&vm->lock);
	vm_metadata_leave();
	return error;
}

int
vmspace_brk(struct vmspace *vm, uintptr_t requested, uintptr_t *result)
{
	int error;
	if (vm == NULL || vm == &kernel_vmspace)
		return EINVAL;
	vm_metadata_enter();
	mutex_lock(&vm->lock);
	error = vmspace_brk_locked(vm, requested, result);
	if (error == 0 && requested != 0)
		vmspace_generation_advance_locked(vm);
	mutex_unlock(&vm->lock);
	vm_metadata_leave();
	return error;
}

int
vmspace_unmap(struct vmspace *vm, uintptr_t start, size_t size)
{
	struct vm_region *retired = NULL;
	int error;
	if (vm == NULL || vm == &kernel_vmspace)
		return EINVAL;
	vm_metadata_enter();
	mutex_lock(&vm->lock);
	error = vmspace_unmap_locked(vm, start, size, &retired);
	if (error == 0)
		vmspace_generation_advance_locked(vm);
	mutex_unlock(&vm->lock);
	vm_metadata_leave();
	/* Test-only weak checkpoint: VA is free and old mappings are detached,
	 * while heavyweight backing/object retirement has not started yet. */
	if (error == 0 && retired != NULL &&
	    vmspace_unmap_retire_checkpoint != NULL)
		vmspace_unmap_retire_checkpoint(vm, start, size);
	release_retired_regions(retired);
	return error;
}

int
vmspace_protect(struct vmspace *vm, uintptr_t start, size_t size,
	uint32_t prot)
{
	int error;
	if (vm == NULL || vm == &kernel_vmspace)
		return EINVAL;
	vm_metadata_enter();
	mutex_lock(&vm->lock);
	error = vmspace_protect_locked(vm, start, size, prot);
	if (error == 0)
		vmspace_generation_advance_locked(vm);
	mutex_unlock(&vm->lock);
	vm_metadata_leave();
	return error;
}

int
vmspace_set_address_limit(struct vmspace *vm, uint64_t limit)
{
	int error;
	if (vm == NULL || vm == &kernel_vmspace)
		return EINVAL;
	vm_metadata_enter();
	mutex_lock(&vm->lock);
	error = vmspace_set_address_limit_locked(vm, limit);
	if (error == 0)
		vmspace_generation_advance_locked(vm);
	mutex_unlock(&vm->lock);
	vm_metadata_leave();
	return error;
}

int
vmspace_set_data_limit(struct vmspace *vm, uint64_t limit)
{
	int error;
	if (vm == NULL || vm == &kernel_vmspace)
		return EINVAL;
	vm_metadata_enter();
	mutex_lock(&vm->lock);
	error = vmspace_set_data_limit_locked(vm, limit);
	if (error == 0)
		vmspace_generation_advance_locked(vm);
	mutex_unlock(&vm->lock);
	vm_metadata_leave();
	return error;
}

void
vmspace_set_stack_limit(struct vmspace *vm, uint64_t limit)
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
