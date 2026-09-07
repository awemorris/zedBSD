/* -*- mode: c; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Page-backed descriptors and exact physical ownership for VM object pages.
 * The private slab lock never nests a physical allocator or VM object lock.
 */

struct vm_page_slab {
	struct hal_pmem memory;
	struct vm_page_slab *next;
	struct vm_page_slab *previous;
	struct vm_object_page *free_pages;
	unsigned used;
};

static struct vm_page_slab *page_slabs VM_OBJECT_DATA;
static atomic_uint_t page_slab_lock VM_OBJECT_DATA;

extern int cache_memory_reserve(enum cache_memory_kind, size_t, int) __attribute__((weak));
extern void cache_memory_commit(enum cache_memory_kind, size_t) __attribute__((weak));
extern void cache_memory_cancel(enum cache_memory_kind, size_t) __attribute__((weak));
extern void cache_memory_release(enum cache_memory_kind, size_t) __attribute__((weak));
extern size_t cache_memory_reclaim(size_t) __attribute__((weak));

static int object_memory_reserve(enum cache_memory_kind kind, int optional);
static bool object_slab_lock(void);
static void object_slab_unlock(bool enabled);
static struct vm_object_page *object_descriptor_alloc(int optional);
static void object_descriptor_free(struct vm_object_page *page);
static struct vm_page_slab *object_slab_alloc(int optional);

/* Tries one bounded clean-pressure pass before refusing optional cache growth. */
static int
object_memory_reserve(
	enum cache_memory_kind kind,
	int optional)
{
	int error;

	/* Keeps standalone VM fixtures usable without installing a budget owner. */
	if (cache_memory_reserve == NULL)
		return 0;
	error = cache_memory_reserve(kind, PAGE_SIZE, optional);
	if (error == 0 || !optional || cache_memory_reclaim == NULL)
		return error;

	/* Reclaims only independent idle ownership; the current read stays active. */
	if (cache_memory_reclaim(PAGE_SIZE) == 0)
		return error;
	error = cache_memory_reserve(kind, PAGE_SIZE, optional);
	return error;
}

/* Serializes descriptor ownership without sleeping or taking another lock. */
static bool
object_slab_lock(
	void)
{
	unsigned expected;
	bool enabled;

	/* Preserves interrupt state while acquiring the short metadata guard. */
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

/* Constructs a private metadata slab outside all VM and slab locks. */
static struct vm_page_slab *
object_slab_alloc(
	int optional)
{
	struct hal_pmem memory;
	struct vm_page_slab *slab;
	struct vm_object_page *page;
	size_t header;
	size_t offset;

	/* Reserves the entire physical page, including unused descriptor capacity. */
	if (object_memory_reserve(CACHE_MEMORY_FILE_META, optional) != 0)
		return NULL;
	memset(&memory, 0, sizeof(memory));
	if (alloc_vm_page(&memory) != HAL_OK) {
		if (cache_memory_cancel != NULL)
			cache_memory_cancel(CACHE_MEMORY_FILE_META, PAGE_SIZE);
		return NULL;
	}
	if (memory.size != PAGE_SIZE || memory.vaddr == NULL)
		HAL_FATAL("unexpected VM descriptor slab allocation");

	/* Formats aligned descriptors without consuming the fixed kernel heap. */
	slab = memory.vaddr;
	memset(slab, 0, PAGE_SIZE);
	slab->memory = memory;
	header = (sizeof(*slab) + __alignof__(struct vm_object_page) - 1U) &
	    ~(size_t)(__alignof__(struct vm_object_page) - 1U);
	for (offset = header; offset + sizeof(*page) <= PAGE_SIZE;
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

	/* Finds existing capacity without entering an allocator under the guard. */
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

	/* Holds the slab through the descriptor's complete private/resident lifetime. */
	page = slab->free_pages;
	slab->free_pages = page->next;
	slab->used++;
	if (slab->free_pages == NULL) {
		page_slabs = slab->next;
		if (page_slabs != NULL)
			page_slabs->previous = NULL;
		slab->next = NULL;
	}
	object_slab_unlock(enabled);
	memset(page, 0, sizeof(*page));
	page->metadata_slab = slab;
	return page;
}

/* Returns one descriptor and retires its slab only after the last owner leaves. */
static void
object_descriptor_free(
	struct vm_object_page *page)
{
	struct vm_page_slab *slab;
	struct hal_pmem memory;
	bool enabled;

	/* Removes empty slabs from allocation visibility before physical retirement. */
	slab = page->metadata_slab;
	memset(&memory, 0, sizeof(memory));
	enabled = object_slab_lock();
	if (slab == NULL || slab->used == 0)
		HAL_FATAL("VM page descriptor ownership underflow");
	if (slab->free_pages == NULL) {
		slab->previous = NULL;
		slab->next = page_slabs;
		if (page_slabs != NULL)
			page_slabs->previous = slab;
		page_slabs = slab;
	}
	page->next = slab->free_pages;
	slab->free_pages = page;
	slab->used--;
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
		if (hal_pmem_free(&memory) != HAL_OK)
			HAL_FATAL("VM descriptor slab retirement failed");
		if (cache_memory_release != NULL)
			cache_memory_release(CACHE_MEMORY_FILE_META, PAGE_SIZE);
	}
}

/* Allocates a fully charged private frame before any page index publication. */
static struct vm_object_page *
alloc_object_page(
	struct vm_object *object,
	off_t offset,
	int optional)
{
	struct vm_object_page *page;

	/* Reserves optional cache growth or records mandatory mapping commitment. */
	if (object_memory_reserve(CACHE_MEMORY_FILE_DATA, optional) != 0)
		return NULL;
	page = object_descriptor_alloc(optional);
	if (page == NULL) {
		if (cache_memory_cancel != NULL)
			cache_memory_cancel(CACHE_MEMORY_FILE_DATA, PAGE_SIZE);
		return NULL;
	}
	if (alloc_vm_page(&page->pmem) != HAL_OK) {
		object_descriptor_free(page);
		if (cache_memory_cancel != NULL)
			cache_memory_cancel(CACHE_MEMORY_FILE_DATA, PAGE_SIZE);
		return NULL;
	}
	if (page->pmem.size != PAGE_SIZE || page->pmem.vaddr == NULL)
		HAL_FATAL("unexpected VM object frame allocation");

	/* Publishes resident ownership while the initialized descriptor stays private. */
	page->owner = object;
	page->offset = offset;
	page->flags = VM_OBJECT_PAGE_BUSY;
	if (cache_memory_commit != NULL)
		cache_memory_commit(CACHE_MEMORY_FILE_DATA, PAGE_SIZE);
	return page;
}

/* Frees private or detached frame storage without changing publication counts. */
static void
release_object_page_storage(
	struct vm_object_page *page)
{
	/* Ends optional attribution before the descriptor or its storage can be reused. */
	object_prefetch_retire(page);

	/* Returns data credits only after the physical frame has retired. */
	if (hal_pmem_free(&page->pmem) != HAL_OK)
		HAL_FATAL("VM object frame retirement failed");
	if (cache_memory_release != NULL)
		cache_memory_release(CACHE_MEMORY_FILE_DATA, PAGE_SIZE);
	object_descriptor_free(page);
}
