/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/vm-reclaim.h"
#include "kern/vmspace.h"
#include "kern/swap.h"
#include "kern/kmem.h"
#include "kern/lock.h"
#include "kern/page.h"
#include "kern/sched.h"
#include "kern/vm-lock.h"

#include <errno.h>
#include <hal/hal.h>
#include <string.h>

#define PAGE_SIZE ZEDBSD_PAGE_SIZE
#define VM_PAGE_TRACKED 0x0010U

static struct vm_private_page *page_queue;
static struct mutex reclaim_lock;
struct vm_reclaim_stats vm_reclaim_counters;
#define stats vm_reclaim_counters

__attribute__((weak)) int vm_object_reclaim_one(void) { return ENOMEM; }
__attribute__((weak)) unsigned vm_object_page_count(void) { return 0; }

static void
private_page_advance_locked(struct vm_private_page *backing)
{
	if (++backing->generation == 0)
		backing->generation++;
}

void
vm_private_page_init(struct vm_private_page *backing)
{
	if (backing == NULL)
		return;
	refcount_init(&backing->refs, 1);
	spin_init(&backing->state_lock, LOCK_RANK_VM_OBJECT,
	    "VM private backing");
	waitq_init(&backing->state_waitq, "VM private backing");
	backing->generation = 1;
	backing->swap_slot = SWAP_SLOT_NONE;
}

void
vm_private_page_ref(struct vm_private_page *backing)
{
	if (backing != NULL)
		refcount_get(&backing->refs);
}

void
vm_private_page_put(struct vm_private_page *backing)
{
	struct hal_pmem memory;
	uint32_t slot;
	unsigned long irq;

	if (backing == NULL || !refcount_put(&backing->refs))
		return;
	irq = spin_lock_irqsave(&backing->state_lock);
	if (backing->mapping_count != 0 || backing->mappings != NULL ||
	    backing->active_operations != 0 || backing->pin_count != 0 ||
	    (backing->flags & (VM_PAGE_BUSY | VM_PAGE_TRACKED)) != 0)
		HAL_FATAL("destroying active VM private backing");
	memory = backing->pmem;
	memset(&backing->pmem, 0, sizeof(backing->pmem));
	slot = backing->swap_slot;
	backing->swap_slot = SWAP_SLOT_NONE;
	spin_unlock_irqrestore(&backing->state_lock, irq);
	if (memory.size != 0)
		(void)hal_pmem_free(&memory);
	if (slot != SWAP_SLOT_NONE && swap_system_backend() != NULL)
		swap_free_slot(swap_system_backend(), slot);
	vm_private_page_free_metadata(backing);
}

static int
private_page_wait_sequence(struct vm_private_page *backing, uint64_t sequence)
{
	unsigned long irq;
	int error;

	irq = spin_lock_irqsave(&backing->state_lock);
	error = waitq_sleep(&backing->state_waitq, &backing->state_lock,
	    sequence, 0, 0);
	spin_unlock_irqrestore(&backing->state_lock, irq);
	return error;
}

int
vm_private_page_io_acquire(struct vm_private_page *backing)
{
	if (backing == NULL)
		return EINVAL;
	for (;;) {
		uint64_t sequence;
		unsigned long irq = spin_lock_irqsave(&backing->state_lock);

		if ((backing->flags & VM_PAGE_BUSY) == 0 &&
		    backing->active_operations == 0 && backing->pin_count == 0) {
			backing->flags |= VM_PAGE_BUSY;
			private_page_advance_locked(backing);
			spin_unlock_irqrestore(&backing->state_lock, irq);
			return 0;
		}
		sequence = waitq_sequence(&backing->state_waitq);
		spin_unlock_irqrestore(&backing->state_lock, irq);
		{
			int error = private_page_wait_sequence(backing, sequence);
			if (error != 0 && error != EAGAIN)
				return error;
		}
	}
}

int
vm_private_page_io_try_acquire(struct vm_private_page *backing)
{
	unsigned long irq;

	if (backing == NULL)
		return EINVAL;
	irq = spin_lock_irqsave(&backing->state_lock);
	if ((backing->flags & VM_PAGE_BUSY) != 0 ||
	    backing->active_operations != 0 || backing->pin_count != 0) {
		spin_unlock_irqrestore(&backing->state_lock, irq);
		return EBUSY;
	}
	/* The returned I/O ownership includes an explicit lifetime hold. */
	refcount_get(&backing->refs);
	backing->flags |= VM_PAGE_BUSY;
	private_page_advance_locked(backing);
	spin_unlock_irqrestore(&backing->state_lock, irq);
	return 0;
}

void
vm_private_page_io_release(struct vm_private_page *backing)
{
	unsigned long irq;

	if (backing == NULL)
		return;
	irq = spin_lock_irqsave(&backing->state_lock);
	if ((backing->flags & VM_PAGE_BUSY) == 0)
		HAL_FATAL("VM private backing I/O ownership underflow");
	backing->flags &= ~VM_PAGE_BUSY;
	private_page_advance_locked(backing);
	waitq_wake_all(&backing->state_waitq);
	spin_unlock_irqrestore(&backing->state_lock, irq);
}

