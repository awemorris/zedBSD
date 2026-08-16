/*
 * VM object
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#include "kern/vm-object.h"
#include "kern/file.h"
#include "kern/kmem.h"
#include "kern/page.h"
#include "kern/vm-reclaim.h"
#include "kern/vmspace.h"

#include <errno.h>
#include <string.h>
#include <sys/mman.h>

extern bool hal_irq_disable(void) __attribute__((weak));
extern void hal_irq_enable(void) __attribute__((weak));

#define PAGE_SIZE ZEDBSD_PAGE_SIZE
#define VM_OBJECT_DATA __attribute__((section(".vfs_bss")))

static struct vm_object *shared_objects VM_OBJECT_DATA;
static unsigned object_count VM_OBJECT_DATA;
static atomic_uint_t object_pages VM_OBJECT_DATA;
static atomic_uint_t object_registry_lock VM_OBJECT_DATA;

static bool
registry_lock(void)
{
	bool enabled = hal_irq_disable != NULL ? hal_irq_disable() : false;

	while (!atomic_try_acquire_zero(&object_registry_lock))
		hal_compiler_barrier();
	return enabled;
}

static void
registry_unlock(bool enabled)
{
	atomic_store_release(&object_registry_lock, 0);
	if (enabled && hal_irq_enable != NULL)
		hal_irq_enable();
}

static int
alloc_vm_page(struct hal_pmem *memory)
{
	const struct hal_pmem_request request = {
		HAL_PMEM_PADDR_ANY, PAGE_SIZE, PAGE_SIZE,
		HAL_PMEM_TYPE_RAM, 0
	};
	return hal_pmem_alloc(&request, memory);
}

static struct vm_object_page *
find_page(struct vm_object *object, off_t offset)
{
	struct vm_object_page *page;
	for (page = object->pages; page != NULL; page = page->next)
		if (page->offset == offset)
			return page;
	return NULL;
}

static int
page_overlaps(const struct vm_object_page *page, uint64_t start, uint64_t end)
{
	uint64_t page_start, page_end;
	if (page->offset < 0)
		return 0;
	page_start = (uint64_t)page->offset;
	page_end = page_start + PAGE_SIZE;
	return page_end > start && page_start < end;
}

int
vm_object_get_shared(struct file *file, struct vm_object **result)
{
	struct vm_object *object;
	struct inode *inode = file_vm_inode(file);
	int writable;
	bool enabled;
	if (file == NULL || inode == NULL || result == NULL)
		return EINVAL;
	writable = (file->f_flags & O_ACCMODE) != O_RDONLY &&
	    file->f_ops != NULL && file->f_ops->pwrite != NULL;
	enabled = registry_lock();
	for (object = shared_objects; object != NULL; object = object->next) {
		if (object->inode == inode) {
			if (object->mapping_count == 0) {
				if (!(object->flags & VM_OBJECT_RETAINED_WRITEBACK))
					HAL_FATAL("reviving unretained VM object");
				object->flags &= ~VM_OBJECT_RETAINED_WRITEBACK;
			}
			if (writable && object->write_file == NULL) {
				file_ref(file);
				object->write_file = file;
			}
			object->mapping_count++;
			refcount_get(&object->refs);
			*result = object;
			registry_unlock(enabled);
			return 0;
		}
	}
	registry_unlock(enabled);
	object = kern_calloc(1, sizeof(*object));
	if (object == NULL)
		return ENOMEM;
	refcount_init(&object->refs, 2); /* registry plus returned region */
	object->mapping_count = 1;
	object->file = file;
	object->inode = inode;
	file_ref(file);
	if (writable) {
		object->write_file = file;
		file_ref(file);
	}
	enabled = registry_lock();
	/* Another CPU may have published the inode while allocation slept. */
	{
		struct vm_object *existing;
		for (existing = shared_objects; existing != NULL;
		     existing = existing->next) {
			if (existing->inode != inode)
				continue;
			if (existing->mapping_count == 0) {
				if (!(existing->flags & VM_OBJECT_RETAINED_WRITEBACK))
					HAL_FATAL("reviving unretained VM object");
				existing->flags &= ~VM_OBJECT_RETAINED_WRITEBACK;
			}
			existing->mapping_count++;
			refcount_get(&existing->refs);
			if (writable && existing->write_file == NULL) {
				file_ref(file);
				existing->write_file = file;
			}
			registry_unlock(enabled);
			(void)file_close(object->file);
			if (object->write_file != NULL)
				(void)file_close(object->write_file);
			kern_free(object);
			*result = existing;
			return 0;
		}
	}
	object->next = shared_objects;
	shared_objects = object;
	object_count++;
	registry_unlock(enabled);
	*result = object;
	return 0;
}

