/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * User memory access.
 *
 * Copies between kernel and user memory go through the current vmspace
 * after a range check.  A pin snapshots the pages behind a user range so
 * that later copies cannot fault and cannot see the range unmapped.  The
 * pin paths live in the high text section because they run during page-in.
 */

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

static struct vmspace *current_vmspace(void);
static UACCESS_EXT int pinned_range(const struct uaccess_pin *pin, size_t offset, size_t size, uint32_t prot, size_t *position);

/*
 * Checks that a user range is mapped with the requested protection.
 */
int
user_range_check(
	uintptr_t address,
	size_t size,
	uint32_t prot)
{
	struct vmspace *vm;
	int error;

	/* Asks the current vmspace about the range. */
	vm = current_vmspace();
	error = vmspace_check(vm, address, size, prot);

	/* Reports the check result. */
	return error;
}

/*
 * Adds an offset to a user address, refusing wraparound.
 */
UACCESS_EXT int
user_address_add(
	uintptr_t address,
	size_t delta,
	uintptr_t *result)
{
	/* Rejects a missing result or a sum that wraps. */
	if (result == NULL || delta > UINTPTR_MAX - address)
		return EOVERFLOW;

	*result = address + delta;

	/* Reports the sum. */
	return 0;
}

/*
 * Adds two sizes, refusing wraparound.
 */
UACCESS_EXT int
size_add_checked(
	size_t left,
	size_t right,
	size_t *result)
{
	/* Rejects a missing result or a sum that wraps. */
	if (result == NULL || right > SIZE_MAX - left)
		return EOVERFLOW;

	*result = left + right;

	/* Reports the sum. */
	return 0;
}

/*
 * Adds a size to a file offset, refusing a negative or overflowing result.
 */
UACCESS_EXT int
off_add_size(
	off_t offset,
	size_t delta,
	off_t *result)
{
	uint64_t maximum;

	/* Takes the largest offset the off_t width can represent. */
	if (sizeof(off_t) == sizeof(int64_t))
		maximum = (uint64_t)INT64_MAX;
	else
		maximum = (uint64_t)INT32_MAX;

	/* Rejects a missing result, a negative offset, or an overflowing sum. */
	if (result == NULL ||
	    offset < 0 ||
	    (uint64_t)offset > maximum ||
	    (uint64_t)delta > maximum - (uint64_t)offset)
		return EOVERFLOW;

	*result = offset + (off_t)delta;

	/* Reports the sum. */
	return 0;
}

/*
 * Pins the pages behind a user range of a vmspace.
 *
 * The pin holds its own page references, so the vmspace reference taken
 * for the lookup is dropped before returning.  An empty range pins nothing
 * and leaves the pin inactive.
 */
UACCESS_EXT int
uaccess_pin_vmspace(
	struct vmspace *vm,
	uintptr_t address,
	size_t size,
	uint32_t prot,
	struct uaccess_pin *pin)
{
	struct vmspace_pinned_page *pages;
	uintptr_t first;
	uintptr_t last;
	size_t page_count;
	int error;

	/* Rejects a missing pin. */
	if (pin == NULL)
		return EINVAL;

	/* An empty range needs no pages. */
	memset(pin, 0, sizeof(*pin));
	if (size == 0)
		return 0;

	/* Holds the vmspace while its pages are looked up. */
	if (!vmspace_tryref(vm))
		return EFAULT;
	if (!vmspace_user_range_valid(address, size)) {
		vmspace_put(vm);
		return EFAULT;
	}

	/* Allocates one record per page of the range. */
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

	/* Pins the pages; the pin then owns them independently of the vmspace. */
	error = vmspace_pin_user_pages(vm, address, size, prot, pages, page_count);
	vmspace_put(vm);
	if (error != 0) {
		kern_free(pages);
		if (error == ENOMEM)
			return ENOMEM;
		return EFAULT;
	}

	/* Records the pinned range. */
	pin->address = address;
	pin->size = size;
	pin->prot = prot;
	pin->first_offset = address - first;
	pin->page_count = page_count;
	pin->pages = pages;
	pin->active = 1;

	/* Reports the active pin. */
	return 0;
}

/*
 * Pins the pages behind a user range of the current vmspace.
 */
UACCESS_EXT int
uaccess_pin(
	uintptr_t address,
	size_t size,
	uint32_t prot,
	struct uaccess_pin *pin)
{
	struct vmspace *vm;
	int error;

	/* Pins the range in the current vmspace. */
	vm = current_vmspace();
	error = uaccess_pin_vmspace(vm, address, size, prot, pin);

	/* Reports the pin result. */
	return error;
}

/*
 * Releases a pin and its pages.
 */
UACCESS_EXT void
uaccess_unpin(
	struct uaccess_pin *pin)
{
	/* Ignores a missing or inactive pin. */
	if (pin == NULL || !pin->active)
		return;

	/* Releases the pages and clears the pin. */
	vmspace_unpin_user_pages(pin->pages, pin->page_count);
	kern_free(pin->pages);
	memset(pin, 0, sizeof(*pin));
}

/*
 * Copies bytes from a pinned user range into kernel memory.
 *
 * Each page is read through its pinned backing: a private page directly,
 * an object page through the object's pin reader.
 */