int
vm_private_page_wait_idle(struct vm_private_page *backing)
{
	if (backing == NULL)
		return EINVAL;
	for (;;) {
		uint64_t sequence;
		unsigned long irq = spin_lock_irqsave(&backing->state_lock);

		if ((backing->flags & VM_PAGE_BUSY) == 0 &&
		    backing->active_operations == 0 && backing->pin_count == 0) {
			spin_unlock_irqrestore(&backing->state_lock, irq);
			return 0;
		}
		sequence = waitq_sequence(&backing->state_waitq);
		spin_unlock_irqrestore(&backing->state_lock, irq);
		{
			int error = private_page_wait_sequence(backing, sequence);
			if (error != 0 && error != EAGAIN)
				return error;
		}
	}
}

int
vm_private_page_operation_try_begin(struct vm_private_page *backing)
{
	unsigned long irq;

	if (backing == NULL)
		return EINVAL;
	irq = spin_lock_irqsave(&backing->state_lock);
	if ((backing->flags & VM_PAGE_BUSY) != 0 || backing->pin_count != 0) {
		spin_unlock_irqrestore(&backing->state_lock, irq);
		return EBUSY;
	}
	refcount_get(&backing->refs);
	backing->active_operations++;
	if (backing->active_operations == 0)
		HAL_FATAL("VM private backing operation counter overflow");
	spin_unlock_irqrestore(&backing->state_lock, irq);
	return 0;
}

void
vm_private_page_operation_end(struct vm_private_page *backing)
{
	unsigned long irq;

	if (backing == NULL)
		return;
	irq = spin_lock_irqsave(&backing->state_lock);
	if (backing->active_operations == 0)
		HAL_FATAL("VM private backing operation counter underflow");
	backing->active_operations--;
	private_page_advance_locked(backing);
	if (backing->active_operations == 0)
		waitq_wake_all(&backing->state_waitq);
	spin_unlock_irqrestore(&backing->state_lock, irq);
	vm_private_page_put(backing);
}

void
vm_private_page_mark_dirty(struct vm_private_page *backing)
{
	unsigned long irq;

	if (backing == NULL)
		return;
	irq = spin_lock_irqsave(&backing->state_lock);
	backing->flags |= VM_PAGE_DIRTY;
	private_page_advance_locked(backing);
	spin_unlock_irqrestore(&backing->state_lock, irq);
}

int
vm_private_page_pin(struct vm_private_page *backing,
	struct hal_pmem *memory)
{
	unsigned long irq;

	if (backing == NULL || memory == NULL)
		return EINVAL;
	irq = spin_lock_irqsave(&backing->state_lock);
	if ((backing->flags & VM_PAGE_BUSY) != 0 ||
	    backing->active_operations != 0 ||
	    (backing->flags & VM_PAGE_RESIDENT) == 0) {
		spin_unlock_irqrestore(&backing->state_lock, irq);
		return EBUSY;
	}
	refcount_get(&backing->refs);
	backing->pin_count++;
	if (backing->pin_count == 0)
		HAL_FATAL("VM private backing pin counter overflow");
	*memory = backing->pmem;
	spin_unlock_irqrestore(&backing->state_lock, irq);
	return 0;
}

void
vm_private_page_unpin(struct vm_private_page *backing)
{
	unsigned long irq;

	if (backing == NULL)
		return;
	irq = spin_lock_irqsave(&backing->state_lock);
	if (backing->pin_count == 0)
		HAL_FATAL("VM private backing pin counter underflow");
	backing->pin_count--;
	private_page_advance_locked(backing);
	if (backing->pin_count == 0)
		waitq_wake_all(&backing->state_waitq);
	spin_unlock_irqrestore(&backing->state_lock, irq);
	vm_private_page_put(backing);
}

static uint32_t mapping_prot(const struct vm_page *page)
{
	uint32_t prot = page->region->prot;
	if (page->flags & VM_MAPPING_COW)
		prot &= ~HAL_SPACE_WRITE;
	return prot;
}

void vm_reclaim_init(void)
{
	vm_metadata_init();
	page_queue = NULL;
	memset(&stats, 0, sizeof(stats));
	(void)mutex_init(&reclaim_lock, LOCK_RANK_VMSPACE, "VM reclaim");
}

static void queue_remove(struct vm_private_page *backing)
{
	struct vm_private_page **link = &page_queue;
	while (*link != NULL && *link != backing)
		link = &(*link)->queue_next;
	if (*link == backing)
		*link = backing->queue_next;
}

void vm_page_track(struct vm_page *page)
{
	struct vm_private_page *backing;
	unsigned long irq;
	int resident;
	if (page == NULL || (backing = page->private_page) == NULL)
		return;
	vm_metadata_enter();
	mutex_lock(&reclaim_lock);
	irq = spin_lock_irqsave(&backing->state_lock);
	if (backing->mapping_count == 0 ||
	    (backing->flags & VM_PAGE_TRACKED) != 0)
		HAL_FATAL("invalid VM private backing track");
	backing->flags |= VM_PAGE_TRACKED;
	resident = (backing->flags & VM_PAGE_RESIDENT) != 0;
	spin_unlock_irqrestore(&backing->state_lock, irq);
	backing->queue_next = page_queue;
	page_queue = backing;
	if (resident)
		stats.resident++;
	mutex_unlock(&reclaim_lock);
	vm_metadata_leave();
}

