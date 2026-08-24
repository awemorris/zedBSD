/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Process image construction
 */

#ifndef ZEDBSD_KERN_EXEC_H
#define ZEDBSD_KERN_EXEC_H

#include <stddef.h>
#include <stdint.h>
#include <limits.h>
#include <sys/stat.h>
#include <kern/page.h>

struct file;
struct file_content_lease;
struct process;
struct ucred;
struct vmspace;

#define EXEC_STACK_DEFAULT_SIZE		(1024U * 1024U)
#define EXEC_STACK_HARD_MAX		(1024U * 1024U)
#define EXEC_STACK_GUARD_SIZE		ZEDBSD_PAGE_SIZE
#define EXEC_INTERP_MAX			64U
#define EXEC_INTERP_PATH		"/lib/ld.so"
#define EXEC_SHEBANG_LINE_MAX		256U
#define EXEC_SHEBANG_DEPTH_MAX		4U

#ifndef PATH_MAX
#define PATH_MAX			256U
#endif

struct exec_shebang {
	char interpreter[PATH_MAX];
	char optional_argument[EXEC_SHEBANG_LINE_MAX + 1U];
	unsigned has_optional_argument;
};

struct elf32_image_info {
	uintptr_t entry;
	uintptr_t brk_start;
	size_t static_data_size;
	uintptr_t program_headers;
	uintptr_t load_bias;
	size_t stack_size;
	uint16_t program_header_size;
	uint16_t program_header_count;
	unsigned has_interpreter;
	char interpreter[EXEC_INTERP_MAX]
};

struct elf64_image_info {
	uintptr_t entry;
	uintptr_t brk_start;
	size_t static_data_size;
	uintptr_t program_headers;
	uintptr_t load_bias;
	size_t stack_size;
	uint16_t program_header_size;
	uint16_t program_header_count;
	unsigned has_interpreter;
	char interpreter[EXEC_INTERP_MAX]
};

struct exec_auxv_info {
	uintptr_t program_headers;
	uintptr_t interpreter_base;
	uintptr_t program_entry;
	uint16_t program_header_size;
	uint16_t program_header_count;
	uint32_t uid;
	uint32_t euid;
	uint32_t gid;
	uint32_t egid;
	unsigned secure;
	const char *exec_path;
};

/*
 * Pure preparation helpers are kept separate from the image commit so their
 * length, ownership and credential-transition rules can be host-tested.
 */

/*
 * Returns 0 for a non-script, 1 for a valid script and -errno for a malformed
 * or overlong shebang.  at_eof says the supplied bytes reach end-of-file.
 */
int
exec_shebang_parse(
	const void *contents,
	size_t size,
	int at_eof,
	struct exec_shebang *result);

int
exec_script_argv_build(
	char *const old_argv[],
	const struct exec_shebang *shebang,
	const char *script_path,
	char ***result);

void
exec_script_argv_free(
	char **argv);

void
exec_credential_prepare(
	struct ucred *credential,
	const struct stat *status,
	unsigned mount_flags,
	int script,
	unsigned *secure);

int
elf32_load(
	struct file *file,
	struct vmspace *vm,
	struct elf32_image_info *image);

int
elf64_load(
	struct file *file,
	struct vmspace *vm,
	struct elf64_image_info *image);

int
elf32_load_content(
	struct file_content_lease *lease,
	struct vmspace *vm,
	struct elf32_image_info *image);

int
elf64_load_content(
	struct file_content_lease *lease,
	struct vmspace *vm,
	struct elf64_image_info *image);

int
elf32_load_interpreter(
	struct file *file,
	struct vmspace *vm,
	struct elf32_image_info *image);
int
elf64_load_interpreter(
	struct file *file,
	struct vmspace *vm,
	struct elf64_image_info *image);

int
exec_build_initial_stack(
	struct vmspace *vm,
	size_t stack_size,
	char *const argv[],
	char *const envp[],
	const struct exec_auxv_info *aux,
	uintptr_t *sp);

int
process_spawn(
	const char *path,
	char *const argv[],
	char *const envp[],
	struct process **result);

int
process_spawn_from(
	struct process *parent,
	const char *path,
	char *const argv[],
	char *const envp[],
	struct process **result);

int
process_spawn_init(
	const char *path,
	struct process **result);

int
process_execve(
	struct process *process,
	const char *path,
	char *const argv[],
	char *const envp[]);

int
process_fexecve(
	struct process *process,
	struct file *file,
	char *const argv[],
	char *const envp[]);

#endif
