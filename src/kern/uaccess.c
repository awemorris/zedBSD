/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/uaccess.h"
#include "kern/process.h"
#include "kern/thread.h"
#include "kern/vmspace.h"

#include <errno.h>
#include <hal/hal.h>
#include <string.h>

static struct vmspace *current_vmspace(void)
{
	return curthread != NULL && curthread->proc != NULL ?
		curthread->proc->vmspace : NULL;
}

int user_range_check(uintptr_t address, size_t size, uint32_t prot)
{
	return vmspace_check(current_vmspace(), address, size, prot);
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
	size_t used;

	if (destination == NULL || capacity == 0)
		return EINVAL;
	for (used = 0; used < capacity; used++) {
		char byte;
		int error = copyin(source + used, &byte, 1);
		if (error != 0)
			return error;
		destination[used] = byte;
		if (byte == '\0') {
			if (length != NULL)
				*length = used + 1U;
			return 0;
		}
	}
	destination[capacity - 1U] = '\0';
	return ENAMETOOLONG;
}
