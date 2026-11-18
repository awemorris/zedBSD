/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The slab allocators of the per-page metadata of address spaces: the
 * mapping descriptors (struct vm_page) and the private page backings.
 * They work on fault paths that must not enter the general kernel heap.
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
 * One page of storage carved into mapping descriptors.
 *
 * Descriptors are handed out from a slab's free mask rather than allocated
 * one at a time, so a mapping can be created on a fault path that must not
 * enter the general allocator.  A slab is on the partial list while it has
 * a free slot; a full slab is on no list, because a record names its slab
 * by address alone: every slab is one page-aligned physical page.
 */
struct vm_page_slab {
	struct kern_pmem memory;
	struct vm_page_slab *next;
	struct vm_page_slab *prev;
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
	struct kern_pmem memory;
	struct vm_private_page_slab *next;
	struct vm_private_page_slab *prev;
	uint32_t free_mask;
	unsigned used;
	struct vm_private_page slots[VM_PRIVATE_PAGE_SLAB_SLOTS];
};

/*
 * Guards the mapping slab list below.
 *
 * It is a spinlock rather than a mutex because descriptors are taken on
 * fault paths that must not sleep.
 */
struct spinlock vm_page_slab_lock;

/*
 * The mapping slabs that have a free slot, most recently added or
 * reopened first.
 *
 * A slab leaves the list when its last slot is taken and returns to its
 * head when a slot is freed again; an emptied slab is returned to the
 * system.
 */
struct vm_page_slab *vm_page_slabs;

/*
 * Guards the private page slab list below.
 */
struct spinlock vm_private_page_slab_lock;

/*
 * The private page slabs that have a free slot, kept as the mapping
 * slabs above.
 */
struct vm_private_page_slab *vm_private_page_slabs;

static int alloc_vm_page(struct kern_pmem *memory);
static int alloc_vm_metadata_page(struct kern_pmem *memory);
static struct vm_page * vm_page_slab_take_locked(void);
static void vm_page_slab_link_locked(struct vm_page_slab *slab);
static void vm_page_slab_unlink_locked(struct vm_page_slab *slab);
static struct vm_private_page * vm_private_page_slab_take_locked(void);
static void vm_private_page_slab_link_locked(struct vm_private_page_slab *slab);
static void vm_private_page_slab_unlink_locked(struct vm_private_page_slab *slab);
static struct vm_private_page * vm_private_page_alloc_metadata(void);

/*
 * Static asserts.
 */
_Static_assert(sizeof(struct vm_page_slab) <= PAGE_SIZE, "VM page metadata slab exceeds one physical page");
_Static_assert(VM_PRIVATE_PAGE_SLAB_SLOTS < 32U, "VM private metadata slab bitmap must fit uint32_t");
_Static_assert(sizeof(struct vm_private_page_slab) <= PAGE_SIZE, "VM private metadata slab exceeds one physical page");

/*
 * Allocates a zeroed page-metadata record from the slab pool, growing
 * the pool by one physical page when it is empty.
 */
struct vm_page *
vm_page_alloc_metadata(
	void)
{
	struct kern_pmem memory;
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
	fresh = hal_pmem_to_kernel(memory.paddr);
	kern_memset(fresh, 0, PAGE_SIZE);
	fresh->memory = memory;
	fresh->free_mask = UINT32_MAX;

	irq = spin_lock_irqsave(&vm_page_slab_lock);

	/* Adds the freshly built slab only when no slot became free meanwhile. */
	page = vm_page_slab_take_locked();
	if (page == NULL) {
		vm_page_slab_link_locked(fresh);
		page = vm_page_slab_take_locked();
		fresh = NULL;
	}

	spin_unlock_irqrestore(&vm_page_slab_lock, irq);

	if (fresh != NULL)
		(void)hal_pmem_free(&memory.paddr, memory.size);

	return page;
}

/*
 * Returns a page-metadata record to its slab, freeing an emptied slab.
 */
void
vm_page_free_metadata(
	struct vm_page *page)
{
	struct vm_page_slab *slab;
	struct kern_pmem released;
	uintptr_t address;
	unsigned long irq;
	int release;
	int was_full;
	int error;
	void *owner;
	uintptr_t first;
	uintptr_t end;
	unsigned slot;

	release = 0;

	if (page == NULL)
		return;
	address = (uintptr_t)page;

	/* Names the slab from the record's address: the page that holds it. */
	slab = (struct vm_page_slab *)(address & ~((uintptr_t)PAGE_SIZE - 1U));
	first = (uintptr_t)&slab->slots[0];
	end = (uintptr_t)&slab->slots[VM_PAGE_SLAB_SLOTS];

	/* Checks the record under the slab lock. */
	irq = spin_lock_irqsave(&vm_page_slab_lock);

	/* Rejects a record that no slab of this pool holds. */
	owner = hal_pmem_to_kernel(slab->memory.paddr);
	if (owner != (void *)slab ||
	    address < first ||
	    address >= end ||
	    (address - first) % sizeof(struct vm_page) != 0)
		HAL_FATAL("foreign VM page metadata free");

