/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Where object pages live: the descriptor slabs, page storage, the dirty
 * and busy tests, the AVL index of an object's pages and the clock of the
 * resident file pages.
 */

#include "kern/vm-object.h"
#include "kern/cred.h"
#include "kern/file.h"
#include "kern/kmem.h"
#include "kern/page.h"
#include "kern/vm-reclaim.h"
#include "kern/vm-lock.h"
#include "kern/vmspace.h"
#include "kern/vm-commit.h"
#include "kern/io-pool.h"
#include "kern/disk.h"
#include "kern/cache-memory.h"
#include "kern/writeback.h"
#include <uapi/errno.h>
#include <uapi/mman.h>
#include "kern/lock.h"
#include "kern/swap.h"
#include <hal/hal.h>
#include <kern/pmem.h>
#include <limits.h>
#include "kern/sched.h"
#include "kern/clock.h"
#include "kern/thread.h"
#include "kern/process.h"
#include "kern/signal.h"
#include "kern/klog.h"
#include <uapi/signal.h>
#include <kern/kcrt.h>
#include "reclaim-internal.h"
#include "object-internal.h"

/*
 * One page of storage carved into page descriptors.
 *
 * Descriptors are handed out from a slab's free list rather than allocated
 * one at a time, and a slab that still has a free descriptor stays on the
 * global list so the next allocation finds it without a search.
 */
struct vm_page_slab {
	struct kern_pmem memory;
	struct vm_page_slab *next;
	struct vm_page_slab *previous;
	struct vm_object_page *free_pages;
	unsigned used;
};

/*
 * The slabs that back the object page descriptors.
 */
static struct vm_page_slab *page_slabs VM_OBJECT_DATA;

/*
 * The spin word that guards the slab list above.
 */
static atomic_uint_t page_slab_lock VM_OBJECT_DATA;

/*
 * The clock of the resident pages of every file-backed object.
 *
 * A page joins it when it is published in its object's index and leaves it
 * when it is taken out of the index, so a linked page's descriptor and its
 * owner stay allocated for as long as it is linked.  Pages of anonymous
 * shared objects are not file data and stay off it.  file_clock_lock guards
 * the ring and the hand; it is taken inside an object lock and nothing is
 * taken inside it.
 */
static struct page_clock file_clock VM_OBJECT_DATA;

/*
 * The spin word that guards file_clock, taken with interrupts disabled
 * because pages are published from completion paths under object locks.
 */
static atomic_uint_t file_clock_lock VM_OBJECT_DATA;

/*
 * Optional collaborators.
 *
 * Each of these lives in a translation unit that some platforms leave out of
 * the kernel, so the reference is weak and every call site tests it first.
 */
extern void writeback_budget_clean(struct writeback_budget *, size_t) __attribute__((weak));
extern bool hal_irq_disable(void) __attribute__((weak));
extern void hal_irq_enable(void) __attribute__((weak));
extern size_t cache_memory_reclaim(size_t) __attribute__((weak));
extern int cache_memory_reserve(enum cache_memory_kind, size_t, int) __attribute__((weak));
extern void cache_memory_commit(enum cache_memory_kind, size_t) __attribute__((weak));
extern void cache_memory_cancel(enum cache_memory_kind, size_t) __attribute__((weak));
extern void cache_memory_release(enum cache_memory_kind, size_t) __attribute__((weak));

static int alloc_vm_page(struct kern_pmem *memory);
static int object_memory_reserve(enum cache_memory_kind kind, int optional);
static bool object_slab_lock(void);
static void object_slab_unlock(bool enabled);
static bool clock_lock(void);
static void clock_unlock(bool enabled);
static struct vm_object_page *clock_page(struct page_clock_node *node);
static struct vm_page_slab *object_slab_alloc(int optional);
static struct vm_object_page *object_descriptor_alloc(int optional);
static void object_descriptor_free(struct vm_object_page *page);
static unsigned page_index_height(const struct vm_object_page *page);
static void page_index_update(struct vm_object_page *page);
static struct vm_object_page *page_index_rotate_left(struct vm_object_page *page);
static struct vm_object_page *page_index_rotate_right(struct vm_object_page *page);
static struct vm_object_page *page_index_balance(struct vm_object_page *page);
static struct vm_object_page *page_index_insert(struct vm_object_page *root, struct vm_object_page *page);
static struct vm_object_page *page_index_extract_min(struct vm_object_page *root, struct vm_object_page **minimum);
static struct vm_object_page *page_index_remove(struct vm_object_page *root, struct vm_object_page *page);

