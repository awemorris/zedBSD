/*
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#include "kern/exec.h"
#include "kern/file.h"
#include "kern/process.h"
#include "kern/thread.h"
#include "kern/vmspace.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>

#define USER_STACK_TOP 0x7ffff000U
#define USER_STACK_SIZE (64U * 1024U)
#define USER_STACK_BOTTOM (USER_STACK_TOP - USER_STACK_SIZE)

int
exec_build_initial_stack(struct vmspace *vm, const char *name, uintptr_t *sp_out)
{
	struct vm_region *region;
	uintptr_t sp, string_address;
	uint8_t *base;
	size_t length;
	uint32_t *words;
	int error;

	if (vm == NULL || name == NULL || sp_out == NULL)
		return EINVAL;
	length = strlen(name) + 1U;
	if (length > 256U)
		return ENAMETOOLONG;
	error = vmspace_map_anon(vm, USER_STACK_BOTTOM, USER_STACK_SIZE,
				 HAL_SPACE_READ | HAL_SPACE_WRITE, &region);
	if (error != 0)
		return error;
	base = (uint8_t *)region->pmem.vaddr;
	sp = USER_STACK_TOP - length;
	string_address = sp;
	memcpy(base + (sp - USER_STACK_BOTTOM), name, length);
	sp &= ~(uintptr_t)15U;
	sp -= 4U * sizeof(uint32_t);
	if (sp < USER_STACK_BOTTOM)
		return EOVERFLOW;
	words = (uint32_t *)(base + (sp - USER_STACK_BOTTOM));
	words[0] = 1;
	words[1] = (uint32_t)string_address;
	words[2] = 0;
	words[3] = 0;
	vm->stack_bottom = USER_STACK_BOTTOM;
	vm->stack_top = USER_STACK_TOP;
	*sp_out = sp;
	return 0;
}

int
process_spawn_init(const char *path, struct process **result)
{
	struct file *file = NULL;
	struct process *process = NULL;
	struct thread *thread;
	uintptr_t entry, sp;
	int error;

	if (path == NULL || process0.cwdi == NULL)
		return EINVAL;
	if (process_find(1) != NULL)
		return EBUSY;
	error = file_openat(process0.cwdi, path, O_RDONLY, 0, &file);
	if (error != 0)
		return error;
	error = process_create(&process0, 1, &process);
	if (error != 0)
		goto out;
	process->vmspace = vmspace_create();
	if (process->vmspace == NULL) {
		error = ENOMEM;
		goto out;
	}
	error = elf32_load(file, process->vmspace, &entry);
	if (error != 0)
		goto out;
	error = exec_build_initial_stack(process->vmspace, path, &sp);
	if (error != 0)
		goto out;
	error = thread_create(process, entry, sp, &thread);
	if (error != 0)
		goto out;
	process_publish(process);
	thread_start(thread);
	if (result != NULL)
		*result = process;
	process = NULL;
out:
	if (file != NULL)
		(void)file_close(file);
	if (process != NULL)
		process_free_mem(process);
	return error;
}