	/* Names the slot inside the slab. */
	slot = (unsigned)((address - first) / sizeof(struct vm_page));
	if ((slab->free_mask & (1U << slot)) != 0 || slab->used == 0)
		HAL_FATAL("invalid VM page metadata free");

	/* Clears the slot. */
	was_full = slab->free_mask == 0;
	kern_memset(page, 0, sizeof(*page));
	slab->free_mask |= 1U << slot;
	slab->used--;

	/* Releases a slab left empty, or reopens a full one to the takers. */
	if (slab->used == 0) {
		if (!was_full)
			vm_page_slab_unlink_locked(slab);
		released = slab->memory;
		release = 1;
	} else if (was_full) {
		vm_page_slab_link_locked(slab);
	}

	spin_unlock_irqrestore(&vm_page_slab_lock, irq);

	/* Returns an emptied slab's page to the system. */
	if (release) {
		error = hal_pmem_free(&released.paddr, released.size);
		if (error != HAL_OK)
			HAL_FATAL("VM page metadata slab free failed");
	}
}

/*
 * Returns a private-backing record to its slab, freeing an emptied slab.
 */
void
vm_private_page_free_metadata(
	struct vm_private_page *backing)
{
	struct vm_private_page_slab *slab;
	struct kern_pmem released;
	uintptr_t address;
	unsigned long irq;
	int release;
	int was_full;
	int error;
	void *owner;
	uintptr_t first;
	uintptr_t end;
	unsigned slot;

	release = 0;

	if (backing == NULL)
		return;

	address = (uintptr_t)backing;

	/* Names the slab from the record's address: the page that holds it. */
	slab = (struct vm_private_page_slab *)(address & ~((uintptr_t)PAGE_SIZE - 1U));
	first = (uintptr_t)&slab->slots[0];
	end = (uintptr_t)&slab->slots[VM_PRIVATE_PAGE_SLAB_SLOTS];

	/* Checks the record under the slab lock. */
	irq = spin_lock_irqsave(&vm_private_page_slab_lock);

	/* Rejects a record that no slab of this pool holds. */
	owner = hal_pmem_to_kernel(slab->memory.paddr);
	if (owner != (void *)slab ||
	    address < first ||
	    address >= end ||
	    (address - first) % sizeof(struct vm_private_page) != 0)
		HAL_FATAL("foreign VM private metadata free");

	/* Names the slot inside the slab. */
	slot = (unsigned)((address - first) / sizeof(struct vm_private_page));
	if ((slab->free_mask & (1U << slot)) != 0 || slab->used == 0)
		HAL_FATAL("invalid VM private metadata free");

	/* Clears the slot. */
	was_full = slab->free_mask == 0;
	kern_memset(backing, 0, sizeof(*backing));
	slab->free_mask |= 1U << slot;
	slab->used--;

	/* Releases a slab left empty, or reopens a full one to the takers. */
	if (slab->used == 0) {
		if (!was_full)
			vm_private_page_slab_unlink_locked(slab);
		released = slab->memory;
		release = 1;
	} else if (was_full) {
		vm_private_page_slab_link_locked(slab);
	}

	spin_unlock_irqrestore(&vm_private_page_slab_lock, irq);

	/* Returns an emptied slab's page to the system. */
	if (release) {
		error = hal_pmem_free(&released.paddr, released.size);
		if (error != HAL_OK)
			HAL_FATAL("VM private metadata slab free failed");
	}
}

/*
 * Allocates and initializes a private backing without a frame.
 */
struct vm_private_page *
vmspace_private_page_alloc(
	void)
{
	struct vm_private_page *backing;

	backing = vm_private_page_alloc_metadata();
	if (backing == NULL)
		return NULL;
	vm_private_page_init(backing);
	return backing;
}

/*
 * Attaches a fresh private backing to its first mapping.
 */