UACCESS_EXT int
copyin_pinned(
	const struct uaccess_pin *pin,
	size_t offset,
	void *destination,
	size_t size)
{
	const struct vmspace_pinned_page *page;
	uint8_t *bytes;
	size_t position;
	size_t page_offset;
	size_t chunk;
	int error;

	bytes = destination;

	/* An empty copy succeeds; a missing destination does not. */
	if (size == 0)
		return 0;
	if (destination == NULL)
		return EINVAL;

	/* Locates the range within the pin. */
	error = pinned_range(pin, offset, size, HAL_SPACE_READ, &position);
	if (error != 0)
		return error;

	/* Copies one page at a time. */
	while (size != 0) {
		page = &pin->pages[position / PAGE_SIZE];
		page_offset = position & (PAGE_SIZE - 1U);
		chunk = PAGE_SIZE - page_offset;
		if (chunk > size)
			chunk = size;

		/* Reads through the backing that pinned the page. */
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

	/* Reports the completed copy. */
	return 0;
}

/*
 * Copies bytes from kernel memory into a pinned user range.
 *
 * Each page is written through its pinned backing: a private page directly
 * and then marked dirty, an object page through the object's pin writer.
 */
UACCESS_EXT int
copyout_pinned(
	const struct uaccess_pin *pin,
	size_t offset,
	const void *source,
	size_t size)
{
	const struct vmspace_pinned_page *page;
	const uint8_t *bytes;
	size_t position;
	size_t page_offset;
	size_t chunk;
	int error;

	bytes = source;

	/* An empty copy succeeds; a missing source does not. */
	if (size == 0)
		return 0;
	if (source == NULL)
		return EINVAL;

	/* Locates the range within the pin. */
	error = pinned_range(pin, offset, size, HAL_SPACE_WRITE, &position);
	if (error != 0)
		return error;

	/* Copies one page at a time. */
	while (size != 0) {
		page = &pin->pages[position / PAGE_SIZE];
		page_offset = position & (PAGE_SIZE - 1U);
		chunk = PAGE_SIZE - page_offset;
		if (chunk > size)
			chunk = size;

		/* Writes through the backing that pinned the page. */
		if (page->kind == VMSPACE_PINNED_PRIVATE) {
			memcpy((uint8_t *)page->memory.vaddr + page_offset,
			    bytes, chunk);

			/*
			 * The backing pin excludes reclaim I/O and fork sharing
			 * from the memcpy through dirty publication.  Unmap may
			 * remove metadata but cannot release this saved frame or
			 * its backing reference.
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

	/* Reports the completed copy. */
	return 0;
}

/*
 * Copies bytes from user memory into kernel memory.
 */
int
copyin(
	uintptr_t source,
	void *destination,
	size_t size)
{
	struct vmspace *vm;
	int error;

	/* An empty copy succeeds without a check. */
	if (size == 0)
		return 0;

	/* Rejects a missing destination or an unreadable source range. */
	if (destination == NULL ||
	    user_range_check(source, size, HAL_SPACE_READ) != 0)
		return EFAULT;

	/* Copies through the current vmspace. */
	vm = current_vmspace();
	error = vmspace_copy_from(vm, destination, source, size);

	/* Reports the copy result. */
	return error;
}

/*
 * Copies bytes from kernel memory into user memory.
 */
int
copyout(
	const void *source,
	uintptr_t destination,
	size_t size)
{
	struct vmspace *vm;
	int error;

	/* An empty copy succeeds without a check. */
	if (size == 0)
		return 0;

	/* Rejects a missing source or an unwritable destination range. */
	if (source == NULL ||
	    user_range_check(destination, size, HAL_SPACE_WRITE) != 0)
		return EFAULT;

	/* Copies through the current vmspace. */
	vm = current_vmspace();
	error = vmspace_copy_to(vm, destination, source, size);

	/* Reports the copy result. */
	return error;
}

/*
 * Copies a terminated string from user memory.
 *
 * The string is read one page at a time so that a terminator before an
 * unmapped page succeeds.  Without a terminator within capacity the buffer
 * is terminated anyway and ENAMETOOLONG reported.
 */
int
copyinstr(
	uintptr_t source,
	char *destination,
	size_t capacity,
	size_t *length)
{
	uintptr_t address;
	size_t used;
	size_t chunk;
	size_t index;
	int error;

	used = 0;

	/* Rejects a missing or empty buffer. */
	if (destination == NULL || capacity == 0)
		return EINVAL;

	/* Copies up to the end of each page and scans for the terminator. */
	while (used < capacity) {
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

		/* Reports the length including the terminator once found. */
		for (index = 0; index < chunk; index++) {
			if (destination[used + index] == '\0') {
				if (length != NULL)
					*length = used + index + 1U;
				return 0;
			}
		}
		used += chunk;
	}

	/* Terminates the truncated string. */
	destination[capacity - 1U] = '\0';

	/* Reports an overlong string. */
	return ENAMETOOLONG;
}

/* Finds the vmspace of the current process, or none. */
static struct vmspace *
current_vmspace(
	void)
{
	/* There is no vmspace without a current thread and process. */
	if (curthread == NULL)
		return NULL;
	if (curthread->proc == NULL)
		return NULL;

	/* Reports the process vmspace. */
	return curthread->proc->vmspace;
}

/* Locates a range within an active pin and checks its protection. */
static UACCESS_EXT int
pinned_range(
	const struct uaccess_pin *pin,
	size_t offset,
	size_t size,
	uint32_t prot,
	size_t *position)
{
	/* Rejects an inactive pin, a weaker protection, or a range outside it. */
	if (pin == NULL ||
	    position == NULL ||
	    !pin->active ||
	    pin->pages == NULL ||
	    (pin->prot & prot) != prot ||
	    offset > pin->size ||
	    size > pin->size - offset ||
	    offset > SIZE_MAX - pin->first_offset)
		return EFAULT;

	*position = pin->first_offset + offset;

	/* Reports the position within the pinned pages. */
	return 0;
}
