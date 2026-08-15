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

int
vm_object_get_shared(struct file *file, struct vm_object **result)
{
	struct vm_object *object;
	struct inode *inode = file_vm_inode(file);
	if (file == NULL || inode == NULL || result == NULL)
		return EINVAL;
	for (object = shared_objects; object != NULL; object = object->next) {
		if (object->inode == inode) {
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
write_page(struct vm_object *object, struct vm_object_page *page)
{
	struct vm_page *mapping;
	size_t length;
	ssize_t count;
	if (!page_is_dirty(page))
		return 0;
	if (page->offset < 0 || page->offset >= object->inode->i_size)
		return 0;
	length = (size_t)(object->inode->i_size - page->offset);
	if (length > PAGE_SIZE)
		length = PAGE_SIZE;
	page->flags |= VM_OBJECT_PAGE_BUSY;
	count = file_pwrite(object->file, (const void *)page->pmem.vaddr,
	    length, page->offset);
	page->flags &= ~VM_OBJECT_PAGE_BUSY;
	if (count != (ssize_t)length) {
		object->writeback_error = count < 0 ? (int)-count : EIO;
		return object->writeback_error;
	}
	page->flags &= ~VM_OBJECT_PAGE_DIRTY;
	for (mapping = page->mappings; mapping != NULL;
	     mapping = mapping->object_next)
		(void)hal_page_clear_flags(mapping->vm->space,
		    (void *)mapping->address, HAL_PAGE_DIRTY);
	return 0;
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

void
vm_object_put(struct vm_object *object)
{
	struct vm_object **link;
	struct vm_object_page *page;
	if (object == NULL || object->usecount == 0 || --object->usecount != 0)
		return;
	(void)vm_object_sync_range(object, 0, SIZE_MAX, MS_SYNC);
	for (link = &shared_objects; *link != NULL; link = &(*link)->next)
		if (*link == object) {
			*link = object->next;
			break;
		}
	while ((page = object->pages) != NULL) {
		object->pages = page->next;
		free_object_page(page);
	}
	(void)file_close(object->file);
	kern_free(object);
	if (object_count != 0)
		object_count--;
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

int
vm_object_sync_range(struct vm_object *object, off_t offset, size_t size,
		     int flags)
{
	struct vm_object_page **link;
	uint64_t end;
	int first_error = 0;
	if (object == NULL || offset < 0 || size == 0)
		return EINVAL;
	end = (uint64_t)offset + size;
	if (end < (uint64_t)offset)
		end = UINT64_MAX;
	for (link = &object->pages; *link != NULL; ) {
		struct vm_object_page *page = *link;
		uint64_t page_end = (uint64_t)page->offset + PAGE_SIZE;
		int error;
		if (page_end <= (uint64_t)offset ||
		    (uint64_t)page->offset >= end) {
			link = &page->next;
			continue;
		}
		error = write_page(object, page);
		if (error != 0 && first_error == 0)
			first_error = error;
		if ((flags & MS_INVALIDATE) != 0 && error == 0 &&
		    !page_is_dirty(page)) {
			struct vm_page *mapping;
			for (mapping = page->mappings; mapping != NULL;
			     mapping = mapping->object_next)
				if (mapping->wire_count != 0)
					return EBUSY;
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
			*link = page->next;
			free_object_page(page);
			continue;
		}
		link = &page->next;
	}
	return first_error;
}

int
vm_object_sync_inode(struct inode *inode)
{
	struct vm_object *object;
	int first_error = 0;
	for (object = shared_objects; object != NULL; object = object->next) {
		int error;
		if (object->inode != inode)
			continue;
		error = vm_object_sync_range(object, 0, SIZE_MAX, MS_SYNC);
		if (error != 0 && first_error == 0)
			first_error = error;
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
	struct vm_object *object;
	for (object = shared_objects; object != NULL; object = object->next) {
		struct vm_object_page **link;
		for (link = &object->pages; *link != NULL; link = &(*link)->next) {
			struct vm_object_page *page = *link;
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
			if (wired || write_page(object, page) != 0)
				continue;
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
			*link = page->next;
			free_object_page(page);
			return 0;
		}
	}
	return ENOMEM;
}

unsigned vm_object_count(void) { return object_count; }
unsigned vm_object_page_count(void) { return object_pages; }