void vm_page_untrack(struct vm_page *page)
{
	struct vm_private_page *backing;
	struct vm_page **link;
	unsigned long irq;
	int last_mapping, resident, swapped, tracked;
	if (page == NULL || (backing = page->private_page) == NULL)
		return;
	vm_metadata_enter();
	mutex_lock(&reclaim_lock);
	link = &backing->mappings;
	while (*link != NULL && *link != page)
		link = &(*link)->private_next;
	if (*link != page)
		HAL_FATAL("VM private page reverse mapping not linked");
	*link = page->private_next;
	page->private_page = NULL;
	page->private_next = NULL;
	irq = spin_lock_irqsave(&backing->state_lock);
	if (backing->mapping_count == 0)
		HAL_FATAL("VM private backing mapping counter underflow");
	last_mapping = --backing->mapping_count == 0;
	resident = (backing->flags & VM_PAGE_RESIDENT) != 0;
	swapped = (backing->flags & VM_PAGE_SWAPPED) != 0;
	tracked = (backing->flags & VM_PAGE_TRACKED) != 0;
	if (last_mapping)
		backing->flags &= ~VM_PAGE_TRACKED;
	spin_unlock_irqrestore(&backing->state_lock, irq);
	if (last_mapping && tracked) {
		queue_remove(backing);
		if (resident && stats.resident)
			stats.resident--;
		if (swapped && stats.swapped)
			stats.swapped--;
	}
	mutex_unlock(&reclaim_lock);
	vm_metadata_leave();
	vm_private_page_put(backing);
}

void vm_page_replace_private(struct vm_page *page,
	struct vm_private_page *fresh)
{
	struct vm_private_page *old;
	struct vm_page **link;
	unsigned long old_irq, fresh_irq;
	int old_last, old_resident, old_swapped, fresh_resident;
	if (page == NULL || fresh == NULL || page->private_page == NULL)
		HAL_FATAL("invalid private page replacement");
	vm_metadata_enter();
	old = page->private_page;
	mutex_lock(&reclaim_lock);
	old_irq = spin_lock_irqsave(&old->state_lock);
	if ((old->flags & VM_PAGE_BUSY) == 0 || old->mapping_count == 0)
		HAL_FATAL("replacing an unowned VM private backing");
	spin_unlock_irqrestore(&old->state_lock, old_irq);
	link = &old->mappings;
	while (*link != NULL && *link != page)
		link = &(*link)->private_next;
	if (*link != page)
		HAL_FATAL("private page mapping not linked");
	*link = page->private_next;
	old_irq = spin_lock_irqsave(&old->state_lock);
	old_last = --old->mapping_count == 0;
	old_resident = (old->flags & VM_PAGE_RESIDENT) != 0;
	old_swapped = (old->flags & VM_PAGE_SWAPPED) != 0;
	if (old_last)
		old->flags &= ~VM_PAGE_TRACKED;
	spin_unlock_irqrestore(&old->state_lock, old_irq);
	fresh_irq = spin_lock_irqsave(&fresh->state_lock);
	if (fresh->mapping_count != 0 || fresh->mappings != NULL ||
	    (fresh->flags & (VM_PAGE_BUSY | VM_PAGE_TRACKED)) != 0)
		HAL_FATAL("replacement VM private backing is not fresh");
	fresh->mapping_count = 1;
	fresh->flags |= VM_PAGE_TRACKED;
	fresh_resident = (fresh->flags & VM_PAGE_RESIDENT) != 0;
	spin_unlock_irqrestore(&fresh->state_lock, fresh_irq);
	page->private_page = fresh;
	page->private_next = fresh->mappings;
	fresh->mappings = page;
	fresh->queue_next = page_queue;
	page_queue = fresh;
	if (fresh_resident)
		stats.resident++;
	if (old_last) {
		queue_remove(old);
		if (old_resident && stats.resident)
			stats.resident--;
		if (old_swapped && stats.swapped)
			stats.swapped--;
	}
	mutex_unlock(&reclaim_lock);
	vm_metadata_leave();
	/* The fault's explicit I/O hold keeps old alive across this mapping drop. */
	vm_private_page_put(old);
}

