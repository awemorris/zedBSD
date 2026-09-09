/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Program execution.
 *
 * exec and spawn resolve a path through any #! interpreters, load the
 * ELF image and its dynamic linker into a fresh address space, build the
 * initial stack with the arguments, environment, and auxiliary vector,
 * and switch the process to the new image.  The executable's identity is
 * revalidated around the loader so that a set-id change cannot race the
 * credential transition.
 */

#include "kern/exec.h"
#include "kern/cred.h"
#include "kern/file.h"
#include "kern/kmem.h"
#include "kern/mount.h"
#include "kern/process.h"
#include "kern/process-timer.h"
#include "kern/thread.h"
#include "kern/tty.h"
#include "kern/vmspace.h"
#include "kern/filedesc.h"
#include "kern/signal.h"
#include "kern/resource-limit.h"
#include "kern/test-checkpoint.h"

#include <zedbsd/auxv.h>
#include <zedbsd/process.h>
#include <errno.h>
#include <fcntl.h>
#include <hal/hal.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#define EXEC_ARG_MAX ZEDBSD_SPAWN_ARG_MAX
#define EXEC_ENV_MAX ZEDBSD_SPAWN_ENV_MAX
#define EXEC_STRING_MAX ZEDBSD_ARG_MAX

#ifdef ZEDBSD_USER_ABI_LP64
typedef uintptr_t exec_user_word_t;
#define EXEC_IMAGE_INFO struct elf64_image_info
#define exec_elf_load elf64_load
#define exec_elf_load_content elf64_load_content
#define exec_elf_load_interpreter elf64_load_interpreter
#else
typedef uint32_t exec_user_word_t;
#define EXEC_IMAGE_INFO struct elf32_image_info
#define exec_elf_load elf32_load
#define exec_elf_load_content elf32_load_content
#define exec_elf_load_interpreter elf32_load_interpreter
#endif

#define EXEC_AUXV_PAIRS 13U

struct exec_target {
	struct file *file;
	struct file_content_lease lease;
	char **argv;
	unsigned argv_owned;
	unsigned script_depth;
	struct stat status;
	unsigned mount_flags;
};

static int exec_bounded_length(const char *string, size_t maximum, size_t *result);
static int script_argv_count(const char *value, size_t table_bytes, size_t *string_bytes);
static void script_argv_copy(char **vector, unsigned *index, char **cursor, const char *value);
static void exec_commit_release_lease(struct file_content_lease *lease, struct process *process);
static int load_executable(struct cwdinfo *cwdi, const struct ucred *cred, const char *path, struct file *file, struct file_content_lease *lease, struct vmspace *vm, EXEC_IMAGE_INFO *image, uintptr_t *execution_entry, uintptr_t *interpreter_base);
static void exec_target_release(struct exec_target *target);
static int exec_target_resolve(struct cwdinfo *cwdi, const struct ucred *cred, const char *path, struct file *provided_file, int reopenable_path, char *const argv[], struct exec_target *target);
static int exec_target_revalidate(const struct exec_target *target, const struct ucred *cred);
static void fill_auxv_info(struct exec_auxv_info *aux, const EXEC_IMAGE_INFO *image, uintptr_t interpreter_base, const struct ucred *cred, unsigned secure, const char *path);
static int setup_standard_files(struct process *parent, struct process *process, const struct ucred *credential);
static int process_exec_file(struct process *process, const char *path, struct file *provided_file, int reopenable_path, char *const argv[], char *const envp[]);

/*
 * Parses a #! line at the start of a file.
 *
 * Reports 1 with the interpreter and its optional argument, 0 for a file
 * without a #! line, and a negative error for a malformed one.  The
 * line must be complete unless the buffer holds the whole file.
 */
int
exec_shebang_parse(
	const void *contents,
	size_t size,
	int at_eof,
	struct exec_shebang *result)
{
	const unsigned char *bytes;
	size_t begin;
	size_t end;
	size_t path_end;
	size_t argument_begin;
	size_t argument_end;
	size_t i;
	size_t length;

	bytes = contents;

	/* Rejects missing contents or a missing result. */
	if ((contents == NULL && size != 0) || result == NULL)
		return -EINVAL;
	memset(result, 0, sizeof(*result));

	/* A file without the magic has no interpreter. */
	if (size < 2U || bytes[0] != '#' || bytes[1] != '!')
		return 0;

	/* Finds the end of the line, refusing control characters. */
	for (end = 2U; end < size && bytes[end] != '\n'; end++) {
		if (bytes[end] == '\0' ||
		    (bytes[end] < 0x20U &&
		     bytes[end] != '\t' &&
		     bytes[end] != '\r'))
			return -ENOEXEC;
	}

	if (end == size && !at_eof)
		return -E2BIG;
	if (end > EXEC_SHEBANG_LINE_MAX)
		return -E2BIG;
	if (end > 2U && bytes[end - 1U] == '\r')
		end--;

	/* The interpreter is an absolute path after optional blanks. */
	begin = 2U;
	while (begin < end && (bytes[begin] == ' ' || bytes[begin] == '\t'))
		begin++;
	if (begin == end || bytes[begin] != '/')
		return -ENOEXEC;
	for (path_end = begin;
	    path_end < end && bytes[path_end] != ' ' && bytes[path_end] != '\t';
	    path_end++) {
		if (bytes[path_end] == '\r')
			return -ENOEXEC;
	}