void
vm_object_ref(struct vm_object *object)
{
	bool enabled;
	if (object == NULL)
		return;
	enabled = registry_lock();
	if (object->mapping_count == 0)
		HAL_FATAL("referencing detached VM object");
	object->mapping_count++;
	refcount_get(&object->refs);
	registry_unlock(enabled);
}

static int
page_is_dirty(struct vm_object_page *page)
{
	struct vm_page *mapping;
	if (page->flags & VM_OBJECT_PAGE_DIRTY)
		return 1;
	for (mapping = page->mappings; mapping != NULL;
	     mapping = mapping->object_next) {
		uint32_t flags = 0;
		if (hal_page_query(mapping->vm->space, (void *)mapping->address,
		    &flags) == HAL_OK && (flags & HAL_PAGE_DIRTY) != 0) {
			page->flags |= VM_OBJECT_PAGE_DIRTY;
			return 1;
		}
	}
	return 0;
}

static int
object_has_dirty_pages(struct vm_object *object)
{
	struct vm_object_page *page;
	for (page = object->pages; page != NULL; page = page->next)
		if (page_is_dirty(page))
			return 1;
	return 0;
}

static int
object_has_busy_pages(const struct vm_object *object)
{
	const struct vm_object_page *page;
	for (page = object->pages; page != NULL; page = page->next)
		if (page->flags & VM_OBJECT_PAGE_BUSY)
			return 1;
	return 0;
}

static void
object_record_writeback_error(struct vm_object *object, int error)
{
	if (object->writeback_error == 0 && error != 0)
		object->writeback_error = error;
}

static int
write_page_data(struct vm_object *object, struct vm_object_page *page)
{
	size_t length;
	ssize_t count;
	if (page->flags & VM_OBJECT_PAGE_BUSY)
		return EBUSY;
	if (object->write_file == NULL)
		return EACCES;
	if (page->offset < 0)
		return EIO;
	/* Truncate already discarded full pages beyond the new EOF. */
	if (page->offset >= object->inode->i_size)
		return 0;
	length = (size_t)(object->inode->i_size - page->offset);
	if (length > PAGE_SIZE)
		length = PAGE_SIZE;
	page->flags |= VM_OBJECT_PAGE_BUSY;
	count = file_pwrite(object->write_file,
	    (const void *)page->pmem.vaddr, length, page->offset);
	page->flags &= ~VM_OBJECT_PAGE_BUSY;
	return count == (ssize_t)length ? 0 : count < 0 ? (int)-count : EIO;
}

static void
clear_page_dirty(struct vm_object_page *page)
{
	struct vm_page *mapping;
	page->flags &= ~VM_OBJECT_PAGE_DIRTY;
	for (mapping = page->mappings; mapping != NULL;
	     mapping = mapping->object_next)
		(void)hal_page_clear_flags(mapping->vm->space,
		    (void *)mapping->address, HAL_PAGE_DIRTY);
}

static void
free_object_page(struct vm_object_page *page)
{
	/* Region teardown must remove every mapping before the final object ref. */
	if (page->mapping_count != 0)
		HAL_FATAL("destroying mapped VM object page");
	(void)hal_pmem_free(&page->pmem);
	kern_free(page);
	if (atomic_load_acquire(&object_pages) == 0)
		HAL_FATAL("VM object page counter underflow");
	(void)atomic_fetch_add_relaxed(&object_pages, (unsigned)-1);
}

static int
object_can_destroy(struct vm_object *object)
{
	return object->mapping_count == 0 && object->writeback_error == 0 &&
	    !object_has_dirty_pages(object) && !object_has_busy_pages(object);
}