int vm_page_share_private(struct vm_page *source, struct vm_page *copy)
{
	struct vm_private_page *backing;
	unsigned long irq;
	int error = 0;
	if (source == NULL || copy == NULL || source->private_page == NULL)
		return EINVAL;
	backing = source->private_page;
	error = vm_private_page_operation_try_begin(backing);
	if (error != 0)
		return error;
	vm_metadata_enter();
	mutex_lock(&reclaim_lock);
	if (source->private_page != backing) {
		error = EAGAIN;
		goto out;
	}
	irq = spin_lock_irqsave(&backing->state_lock);
	if ((backing->flags & VM_PAGE_BUSY) != 0)
		HAL_FATAL("VM private operation overlapped I/O owner");
	refcount_get(&backing->refs);
	backing->mapping_count++;
	if (backing->mapping_count == 0)
		HAL_FATAL("VM private backing mapping counter overflow");
	spin_unlock_irqrestore(&backing->state_lock, irq);
	copy->private_page = backing;
	copy->private_next = backing->mappings;
	backing->mappings = copy;
	source->flags |= VM_MAPPING_COW;
	copy->flags |= VM_MAPPING_COW;
	if (error != 0) {
		backing->mappings = copy->private_next;
		copy->private_page = NULL;
		copy->private_next = NULL;
		irq = spin_lock_irqsave(&backing->state_lock);
		if (backing->mapping_count == 0)
			HAL_FATAL("VM private backing mapping rollback underflow");
		backing->mapping_count--;
		spin_unlock_irqrestore(&backing->state_lock, irq);
		if (refcount_put(&backing->refs))
			HAL_FATAL("shared VM private backing lost source mapping");
	}
out:
	mutex_unlock(&reclaim_lock);
	vm_metadata_leave();
	if (error != 0)
		vm_private_page_operation_end(backing);
	return error;
}

static int backing_wired_or_avoided(struct vm_private_page *backing,
	struct vm_page *avoid)
{
	struct vm_page *page;
	for (page = backing->mappings; page != NULL; page = page->private_next)
		if (page == avoid || page->wire_count != 0 ||
		    (page->flags & VM_MAPPING_BUSY) != 0)
			return 1;
	return 0;
}

static int
backing_has_wired_mapping(struct vm_private_page *backing)
{
	struct vm_page *page;

	for (page = backing->mappings; page != NULL; page = page->private_next)
		if (page->wire_count != 0)
			return 1;
	return 0;
}

static int
backing_has_busy_mapping(struct vm_private_page *backing)
{
	struct vm_page *page;

	for (page = backing->mappings; page != NULL; page = page->private_next)
		if ((page->flags & VM_MAPPING_BUSY) != 0)
			return 1;
	return 0;
}

static uint32_t backing_pte_flags(struct vm_private_page *backing)
{
	struct vm_page *page;
	uint32_t combined = 0, flags;
	for (page = backing->mappings; page != NULL; page = page->private_next) {
		if (!(page->flags & VM_MAPPING_MAPPED))
			continue;
		flags = 0;
		if (hal_page_query(page->vm->space, (void *)page->address,
		    &flags) == HAL_OK)
			combined |= flags;
	}
	return combined;
}

static void
vmspace_fault_wake(struct vmspace *vm)
{
	unsigned long irq = spin_lock_irqsave(&vm->lock.guard);

	waitq_wake_all(&vm->fault_waitq);
	spin_unlock_irqrestore(&vm->lock.guard, irq);
}

/* Called with the global metadata and reclaim locks held. */
static void
pin_backing_mappings(struct vm_private_page *backing)
{
	struct vm_page *page;

	for (page = backing->mappings; page != NULL; page = page->private_next) {
		vmspace_ref(page->vm);
		mutex_lock(&page->vm->lock);
		if ((page->flags & VM_MAPPING_BUSY) != 0 ||
		    page->region->hold_count == (unsigned)-1)
			HAL_FATAL("VM reclaim mapping pin invariant failed");
		page->flags |= VM_MAPPING_BUSY;
		page->region->hold_count++;
		mutex_unlock(&page->vm->lock);
	}
}

/* Called with the global metadata and reclaim locks held. */
static void
unpin_backing_mappings(struct vm_private_page *backing)
{
	struct vm_page *page;

	for (page = backing->mappings; page != NULL; page = page->private_next) {
		struct vmspace *vm = page->vm;
		mutex_lock(&vm->lock);
		if ((page->flags & VM_MAPPING_RECLAIM_UNMAPPED) != 0) {
			page->flags &= ~(VM_MAPPING_RECLAIM_UNMAPPED |
			    VM_MAPPING_RECLAIM_PROTECTED | VM_MAPPING_MAPPED);
		}
		page->flags &= ~VM_MAPPING_RECLAIM_PROTECTED;
		page->flags &= ~VM_MAPPING_BUSY;
		if (page->region->hold_count == 0)
			HAL_FATAL("VM reclaim region hold underflow");
		page->region->hold_count--;
		if (++vm->generation == 0)
			vm->generation++;
		vmspace_fault_wake(vm);
		mutex_unlock(&vm->lock);
		/* Never run final vmspace destruction under either VM metadata lock. */
		vmspace_put_deferred(vm);
	}
}

int
vm_private_page_in_owned(struct vm_private_page *backing,
	struct vm_page *accounting_page)
{
	const struct hal_pmem_request request = {
		HAL_PMEM_PADDR_ANY, PAGE_SIZE, PAGE_SIZE,
		HAL_PMEM_TYPE_RAM, 0
	};
	struct swap_backend *backend = swap_system_backend();
	uint32_t slot;
	unsigned long irq;
	int error;

