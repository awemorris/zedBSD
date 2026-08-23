/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/uaccess.h"
#include "kern/kmem.h"
#include "kern/process.h"
#include "kern/page.h"
#include "kern/thread.h"
#include "kern/vm-object.h"
#include "kern/vm-reclaim.h"
#include "kern/vmspace.h"

#include <errno.h>
#include <hal/hal.h>
#include <limits.h>
#include <string.h>

#define UACCESS_EXT __attribute__((section(".hightext")))
#define PAGE_SIZE ZEDBSD_PAGE_SIZE

static struct vmspace *current_vmspace(void)
{
	return curthread != NULL && curthread->proc != NULL ?
		curthread->proc->vmspace : NULL;
}

int user_range_check(uintptr_t address, size_t size, uint32_t prot)
{
	return vmspace_check(current_vmspace(), address, size, prot);
}

UACCESS_EXT int
user_address_add(uintptr_t address, size_t delta, uintptr_t *result)
{
	if (result == NULL || delta > UINTPTR_MAX - address)
		return EOVERFLOW;
	*result = address + delta;
	return 0;
}

UACCESS_EXT int
size_add_checked(size_t left, size_t right, size_t *result)
{
	if (result == NULL || right > SIZE_MAX - left)
		return EOVERFLOW;
	*result = left + right;
	return 0;
}

UACCESS_EXT int
off_add_size(off_t offset, size_t delta, off_t *result)
{
	uint64_t maximum = sizeof(off_t) == sizeof(int64_t) ?
		(uint64_t)INT64_MAX : (uint64_t)INT32_MAX;

	if (result == NULL || offset < 0 || (uint64_t)offset > maximum ||
	    (uint64_t)delta > maximum - (uint64_t)offset)
		return EOVERFLOW;
	*result = offset + (off_t)delta;
	return 0;
}

UACCESS_EXT int
uaccess_pin_vmspace(struct vmspace *vm, uintptr_t address, size_t size,
	uint32_t prot, struct uaccess_pin *pin)
{
	struct vmspace_pinned_page *pages;
	uintptr_t first, last;
	size_t page_count;
	int error;

	if (pin == NULL)
		return EINVAL;
	memset(pin, 0, sizeof(*pin));
	if (size == 0)
		return 0;
	if (!vmspace_tryref(vm))
		return EFAULT;
	if (!vmspace_user_range_valid(address, size)) {
		vmspace_put(vm);
		return EFAULT;
	}
	first = address & ~(uintptr_t)(PAGE_SIZE - 1U);
	last = (address + size - 1U) & ~(uintptr_t)(PAGE_SIZE - 1U);
	page_count = (size_t)((last - first) / PAGE_SIZE) + 1U;
	if (page_count > SIZE_MAX / sizeof(*pages)) {
		vmspace_put(vm);
		return ENOMEM;
	}
	pages = kern_calloc(page_count, sizeof(*pages));
	if (pages == NULL) {
		vmspace_put(vm);
		return ENOMEM;
	}
	error = vmspace_pin_user_pages(vm, address, size, prot, pages, page_count);
	/* Backing pins own the successful snapshot independently of the vmspace. */
	vmspace_put(vm);
	if (error != 0) {
		kern_free(pages);
		return error == ENOMEM ? ENOMEM : EFAULT;
	}
	pin->address = address;
	pin->size = size;
	pin->prot = prot;
	pin->first_offset = address - first;
	pin->page_count = page_count;
	pin->pages = pages;
	pin->active = 1;
	return 0;
}

UACCESS_EXT int
uaccess_pin(uintptr_t address, size_t size, uint32_t prot,
	    struct uaccess_pin *pin)
{
	return uaccess_pin_vmspace(current_vmspace(), address, size, prot, pin);
}

UACCESS_EXT void
uaccess_unpin(struct uaccess_pin *pin)
{
	if (pin == NULL || !pin->active)
		return;
	vmspace_unpin_user_pages(pin->pages, pin->page_count);
	kern_free(pin->pages);
	memset(pin, 0, sizeof(*pin));
}

static UACCESS_EXT int
pinned_range(const struct uaccess_pin *pin, size_t offset, size_t size,
	    uint32_t prot, size_t *position)
{
	if (pin == NULL || position == NULL || !pin->active || pin->pages == NULL ||
	    (pin->prot & prot) != prot || offset > pin->size ||
	    size > pin->size - offset || offset > SIZE_MAX - pin->first_offset)
		return EFAULT;
	*position = pin->first_offset + offset;
	return 0;
}

