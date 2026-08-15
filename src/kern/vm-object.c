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

#define PAGE_SIZE ZEDBSD_PAGE_SIZE
#define VM_OBJECT_DATA __attribute__((section(".vfs_bss")))

static struct vm_object *shared_objects VM_OBJECT_DATA;
static unsigned object_count VM_OBJECT_DATA;
static unsigned object_pages VM_OBJECT_DATA;

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
	if (file == NULL || inode == NULL || result == NULL)
		return EINVAL;
	writable = (file->f_flags & O_ACCMODE) != O_RDONLY &&
	    file->f_ops != NULL && file->f_ops->pwrite != NULL;
	for (object = shared_objects; object != NULL; object = object->next) {
		if (object->inode == inode) {
			if (object->usecount == 0) {
				if (!(object->flags & VM_OBJECT_RETAINED_WRITEBACK))
					HAL_FATAL("reviving unretained VM object");
				object->flags &= ~VM_OBJECT_RETAINED_WRITEBACK;
			}
			if (writable && object->write_file == NULL) {
				file_ref(file);
				object->write_file = file;
			}
			object->usecount++;
			*result = object;
			return 0;
		}
	}
	object = kern_calloc(1, sizeof(*object));
	if (object == NULL)
		return ENOMEM;
	object->usecount = 1;
	object->file = file;
	object->inode = inode;
	file_ref(file);
	if (writable) {
		object->write_file = file;
		file_ref(file);
	}
	object->next = shared_objects;
	shared_objects = object;
	object_count++;
	*result = object;
	return 0;
}

void
vm_object_ref(struct vm_object *object)
{
	if (object != NULL && object->usecount != 0)
		object->usecount++;
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
		    &flags) == HAL_PMEM_SUCCESS && (flags & HAL_PAGE_DIRTY) != 0) {
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
	if (object_pages != 0)
		object_pages--;
}

static int
object_can_destroy(struct vm_object *object)
{
	return object->usecount == 0 && object->writeback_error == 0 &&
	    !object_has_dirty_pages(object) && !object_has_busy_pages(object);
}

static void
unlink_and_destroy_object(struct vm_object *object)
{
	struct vm_object **link;
	struct vm_object_page *page;
	int found = 0;
	if (!object_can_destroy(object))
		HAL_FATAL("destroying unsynchronized VM object");
	for (link = &shared_objects; *link != NULL; link = &(*link)->next)
		if (*link == object) {
			*link = object->next;
			found = 1;
			break;
		}
	if (!found)
		HAL_FATAL("VM object list unlink failed");
	while ((page = object->pages) != NULL) {
		object->pages = page->next;
		free_object_page(page);
	}
	(void)file_close(object->file);
	if (object->write_file != NULL)
		(void)file_close(object->write_file);
	kern_free(object);
	if (object_count == 0)
		HAL_FATAL("VM object counter underflow");
	object_count--;
}

static void
retain_object(struct vm_object *object, int error)
{
	if (object->usecount != 0)
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
	if (object == NULL || object->usecount == 0 || --object->usecount != 0)
		return;
	error = vm_object_sync_range(object, 0, SIZE_MAX, MS_SYNC);
	if (error == 0 && object_can_destroy(object))
		unlink_and_destroy_object(object);
	else
		retain_object(object, error);
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
	if (hal_pmem_alloc(PAGE_SIZE, &page->pmem, 0) != HAL_PMEM_SUCCESS &&
	    (vm_reclaim_one(NULL) != 0 ||
	     hal_pmem_alloc(PAGE_SIZE, &page->pmem, 0) != HAL_PMEM_SUCCESS)) {
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
	object_pages++;
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
	struct vm_object *object, *next;
	int first_error = 0;
	for (object = shared_objects; object != NULL; object = next) {
		int error;
		next = object->next;
		if (object->inode != inode)
			continue;
		error = vm_object_sync_range(object, 0, SIZE_MAX, MS_SYNC);
		if (error != 0 && first_error == 0)
			first_error = error;
		if (object->usecount != 0)
			continue;
		if (error == 0 && object_can_destroy(object))
			unlink_and_destroy_object(object);
		else
			retain_object(object, error);
	}
	return first_error;
}

void
vm_object_truncate_inode(struct inode *inode, off_t size)
{
	struct vm_object *object;
	for (object = shared_objects; object != NULL; object = object->next) {
		struct vm_object_page **link;
		if (object->inode != inode)
			continue;
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
	}
}

int
vm_object_reclaim_one(void)
{
	struct vm_object *object, *next;
	for (object = shared_objects; object != NULL; object = next) {
		struct vm_object_page *page;
		next = object->next;
		for (page = object->pages; page != NULL; page = page->next) {
			struct vm_page *mapping;
			int wired = 0;
			int error;
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
			error = vm_object_sync_range(object, page->offset, PAGE_SIZE,
			    MS_SYNC | MS_INVALIDATE);
			if (error != 0)
				continue;
			if (object->usecount == 0 && object_can_destroy(object))
				unlink_and_destroy_object(object);
			return 0;
		}
	}
	return ENOMEM;
}

unsigned vm_object_count(void) { return object_count; }
unsigned vm_object_page_count(void) { return object_pages; }

unsigned
vm_object_retained_count(void)
{
	struct vm_object *object;
	unsigned count = 0;
	for (object = shared_objects; object != NULL; object = object->next)
		if (object->flags & VM_OBJECT_RETAINED_WRITEBACK)
			count++;
	return count;
}