	if (path_end - begin >= sizeof(result->interpreter))
		return -ENAMETOOLONG;
	memcpy(result->interpreter, bytes + begin, path_end - begin);
	result->interpreter[path_end - begin] = '\0';

	/*
	 * Historical shebang handling passes the trimmed remainder of the
	 * line as one optional argument.  Embedded whitespace does not split
	 * it into multiple argv entries.
	 */
	argument_begin = path_end;
	while (argument_begin < end &&
	    (bytes[argument_begin] == ' ' || bytes[argument_begin] == '\t'))
		argument_begin++;
	argument_end = end;
	while (argument_end > argument_begin &&
	    (bytes[argument_end - 1U] == ' ' ||
	    bytes[argument_end - 1U] == '\t'))
		argument_end--;
	for (i = argument_begin; i < argument_end; i++) {
		if (bytes[i] == '\r')
			return -ENOEXEC;
	}

	if (argument_end != argument_begin) {
		length = argument_end - argument_begin;
		if (length >= sizeof(result->optional_argument))
			return -E2BIG;
		memcpy(result->optional_argument, bytes + argument_begin, length);
		result->optional_argument[length] = '\0';
		result->has_optional_argument = 1;
	}

	/* Reports a parsed interpreter line. */
	return 1;
}

/*
 * Builds the argument vector of a script interpreter.
 *
 * The vector is the interpreter, its optional argument, the script path,
 * and the old arguments after the first, all in one allocation that the
 * caller frees with exec_script_argv_free().
 */
int
exec_script_argv_build(
	char *const old_argv[],
	const struct exec_shebang *shebang,
	const char *script_path,
	char ***result)
{
	char **vector;
	char *cursor;
	size_t old_count;
	size_t new_count;
	size_t table_bytes;
	size_t string_bytes;
	unsigned index;
	int error;

	old_count = 0;
	string_bytes = 0;
	index = 0;

	/* Rejects a missing operand or an empty vector or interpreter. */
	if (old_argv == NULL ||
	    old_argv[0] == NULL ||
	    shebang == NULL ||
	    shebang->interpreter[0] == '\0' ||
	    script_path == NULL ||
	    script_path[0] == '\0' ||
	    result == NULL)
		return EINVAL;

	/*
	 * Counts the old vector without probing beyond a caller-supplied
	 * array merely to distinguish an exactly-full unterminated array
	 * from an oversized one.
	 */
	while (old_count < ZEDBSD_EXEC_VECTOR_MAX && old_argv[old_count] != NULL)
		old_count++;
	if (old_count == ZEDBSD_EXEC_VECTOR_MAX)
		return E2BIG;
	new_count = old_count + 1U + shebang->has_optional_argument;
	if (new_count > ZEDBSD_EXEC_VECTOR_MAX)
		return E2BIG;
	table_bytes = (new_count + 1U) * sizeof(*vector);
	if (table_bytes > ZEDBSD_ARG_MAX)
		return E2BIG;

	/* Sizes the strings against the argument limit. */
	error = script_argv_count(shebang->interpreter, table_bytes,
	    &string_bytes);
	if (error != 0)
		return error;
	if (shebang->has_optional_argument) {
		error = script_argv_count(shebang->optional_argument, table_bytes,
		    &string_bytes);
		if (error != 0)
			return error;
	}

	error = script_argv_count(script_path, table_bytes, &string_bytes);
	if (error != 0)
		return error;
	for (old_count = 1U; old_argv[old_count] != NULL; old_count++) {
		error = script_argv_count(old_argv[old_count], table_bytes,
		    &string_bytes);
		if (error != 0)
			return error;
	}

	/* Allocates the table with the strings behind it and fills both. */
	vector = kern_calloc(1, table_bytes + string_bytes);
	if (vector == NULL)
		return ENOMEM;
	cursor = (char *)(vector + new_count + 1U);
	script_argv_copy(vector, &index, &cursor, shebang->interpreter);
	if (shebang->has_optional_argument)
		script_argv_copy(vector, &index, &cursor, shebang->optional_argument);
	script_argv_copy(vector, &index, &cursor, script_path);
	for (old_count = 1U; old_argv[old_count] != NULL; old_count++)
		script_argv_copy(vector, &index, &cursor, old_argv[old_count]);
	vector[index] = NULL;
	*result = vector;

	/* Reports the built vector. */
	return 0;
}

/*
 * Frees a vector built by exec_script_argv_build().
 */
void
exec_script_argv_free(
	char **argv)
{
	kern_free(argv);
}

/*
 * Applies the set-id bits of an executable to a credential.
 *
 * Scripts and nosuid mounts get no privilege change.  The saved ids
 * follow the effective ones, and secure reports whether any id differs
 * from its real counterpart.
 */