	if (backing == NULL || accounting_page == NULL || backend == NULL)
		return EIO;
	irq = spin_lock_irqsave(&backing->state_lock);
	if ((backing->flags & (VM_PAGE_BUSY | VM_PAGE_SWAPPED)) !=
	    (VM_PAGE_BUSY | VM_PAGE_SWAPPED)) {
		spin_unlock_irqrestore(&backing->state_lock, irq);
		return EIO;
	}
	slot = backing->swap_slot;
	spin_unlock_irqrestore(&backing->state_lock, irq);

	error = hal_pmem_alloc(&request, &backing->pmem) == HAL_OK ? 0 : ENOMEM;
	if (error != 0 && vm_reclaim_one(accounting_page) == 0)
		error = hal_pmem_alloc(&request, &backing->pmem) == HAL_OK ? 0 :
		    ENOMEM;
	if (error == 0)
		error = swap_read_page(backend, slot,
		    (void *)backing->pmem.vaddr);
	if (error != 0) {
		if (backing->pmem.size != 0) {
			struct hal_pmem memory = backing->pmem;

			memset(&backing->pmem, 0, sizeof(backing->pmem));
			(void)hal_pmem_free(&memory);
		}
		return error;
	}

	/* No source state is held while the backing operation completes.  The
	 * exclusive private-page I/O owner keeps the token stable until this
	 * resident state is published. */
	swap_free_slot(backend, slot);
	irq = spin_lock_irqsave(&backing->state_lock);
	if ((backing->flags & (VM_PAGE_BUSY | VM_PAGE_SWAPPED)) !=
	    (VM_PAGE_BUSY | VM_PAGE_SWAPPED) || backing->swap_slot != slot)
		HAL_FATAL("VM private page-in state changed under I/O owner");
	backing->swap_slot = SWAP_SLOT_NONE;
	backing->flags &= ~VM_PAGE_SWAPPED;
	/* The old slot was the only persistent copy.  Keep the resident page
	 * dirty until a later reclaim writes it to an active source. */
	backing->flags |= VM_PAGE_RESIDENT | VM_PAGE_DIRTY;
	private_page_advance_locked(backing);
	spin_unlock_irqrestore(&backing->state_lock, irq);
	vm_page_note_in(accounting_page);
	return 0;
}

static int
private_page_targets_source(struct vm_private_page *backing,
	unsigned source_id)
{
	uint32_t slot;
	unsigned decoded_source;
	unsigned flags;
	unsigned long irq;

	irq = spin_lock_irqsave(&backing->state_lock);
	flags = backing->flags;
	slot = backing->swap_slot;
	spin_unlock_irqrestore(&backing->state_lock, irq);
	return (flags & VM_PAGE_SWAPPED) != 0 && slot != SWAP_SLOT_NONE &&
	    swap_slot_decode(slot, &decoded_source, NULL) == 0 &&
	    decoded_source == source_id;
}

static void
drain_mapping_pins_release(struct vm_private_page *backing)
{
	vm_metadata_enter();
	mutex_lock(&reclaim_lock);
	unpin_backing_mappings(backing);
	mutex_unlock(&reclaim_lock);
	vm_metadata_leave();
}

int
vm_reclaim_drain_swap_source(unsigned source_id)
{
	if (source_id >= SWAP_SOURCE_COUNT)
		return EINVAL;
	for (;;) {
		struct vm_private_page *selected = NULL;
		struct vm_private_page *wait_backing = NULL;
		struct vm_page *accounting_page = NULL;
		struct swap_backend *backend;
		struct swap_source_stats source_stats;
		int scan_error = 0;
		int error;

		/* Select and pin one target while reverse mappings are stable.  No VM
		 * metadata lock survives physical allocation or backing I/O. */
		vm_metadata_enter();
		mutex_lock(&reclaim_lock);
		for (struct vm_private_page *backing = page_queue;
		     backing != NULL; backing = backing->queue_next) {
			if (!private_page_targets_source(backing, source_id))
				continue;
			/* A wired page cannot legitimately have been reclaimed to swap.
			 * Report the broken invariant instead of waiting forever. */
			if (backing_has_wired_mapping(backing)) {
				scan_error = EIO;
				break;
			}
			if (backing_has_busy_mapping(backing) ||
			    vm_private_page_io_try_acquire(backing) != 0) {
				vm_private_page_ref(backing);
				wait_backing = backing;
				break;
			}
			/* A fault may have completed between the first observation and
			 * our exclusive acquisition. */
			if (!private_page_targets_source(backing, source_id)) {
				vm_private_page_io_release(backing);
				vm_private_page_put(backing);
				continue;
			}
			if (backing->mappings == NULL)
				HAL_FATAL("tracked swapped backing has no mapping");
			pin_backing_mappings(backing);
			accounting_page = backing->mappings;
			selected = backing;
			break;
		}
		mutex_unlock(&reclaim_lock);
		vm_metadata_leave();
		if (scan_error != 0)
			return scan_error;

		if (selected != NULL) {
			error = vm_private_page_in_owned(selected, accounting_page);
			drain_mapping_pins_release(selected);
			vm_private_page_io_release(selected);
			vm_private_page_put(selected);
			if (error != 0)
				return error;
			continue;
		}
		if (wait_backing != NULL) {
			error = vm_private_page_wait_idle(wait_backing);
			vm_private_page_put(wait_backing);
			if (error != 0 && error != EAGAIN)
				return error;
			sched_yield();
			continue;
		}

		/* A slot allocator which crossed begin_drain may not have published
		 * its backing token yet.  Source counters close that scan/publication
		 * window; yield until it finishes, then scan again. */
		backend = swap_system_backend();
		if (backend == NULL)
			return ENXIO;
		error = swap_source_get_stats(backend, source_id, &source_stats);
		if (error != 0)
			return error;
		if (source_stats.state != SWAP_SOURCE_STATE_DRAINING)
			return EBUSY;
		if (source_stats.allocated_slots == 0 &&
		    source_stats.inflight == 0)
			return 0;
		sched_yield();
	}
}