UACCESS_EXT int
copyin_pinned(const struct uaccess_pin *pin, size_t offset,
	      void *destination, size_t size)
{
	uint8_t *bytes = destination;
	size_t position;
	int error;

	if (size == 0)
		return 0;
	if (destination == NULL)
		return EINVAL;
	error = pinned_range(pin, offset, size, HAL_SPACE_READ, &position);
	if (error != 0)
		return error;
	while (size != 0) {
		const struct vmspace_pinned_page *page =
		    &pin->pages[position / PAGE_SIZE];
		size_t page_offset = position & (PAGE_SIZE - 1U);
		size_t chunk = PAGE_SIZE - page_offset;

		if (chunk > size)
			chunk = size;
		if (page->kind == VMSPACE_PINNED_PRIVATE) {
			memcpy(bytes, (const uint8_t *)page->memory.vaddr +
			    page_offset, chunk);
			error = 0;
		} else if (page->kind == VMSPACE_PINNED_OBJECT) {
			error = vm_object_page_pin_read(page->owner.object_page,
			    page_offset, bytes, chunk);
		} else {
			error = EFAULT;
		}
		if (error != 0)
			return error;
		bytes += chunk;
		position += chunk;
		size -= chunk;
	}
	return 0;
}

UACCESS_EXT int
copyout_pinned(const struct uaccess_pin *pin, size_t offset,
	       const void *source, size_t size)
{
	const uint8_t *bytes = source;
	size_t position;
	int error;

	if (size == 0)
		return 0;
	if (source == NULL)
		return EINVAL;
	error = pinned_range(pin, offset, size, HAL_SPACE_WRITE, &position);
	if (error != 0)
		return error;
	while (size != 0) {
		const struct vmspace_pinned_page *page =
		    &pin->pages[position / PAGE_SIZE];
		size_t page_offset = position & (PAGE_SIZE - 1U);
		size_t chunk = PAGE_SIZE - page_offset;

		if (chunk > size)
			chunk = size;
		if (page->kind == VMSPACE_PINNED_PRIVATE) {
			memcpy((uint8_t *)page->memory.vaddr + page_offset,
			    bytes, chunk);
			/*
			 * The backing pin excludes reclaim I/O and fork sharing from the
			 * memcpy through dirty publication.  Unmap may remove metadata but
			 * cannot release this saved frame or its backing reference.
			 */
			vm_private_page_mark_dirty(page->owner.private_page);
			error = 0;
		} else if (page->kind == VMSPACE_PINNED_OBJECT) {
			error = vm_object_page_pin_write(page->owner.object_page,
			    page_offset, bytes, chunk);
		} else {
			error = EFAULT;
		}
		if (error != 0)
			return error;
		bytes += chunk;
		position += chunk;
		size -= chunk;
	}
	return 0;
}

int copyin(uintptr_t source, void *destination, size_t size)
{
	if (size == 0)
		return 0;
	if (destination == NULL ||
	    user_range_check(source, size, HAL_SPACE_READ) != 0)
		return EFAULT;
	return vmspace_copy_from(current_vmspace(), destination, source, size);
}

int copyout(const void *source, uintptr_t destination, size_t size)
{
	if (size == 0)
		return 0;
	if (source == NULL ||
	    user_range_check(destination, size, HAL_SPACE_WRITE) != 0)
		return EFAULT;
	return vmspace_copy_to(current_vmspace(), destination, source, size);
}

int copyinstr(uintptr_t source, char *destination, size_t capacity,
	      size_t *length)
{
	size_t used = 0;

	if (destination == NULL || capacity == 0)
		return EINVAL;
	while (used < capacity) {
		uintptr_t address;
		size_t chunk, index;
		int error;

		error = user_address_add(source, used, &address);
		if (error != 0)
			return error;
		chunk = ZEDBSD_PAGE_SIZE -
		    (size_t)(address & (ZEDBSD_PAGE_SIZE - 1U));
		if (chunk > capacity - used)
			chunk = capacity - used;
		error = copyin(address, destination + used, chunk);
		if (error != 0)
			return error;
		for (index = 0; index < chunk; index++)
			if (destination[used + index] == '\0') {
				if (length != NULL)
					*length = used + index + 1U;
				return 0;
			}
		used += chunk;
	}
	destination[capacity - 1U] = '\0';
	return ENAMETOOLONG;
}