static int
unlink_object_locked(struct vm_object *object)
{
	struct vm_object **link;
	if (!object_can_destroy(object))
		HAL_FATAL("destroying unsynchronized VM object");
	for (link = &shared_objects; *link != NULL; link = &(*link)->next)
		if (*link == object) {
			*link = object->next;
			object->next = NULL;
			if (object_count == 0)
				HAL_FATAL("VM object counter underflow");
			object_count--;
			return 1;
		}
	return 0;
}

static void
destroy_object(struct vm_object *object)
{
	struct vm_object_page *page;

	while ((page = object->pages) != NULL) {
		object->pages = page->next;
		free_object_page(page);
	}
	(void)file_close(object->file);
	if (object->write_file != NULL)
		(void)file_close(object->write_file);
	kern_free(object);
}

static void
retain_object(struct vm_object *object, int error)
{
	if (object->mapping_count != 0)
		HAL_FATAL("retaining referenced VM object");
	if (error == 0)
		error = object_has_busy_pages(object) ? EBUSY : EIO;
	object_record_writeback_error(object, error);
	object->flags |= VM_OBJECT_RETAINED_WRITEBACK;
}

void
vm_object_put(struct vm_object *object)
{
	int error;
	int removed = 0;
	bool enabled;

	if (object == NULL)
		return;
	enabled = registry_lock();
	if (object->mapping_count == 0)
		HAL_FATAL("VM object mapping reference underflow");
	object->mapping_count--;
	if (object->mapping_count != 0) {
		registry_unlock(enabled);
		if (refcount_put(&object->refs))
			HAL_FATAL("mapped VM object lost registry reference");
		return;
	}
	registry_unlock(enabled);
	error = vm_object_sync_range(object, 0, SIZE_MAX, MS_SYNC);
	enabled = registry_lock();
	if (object->mapping_count == 0) {
		if (error == 0 && object_can_destroy(object))
			removed = unlink_object_locked(object);
		else
			retain_object(object, error);
	}
	registry_unlock(enabled);
	if (removed && refcount_put(&object->refs))
		HAL_FATAL("VM object registry reference was last unexpectedly");
	if (refcount_put(&object->refs))
		destroy_object(object);
}

int
vm_object_fault(struct vm_object *object, off_t offset,
		struct vm_object_page **result)
{
	struct vm_object_page *page;
	size_t length;
	ssize_t count;
	if (object == NULL || result == NULL || offset < 0 ||
	    (offset & (PAGE_SIZE - 1U)) != 0)
		return EINVAL;
	if (offset >= object->inode->i_size)
		return ENXIO;
	page = find_page(object, offset);
	if (page != NULL) {
		*result = page;
		return 0;
	}
	page = kern_calloc(1, sizeof(*page));
	if (page == NULL)
		return ENOMEM;
	page->offset = offset;
	page->flags = VM_OBJECT_PAGE_BUSY;
	if (alloc_vm_page(&page->pmem) != HAL_OK &&
	    (vm_reclaim_one(NULL) != 0 ||
	     alloc_vm_page(&page->pmem) != HAL_OK)) {
		kern_free(page);
		return ENOMEM;
	}
	memset((void *)page->pmem.vaddr, 0, PAGE_SIZE);
	length = (size_t)(object->inode->i_size - offset);
	if (length > PAGE_SIZE)
		length = PAGE_SIZE;
	count = file_pread(object->file, (void *)page->pmem.vaddr,
	    length, offset);
	if (count != (ssize_t)length) {
		(void)hal_pmem_free(&page->pmem);
		kern_free(page);
		return count < 0 ? (int)-count : EIO;
	}
	page->flags = 0;
	page->next = object->pages;
	object->pages = page;
	(void)atomic_fetch_add_relaxed(&object_pages, 1);
	*result = page;
	return 0;
}

void
vm_object_mapping_add(struct vm_object_page *object_page,
		      struct vm_page *mapping)
{
	mapping->object_next = object_page->mappings;
	object_page->mappings = mapping;
	object_page->mapping_count++;
}