/* Mapping metadata is pinned and the backing has one exclusive I/O owner. */
static int
unmap_backing_ptes(struct vm_private_page *backing, uint32_t *pte_flags)
{
	struct vm_page *page;
	int error = 0;

	if (pte_flags == NULL)
		return EINVAL;
	*pte_flags = 0;
	/*
	 * A pre-unmap query is not a stable dirty snapshot: userspace can write
	 * between the query and shootdown.  First make every mapping read-only and
	 * wait for that protection change, then collect flags and remove the PTEs.
	 */
	for (page = backing->mappings; page != NULL; page = page->private_next) {
		uint32_t flags = 0;
		uint32_t readonly;

		if ((page->flags & VM_MAPPING_MAPPED) == 0)
			continue;
		readonly = mapping_prot(page) & ~HAL_SPACE_WRITE;
		if (readonly == 0) {
			/* No non-writable representation exists for a write-only PTE.
			 * Synchronous unmap is the revoke; conservatively write it back. */
			if (hal_page_unmap(page->vm->space,
			    (void *)page->address, PAGE_SIZE) != HAL_OK) {
				error = EIO;
				break;
			}
			page->flags |= VM_MAPPING_RECLAIM_UNMAPPED;
			*pte_flags |= HAL_PAGE_DIRTY;
			continue;
		}
		if (hal_page_prot_query(page->vm->space,
		    (void *)page->address, PAGE_SIZE, readonly, &flags) != HAL_OK) {
			error = EIO;
			break;
		}
		page->flags |= VM_MAPPING_RECLAIM_PROTECTED;
		*pte_flags |= flags;
	}
	if (error != 0)
		goto rollback;
	for (page = backing->mappings; page != NULL; page = page->private_next) {
		if ((page->flags & VM_MAPPING_MAPPED) == 0 ||
		    (page->flags & VM_MAPPING_RECLAIM_UNMAPPED) != 0)
			continue;
		if (hal_page_unmap(page->vm->space, (void *)page->address,
		    PAGE_SIZE) != HAL_OK) {
			error = EIO;
			break;
		}
		page->flags |= VM_MAPPING_RECLAIM_UNMAPPED;
	}
	if (error == 0)
		return 0;

rollback:
	/* Roll back outside all VM locks.  A failed remap remains lazy. */
	for (page = backing->mappings; page != NULL; page = page->private_next) {
		if ((page->flags & VM_MAPPING_RECLAIM_UNMAPPED) != 0) {
			if (hal_page_map(page->vm->space, (void *)page->address,
			    backing->pmem.paddr, PAGE_SIZE,
			    mapping_prot(page)) == HAL_OK)
				page->flags &= ~(VM_MAPPING_RECLAIM_UNMAPPED |
				    VM_MAPPING_RECLAIM_PROTECTED);
			continue;
		}
		if ((page->flags & VM_MAPPING_RECLAIM_PROTECTED) == 0)
			continue;
		if (hal_page_prot(page->vm->space, (void *)page->address,
		    PAGE_SIZE, mapping_prot(page)) == HAL_OK) {
			page->flags &= ~VM_MAPPING_RECLAIM_PROTECTED;
			continue;
		}
		/* A stale read-only PTE would livelock a later write fault. */
		if (hal_page_unmap(page->vm->space, (void *)page->address,
		    PAGE_SIZE) != HAL_OK)
			HAL_FATAL("VM reclaim protection rollback failed");
		page->flags |= VM_MAPPING_RECLAIM_UNMAPPED;
	}
	return error;
}

static void
rollback_backing_ptes(struct vm_private_page *backing)
{
	struct vm_page *page;

	for (page = backing->mappings; page != NULL; page = page->private_next) {
		if ((page->flags & VM_MAPPING_RECLAIM_UNMAPPED) != 0) {
			if (hal_page_map(page->vm->space, (void *)page->address,
			    backing->pmem.paddr, PAGE_SIZE,
			    mapping_prot(page)) == HAL_OK)
				page->flags &= ~(VM_MAPPING_RECLAIM_UNMAPPED |
				    VM_MAPPING_RECLAIM_PROTECTED);
			/* A failed remap remains lazy until the next fault. */
			continue;
		}
		if ((page->flags & VM_MAPPING_RECLAIM_PROTECTED) == 0)
			continue;
		if (hal_page_prot(page->vm->space, (void *)page->address,
		    PAGE_SIZE, mapping_prot(page)) == HAL_OK) {
			page->flags &= ~VM_MAPPING_RECLAIM_PROTECTED;
			continue;
		}
		if (hal_page_unmap(page->vm->space, (void *)page->address,
		    PAGE_SIZE) != HAL_OK)
			HAL_FATAL("VM reclaim PTE rollback failed");
		page->flags |= VM_MAPPING_RECLAIM_UNMAPPED;
	}
}

