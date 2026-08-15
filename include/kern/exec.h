/*
 * Process image construction
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_KERN_EXEC_H
#define ZEDBSD_KERN_EXEC_H

#include <stddef.h>
#include <stdint.h>
#include <kern/page.h>

struct file;
struct process;
struct vmspace;

#define PROCESS_SPAWN_RESULT 0x00000001U
#define EXEC_STACK_DEFAULT_SIZE (1024U * 1024U)
#define EXEC_STACK_HARD_MAX     (1024U * 1024U)
#define EXEC_STACK_GUARD_SIZE   ZEDBSD_PAGE_SIZE

struct elf32_image_info {
	uintptr_t entry;
	uintptr_t brk_start;
	size_t stack_size;
};

struct elf64_image_info {
	uintptr_t entry;
	uintptr_t brk_start;
	size_t stack_size;
};

int elf32_load(struct file *, struct vmspace *, struct elf32_image_info *);
int elf64_load(struct file *, struct vmspace *, struct elf64_image_info *);
int exec_build_initial_stack(struct vmspace *, size_t, char *const [],
			     char *const [], uintptr_t *sp);
int process_spawn(const char *, char *const [], char *const [], unsigned,
		  struct process **);
int process_spawn_from(struct process *, const char *, char *const [],
		       char *const [], unsigned, struct process **);
int process_spawn_init(const char *, struct process **);
int process_execve(struct process *, const char *, char *const [],
		   char *const []);

#endif