void
vm_object_mapping_remove(struct vm_object_page *object_page,
			 struct vm_page *mapping)
{
	struct vm_page **link;
	if (object_page == NULL || mapping == NULL)
		return;
	for (link = &object_page->mappings; *link != NULL;
	     link = &(*link)->object_next)
		if (*link == mapping) {
			*link = mapping->object_next;
			mapping->object_next = NULL;
			if (object_page->mapping_count != 0)
				object_page->mapping_count--;
			return;
		}
}

void
vm_object_mark_dirty(struct vm_object_page *page)
{
	if (page != NULL)
		page->flags |= VM_OBJECT_PAGE_DIRTY;
}

static int
range_has_wired_mapping(struct vm_object *object, uint64_t start, uint64_t end)
{
	struct vm_object_page *page;
	for (page = object->pages; page != NULL; page = page->next) {
		struct vm_page *mapping;
		if (!page_overlaps(page, start, end))
			continue;
		for (mapping = page->mappings; mapping != NULL;
		     mapping = mapping->object_next)
			if (mapping->wire_count != 0)
				return 1;
	}
	return 0;
}

static void
invalidate_page(struct vm_object_page *page)
{
	struct vm_page *mapping;
	while ((mapping = page->mappings) != NULL) {
		struct vm_page **map_link = &mapping->region->pages;
		while (*map_link != NULL && *map_link != mapping)
			map_link = &(*map_link)->next;
		if (*map_link == mapping)
			*map_link = mapping->next;
		(void)hal_page_unmap(mapping->vm->space,
		    (void *)mapping->address, PAGE_SIZE);
		vm_object_mapping_remove(page, mapping);
		kern_free(mapping);
	}
}

int
vm_object_sync_range(struct vm_object *object, off_t offset, size_t size,
		     int flags)
{
	struct vm_object_page *page;
	struct vm_object_page **link;
	uint64_t start, end;
	int first_error = 0;
	int selected = 0;
	if (object == NULL || offset < 0 || size == 0 ||
	    (flags & ~(MS_ASYNC | MS_INVALIDATE | MS_SYNC)) != 0 ||
	    (flags & (MS_ASYNC | MS_SYNC)) == 0 ||
	    (flags & (MS_ASYNC | MS_SYNC)) == (MS_ASYNC | MS_SYNC))
		return EINVAL;
	start = (uint64_t)offset;
	/* offset zero plus SIZE_MAX is the internal full-object sentinel. */
	if (start == 0 && size == SIZE_MAX)
		end = UINT64_MAX;
	else {
		end = start + size;
		if (end < start)
			end = UINT64_MAX;
	}
	if ((flags & MS_INVALIDATE) != 0 &&
	    range_has_wired_mapping(object, start, end))
		return EBUSY;

	for (page = object->pages; page != NULL; page = page->next) {
		int error;
		if (!page_overlaps(page, start, end) || !page_is_dirty(page))
			continue;
		page->flags |= VM_OBJECT_PAGE_WRITEBACK;
		selected = 1;
		error = write_page_data(object, page);
		if (error != 0 && first_error == 0)
			first_error = error;
	}
	if (selected || object->writeback_error != 0) {
		int error = object->write_file != NULL ?
		    file_fsync(object->write_file) : EACCES;
		if (error != 0 && first_error == 0)
			first_error = error;
	}
	if (first_error != 0) {
		object_record_writeback_error(object, first_error);
		for (page = object->pages; page != NULL; page = page->next)
			page->flags &= ~VM_OBJECT_PAGE_WRITEBACK;
		return first_error;
	}

	for (page = object->pages; page != NULL; page = page->next) {
		if (!(page->flags & VM_OBJECT_PAGE_WRITEBACK))
			continue;
		clear_page_dirty(page);
		page->flags &= ~VM_OBJECT_PAGE_WRITEBACK;
	}
	if ((flags & MS_INVALIDATE) != 0) {
		for (link = &object->pages; *link != NULL; ) {
			page = *link;
			if (!page_overlaps(page, start, end) || page_is_dirty(page)) {
				link = &page->next;
				continue;
			}
			invalidate_page(page);
			*link = page->next;
			free_object_page(page);
		}
	}
	if (!object_has_dirty_pages(object))
		object->writeback_error = 0;
	return 0;
}