void
vmspace_private_page_attach_new(
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

/* Allocates one physical page for user memory. */
static int
alloc_vm_page(
	struct kern_pmem *memory)
{
	int error;

	/* Reports the failure. */
	error = vm_private_page_alloc(memory);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Allocates a metadata slab page, reclaiming once when not under the metadata lock. */
static int
alloc_vm_metadata_page(
	struct kern_pmem *memory)
{
	int error;
	int owned;

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
	 * not allocate mapping metadata, so reclaiming cannot recurse through
	 * this helper.
	 */
	error = alloc_vm_page(memory);
	if (error == HAL_OK)
		return HAL_OK;

	/* Reports the shortage to a caller inside the metadata lock. */
	owned = vm_metadata_owned();
	if (owned)
		return error;

	/* Reclaims until a frame is free, as a page fault does. */
	error = vm_reclaim_frame(memory, NULL);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Takes a zeroed page-metadata slot from the partial list; the caller holds the slab lock. */
static struct vm_page *
vm_page_slab_take_locked(
	void)
{
	struct vm_page_slab *slab;
	unsigned slot;

	/* Reports that every slab is full. */
	slab = vm_page_slabs;
	if (slab == NULL)
		return NULL;
	if (slab->free_mask == 0)
		HAL_FATAL("full VM page slab on the partial list");

	/* Finds the first free slot of the first partial slab. */
	for (slot = 0; slot < VM_PAGE_SLAB_SLOTS; slot++) {
		if ((slab->free_mask & (1U << slot)) != 0)
			break;
	}

	/* Takes the slot the free mask named. */
	if (slot == VM_PAGE_SLAB_SLOTS)
		HAL_FATAL("invalid VM page slab bitmap");
	slab->free_mask &= ~(1U << slot);
	slab->used++;

	/* Leaves the list when the slab is now full. */
	if (slab->free_mask == 0)
		vm_page_slab_unlink_locked(slab);

	/* Hands the slot back cleared. */
	kern_memset(&slab->slots[slot], 0, sizeof(slab->slots[slot]));
	return &slab->slots[slot];
}

/* Puts a slab at the head of the partial list; the caller holds the slab lock. */
static void
vm_page_slab_link_locked(
	struct vm_page_slab *slab)
{
	/* Links the slab before the current head. */
	slab->prev = NULL;
	slab->next = vm_page_slabs;
	if (vm_page_slabs != NULL)
		vm_page_slabs->prev = slab;
	vm_page_slabs = slab;
}

/* Takes a slab off the partial list; the caller holds the slab lock. */
static void
vm_page_slab_unlink_locked(
	struct vm_page_slab *slab)
{
	/* Joins the neighbours around the slab. */
	if (slab->prev != NULL)
		slab->prev->next = slab->next;
	else
		vm_page_slabs = slab->next;
	if (slab->next != NULL)
		slab->next->prev = slab->prev;

	/* Leaves no stale link behind. */
	slab->prev = NULL;
	slab->next = NULL;
}

/* Takes a zeroed private-backing slot from the partial list; the caller holds the slab lock. */
static struct vm_private_page *
vm_private_page_slab_take_locked(
	void)
{
	struct vm_private_page_slab *slab;
	unsigned slot;

	/* Reports that every slab is full. */
	slab = vm_private_page_slabs;
	if (slab == NULL)
		return NULL;
	if (slab->free_mask == 0)
		HAL_FATAL("full VM private metadata slab on the partial list");

	/* Finds the first free slot of the first partial slab. */
	for (slot = 0; slot < VM_PRIVATE_PAGE_SLAB_SLOTS; slot++) {
		if ((slab->free_mask & (1U << slot)) != 0)
			break;
	}

	/* Takes the slot the free mask named. */
	if (slot == VM_PRIVATE_PAGE_SLAB_SLOTS)
		HAL_FATAL("invalid VM private metadata slab bitmap");
	slab->free_mask &= ~(1U << slot);
	slab->used++;

	/* Leaves the list when the slab is now full. */
	if (slab->free_mask == 0)
		vm_private_page_slab_unlink_locked(slab);

	/* Hands the slot back cleared. */
	kern_memset(&slab->slots[slot], 0, sizeof(slab->slots[slot]));
	return &slab->slots[slot];
}

/* Puts a slab at the head of the partial list; the caller holds the slab lock. */
static void
vm_private_page_slab_link_locked(
	struct vm_private_page_slab *slab)
{
	/* Links the slab before the current head. */
	slab->prev = NULL;
	slab->next = vm_private_page_slabs;
	if (vm_private_page_slabs != NULL)
		vm_private_page_slabs->prev = slab;
	vm_private_page_slabs = slab;
}

/* Takes a slab off the partial list; the caller holds the slab lock. */
static void
vm_private_page_slab_unlink_locked(
	struct vm_private_page_slab *slab)
{
	/* Joins the neighbours around the slab. */
	if (slab->prev != NULL)
		slab->prev->next = slab->next;
	else
		vm_private_page_slabs = slab->next;
	if (slab->next != NULL)
		slab->next->prev = slab->prev;

	/* Leaves no stale link behind. */
	slab->prev = NULL;
	slab->next = NULL;
}

/* Allocates a zeroed private-backing record, growing the slab pool when empty. */
static struct vm_private_page *
vm_private_page_alloc_metadata(
	void)
{
	struct kern_pmem memory;
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
	fresh = hal_pmem_to_kernel(memory.paddr);
	kern_memset(fresh, 0, PAGE_SIZE);
	fresh->memory = memory;
	fresh->free_mask = VM_PRIVATE_PAGE_SLAB_FREE_MASK;
	irq = spin_lock_irqsave(&vm_private_page_slab_lock);

	backing = vm_private_page_slab_take_locked();
	if (backing == NULL) {
		vm_private_page_slab_link_locked(fresh);
		backing = vm_private_page_slab_take_locked();
		fresh = NULL;
	}

	spin_unlock_irqrestore(&vm_private_page_slab_lock, irq);

	if (fresh != NULL && hal_pmem_free(&memory.paddr, memory.size) != HAL_OK)
		HAL_FATAL("unused VM private metadata slab free failed");
	return backing;
}
