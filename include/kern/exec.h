/*
 * ELF process image construction.
 * Copyright (C) 2026, Awe Morris.
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_KERN_EXEC_H
#define ZEDBSD_KERN_EXEC_H

#include <stdint.h>

struct file;
struct process;
struct vmspace;

#define PROCESS_SPAWN_RESULT 0x00000001U

int elf32_load(struct file *, struct vmspace *, uintptr_t *entry);
int exec_build_initial_stack(struct vmspace *, char *const [],
			     char *const [], uintptr_t *sp);
int process_spawn(const char *, char *const [], char *const [], unsigned,
		  struct process **);
int process_spawn_init(const char *, struct process **);

#endif
