/*
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#include "kern/exec.h"
#include "kern/cred.h"
#include "kern/file.h"
#include "kern/process.h"
#include "kern/process-timer.h"
#include "kern/thread.h"
#include "kern/tty.h"
#include "kern/vmspace.h"
#include "kern/filedesc.h"
#include "kern/signal.h"
#include "kern/resource-limit.h"

#include <zedbsd/auxv.h>
#include <errno.h>
#include <fcntl.h>
#include <hal/hal.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#define EXEC_ARG_MAX 32U
#define EXEC_ENV_MAX 64U
#define EXEC_STRING_MAX (16U * 1024U)

#ifdef ZEDBSD_USER_ABI_LP64
typedef uintptr_t exec_user_word_t;
#define EXEC_IMAGE_INFO struct elf64_image_info
#define exec_elf_load elf64_load
#define exec_elf_load_interpreter elf64_load_interpreter
#else
typedef uint32_t exec_user_word_t;
#define EXEC_IMAGE_INFO struct elf32_image_info
#define exec_elf_load elf32_load
#define exec_elf_load_interpreter elf32_load_interpreter
#endif

#define EXEC_AUXV_PAIRS 13U

int
exec_build_initial_stack(struct vmspace *vm, size_t stack_size,
			 char *const argv[],
			 char *const envp[], const struct exec_auxv_info *aux,
			 uintptr_t *sp_out)
{
	uintptr_t sp;
	size_t total = 0;
	size_t table_size;
	unsigned argc = 0, envc = 0, i;
	exec_user_word_t argv_address[EXEC_ARG_MAX];
	exec_user_word_t env_address[EXEC_ENV_MAX];
	exec_user_word_t execfn_address;
	exec_user_word_t words[1U + EXEC_ARG_MAX + 1U + EXEC_ENV_MAX + 1U +
	    EXEC_AUXV_PAIRS * 2U];
	unsigned word_count = 0;
	int error;

	if (vm == NULL || argv == NULL || argv[0] == NULL || aux == NULL ||
	    aux->exec_path == NULL || sp_out == NULL)
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
	{
		size_t length = strlen(aux->exec_path) + 1U;
		if (length > EXEC_STRING_MAX - total)
			return E2BIG;
		total += length;
	}
	vmspace_layout_init();
	error = vmspace_map_stack(vm, vm_layout.stack_top, stack_size,
				  EXEC_STACK_GUARD_SIZE);
	if (error != 0)
		return error;
	sp = vm->stack_top;
	{
		size_t length = strlen(aux->exec_path) + 1U;
		sp -= length;
		error = vmspace_copy_to(vm, sp, aux->exec_path, length);
		if (error != 0)
			return error;
		execfn_address = (exec_user_word_t)sp;
	}
	for (i = envc; i != 0; i--) {
		size_t length = strlen(envp[i - 1U]) + 1U;
		sp -= length;
		error = vmspace_copy_to(vm, sp, envp[i - 1U], length);
		if (error != 0)
			return error;
		env_address[i - 1U] = (exec_user_word_t)sp;
	}
	for (i = argc; i != 0; i--) {
		size_t length = strlen(argv[i - 1U]) + 1U;
		sp -= length;
		error = vmspace_copy_to(vm, sp, argv[i - 1U], length);
		if (error != 0)
			return error;
		argv_address[i - 1U] = (exec_user_word_t)sp;
	}
	table_size = (1U + argc + 1U + envc + 1U +
	    EXEC_AUXV_PAIRS * 2U) * sizeof(exec_user_word_t);
	if (sp < table_size)
		return EOVERFLOW;
	sp = (sp - table_size) & ~(uintptr_t)15U;
	if (sp < vm->stack_bottom)
		return EOVERFLOW;
	words[word_count++] = argc;
	for (i = 0; i < argc; i++) words[word_count++] = argv_address[i];
	words[word_count++] = 0;
	for (i = 0; i < envc; i++) words[word_count++] = env_address[i];
	words[word_count++] = 0;
#define APPEND_AUX(type, value) do { \
	words[word_count++] = (exec_user_word_t)(type); \
	words[word_count++] = (exec_user_word_t)(value); \
} while (0)
	APPEND_AUX(AT_PHDR, aux->program_headers);
	APPEND_AUX(AT_PHENT, aux->program_header_size);
	APPEND_AUX(AT_PHNUM, aux->program_header_count);
	APPEND_AUX(AT_PAGESZ, ZEDBSD_PAGE_SIZE);
	APPEND_AUX(AT_BASE, aux->interpreter_base);
	APPEND_AUX(AT_ENTRY, aux->program_entry);
	APPEND_AUX(AT_UID, aux->uid);
	APPEND_AUX(AT_EUID, aux->euid);
	APPEND_AUX(AT_GID, aux->gid);
	APPEND_AUX(AT_EGID, aux->egid);
	APPEND_AUX(AT_SECURE, 0);
	APPEND_AUX(AT_EXECFN, execfn_address);
	APPEND_AUX(AT_NULL, 0);
#undef APPEND_AUX
	if (word_count != 1U + argc + 1U + envc + 1U +
	    EXEC_AUXV_PAIRS * 2U)
		return EOVERFLOW;
	error = vmspace_copy_to(vm, sp, words,
				word_count * sizeof(words[0]));
	if (error != 0)
		return error;
	*sp_out = sp;
	return 0;
}

static int
load_executable(struct cwdinfo *cwdi, const struct ucred *cred,
	const char *path, struct file *file, struct vmspace *vm,
	EXEC_IMAGE_INFO *image, uintptr_t *execution_entry,
	uintptr_t *interpreter_base)
{
	struct file *interpreter_file = NULL;
	EXEC_IMAGE_INFO interpreter;
	int error;

	if (cwdi == NULL || cred == NULL || path == NULL || file == NULL ||
	    vm == NULL || image == NULL || execution_entry == NULL ||
	    interpreter_base == NULL)
		return EINVAL;
	error = exec_elf_load(file, vm, image);
	if (error != 0)
		return error;
	*execution_entry = image->entry;
	*interpreter_base = 0;
	if (!image->has_interpreter)
		return 0;
	error = file_openat_cred(cwdi, cred, image->interpreter, O_RDONLY, 0,
	    &interpreter_file);
	if (error == 0)
		error = vfs_access(interpreter_file->f_inode, cred, X_OK);
	if (error == 0)
		error = exec_elf_load_interpreter(interpreter_file, vm, &interpreter);
	if (error == 0) {
		*execution_entry = interpreter.entry;
		*interpreter_base = interpreter.load_bias;
	}
	if (interpreter_file != NULL)
		(void)file_close(interpreter_file);
	return error;
}

static void
fill_auxv_info(struct exec_auxv_info *aux, const EXEC_IMAGE_INFO *image,
	uintptr_t interpreter_base, const struct ucred *cred, const char *path)
{
	memset(aux, 0, sizeof(*aux));
	aux->program_headers = image->program_headers;
	aux->interpreter_base = interpreter_base;
	aux->program_entry = image->entry;
	aux->program_header_size = image->program_header_size;
	aux->program_header_count = image->program_header_count;
	aux->uid = cred->ruid;
	aux->euid = cred->euid;
	aux->gid = cred->rgid;
	aux->egid = cred->egid;
	aux->exec_path = path;
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
		file = filedesc_get_ref(process->fd, descriptor);
		if (file != NULL) {
			(void)file_close(file);
			continue;
		}
		error = file_openat_cred(process->cwdi, process->cred, "/dev/console",
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

int
process_spawn_from(struct process *parent, const char *path,
		   char *const argv[], char *const envp[],
		   struct process **result)
{
	struct file *file = NULL;
	struct process *process = NULL;
	struct thread *thread;
	EXEC_IMAGE_INFO image;
	struct exec_auxv_info aux;
	uintptr_t sp;
	uintptr_t execution_entry, interpreter_base;
	int error;
	const char *stage = "open executable";

	if (parent == NULL || path == NULL || argv == NULL || argv[0] == NULL ||
	    parent->cwdi == NULL)
		return EINVAL;
	error = file_openat_cred(parent->cwdi, parent->cred, path, O_RDONLY, 0,
	    &file);
	if (error != 0)
		goto out;
	stage = "check execute access";
	error = vfs_access(file->f_inode, parent->cred, X_OK);
	if (error != 0)
		goto out;
	stage = "create process";
	error = process_create(parent, 0, &process);
	if (error != 0)
		goto out;
	process->vmspace = vmspace_create();
	if (process->vmspace == NULL) {
		error = ENOMEM;
		goto out;
	}
	error = resource_limit_apply_vm(process, process->vmspace);
	if (error != 0)
		goto out;
	stage = "load ELF";
	error = load_executable(process->cwdi, process->cred, path, file,
	    process->vmspace, &image, &execution_entry, &interpreter_base);
	if (error != 0)
		goto out;
	stage = "set brk";
	error = vmspace_set_brk_start(process->vmspace, image.brk_start);
	if (error != 0)
		goto out;
	stage = "build initial stack";
	fill_auxv_info(&aux, &image, interpreter_base, process->cred, path);
	error = exec_build_initial_stack(process->vmspace, image.stack_size, argv,
		envp, &aux, &sp);
	if (error != 0)
		goto out;
	stage = "open standard files";
	error = setup_standard_files(parent, process);
	if (error != 0)
		goto out;
	tty_attach_console(process);
	stage = "create initial thread";
	strncpy(process->command, argv[0], sizeof(process->command) - 1U);
	process->command[sizeof(process->command) - 1U] = '\0';
	error = thread_create(process, execution_entry, sp, &thread);
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
	if (error != 0)
		hal_printf("exec: %s failed for %s (%d)\n", stage, path, error);
	if (file != NULL)
		(void)file_close(file);
	if (process != NULL)
		process_free_mem(process);
	return error;
}

static int
process_exec_file(struct process *process, const char *path,
	struct file *provided_file, char *const argv[], char *const envp[])
{
	struct vmspace *new_vm = NULL, *old_vm;
	struct file *file = provided_file;
	EXEC_IMAGE_INFO image;
	struct exec_auxv_info aux;
	uintptr_t sp;
	uintptr_t execution_entry, interpreter_base;
	bool irq_enabled;
	unsigned long process_irq;
	int error;

	if (process == NULL || process == &process0 || process != curthread->proc ||
	    process->cwdi == NULL || path == NULL ||
	    argv == NULL || argv[0] == NULL)
		return EINVAL;
	process_irq = spin_lock_irqsave(&process->lock);
	if (process->execing) {
		spin_unlock_irqrestore(&process->lock, process_irq);
		return EBUSY;
	}
	process->execing = 1;
	spin_unlock_irqrestore(&process->lock, process_irq);
	if (file != NULL)
		file_ref(file);
	else {
		error = file_openat_cred(process->cwdi, process->cred, path,
		    O_RDONLY, 0, &file);
		if (error != 0)
			goto out;
	}
	if (file->f_inode == NULL || file->f_inode->i_type != INODE_REG) {
		error = EACCES;
		goto out;
	}
	error = vfs_access(file->f_inode, process->cred, X_OK);
	if (error != 0)
		goto out;
	new_vm = vmspace_create();
	if (new_vm == NULL) {
		error = ENOMEM;
		goto out;
	}
	error = resource_limit_apply_vm(process, new_vm);
	if (error != 0)
		goto out;
	error = load_executable(process->cwdi, process->cred, path, file, new_vm,
	    &image, &execution_entry, &interpreter_base);
	if (error == 0)
		error = vmspace_set_brk_start(new_vm, image.brk_start);
	if (error == 0) {
		fill_auxv_info(&aux, &image, interpreter_base, process->cred, path);
		error = exec_build_initial_stack(new_vm, image.stack_size, argv, envp,
		    &aux, &sp);
	}
	if (error != 0)
		goto out;
	/* POSIX exec keeps only the calling thread.  Publish a kernel-only
	 * retirement request and wait until every other task has crossed a
	 * syscall/interrupt return boundary before replacing the vmspace. */
	for (;;) {
		struct thread *other = NULL;
		process_irq = spin_lock_irqsave(&process->lock);
		for (other = process->threads; other != NULL;
		    other = other->proc_next) {
			if (other == curthread || other->state == THREAD_DEAD ||
			    other->state == THREAD_REAPING)
				continue;
			thread_ref(other);
			if (other->state != THREAD_ZOMBIE)
				other->terminate_requested = 1;
			break;
		}
		spin_unlock_irqrestore(&process->lock, process_irq);
		if (other == NULL)
			break;
		if (other->state != THREAD_ZOMBIE) {
			sched_wakeup(other);
			while (other->state != THREAD_ZOMBIE)
				sched_sleep(sched_ticks() + 1U);
		}
		(void)thread_wait(other, NULL);
		thread_release(other);
	}
	old_vm = process->vmspace;
	irq_enabled = hal_irq_disable();
	error = hal_task_exec_current(new_vm->space, execution_entry, sp) == 0 ?
	    0 : EINVAL;
	if (error == 0) {
		process->vmspace = new_vm;
		new_vm = NULL;
	}
	if (irq_enabled)
		hal_irq_enable();
	if (error != 0)
		goto out;
	filedesc_close_on_exec(process->fd);
	process_timer_cleanup(process);
	signal_exec(process);
	process->did_exec = 1;
	strncpy(process->command, argv[0], sizeof(process->command) - 1U);
	process->command[sizeof(process->command) - 1U] = '\0';
	vmspace_free(old_vm);
out:
	process_irq = spin_lock_irqsave(&process->lock);
	process->execing = 0;
	spin_unlock_irqrestore(&process->lock, process_irq);
	if (file != NULL)
		(void)file_close(file);
	if (new_vm != NULL)
		vmspace_free(new_vm);
	return error;
}

int
process_execve(struct process *process, const char *path, char *const argv[],
	char *const envp[])
{
	return process_exec_file(process, path, NULL, argv, envp);
}

int
process_fexecve(struct process *process, struct file *file,
	char *const argv[], char *const envp[])
{
	const char *label = argv != NULL && argv[0] != NULL ? argv[0] : "fexecve";
	return process_exec_file(process, label, file, argv, envp);
}

int
process_spawn(const char *path, char *const argv[], char *const envp[],
	      struct process **result)
{
	return process_spawn_from(&process0, path, argv, envp, result);
}

int
process_spawn_init(const char *path, struct process **result)
{
	char *argv[2];
	char *envp[] = {
		"HOME=/home",
		"PATH=/bin:/usr/bin",
		"REMACS_SKK_DICT=/home/skkjisyo.dic",
		NULL
	};
	argv[0] = (char *)path;
	argv[1] = NULL;
	return process_spawn(path, argv, envp, result);
}