void
exec_credential_prepare(
	struct ucred *credential,
	const struct stat *status,
	unsigned mount_flags,
	int script,
	unsigned *secure)
{
	/* Ignores a missing credential. */
	if (credential == NULL)
		return;

	/* Takes the owner and group of a set-id executable. */
	if (!script && status != NULL && (mount_flags & MOUNT_NOSUID) == 0) {
		if ((status->st_mode & S_ISUID) != 0)
			credential->euid = status->st_uid;
		if ((status->st_mode & S_ISGID) != 0)
			credential->egid = status->st_gid;
	}

	credential->suid = credential->euid;
	credential->sgid = credential->egid;

	/* A privilege change makes the process secure. */
	if (secure != NULL) {
		if (credential->ruid != credential->euid ||
		    credential->rgid != credential->egid)
			*secure = 1;
		else
			*secure = 0;
	}
}

#ifdef ZEDBSD_EXEC_COMMIT_HOST_TEST
/*
 * Releases a content lease at the exec commit boundary for a host test.
 */
void
exec_commit_host_release_lease(
	struct file_content_lease *lease)
{
	exec_commit_release_lease(lease, NULL);
}
#endif

/*
 * Builds the initial user stack of a new image.
 *
 * The strings go at the top, then the argument, environment, and
 * auxiliary tables, with the stack pointer left 16-byte aligned at the
 * argument count.
 */
