/*
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#include "kern/exec.h"
#include "kern/file.h"
#include "kern/process.h"
#include "kern/thread.h"
#include "kern/vmspace.h"
#include "kern/filedesc.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>

#define USER_STACK_TOP 0x7ffff000U
#define USER_STACK_SIZE (64U * 1024U)
#define USER_STACK_BOTTOM (USER_STACK_TOP - USER_STACK_SIZE)
#define EXEC_ARG_MAX 32U
#define EXEC_ENV_MAX 64U
#define EXEC_STRING_MAX (16U * 1024U)

int
exec_build_initial_stack(struct vmspace *vm, char *const argv[],
			 char *const envp[], uintptr_t *sp_out)
{
	struct vm_region *region;
	uintptr_t sp;
	size_t total = 0;
	unsigned argc = 0, envc = 0, i;
	uint32_t argv_address[EXEC_ARG_MAX];
	uint32_t env_address[EXEC_ENV_MAX];
	uint32_t words[1U + EXEC_ARG_MAX + 1U + EXEC_ENV_MAX + 1U];
	unsigned word_count = 0;
	int error;

	if (vm == NULL || argv == NULL || argv[0] == NULL || sp_out == NULL)
		return EINVAL;
	while (argv[argc] != NULL) {
		size_t length;
		if (argc >= EXEC_ARG_MAX)
			return E2BIG;
		length = strlen(argv[argc]) + 1U;
		if (length > EXEC_STRING_MAX - total)
			return E2BIG;
		total += length;
		argc++;
	}
	if (envp != NULL)
		while (envp[envc] != NULL) {
			size_t length;
			if (envc >= EXEC_ENV_MAX)
				return E2BIG;
			length = strlen(envp[envc]) + 1U;
			if (length > EXEC_STRING_MAX - total)
				return E2BIG;
			total += length;
			envc++;
		}
	error = vmspace_map_anon(vm, USER_STACK_BOTTOM, USER_STACK_SIZE,
				 HAL_SPACE_READ | HAL_SPACE_WRITE, &region);
	if (error != 0)
		return error;
	(void)region;
	sp = USER_STACK_TOP;
	for (i = envc; i != 0; i--) {
		size_t length = strlen(envp[i - 1U]) + 1U;
		sp -= length;
		error = vmspace_copy_to(vm, sp, envp[i - 1U], length);
		if (error != 0)
			return error;
		env_address[i - 1U] = (uint32_t)sp;
	}
	for (i = argc; i != 0; i--) {
		size_t length = strlen(argv[i - 1U]) + 1U;
		sp -= length;
		error = vmspace_copy_to(vm, sp, argv[i - 1U], length);
		if (error != 0)
			return error;
		argv_address[i - 1U] = (uint32_t)sp;
	}
	sp &= ~(uintptr_t)15U;
	sp -= (1U + argc + 1U + envc + 1U) * sizeof(uint32_t);
	if (sp < USER_STACK_BOTTOM)
		return EOVERFLOW;
	words[word_count++] = argc;
	for (i = 0; i < argc; i++) words[word_count++] = argv_address[i];
	words[word_count++] = 0;
	for (i = 0; i < envc; i++) words[word_count++] = env_address[i];
	words[word_count++] = 0;
	error = vmspace_copy_to(vm, sp, words,
				word_count * sizeof(words[0]));
	if (error != 0)
		return error;
	vm->stack_bottom = USER_STACK_BOTTOM;
	vm->stack_top = USER_STACK_TOP;
	*sp_out = sp;
	return 0;
}

static int
setup_standard_files(struct process *parent, struct process *process)
{
	static const int flags[3] = { O_RDONLY, O_WRONLY, O_WRONLY };
	int descriptor;
	int error = parent != NULL && parent->fd != NULL ?
		filedesc_clone_stdio(parent->fd, process->fd) : 0;
	if (error != 0)
		return error;
	for (descriptor = 0; descriptor < 3; descriptor++) {
		struct file *file;
		if (filedesc_get(process->fd, descriptor) != NULL)
			continue;
		error = file_openat(process->cwdi, "/dev/console",
			flags[descriptor], 0, &file);
		if (error != 0)
			return error;
		error = filedesc_install_at(process->fd, file, descriptor);
		if (error != 0) {
			(void)file_close(file);
			return error;
		}
	}
	return 0;
}

static ssize_t
result_write(struct file *file, const void *buffer, size_t length)
{
	struct process *process = file != NULL ? file->f_data : NULL;
	size_t available;

	if (process == NULL || buffer == NULL)
		return -EINVAL;
	available = (PROCESS_RESULT_MAX - 1U) - process->result_length;
	if (length > available)
		length = available;
	if (length == 0)
		return -ENOSPC;
	memcpy(process->result + process->result_length, buffer, length);
	process->result_length += length;
	process->result[process->result_length] = '\0';
	return (ssize_t)length;
}

static const struct file_ops result_ops = {
	.write = result_write,
};

static int
setup_result_file(struct process *process)
{
	struct file *file;
	int error = file_create_pseudo(&result_ops, O_WRONLY, process, &file);

	if (error != 0)
		return error;
	error = filedesc_install_at(process->fd, file, 3);
	if (error != 0)
		(void)file_close(file);
	return error;
}

int
process_spawn_from(struct process *parent, const char *path,
		   char *const argv[], char *const envp[], unsigned flags,
		   struct process **result)
{
	struct file *file = NULL;
	struct process *process = NULL;
	struct thread *thread;
	char *result_envp[EXEC_ENV_MAX + 1U];
	char *const *effective_envp = envp;
	uintptr_t entry, sp;
	int error;
	unsigned env_count = 0;

	if (parent == NULL || path == NULL || argv == NULL || argv[0] == NULL ||
	    parent->cwdi == NULL)
		return EINVAL;
	if ((flags & ~PROCESS_SPAWN_RESULT) != 0)
		return EINVAL;
	if ((flags & PROCESS_SPAWN_RESULT) != 0) {
		while (envp != NULL && envp[env_count] != NULL) {
			if (env_count + 1U >= EXEC_ENV_MAX)
				return E2BIG;
			result_envp[env_count] = envp[env_count];
			env_count++;
		}
		result_envp[env_count++] = "ZEDBSD_RESULT_FD=3";
		result_envp[env_count] = NULL;
		effective_envp = result_envp;
	}
	error = file_openat(parent->cwdi, path, O_RDONLY, 0, &file);
	if (error != 0)
		return error;
	error = process_create(parent, 0, &process);
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
	error = exec_build_initial_stack(process->vmspace, argv,
		effective_envp, &sp);
	if (error != 0)
		goto out;
	error = setup_standard_files(parent, process);
	if (error != 0)
		goto out;
	if ((flags & PROCESS_SPAWN_RESULT) != 0) {
		error = setup_result_file(process);
		if (error != 0)
			goto out;
	}
	error = thread_create(process, entry, sp, &thread);
	if (error != 0)
		goto out;
	if (result == NULL)
		process->flags |= PROCESS_AUTOREAP;
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

int
process_spawn(const char *path, char *const argv[], char *const envp[],
	      unsigned flags, struct process **result)
{
	return process_spawn_from(&process0, path, argv, envp, flags, result);
}

int
process_spawn_init(const char *path, struct process **result)
{
	char *argv[2];
	char *envp[] = {
		"HOME=/home",
		"PATH=/bin:/cmd",
		"REMACS_SKK_DICT=/home/skkjisyo.dic",
		NULL
	};
	argv[0] = (char *)path;
	argv[1] = NULL;
	return process_spawn(path, argv, envp, 0, result);
}