/*
 * Finds the cached page at an offset.
 */
struct vm_object_page *
vm_object_find_page(
	struct vm_object *object,
	off_t offset)
{
	struct vm_object_page *page;

	/* Traverses the balanced index without scanning unrelated cache pages. */
	page = object->page_index;
	while (page != NULL) {
		if (page->offset == offset)
			return page;
		if (offset < page->offset)
			page = page->index_left;
		else
			page = page->index_right;
	}

	return NULL;
}

/*
 * Finds the first cached page at or after an offset; the caller holds the
 * object lock.
 */
struct vm_object_page *
vm_object_page_at_or_after(
	const struct vm_object *object,
	uint64_t offset)
{
	struct vm_object_page *node;
	struct vm_object_page *result;

	/* Descends the balanced index, keeping the lowest page not below the offset. */
	result = NULL;
	node = object->page_index;
	while (node != NULL) {
		if ((uint64_t)node->offset >= offset) {
			result = node;
			node = node->index_left;
		} else {
			node = node->index_right;
		}
	}

	/* Reports the page, or none past the last one. */
	return result;
}

/*
 * Finds the resident page that follows a page in offset order; the caller
 * holds the object lock, and the page may already have left the index.
 */
struct vm_object_page *
vm_object_page_after(
	const struct vm_object *object,
	const struct vm_object_page *page)
{
	struct vm_object_page *next;

	/* Searches from the byte after the page, which is not itself in the index any more or is passed over. */
	next = vm_object_page_at_or_after(object, (uint64_t)page->offset + 1U);

	/* Reports the next page, or none past the last one. */
	return next;
}

/*
 * Records a hit on a resident page, so that the clock's hand passes over it
 * once before it is offered for reclaim.
 */
void
vm_object_page_touch(
	struct vm_object_page *page)
{
	page_clock_touch(&page->clock);
}

/*
 * Moves the clock's hand to the next resident file page nobody used since
 * the hand last passed it, looking at no more than *budget pages (and
 * taking those from it), and reports the page with the object and the
 * offset it had then.
 *
 * The caller holds the registry lock, so that an owner it finds registered
 * stays allocated until that lock is dropped; owner is NULL for a page whose
 * object is no longer registered.  The page itself is only a name: once this
 * returns, the caller locks the owner and looks the offset up again before it
 * trusts the page.  Reports ENOENT when the budget ran out or the clock is
 * empty.
 */
int
vm_object_clock_offer_locked(
	size_t *budget,
	struct vm_object_page **page,
	struct vm_object **owner,
	off_t *offset)
{
	struct page_clock_node *node;
	struct vm_object_page *candidate;
	bool enabled;

	/* Sweeps under the clock's lock, which keeps the candidate and its owner allocated. */
	enabled = clock_lock();

	node = page_clock_sweep(&file_clock, budget);
	candidate = NULL;
	if (node != NULL) {
		candidate = clock_page(node);
		*page = candidate;
		*offset = candidate->offset;

		/*
		 * An unregistered owner may be in its last teardown; only a
		 * registered one is kept alive by the caller's registry lock.
		 */
		*owner = NULL;
		if ((candidate->owner->flags & VM_OBJECT_REGISTERED) != 0)
			*owner = candidate->owner;
	}

	clock_unlock(enabled);

	/* Reports a sweep that found nothing to offer. */
	if (candidate == NULL)
		return ENOENT;

	/* Succeeded: the caller has a page to look up again under its owner's lock. */
	return 0;
}

/*
 * Reports how many resident file pages the clock holds.
 */
size_t
vm_object_clock_count(
	void)
{
	size_t count;
	bool enabled;

	/* Reads the count under the lock that keeps it consistent with the ring. */
	enabled = clock_lock();

	count = file_clock.count;

	clock_unlock(enabled);

	/* Reports the number of linked pages. */
	return count;
}

/*
 * Unlinks a page from a list; the caller holds the object lock.
 */
int
vm_object_unlink_page_locked(
	struct vm_object_page **head,
	struct vm_object_page *page)
{
	struct vm_object_page **link;