int
vm_object_sync_inode(struct inode *inode)
{
	struct vm_object *object;
	bool enabled;
	int error, removed = 0;

	enabled = registry_lock();
	for (object = shared_objects; object != NULL; object = object->next)
		if (object->inode == inode) {
			refcount_get(&object->refs);
			break;
		}
	registry_unlock(enabled);
	if (object == NULL)
		return 0;
	error = vm_object_sync_range(object, 0, SIZE_MAX, MS_SYNC);
	enabled = registry_lock();
	if (object->mapping_count == 0) {
		if (error == 0 && object_can_destroy(object))
			removed = unlink_object_locked(object);
		else
			retain_object(object, error);
	}
	registry_unlock(enabled);
	if (removed && refcount_put(&object->refs))
		HAL_FATAL("VM object registry reference was last unexpectedly");
	if (refcount_put(&object->refs))
		destroy_object(object);
	return error;
}

void
vm_object_truncate_inode(struct inode *inode, off_t size)
{
	struct vm_object *object;
	bool enabled;

	enabled = registry_lock();
	for (object = shared_objects; object != NULL; object = object->next)
		if (object->inode == inode) {
			refcount_get(&object->refs);
			break;
		}
	registry_unlock(enabled);
	if (object != NULL) {
		struct vm_object_page **link;
		for (link = &object->pages; *link != NULL; ) {
			struct vm_object_page *page = *link;
			if (page->offset >= size) {
				invalidate_page(page);
				*link = page->next;
				free_object_page(page);
				continue;
			}
			if (size > page->offset &&
			    (size_t)(size - page->offset) < PAGE_SIZE)
				memset((void *)(page->pmem.vaddr + size - page->offset),
				    0, PAGE_SIZE - (size_t)(size - page->offset));
			link = &page->next;
		}
		if (refcount_put(&object->refs))
			HAL_FATAL("published VM object lost registry reference");
	}
}

int
vm_object_reclaim_one(void)
{
	struct vm_object *object;
	struct vm_object_page *candidate = NULL;
	bool enabled;

	enabled = registry_lock();
	for (object = shared_objects; object != NULL; object = object->next) {
		struct vm_object_page *page;
		for (page = object->pages; page != NULL; page = page->next) {
			struct vm_page *mapping;
			int wired = 0;
			if (page->flags & VM_OBJECT_PAGE_BUSY)
				continue;
			for (mapping = page->mappings; mapping != NULL;
			     mapping = mapping->object_next)
				if (mapping->wire_count != 0) {
					wired = 1;
					break;
				}
			if (wired)
				continue;
			candidate = page;
			refcount_get(&object->refs);
			break;
		}
		if (candidate != NULL)
			break;
	}
	registry_unlock(enabled);
	if (candidate == NULL)
		return ENOMEM;
	{
		int error = vm_object_sync_range(object, candidate->offset,
		    PAGE_SIZE, MS_SYNC | MS_INVALIDATE);
		int removed = 0;
		if (error == 0) {
			enabled = registry_lock();
			if (object->mapping_count == 0 && object_can_destroy(object))
				removed = unlink_object_locked(object);
			registry_unlock(enabled);
		}
		if (removed && refcount_put(&object->refs))
			HAL_FATAL("VM object registry reference was last unexpectedly");
		if (refcount_put(&object->refs))
			destroy_object(object);
		/* Reclaim reports whether a page was freed; writeback retains error. */
		return error == 0 ? 0 : ENOMEM;
	}
}

unsigned vm_object_count(void)
{
	bool enabled = registry_lock();
	unsigned count = object_count;
	registry_unlock(enabled);
	return count;
}
unsigned vm_object_page_count(void)
{
	bool enabled = registry_lock();
	unsigned count = atomic_load_acquire(&object_pages);
	registry_unlock(enabled);
	return count;
}

unsigned
vm_object_retained_count(void)
{
	struct vm_object *object;
	unsigned count = 0;
	bool enabled = registry_lock();
	for (object = shared_objects; object != NULL; object = object->next)
		if (object->flags & VM_OBJECT_RETAINED_WRITEBACK)
			count++;
	registry_unlock(enabled);
	return count;
}
