/*
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 *
 * Kernel process objects.
 */
#ifndef BOOTS_KERN_PROCESS_H
#define BOOTS_KERN_PROCESS_H

#include <sys/types.h>
#include <stdint.h>

struct cwdinfo;
struct filedesc;
struct thread;
struct vmspace;

enum process_state {
	PROCESS_NEW = 0,
	PROCESS_RUNNING,
	PROCESS_ZOMBIE,
	PROCESS_DEAD,
};

struct process {
	pid_t pid;
	enum process_state state;
	unsigned flags;
	unsigned thread_count;
	int exit_status;
	struct process *parent;
	struct process *children;
	struct process *sibling;
	struct process *all_next;
	struct thread *threads;
	struct vmspace *vmspace;
	struct filedesc *fd;
	struct cwdinfo *cwdi;
	uint32_t signal_pending;
	uint32_t signal_ignored;
};

extern struct process process0;
void process_init(void);
struct process *process_find(pid_t pid);
struct thread *process_find_by_tid(tid_t tid);
int process_create(struct process *parent, pid_t requested_pid,
		   struct process **result);
void process_publish(struct process *process);
void process_attach_boot_cwd(struct cwdinfo *cwd);
void process_free_mem(struct process *process);
void exit1(int status) __attribute__((noreturn));

#endif