	for (link = head; *link != NULL; link = &(*link)->next) {
		if (*link != page)
			continue;
		*link = page->next;
		page->next = NULL;
		return 1;
	}

	return 0;
}

/*
 * Tests whether a page overlaps a byte range.
 */
int
vm_object_page_overlaps(
	const struct vm_object_page *page,
	uint64_t start,
	uint64_t end)
{
	uint64_t page_start;
	uint64_t page_end;

	/* A page without an offset covers nothing. */
	if (page->offset < 0)
		return 0;

	/* Spans the page and tests it against both ends of the range. */
	page_start = (uint64_t)page->offset;
	page_end = page_start + PAGE_SIZE;
	if (page_end <= start)
		return 0;
	if (page_start >= end)
		return 0;

	/* Reports an overlap. */
	return 1;
}

/*
 * Tests indexed dirty ownership; the caller holds the object lock.
 */
int
vm_object_has_dirty_pages_locked(
	struct vm_object *object)
{
	return object->dirty_pages != NULL;
}

/*
 * Tests whether any page or orphan is busy or held; the caller holds the
 * object lock.
 */
int
vm_object_has_busy_pages_locked(
	const struct vm_object *object)
{
	const struct vm_object_page *page;

	/* Looks for a live page that is under I/O or held, in offset order. */
	for (page = vm_object_page_at_or_after(object, 0);
	     page != NULL;
	     page = vm_object_page_after(object, page)) {
		/* A page another transaction owns or is writing out is busy. */
		if (page->flags & (VM_OBJECT_PAGE_BUSY | VM_OBJECT_PAGE_WRITEBACK))
			return 1;

		/* So is a page some reader is still holding. */
		if (page->hold_count != 0)
			return 1;
	}

	/* Looks for an orphan that is still finishing its writeback. */
	for (page = object->orphan_pages; page != NULL; page = page->next) {
		/*
		 * An orphan is busy on the same terms: owned by another
		 * transaction, still being written out, or still held.
		 */
		if ((page->flags & (VM_OBJECT_PAGE_BUSY | VM_OBJECT_PAGE_WRITEBACK)) != 0 ||
		    page->hold_count != 0)
			return 1;
	}

	/* Reports an object whose pages are all idle. */
	return 0;
}

/*
 * Tests whether any page or orphan is busy or held.
 */
int
vm_object_has_busy_pages(
	struct vm_object *object)
{
	unsigned long irq;
	int busy;

	irq = spin_lock_irqsave(&object->lock);

	busy = vm_object_has_busy_pages_locked(object);

	spin_unlock_irqrestore(&object->lock, irq);

	return busy;
}

/*
 * Clears a page's dirty state; the caller holds the object lock.
 */
void
vm_object_clear_page_dirty_locked(
	struct vm_object_page *page)
{
	/* Takes the page out of the object's dirty index. */
	vm_object_page_dirty_unlink(page);

	/* Gives the delayed write credit back to the budget that granted it. */
	if (page->writeback_budget != NULL) {
		if (writeback_budget_clean == NULL)
			HAL_FATAL("missing writeback credit owner");
		writeback_budget_clean(page->writeback_budget, PAGE_SIZE);
		page->writeback_budget = NULL;
	}

	/* Records the page as clean. */
	page->flags &= ~VM_OBJECT_PAGE_DIRTY;
	page->dirty_generation = 0;
}

/*
 * Frees an unmapped, unheld page and its memory.
 */
void
vm_object_free_page(
	struct vm_object_page *page)
{
	/* Region teardown must remove every mapping before the final object ref. */
	if (page->mapping_count != 0 || page->hold_count != 0)
		HAL_FATAL("destroying mapped VM object page");

	if (page->dirty_linked)
		HAL_FATAL("destroying indexed dirty VM object page");

	/* A committed orphan/anonymous discard also retires optional ownership. */
	if (page->writeback_budget != NULL)
		vm_object_clear_page_dirty_locked(page);

	vm_object_release_page_storage(page);

	if (atomic_load_acquire(&object_pages) == 0)
		HAL_FATAL("VM object page counter underflow");

	(void)atomic_fetch_add_relaxed(&object_pages, (unsigned)-1);
}

/*
 * Allocates a fully charged private frame before any page index publication.
 */
