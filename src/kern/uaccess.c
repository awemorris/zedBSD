/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/uaccess.h"
#include "kern/process.h"
#include "kern/page.h"
#include "kern/thread.h"
#include "kern/vmspace.h"

#include <errno.h>
#include <hal/hal.h>
#include <limits.h>
#include <string.h>

#define UACCESS_EXT __attribute__((section(".hightext")))

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
uaccess_pin(uintptr_t address, size_t size, uint32_t prot,
	    struct uaccess_pin *pin)
{
	struct vmspace *vm;
	int error;

	if (pin == NULL)
		return EINVAL;
	memset(pin, 0, sizeof(*pin));
	if (size == 0)
		return 0;
	vm = current_vmspace();
	if (!vmspace_tryref(vm))
		return EFAULT;
	error = vmspace_wire_range(vm, address, size, prot);
	if (error != 0) {
		vmspace_free(vm);
		return error == ENOMEM ? ENOMEM : EFAULT;
	}
	pin->vm = vm;
	pin->address = address;
	pin->size = size;
	pin->prot = prot;
	pin->active = 1;
	return 0;
}

UACCESS_EXT void
uaccess_unpin(struct uaccess_pin *pin)
{
	if (pin == NULL || !pin->active)
		return;
	vmspace_unwire_range(pin->vm, pin->address, pin->size);
	vmspace_free(pin->vm);
	memset(pin, 0, sizeof(*pin));
}

static UACCESS_EXT int
pinned_range(const struct uaccess_pin *pin, size_t offset, size_t size,
	    uint32_t prot, uintptr_t *address)
{
	if (pin == NULL || address == NULL || !pin->active ||
	    (pin->prot & prot) != prot || offset > pin->size ||
	    size > pin->size - offset)
		return EFAULT;
	return user_address_add(pin->address, offset, address);
}

UACCESS_EXT int
copyin_pinned(const struct uaccess_pin *pin, size_t offset,
	      void *destination, size_t size)
{
	uintptr_t address;
	int error;

	if (size == 0)
		return 0;
	if (destination == NULL)
		return EINVAL;
	error = pinned_range(pin, offset, size, HAL_SPACE_READ, &address);
	return error != 0 ? error :
		vmspace_copy_from(pin->vm, destination, address, size);
}

UACCESS_EXT int
copyout_pinned(const struct uaccess_pin *pin, size_t offset,
	       const void *source, size_t size)
{
	uintptr_t address;
	int error;

	if (size == 0)
		return 0;
	if (source == NULL)
		return EINVAL;
	error = pinned_range(pin, offset, size, HAL_SPACE_WRITE, &address);
	return error != 0 ? error :
		vmspace_copy_to(pin->vm, address, source, size);
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