static int
swap_out_backing_owned(struct vm_private_page *backing)
{
	struct swap_backend *backend = swap_system_backend();
	struct hal_pmem memory;
	uint32_t slot;
	unsigned long irq;
	int slot_allocated = 0;
	int error;

	if (PAGE_SIZE != SWAP_PAGE_SIZE || backend == NULL)
		error = ENOSPC;
	else {
		error = swap_alloc_slot(backend, &slot);
		if (error == 0)
			slot_allocated = 1;
	}
	if (error == 0)
		error = swap_write_page(backend, slot,
		    (const void *)backing->pmem.vaddr);
	if (error != 0) {
		rollback_backing_ptes(backing);
		if (slot_allocated)
			swap_free_slot(backend, slot);
		vm_metadata_enter();
		mutex_lock(&reclaim_lock);
		unpin_backing_mappings(backing);
		if (error != ENOSPC)
			stats.io_errors++;
		mutex_unlock(&reclaim_lock);
		vm_metadata_leave();
		vm_private_page_io_release(backing);
		vm_private_page_put(backing);
		return error;
	}

	/* PTE shootdown and swap I/O are complete before the state is published. */
	memory = backing->pmem;
	(void)hal_pmem_free(&memory);
	vm_metadata_enter();
	mutex_lock(&reclaim_lock);
	irq = spin_lock_irqsave(&backing->state_lock);
	if ((backing->flags & (VM_PAGE_BUSY | VM_PAGE_RESIDENT)) !=
	    (VM_PAGE_BUSY | VM_PAGE_RESIDENT) ||
	    (backing->flags & VM_PAGE_SWAPPED) != 0)
		HAL_FATAL("VM swap-out state changed under I/O owner");
	memset(&backing->pmem, 0, sizeof(backing->pmem));
	backing->swap_slot = slot;
	backing->flags &= ~(VM_PAGE_RESIDENT | VM_PAGE_DIRTY);
	backing->flags |= VM_PAGE_SWAPPED;
	private_page_advance_locked(backing);
	spin_unlock_irqrestore(&backing->state_lock, irq);
	unpin_backing_mappings(backing);
	if (stats.resident)
		stats.resident--;
	stats.swapped++;
	stats.page_outs++;
	stats.reclaims++;
	mutex_unlock(&reclaim_lock);
	vm_metadata_leave();
	vm_private_page_io_release(backing);
	vm_private_page_put(backing);
	return 0;
}

static int
discard_file_backing_owned(struct vm_private_page *backing)
{
	struct vm_page *page, *next;
	struct hal_pmem memory;
	unsigned long irq;

	vm_metadata_enter();
	mutex_lock(&reclaim_lock);
	for (page = backing->mappings; page != NULL; page = next) {
		struct vm_page **link;
		struct vmspace *vm = page->vm;

		next = page->private_next;
		mutex_lock(&vm->lock);
		link = &page->region->pages;
		while (*link != NULL && *link != page)
			link = &(*link)->next;
		if (*link != page)
			HAL_FATAL("discarded VM page left its region");
		*link = page->next;
		if (page->region->hold_count == 0)
			HAL_FATAL("VM discard region hold underflow");
		page->region->hold_count--;
		if (++vm->generation == 0)
			vm->generation++;
		vmspace_fault_wake(vm);
		mutex_unlock(&vm->lock);
		page->private_page = NULL;
		page->private_next = NULL;
		if (refcount_put(&backing->refs))
			HAL_FATAL("VM discard lost I/O lifetime hold");
		vm_page_free_metadata(page);
		vmspace_put_deferred(vm);
	}
	backing->mappings = NULL;
	queue_remove(backing);
	irq = spin_lock_irqsave(&backing->state_lock);
	if (backing->mapping_count == 0 ||
	    (backing->flags & (VM_PAGE_BUSY | VM_PAGE_TRACKED)) !=
	    (VM_PAGE_BUSY | VM_PAGE_TRACKED))
		HAL_FATAL("invalid VM file-discard backing state");
	backing->mapping_count = 0;
	backing->flags &= ~(VM_PAGE_TRACKED | VM_PAGE_RESIDENT |
	    VM_PAGE_DIRTY);
	memory = backing->pmem;
	memset(&backing->pmem, 0, sizeof(backing->pmem));
	private_page_advance_locked(backing);
	spin_unlock_irqrestore(&backing->state_lock, irq);
	if (stats.resident)
		stats.resident--;
	stats.reclaims++;
	mutex_unlock(&reclaim_lock);
	vm_metadata_leave();
	(void)hal_pmem_free(&memory);
	vm_private_page_io_release(backing);
	vm_private_page_put(backing);
	return 0;
}

