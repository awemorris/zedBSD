/*
 * ELF process image construction.
 * Copyright (C) 2026, Awe Morris.
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef BOOTS_KERN_EXEC_H
#define BOOTS_KERN_EXEC_H

#include <stdint.h>

struct file;
struct process;
struct vmspace;

int elf32_load(struct file *, struct vmspace *, uintptr_t *entry);
int exec_build_initial_stack(struct vmspace *, const char *, uintptr_t *sp);
int process_spawn_init(const char *, struct process **);

#endif