struct vm_object_page *
vm_object_alloc_page(
	struct vm_object *object,
	off_t offset,
	int optional)
{
	struct vm_object_page *page;

	/*
	 * Reserves optional cache growth or records mandatory mapping
	 * commitment.
	 */
	if (object_memory_reserve(CACHE_MEMORY_FILE_DATA, optional) != 0)
		return NULL;

	/* Returns the reservation when no descriptor is free. */
	page = object_descriptor_alloc(optional);
	if (page == NULL) {
		if (cache_memory_cancel != NULL)
			cache_memory_cancel(CACHE_MEMORY_FILE_DATA, PAGE_SIZE);
		return NULL;
	}

	/* Returns the descriptor and the reservation when no frame is free. */
	if (alloc_vm_page(&page->pmem) != HAL_OK) {
		object_descriptor_free(page);
		if (cache_memory_cancel != NULL)
			cache_memory_cancel(CACHE_MEMORY_FILE_DATA, PAGE_SIZE);
		return NULL;
	}

	if (page->pmem.size != PAGE_SIZE || hal_pmem_to_kernel(page->pmem.paddr) == NULL)
		HAL_FATAL("unexpected VM object frame allocation");

	/*
	 * Publishes resident ownership while the initialized descriptor stays
	 * private.
	 */
	page->owner = object;
	page->offset = offset;
	page->flags = VM_OBJECT_PAGE_BUSY | VM_OBJECT_PAGE_FILL;

	if (cache_memory_commit != NULL)
		cache_memory_commit(CACHE_MEMORY_FILE_DATA, PAGE_SIZE);

	return page;
}

/*
 * Frees private or detached frame storage without changing publication counts.
 */
void
vm_object_release_page_storage(
	struct vm_object_page *page)
{
	/*
	 * Ends optional attribution before the descriptor or its storage can be
	 * reused.
	 */
	vm_object_prefetch_retire(page);

	/* Returns data credits only after the physical frame has retired. */
	if (hal_pmem_free(&page->pmem.paddr, page->pmem.size) != HAL_OK)
		HAL_FATAL("VM object frame retirement failed");

	if (cache_memory_release != NULL)
		cache_memory_release(CACHE_MEMORY_FILE_DATA, PAGE_SIZE);

	object_descriptor_free(page);
}

/*
 * Publishes a descriptor under the already held object lock.
 */
void
vm_object_page_index_insert(
	struct vm_object *object,
	struct vm_object_page *page)
{
	bool enabled;

	/* Starts with an independent leaf before linking it into the object. */
	page->index_left = NULL;
	page->index_right = NULL;
	page->index_height = 1;
	object->page_index = page_index_insert(object->page_index, page);

	/* A file page becomes a candidate of the clock, the last the hand reaches. */
	if ((object->flags & VM_OBJECT_ANONYMOUS) == 0) {
		enabled = clock_lock();

		page_clock_link(&file_clock, &page->clock);

		clock_unlock(enabled);
	}
}

/*
 * Detaches a descriptor before it becomes an orphan or is retired.
 */
void
vm_object_page_index_remove(
	struct vm_object *object,
	struct vm_object_page *page)
{
	bool enabled;

	/*
	 * Removes the key and clears links before another owner can reuse them.
	 */
	object->page_index = page_index_remove(object->page_index, page);
	page->index_left = NULL;
	page->index_right = NULL;
	page->index_height = 0;

	/* Takes the page off the clock, which then no longer names it. */
	enabled = clock_lock();

	page_clock_unlink(&file_clock, &page->clock);

	clock_unlock(enabled);
}

/* Allocates one page of physical memory. */
static int
alloc_vm_page(
	struct kern_pmem *memory)
{
	int error;

	/* Asks the HAL for one page of ordinary memory. */
	kern_memset(memory, 0, sizeof(*memory));
	error = vm_private_page_alloc(memory);