static int
reclaim_backing_owned(struct vm_private_page *backing, int file_candidate)
{
	uint32_t pte_flags;
	unsigned state_flags;
	unsigned long irq;
	int error;

	error = unmap_backing_ptes(backing, &pte_flags);
	if (error != 0) {
		vm_metadata_enter();
		mutex_lock(&reclaim_lock);
		unpin_backing_mappings(backing);
		stats.io_errors++;
		mutex_unlock(&reclaim_lock);
		vm_metadata_leave();
		vm_private_page_io_release(backing);
		vm_private_page_put(backing);
		return error;
	}
	irq = spin_lock_irqsave(&backing->state_lock);
	state_flags = backing->flags;
	spin_unlock_irqrestore(&backing->state_lock, irq);
	if (file_candidate && (state_flags & VM_PAGE_DIRTY) == 0 &&
	    (pte_flags & HAL_PAGE_DIRTY) == 0)
		return discard_file_backing_owned(backing);
	return swap_out_backing_owned(backing);
}

int vm_reclaim_private_one(struct vm_page *avoid)
{
	struct vm_private_page *selected = NULL;
	unsigned pass;
	int file_candidate = 0;

	vm_metadata_enter();
	mutex_lock(&reclaim_lock);
	for (pass = 0; pass < 2; pass++) {
		struct vm_private_page *backing;
		for (backing = page_queue; backing != NULL;
		     backing = backing->queue_next) {
			unsigned state_flags;
			unsigned long irq;
			uint32_t flags;

			irq = spin_lock_irqsave(&backing->state_lock);
			state_flags = backing->flags;
			spin_unlock_irqrestore(&backing->state_lock, irq);
			if ((state_flags & VM_PAGE_RESIDENT) == 0 ||
			    (state_flags & VM_PAGE_BUSY) != 0 ||
			    backing_wired_or_avoided(backing, avoid))
				continue;
			flags = backing_pte_flags(backing);
			if (pass == 0 && (flags & HAL_PAGE_ACCESSED))
				continue;
			file_candidate = backing->mappings != NULL &&
			    backing->mappings->region->backing == VM_BACKING_FILE;
			if (vm_private_page_io_try_acquire(backing) != 0)
				continue;
			pin_backing_mappings(backing);
			selected = backing;
			goto out;
		}
	}
out:
	mutex_unlock(&reclaim_lock);
	vm_metadata_leave();
	if (selected != NULL)
		return reclaim_backing_owned(selected, file_candidate);
	return EAGAIN;
}

int vm_reclaim_one(struct vm_page *avoid)
{
	int result = vm_reclaim_private_one(avoid);

	if (result == 0)
		return 0;
	if (result != EAGAIN)
		return result;
	/*
	 * Object writeback can sleep in VFS I/O.  It must not inherit either the
	 * private-page queue lock or the global cross-VM metadata lock from the
	 * candidate scan above.
	 */
	result = vm_object_reclaim_one();
	if (result != 0)
		return ENOMEM;
	vm_metadata_enter();
	mutex_lock(&reclaim_lock);
	stats.reclaims++;
	mutex_unlock(&reclaim_lock);
	vm_metadata_leave();
	return 0;
}

void vm_page_note_in(struct vm_page *page)
{
	if (page == NULL)
		return;
	vm_metadata_enter();
	mutex_lock(&reclaim_lock);
	if (stats.swapped) stats.swapped--;
	stats.resident++;
	stats.page_ins++;
	mutex_unlock(&reclaim_lock);
	vm_metadata_leave();
}

void vm_reclaim_note_fault(void)
{
	vm_metadata_enter();
	mutex_lock(&reclaim_lock);
	stats.faults++;
	mutex_unlock(&reclaim_lock);
	vm_metadata_leave();
}

void vm_reclaim_get_stats(struct vm_reclaim_stats *output)
{
	struct vm_private_page *backing;
	if (output == NULL)
		return;
	vm_metadata_enter();
	mutex_lock(&reclaim_lock);
	memcpy(output, &stats, sizeof(*output));
	output->anonymous_resident = output->file_resident = 0;
	output->wired = output->busy = output->dirty = output->clean = 0;
	for (backing = page_queue; backing != NULL; backing = backing->queue_next) {
		struct vm_page *page;
		unsigned long irq;
		unsigned state_flags;
		uint32_t flags = backing_pte_flags(backing);

		irq = spin_lock_irqsave(&backing->state_lock);
		state_flags = backing->flags;
		spin_unlock_irqrestore(&backing->state_lock, irq);
		if (state_flags & VM_PAGE_BUSY)
			output->busy++;
		for (page = backing->mappings; page != NULL;
		     page = page->private_next)
			if (page->wire_count != 0) { output->wired++; break; }
		if (!(state_flags & VM_PAGE_RESIDENT))
			continue;
		page = backing->mappings;
		if (page != NULL && page->region->backing == VM_BACKING_ANON)
			output->anonymous_resident++;
		else
			output->file_resident++;
		if ((state_flags & VM_PAGE_DIRTY) || (flags & HAL_PAGE_DIRTY))
			output->dirty++;
		else
			output->clean++;
	}
	output->file_resident += vm_object_page_count();
	output->resident += vm_object_page_count();
	mutex_unlock(&reclaim_lock);
	vm_metadata_leave();
}