int
exec_build_initial_stack(
	struct vmspace *vm,
	size_t stack_size,
	char *const argv[],
	char *const envp[],
	const struct exec_auxv_info *aux,
	uintptr_t *sp_out)
{
	exec_user_word_t *address;
	exec_user_word_t *argv_address;
	exec_user_word_t *env_address;
	exec_user_word_t execfn_address;
	exec_user_word_t *words;
	uintptr_t sp;
	size_t total;
	size_t table_size;
	size_t length;
	unsigned argc;
	unsigned envc;
	unsigned i;
	unsigned word_count;
	int error;

	/* Starts with no stack, no words, and empty vectors. */
	address = NULL;
	words = NULL;
	total = 0;
	argc = 0;
	envc = 0;
	word_count = 0;

	/* Rejects a missing operand or an empty argument vector. */
	if (vm == NULL ||
	    argv == NULL ||
	    argv[0] == NULL ||
	    aux == NULL ||
	    aux->exec_path == NULL ||
	    sp_out == NULL)
		return EINVAL;

	/* Counts the strings against the limits. */
	while (argv[argc] != NULL) {
		if (argc >= EXEC_ARG_MAX)
			return E2BIG;
		length = strlen(argv[argc]) + 1U;
		if (length > EXEC_STRING_MAX - total)
			return E2BIG;
		total += length;
		argc++;
	}

	if (envp != NULL) {
		while (envp[envc] != NULL) {
			if (envc >= EXEC_ENV_MAX)
				return E2BIG;
			length = strlen(envp[envc]) + 1U;
			if (length > EXEC_STRING_MAX - total)
				return E2BIG;
			total += length;
			envc++;
		}
	}

	length = strlen(aux->exec_path) + 1U;
	if (length > EXEC_STRING_MAX - total)
		return E2BIG;
	total += length;
	table_size = (1U + argc + 1U + envc + 1U + EXEC_AUXV_PAIRS * 2U) *
		     sizeof(exec_user_word_t);
	if (table_size > EXEC_STRING_MAX - total)
		return E2BIG;

	/* Allocates the pointer scratch and the table image. */
	address = kern_calloc(argc + envc, sizeof(*address));
	words = kern_calloc(1U + argc + 1U + envc + 1U + EXEC_AUXV_PAIRS * 2U,
			    sizeof(*words));
	if ((argc + envc != 0U && address == NULL) || words == NULL) {
		error = ENOMEM;
		goto out;
	}

	argv_address = address;
	env_address = address + argc;

	/* Maps the stack. */
	vmspace_layout_init();
	error = vmspace_map_stack(vm, vm_layout.stack_top, stack_size,
				  EXEC_STACK_GUARD_SIZE);
	if (error != 0)
		goto out;
	sp = vm->stack_top;

	/* Copies the executable path and the strings from the top down. */
	length = strlen(aux->exec_path) + 1U;
	sp -= length;
	error = vmspace_copy_to(vm, sp, aux->exec_path, length);
	if (error != 0)
		goto out;
	execfn_address = (exec_user_word_t)sp;
	for (i = envc; i != 0; i--) {
		length = strlen(envp[i - 1U]) + 1U;
		sp -= length;
		error = vmspace_copy_to(vm, sp, envp[i - 1U], length);
		if (error != 0)
			goto out;
		env_address[i - 1U] = (exec_user_word_t)sp;
	}

	for (i = argc; i != 0; i--) {
		length = strlen(argv[i - 1U]) + 1U;
		sp -= length;
		error = vmspace_copy_to(vm, sp, argv[i - 1U], length);
		if (error != 0)
			goto out;
		argv_address[i - 1U] = (exec_user_word_t)sp;
	}

	/* Places the tables below the strings, aligned. */
	if (sp < table_size) {
		error = EOVERFLOW;
		goto out;
	}

	sp = (sp - table_size) & ~(uintptr_t)15U;
	if (sp < vm->stack_bottom) {
		error = EOVERFLOW;
		goto out;
	}

	/* Builds the argument, environment, and auxiliary tables. */
	words[word_count++] = argc;
	for (i = 0; i < argc; i++)
		words[word_count++] = argv_address[i];
	words[word_count++] = 0;
	for (i = 0; i < envc; i++)
		words[word_count++] = env_address[i];
	words[word_count++] = 0;
#define APPEND_AUX(type, value)                                                \
	do {                                                                   \
		words[word_count++] = (exec_user_word_t)(type);                \
		words[word_count++] = (exec_user_word_t)(value);               \
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
	APPEND_AUX(AT_SECURE, aux->secure);
	APPEND_AUX(AT_EXECFN, execfn_address);
	APPEND_AUX(AT_NULL, 0);
#undef APPEND_AUX
	if (word_count != 1U + argc + 1U + envc + 1U + EXEC_AUXV_PAIRS * 2U) {
		error = EOVERFLOW;
		goto out;
	}

	/* Copies the tables to the stack. */
	error = vmspace_copy_to(vm, sp, words, word_count * sizeof(words[0]));
	if (error != 0)
		goto out;
	*sp_out = sp;
out:
	kern_free(words);
	kern_free(address);

	/* Reports why the stack failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Spawns a new process running a program, as a child of a parent.
 *
 * The child gets the parent's working directory and standard files,
 * falling back to the console, and starts with the credentials the
 * executable's set-id bits imply.  Without a result the child reaps
 * itself.
 */
int
process_spawn_from(
	struct process *parent,
	const char *path,
	char *const argv[],
	char *const envp[],
	struct process **result)
{
	struct exec_target target;
	struct process *process;
	struct process_cred_reservation *cred_reservation;
	struct ucred *access_cred;
	struct ucred *prospective_cred;
	struct thread *thread;
	EXEC_IMAGE_INFO image;
	struct exec_auxv_info aux;
	const char *stage;
	uintptr_t sp;
	uintptr_t execution_entry;
	uintptr_t interpreter_base;
	unsigned secure;
	int error;

	/* Starts before the first stage, holding nothing. */
	process = NULL;
	cred_reservation = NULL;
	access_cred = NULL;
	prospective_cred = NULL;
	secure = 0;
	stage = "resolve executable";

	memset(&target, 0, sizeof(target));

	/* Rejects a missing operand or a parent without a directory. */
	if (parent == NULL ||
	    path == NULL ||
	    argv == NULL ||
	    argv[0] == NULL ||
	    parent->cwdi == NULL)
		return EINVAL;

	/* Resolves the executable with the parent's credential. */
	access_cred = cred_process_ref(parent);
	if (access_cred == NULL)
		return EINVAL;
	error = exec_target_resolve(parent->cwdi, access_cred, path, NULL, 1,
				    argv, &target);
	if (error != 0)
		goto out;

	/*
	 * target.status describes the final non-script image.  Script inode
	 * set-id bits were discarded while resolving; a set-id ELF
	 * interpreter itself retains normal executable semantics.
	 */
	prospective_cred = cred_copy(access_cred);
	if (prospective_cred == NULL) {
		error = ENOMEM;
		goto out;
	}

	exec_credential_prepare(prospective_cred, &target.status,
				target.mount_flags, 0, &secure);

	/* Creates the process with its address space. */
	stage = "create process";
	error = process_create(parent, 0, &process);
	if (error != 0)
		goto out;
	error = process_cred_reserve(process, &cred_reservation);
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

	/* Loads the image after checking that the executable is unchanged. */
	stage = "load ELF";
	error = exec_target_revalidate(&target, access_cred);
	if (error == 0)
		error = load_executable(process->cwdi, access_cred, path,
					target.file, &target.lease,
					process->vmspace, &image,
					&execution_entry, &interpreter_base);
	if (error != 0)
		goto out;
	stage = "set brk";
	error = vmspace_set_brk_start(process->vmspace, image.brk_start,
				      image.static_data_size);
	if (error != 0)
		goto out;

	/* Builds the stack with the prospective credential's identity. */
	stage = "build initial stack";
	fill_auxv_info(&aux, &image, interpreter_base, prospective_cred, secure,
		       path);
	error = exec_build_initial_stack(process->vmspace, image.stack_size,
					 target.argv, envp, &aux, &sp);
	if (error != 0)
		goto out;

	/* Opens the standard files. */
	stage = "open standard files";
	error = setup_standard_files(parent, process, access_cred);
	if (error != 0)
		goto out;

	/*
	 * PID 1 receives console file descriptors for diagnostics, but an
	 * init supervisor must not own the controlling terminal: its getty
	 * creates the login session and claims the console.  The explicitly
	 * requested /bin/sh rescue PID 1 is interactive and therefore claims
	 * it.
	 */
	if (process->pid != 1 || strcmp(path, "/bin/sh") == 0)
		tty_attach_console(process);

	/* Creates the initial thread and checks the executable once more. */
	stage = "create initial thread";
	strncpy(process->command, argv[0], sizeof(process->command) - 1U);
	process->command[sizeof(process->command) - 1U] = '\0';
	error = thread_create(process, execution_entry, sp, &thread);
	if (error != 0)
		goto out;
	hal_task_set_tls(thread->task, image.thread_pointer);
	error = exec_target_revalidate(&target, access_cred);
	if (error != 0) {
		if (thread_abort_new(thread) != 0)
			HAL_FATAL("cannot abort unpublished exec thread");
		goto out;
	}

	/*
	 * The initial thread is still private and stopped, so credential
	 * publication cannot race userspace or fail.
	 */
	process_cred_commit_reserved(process, prospective_cred,
				     cred_reservation);
	prospective_cred = NULL;
	cred_reservation = NULL;

	/* Publishes and starts the process. */
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
	exec_target_release(&target);
	process_cred_reservation_abort(cred_reservation);
	cred_release(prospective_cred);
	cred_release(access_cred);
	if (process != NULL)
		process_free_mem(process);

	/* Reports why the spawn failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Replaces the current process image with a program at a path.
 */
int
process_execve(
	struct process *process,
	const char *path,
	char *const argv[],
	char *const envp[])
{
	int error;

	/* Reports why the exec failed. */
	error = process_exec_file(process, path, NULL, 1, argv, envp);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Replaces the current process image with a program given as an open file.
 */
int
process_fexecve(
	struct process *process,
	struct file *file,
	char *const argv[],
	char *const envp[])
{
	const char *label;
	int error;

	/* Names the image after its first argument for diagnostics. */
	if (argv != NULL && argv[0] != NULL)
		label = argv[0];
	else
		label = "fexecve";

	/* Reports why the exec failed. */
	error = process_exec_file(process, label, file, 0, argv, envp);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Spawns a process as a child of the kernel process.
 */
int
process_spawn(
	const char *path,
	char *const argv[],
	char *const envp[],
	struct process **result)
{
	int error;

	/* Reports why the spawn failed. */
	error = process_spawn_from(&process0, path, argv, envp, result);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Spawns init with the default environment.
 */
int
process_spawn_init(
	const char *path,
	struct process **result)
{
	char *argv[2];
	char *envp[] = {"HOME=/home", "PATH=/bin:/sbin:/usr/bin",
			"REMACS_SKK_DICT=/home/skkjisyo.dic", NULL};
	int error;

	/* Runs the path as its own argument. */
	argv[0] = (char *)path;
	argv[1] = NULL;

	/* Reports why the spawn failed. */
	error = process_spawn(path, argv, envp, result);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Measures a terminated string that must fit within a maximum. */
static int
exec_bounded_length(
	const char *string,
	size_t maximum,
	size_t *result)
{
	size_t length;

	/* Rejects a missing string or result. */
	if (string == NULL || result == NULL)
		return EINVAL;

	/* Counts up to the maximum, which the terminator must precede. */
	length = 0;
	while (length < maximum && string[length] != '\0')
		length++;
	if (length == maximum)
		return E2BIG;
	*result = length + 1U;

	/* Reports the length including the terminator. */
	return 0;
}

/* Accounts one string of a script argument vector against the limit. */
static int
script_argv_count(
	const char *value,
	size_t table_bytes,
	size_t *string_bytes)
{
	size_t length;
	int error;

	/* The strings and the table must fit the argument limit together. */
	error = exec_bounded_length(value, ZEDBSD_ARG_MAX - *string_bytes,
	    &length);
	if (error != 0)
		return E2BIG;
	if (length > ZEDBSD_ARG_MAX - table_bytes - *string_bytes)
		return E2BIG;
	*string_bytes += length;

	/* Reports the accounted string. */
	return 0;
}

/* Copies one string into a vector's string area and records its pointer. */
static void
script_argv_copy(
	char **vector,
	unsigned *index,
	char **cursor,
	const char *value)
{
	size_t copy_length;

	copy_length = strlen(value) + 1U;
	vector[*index] = *cursor;
	(*index)++;
	memcpy(*cursor, value, copy_length);
	*cursor += copy_length;
}

/* Ends the last failure-capable phase and publishes the exec boundary. */
static void
exec_commit_release_lease(
	struct file_content_lease *lease,
	struct process *process)
{
	(void)process;

	/*
	 * Test builds call this small helper directly so the
	 * wake-before-retirement ordering can be exercised without mocking
	 * the architecture image commit.
	 */
	file_content_lease_end(lease);
	KERN_TEST_CHECKPOINT(KERN_TEST_EXEC_LEASE_RELEASED, process);
	KERN_TEST_CHECKPOINT(KERN_TEST_EXEC_RETIREMENT_BEGIN, process);
}

/* Loads an ELF image and, when it names one, its interpreter. */
static int
load_executable(
	struct cwdinfo *cwdi,
	const struct ucred *cred,
	const char *path,
	struct file *file,
	struct file_content_lease *lease,
	struct vmspace *vm,
	EXEC_IMAGE_INFO *image,
	uintptr_t *execution_entry,
	uintptr_t *interpreter_base)
{
	struct file *interpreter_file;
	EXEC_IMAGE_INFO interpreter;
	int error;

	interpreter_file = NULL;

	/* Rejects a missing operand or an inactive lease. */
	if (cwdi == NULL ||
	    cred == NULL ||
	    path == NULL ||
	    file == NULL ||
	    lease == NULL ||
	    !lease->active ||
	    vm == NULL ||
	    image == NULL ||
	    interpreter_base == NULL)
		return EINVAL;

	/* Loads the main image; a static one is entered directly. */
	error = exec_elf_load_content(lease, vm, image);
	if (error != 0)
		return error;
	*execution_entry = image->entry;
	*interpreter_base = 0;
	if (!image->has_interpreter)
		return 0;

	/*
	 * The kernel loader needs read access to bytes, but user permission
	 * is execute-only: an X-only image remains executable.
	 */
	error = file_openat(cwdi, image->interpreter, O_RDONLY, 0,
			    &interpreter_file);
	if (error == 0 && interpreter_file->f_inode == file->f_inode)
		error = ELOOP;
	if (error == 0)
		error = vfs_access(interpreter_file->f_inode, cred, X_OK);
	if (error == 0)
		error = exec_elf_load_interpreter(interpreter_file, vm,
						  &interpreter);

	/* The interpreter is entered first. */
	if (error == 0) {
		*execution_entry = interpreter.entry;
		*interpreter_base = interpreter.load_bias;
	}

	if (interpreter_file != NULL)
		(void)file_close(interpreter_file);

	/* Reports why the load failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Releases the file, lease, and owned vector of a resolved target. */
static void
exec_target_release(
	struct exec_target *target)
{
	/* Ignores a missing target. */
	if (target == NULL)
		return;

	/* Releases in the reverse order of acquisition. */
	file_content_lease_end(&target->lease);
	if (target->file != NULL)
		(void)file_close(target->file);
	if (target->argv_owned)
		exec_script_argv_free(target->argv);
	memset(target, 0, sizeof(*target));
}

/* Resolves a path through its #! interpreters to the ELF image to load. */
static int
exec_target_resolve(
	struct cwdinfo *cwdi,
	const struct ucred *cred,
	const char *path,
	struct file *provided_file,
	int reopenable_path,
	char *const argv[],
	struct exec_target *target)
{
	unsigned char header[EXEC_SHEBANG_LINE_MAX + 1U];
	char paths[EXEC_SHEBANG_DEPTH_MAX + 1U][PATH_MAX];
	struct exec_shebang shebang;
	struct file *interpreter;
	char **rewritten;
	unsigned path_count;
	unsigned i;
	ssize_t count;
	int parsed;
	int at_eof;
	int error;

	path_count = 0;

	/* Rejects a missing operand or an empty argument vector. */
	if (cwdi == NULL ||
	    cred == NULL ||
	    path == NULL ||
	    argv == NULL ||
	    argv[0] == NULL ||
	    target == NULL)
		return EINVAL;

	/* Starts from the given file or opens the path. */
	memset(target, 0, sizeof(*target));
	target->argv = (char **)argv;
	if (provided_file != NULL) {
		file_ref(provided_file);
		target->file = provided_file;
	} else {
		error = file_openat(cwdi, path, O_RDONLY, 0, &target->file);
		if (error != 0)
			return error;
	}

	error = file_content_lease_begin(target->file, &target->lease);
	if (error != 0)
		goto fail;
	if (reopenable_path) {
		if (strlen(path) >= sizeof(paths[0])) {
			error = ENAMETOOLONG;
			goto fail;
		}

		strcpy(paths[path_count++], path);
	}

	/*
	 * Each interpreter is opened and checked with the immutable pre-exec
	 * credential; PT_INTERP remains the ELF loader's independent
	 * dynamic-linker layer.
	 */
	for (;;) {
		/* The current file must be an executable regular file. */
		if (target->file->f_inode == NULL ||
		    target->file->f_inode->i_type != INODE_REG) {
			error = EACCES;
			goto fail;
		}

		error = vfs_access(target->file->f_inode, cred, X_OK);
		if (error != 0)
			goto fail;
		error = inode_getattr(target->file->f_inode, &target->status);
		if (error != 0)
			goto fail;
		if (target->file->f_path.p_mount != NULL)
			target->mount_flags = target->file->f_path.p_mount->m_flags;
		else
			target->mount_flags = 0;

		/* Reads the head of the file and looks for a #! line. */
		count = file_content_lease_pread(&target->lease, header,
						 sizeof(header), 0);
		if (count < 0) {
			error = (int)-count;
			goto fail;
		}

		at_eof = target->status.st_size >= 0 &&
			 (uint64_t)target->status.st_size <= (uint64_t)count;
		parsed =
		    exec_shebang_parse(header, (size_t)count, at_eof, &shebang);
		if (parsed < 0) {
			error = -parsed;
			goto fail;
		}

		if (parsed == 0)
			return 0;

		/*
		 * A descriptor does not supply a stable pathname that the
		 * interpreter can receive and reopen as argv[1].
		 */
		if (!reopenable_path) {
			error = ENOEXEC;
			goto fail;
		}

		/* Bounds the interpreter chain and refuses a cycle. */
		if (target->script_depth >= EXEC_SHEBANG_DEPTH_MAX) {
			error = ELOOP;
			goto fail;
		}

		for (i = 0; i < path_count; i++) {
			if (!strcmp(paths[i], shebang.interpreter)) {
				error = ELOOP;
				goto fail;
			}
		}

		/* Rewrites the arguments for the interpreter. */
		error = exec_script_argv_build(
		    target->argv, &shebang, paths[path_count - 1U], &rewritten);
		if (error != 0)
			goto fail;
		if (target->argv_owned)
			exec_script_argv_free(target->argv);
		target->argv = rewritten;
		target->argv_owned = 1;

		/* Continues with the interpreter as the target. */
		interpreter = NULL;
		error = file_openat(cwdi, shebang.interpreter, O_RDONLY,
				    0, &interpreter);
		if (error != 0)
			goto fail;
		file_content_lease_end(&target->lease);
		(void)file_close(target->file);
		target->file = interpreter;
		error = file_content_lease_begin(target->file,
						 &target->lease);
		if (error != 0)
			goto fail;
		strcpy(paths[path_count++], shebang.interpreter);
		target->script_depth++;
	}

fail:
	exec_target_release(target);

	/* Reports the resolution failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Checks that a resolved target still has the identity it was resolved with. */
static int
exec_target_revalidate(
	const struct exec_target *target,
	const struct ucred *cred)
{
	struct stat current;
	unsigned mount_flags;
	int error;

	/* Rejects a missing target, file, or credential. */
	if (target == NULL ||
	    target->file == NULL ||
	    target->file->f_inode == NULL ||
	    cred == NULL)
		return EINVAL;

	/* The file must still be executable. */
	error = vfs_access(target->file->f_inode, cred, X_OK);
	if (error != 0)
		return error;
	error = inode_getattr(target->file->f_inode, &current);
	if (error != 0)
		return error;
	if (target->file->f_path.p_mount != NULL)
		mount_flags = target->file->f_path.p_mount->m_flags;
	else
		mount_flags = 0;

	/*
	 * The stack's auxiliary credentials were derived from this
	 * snapshot.  Refuse to commit a different privilege transition if
	 * chmod/chown or a mount-flag change raced the potentially sleeping
	 * ELF load.
	 */
	if (current.st_mode != target->status.st_mode ||
	    current.st_uid != target->status.st_uid ||
	    current.st_gid != target->status.st_gid ||
	    mount_flags != target->mount_flags)
		return EAGAIN;

	/* Reports an unchanged target. */
	return 0;
}

/* Fills the auxiliary vector description from the image and credential. */
static void
fill_auxv_info(
	struct exec_auxv_info *aux,
	const EXEC_IMAGE_INFO *image,
	uintptr_t interpreter_base,
	const struct ucred *cred,
	unsigned secure,
	const char *path)
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
	aux->secure = secure;
	aux->exec_path = path;
}

/* Gives a new process its standard files, opening the console for missing ones. */
static int
setup_standard_files(
	struct process *parent,
	struct process *process,
	const struct ucred *credential)
{
	static const int flags[3] = {O_RDONLY, O_WRONLY, O_WRONLY};
	struct file *file;
	int descriptor;
	int error;

	/* Inherits the parent's standard descriptors when it has a table. */
	if (parent != NULL && parent->fd != NULL)
		error = filedesc_clone_stdio(parent->fd, process->fd);
	else
		error = 0;
	if (error != 0)
		return error;

	/* Opens the console for each descriptor still missing. */
	for (descriptor = 0; descriptor < 3; descriptor++) {
		file = filedesc_get_ref(process->fd, descriptor);
		if (file != NULL) {
			(void)file_close(file);
			continue;
		}

		error =
		    file_openat_cred(process->cwdi, credential, "/dev/console",
				     flags[descriptor], 0, &file);
		if (error != 0)
			return error;
		error = filedesc_install_at(process->fd, file, descriptor);
		if (error != 0) {
			(void)file_close(file);
			return error;
		}
	}

	/* Reports the prepared descriptors. */
	return 0;
}

/* Replaces the image of the calling process with a resolved executable. */
static int
process_exec_file(
	struct process *process,
	const char *path,
	struct file *provided_file,
	int reopenable_path,
	char *const argv[],
	char *const envp[])
{
	struct vmspace *new_vm;
	struct vmspace *old_vm;
	struct exec_target target;
	struct process_cred_reservation *cred_reservation;
	struct ucred *access_cred;
	struct ucred *prospective_cred;
	struct thread *other;
	EXEC_IMAGE_INFO image;
	struct exec_auxv_info aux;
	uintptr_t sp;
	uintptr_t execution_entry;
	uintptr_t interpreter_base;
	unsigned secure;
	unsigned long process_irq;
	bool irq_enabled;
	int error;

	new_vm = NULL;
	cred_reservation = NULL;
	access_cred = NULL;
	prospective_cred = NULL;
	secure = 0;

	memset(&target, 0, sizeof(target));

	/* Only the calling user process may exec itself. */
	if (process == NULL ||
	    process == &process0 ||
	    process != curthread->proc ||
	    process->cwdi == NULL ||
	    path == NULL ||
	    argv == NULL ||
	    argv[0] == NULL)
		return EINVAL;

	/* One exec at a time per process. */
	process_irq = spin_lock_irqsave(&process->lock);

	if (process->execing) {
		spin_unlock_irqrestore(&process->lock, process_irq);
		return EBUSY;
	}

	process->execing = 1;

	spin_unlock_irqrestore(&process->lock, process_irq);

	/* Resolves the executable and prepares the prospective credential. */
	access_cred = cred_process_ref(process);
	if (access_cred == NULL) {
		error = EINVAL;
		goto out;
	}

	error = exec_target_resolve(process->cwdi, access_cred, path, provided_file,
				    reopenable_path, argv, &target);
	if (error != 0)
		goto out;
	prospective_cred = cred_copy(access_cred);
	if (prospective_cred == NULL) {
		error = ENOMEM;
		goto out;
	}

	exec_credential_prepare(prospective_cred, &target.status,
				target.mount_flags, 0, &secure);
	error = process_cred_reserve(process, &cred_reservation);
	if (error != 0)
		goto out;

	/* Loads the new image into a fresh address space. */
	new_vm = vmspace_create();
	if (new_vm == NULL) {
		error = ENOMEM;
		goto out;
	}

	error = resource_limit_apply_vm(process, new_vm);
	if (error != 0)
		goto out;
	error = exec_target_revalidate(&target, access_cred);
	if (error == 0)
		error = load_executable(process->cwdi, access_cred, path,
				    target.file, &target.lease, new_vm, &image,
				    &execution_entry, &interpreter_base);
	if (error == 0)
		error = vmspace_set_brk_start(new_vm, image.brk_start,
					      image.static_data_size);
	if (error == 0) {
		fill_auxv_info(&aux, &image, interpreter_base, prospective_cred,
			       secure, path);
		error = exec_build_initial_stack(new_vm, image.stack_size,
						 target.argv, envp, &aux, &sp);
	}

	if (error != 0)
		goto out;
	error = exec_target_revalidate(&target, access_cred);
	if (error != 0)
		goto out;

	/*
	 * All failure-capable HAL validation precedes sibling retirement.
	 * After this point exec is a one-way commit and the architecture
	 * must accept the exact tuple it validated.
	 */
	if (hal_task_exec_validate(new_vm->space, execution_entry, sp) != 0) {
		error = EINVAL;
		goto out;
	}

	/*
	 * The final inode/mode snapshot and architecture tuple above end all
	 * failure-capable work.  Releasing the stable-image lease below is
	 * the successful exec linearization point; nothing afterwards may
	 * return an exec error.  Release it before asking siblings to
	 * retire: a sibling may itself be sleeping in write, chmod, mmap
	 * fault or unmap on this inode, and waiting for it while retaining
	 * the lease would form a wait cycle.  The target file reference and
	 * fully materialized new image remain alive; later content mutations
	 * are ordered after this exec.
	 */
	exec_commit_release_lease(&target.lease, process);

	/*
	 * POSIX exec keeps only the calling thread.  Publish a kernel-only
	 * retirement request and wait until every other task has crossed a
	 * syscall/interrupt return boundary before replacing the vmspace.
	 */
	for (;;) {
		other = NULL;
		process_irq = spin_lock_irqsave(&process->lock);
		for (other = process->threads; other != NULL;
		     other = other->proc_next) {
			if (other == curthread ||
			    other->state == THREAD_DEAD ||
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
			sched_interrupt(other);
			while (other->state != THREAD_ZOMBIE)
				sched_sleep(sched_ticks() + 1U);
		}

		(void)thread_wait(other, NULL);
		thread_release(other);
	}

	/* Switches the task to the new image. */
	irq_enabled = hal_irq_disable();
	if (hal_task_exec_current(new_vm->space, execution_entry, sp) != 0)
		HAL_FATAL("validated HAL exec commit failed");
	hal_task_set_tls(curthread->task, image.thread_pointer);

	/*
	 * process->vmspace owns one strong reference.  Publish the
	 * replacement under process->lock so out-of-process readers can
	 * safely take their own reference without racing the final put of
	 * old_vm.  IRQs remain disabled across the HAL switch and
	 * publication, so this thread cannot expose the new task context
	 * through an interrupt while the old pointer is visible.
	 */
	process_irq = spin_lock_irqsave(&process->lock);

	old_vm = process->vmspace;
	process->vmspace = new_vm;

	spin_unlock_irqrestore(&process->lock, process_irq);

	new_vm = NULL;

	/*
	 * Every allocation and validation has completed and sibling threads
	 * are gone.  Publish the prospective credentials in the same one-way
	 * commit window as the address space.
	 */
	process_cred_commit_reserved(process, prospective_cred,
				     cred_reservation);
	prospective_cred = NULL;
	cred_reservation = NULL;
	if (irq_enabled)
		hal_irq_enable();

	/* Applies the exec side effects and drops the old address space. */
	filedesc_close_on_exec(process->fd);
	process_timer_cleanup(process);
	signal_exec(process);
	process->did_exec = 1;
	strncpy(process->command, argv[0], sizeof(process->command) - 1U);
	process->command[sizeof(process->command) - 1U] = '\0';
	vmspace_put(old_vm);
out:
	process_irq = spin_lock_irqsave(&process->lock);

	process->execing = 0;

	spin_unlock_irqrestore(&process->lock, process_irq);

	exec_target_release(&target);
	process_cred_reservation_abort(cred_reservation);
	cred_release(prospective_cred);
	cred_release(access_cred);
	if (new_vm != NULL)
		vmspace_put(new_vm);

	/* Reports why the exec failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}