	/* Reports why the allocation failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Tries one bounded clean-pressure pass before refusing optional cache growth.
 */
static int
object_memory_reserve(
	enum cache_memory_kind kind,
	int optional)
{
	int error;

	/*
	 * Keeps standalone VM fixtures usable without installing a budget
	 * owner.
	 */
	if (cache_memory_reserve == NULL)
		return 0;

	/* Asks the budget owner for one page. */
	error = cache_memory_reserve(kind, PAGE_SIZE, optional);
	if (error == 0 || !optional || cache_memory_reclaim == NULL)
		return error;

	/*
	 * Reclaims only independent idle ownership; the current read stays
	 * active.
	 */
	if (cache_memory_reclaim(PAGE_SIZE) == 0)
		return error;

	/* Reports the failure. */
	error = cache_memory_reserve(kind, PAGE_SIZE, optional);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Serializes descriptor ownership without sleeping or taking another lock. */
static bool
object_slab_lock(
	void)
{
	unsigned expected;
	bool enabled;

	/*
	 * Preserves interrupt state while acquiring the short metadata guard.
	 */
	enabled = hal_irq_disable != NULL ? hal_irq_disable() : false;
	expected = 0;
	while (!atomic_compare_exchange(&page_slab_lock, &expected, 1)) {
		expected = 0;
		hal_compiler_barrier();
	}

	return enabled;
}

/* Releases metadata ownership before restoring interrupts. */
static void
object_slab_unlock(
	bool enabled)
{
	atomic_store_release(&page_slab_lock, 0);

	if (enabled && hal_irq_enable != NULL)
		hal_irq_enable();
}

/* Takes the clock's spin word with interrupts disabled. */
static bool
clock_lock(
	void)
{
	unsigned expected;
	bool enabled;

	/* Spins with interrupts off, as a completion may publish a page under an object lock. */
	enabled = hal_irq_disable != NULL ? hal_irq_disable() : false;
	expected = 0;
	while (!atomic_compare_exchange(&file_clock_lock, &expected, 1)) {
		expected = 0;
		hal_compiler_barrier();
	}

	/* Reports the interrupt state the unlock restores. */
	return enabled;
}

/* Releases the clock's spin word before restoring interrupts. */
static void
clock_unlock(
	bool enabled)
{
	atomic_store_release(&file_clock_lock, 0);

	if (enabled && hal_irq_enable != NULL)
		hal_irq_enable();
}

/* Finds the page descriptor a clock node is embedded in. */
static struct vm_object_page *
clock_page(
	struct page_clock_node *node)
{
	struct vm_object_page *page;

	/* Steps back from the node to the start of its descriptor. */
	page = (struct vm_object_page *)(void *)((char *)node - offsetof(struct vm_object_page, clock));

	/* Reports the descriptor. */
	return page;
}

/* Constructs a private metadata slab outside all VM and slab locks. */
static struct vm_page_slab *
object_slab_alloc(
	int optional)
{
	struct kern_pmem memory;
	struct vm_page_slab *slab;
	struct vm_object_page *page;
	size_t header;
	size_t offset;

	/*
	 * Reserves the entire physical page, including unused descriptor
	 * capacity.
	 */
	if (object_memory_reserve(CACHE_MEMORY_FILE_META, optional) != 0)
		return NULL;

	kern_memset(&memory, 0, sizeof(memory));

	if (alloc_vm_page(&memory) != HAL_OK) {
		if (cache_memory_cancel != NULL)
			cache_memory_cancel(CACHE_MEMORY_FILE_META, PAGE_SIZE);
		return NULL;
	}

	if (memory.size != PAGE_SIZE || hal_pmem_to_kernel(memory.paddr) == NULL)
		HAL_FATAL("unexpected VM descriptor slab allocation");

	/*
	 * Formats aligned descriptors without consuming the fixed kernel heap.
	 */
	slab = hal_pmem_to_kernel(memory.paddr);
	kern_memset(slab, 0, PAGE_SIZE);
	slab->memory = memory;

	/*
	 * Carves the page after the slab header into descriptors and puts
	 * every one of them on the slab's free list.  The header size is
	 * rounded up so the first descriptor is correctly aligned, and the
	 * walk stops before the last descriptor that would not fit whole.
	 */
	header = (sizeof(*slab) + __alignof__(struct vm_object_page) - 1U) & ~(size_t)(__alignof__(struct vm_object_page) - 1U);
	for (offset = header;
	     offset + sizeof(*page) <= PAGE_SIZE;
	     offset += sizeof(*page)) {
		page = (struct vm_object_page *)((char *)slab + offset);
		page->metadata_slab = slab;
		page->next = slab->free_pages;
		slab->free_pages = page;
	}

	if (slab->free_pages == NULL)
		HAL_FATAL("VM page descriptor exceeds metadata slab");

	if (cache_memory_commit != NULL)
		cache_memory_commit(CACHE_MEMORY_FILE_META, PAGE_SIZE);

	return slab;
}

/* Borrows a descriptor, adding one private slab only when no slot is free. */
static struct vm_object_page *
object_descriptor_alloc(
	int optional)
{
	struct vm_page_slab *slab;
	struct vm_page_slab *prepared;
	struct vm_object_page *page;
	bool enabled;

	/*
	 * Finds existing capacity without entering an allocator under the
	 * guard.
	 */
	enabled = object_slab_lock();
	slab = page_slabs;
	if (slab == NULL) {
		object_slab_unlock(enabled);

		prepared = object_slab_alloc(optional);
		if (prepared == NULL)
			return NULL;

		enabled = object_slab_lock();

		prepared->next = page_slabs;

		if (page_slabs != NULL)
			page_slabs->previous = prepared;

		page_slabs = prepared;
		slab = prepared;
	}

	/*
	 * Holds the slab through the descriptor's complete private/resident
	 * lifetime.
	 */
	page = slab->free_pages;
	slab->free_pages = page->next;
	slab->used++;

	/* Takes a slab that has run out off the head of the list. */
	if (slab->free_pages == NULL) {
		page_slabs = slab->next;
		if (page_slabs != NULL)
			page_slabs->previous = NULL;
		slab->next = NULL;
	}

	object_slab_unlock(enabled);

	kern_memset(page, 0, sizeof(*page));
	page->metadata_slab = slab;

	return page;
}

/*
 * Returns one descriptor and retires its slab only after the last owner leaves.
 */
static void
object_descriptor_free(
	struct vm_object_page *page)
{
	struct vm_page_slab *slab;
	struct kern_pmem memory;
	bool enabled;

	/*
	 * Removes empty slabs from allocation visibility before physical
	 * retirement.
	 */
	slab = page->metadata_slab;
	kern_memset(&memory, 0, sizeof(memory));

	/* A descriptor the clock still names would be swept after its reuse. */
	if (page->clock.linked)
		HAL_FATAL("VM page descriptor freed on the clock");

	enabled = object_slab_lock();
	if (slab == NULL || slab->used == 0)
		HAL_FATAL("VM page descriptor ownership underflow");

	/* A slab that had no free descriptor becomes allocatable again. */
	if (slab->free_pages == NULL) {
		slab->previous = NULL;
		slab->next = page_slabs;
		if (page_slabs != NULL)
			page_slabs->previous = slab;
		page_slabs = slab;
	}

	/* Returns the descriptor to its slab. */
	page->next = slab->free_pages;
	slab->free_pages = page;
	slab->used--;

	/* An emptied slab leaves allocation and gives its memory back. */
	if (slab->used == 0) {
		if (slab->previous != NULL)
			slab->previous->next = slab->next;
		else
			page_slabs = slab->next;
		if (slab->next != NULL)
			slab->next->previous = slab->previous;
		memory = slab->memory;
	}

	object_slab_unlock(enabled);

	/* Releases physical metadata and its credits only outside the guard. */
	if (memory.size != 0) {
		if (hal_pmem_free(&memory.paddr, memory.size) != HAL_OK)
			HAL_FATAL("VM descriptor slab retirement failed");
		if (cache_memory_release != NULL)
			cache_memory_release(CACHE_MEMORY_FILE_META, PAGE_SIZE);
	}
}

/* Reports the empty subtree height without dereferencing a missing child. */
static unsigned
page_index_height(
	const struct vm_object_page *page)
{
	if (page == NULL)
		return 0;

	return page->index_height;
}

/* Refreshes one node after its children change. */
static void
page_index_update(
	struct vm_object_page *page)
{
	unsigned left;
	unsigned right;

	/* Derives height from already balanced child subtrees. */
	left = page_index_height(page->index_left);
	right = page_index_height(page->index_right);
	page->index_height = 1U + (left > right ? left : right);
}

/* Rotates a right-heavy subtree without changing page identity. */
static struct vm_object_page *
page_index_rotate_left(
	struct vm_object_page *page)
{
	struct vm_object_page *root;

	/* Transfers the middle subtree before refreshing both heights. */
	root = page->index_right;
	page->index_right = root->index_left;
	root->index_left = page;
	page_index_update(page);
	page_index_update(root);
	return root;
}

/* Rotates a left-heavy subtree without changing page identity. */
static struct vm_object_page *
page_index_rotate_right(
	struct vm_object_page *page)
{
	struct vm_object_page *root;

	/* Transfers the middle subtree before refreshing both heights. */
	root = page->index_left;
	page->index_left = root->index_right;
	root->index_right = page;
	page_index_update(page);
	page_index_update(root);
	return root;
}

/* Restores the AVL height bound after one insertion or removal. */
static struct vm_object_page *
page_index_balance(
	struct vm_object_page *page)
{
	unsigned left;
	unsigned right;

	/* Measures the changed children before choosing a rotation. */
	page_index_update(page);
	left = page_index_height(page->index_left);

	/* Corrects a left-heavy subtree, including its inner-heavy case. */
	right = page_index_height(page->index_right);
	if (left > right + 1U) {
		/*
		 * The left subtree is at least two levels taller than the
		 * right one, so an insertion or a removal on the left has
		 * left this node out of balance.
		 */

		/*
		 * When the taller side of that left child is its own right
		 * subtree, the imbalance is the inner-heavy case: one right
		 * rotation here would only move it.  Rotating the child left
		 * first turns it into the outer-heavy case.
		 */
		if (page_index_height(page->index_left->index_right) >
		    page_index_height(page->index_left->index_left))
			page->index_left = page_index_rotate_left(page->index_left);

		/* Rotates right, which lifts the left child into this place. */
		page = page_index_rotate_right(page);
	} else if (right > left + 1U) {
		/*
		 * The mirror case: the right subtree is at least two levels
		 * taller than the left one.
		 */

		/*
		 * When the taller side of that right child is its own left
		 * subtree, this is the inner-heavy case.  Rotating the child
		 * right first turns it into the outer-heavy case.
		 */
		if (page_index_height(page->index_right->index_left) >
		    page_index_height(page->index_right->index_right))
			page->index_right = page_index_rotate_right(page->index_right);

		/* Rotates left, which lifts the right child into this place. */
		page = page_index_rotate_left(page);
	}

	/* Returns the balanced subtree root to its parent. */
	return page;
}

/* Inserts one unique page while preserving ordered child keys. */
static struct vm_object_page *
page_index_insert(
	struct vm_object_page *root,
	struct vm_object_page *page)
{
	/* Publishes a new leaf or rejects an inconsistent duplicate owner. */
	if (root == NULL)
		return page;
	if (root->offset == page->offset)
		HAL_FATAL("duplicate VM object page index key");

	/* Descends through one subtree and repairs heights on return. */
	if (page->offset < root->offset)
		root->index_left = page_index_insert(root->index_left, page);
	else
		root->index_right = page_index_insert(root->index_right, page);

	root = page_index_balance(root);

	return root;
}

/* Extracts the successor node itself, preserving all descriptor identities. */
static struct vm_object_page *
page_index_extract_min(
	struct vm_object_page *root,
	struct vm_object_page **minimum)
{
	/* Removes the smallest node and returns its surviving right subtree. */
	if (root->index_left == NULL) {
		*minimum = root;
		return root->index_right;
	}

	/* Repairs ancestors after unlinking the successor. */
	root->index_left = page_index_extract_min(root->index_left, minimum);
	root = page_index_balance(root);

	return root;
}

/* Removes the exact descriptor without copying another page's contents. */
static struct vm_object_page *
page_index_remove(
	struct vm_object_page *root,
	struct vm_object_page *page)
{
	struct vm_object_page *successor;
	struct vm_object_page *right;

	/* Detects list/index divergence at the mutation which caused it. */
	if (root == NULL)
		HAL_FATAL("missing VM object page index key");

	/* Finds the exact key, preserving a balanced path after removal. */
	if (page->offset < root->offset) {
		root->index_left = page_index_remove(root->index_left, page);
	} else if (page->offset > root->offset) {
		root->index_right = page_index_remove(root->index_right, page);
	} else {
		/* A node with at most one child is replaced by that child. */
		if (root != page)
			HAL_FATAL("VM object page index identity mismatch");
		if (root->index_left == NULL)
			return root->index_right;
		if (root->index_right == NULL)
			return root->index_left;

		/* Otherwise the in-order successor takes the node's place. */
		right = page_index_extract_min(root->index_right, &successor);
		successor->index_left = root->index_left;
		successor->index_right = right;
		root = successor;
	}

	root = page_index_balance(root);

	return root;
}
