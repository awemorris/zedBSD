/*
 * Kernel thread objects backed by opaque HAL tasks
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_KERN_THREAD_H
#define ZEDBSD_KERN_THREAD_H

#include <hal/hal.h>
#include <kern/sched.h>
#include <sys/types.h>
#include <stdint.h>

struct process;

#define THREAD_FLAG_IDLE 0x00000001U

enum thread_state {
	THREAD_NEW = 0,
	THREAD_RUNNABLE,
	THREAD_RUNNING,
	THREAD_SLEEPING,
	THREAD_ZOMBIE,
	THREAD_DEAD,
};

struct thread {
	tid_t tid;
	struct process *proc;
	hal_task_t task;
	enum thread_state state;
	unsigned flags;
	int exit_status;
	struct sched sched;
	struct thread *proc_next;
	void (*kernel_entry)(void *);
	void *kernel_arg;
	uint32_t signal_mask;
	uint32_t signal_pending;
	uint32_t signal_saved_mask;
	uint32_t signal_suspend_mask;
	uint32_t signal_token;
	unsigned signal_suspended;
	uint32_t fault_vector;
	uintptr_t fault_eip;
	uintptr_t fault_address;
};

extern struct thread thread0;
struct thread *thread_current(void);
#define curthread (thread_current())

int thread_create(struct process *, uintptr_t entry, uintptr_t user_sp,
		  struct thread **result);
int thread_fork(struct process *, hal_task_t, struct thread **);
int kthread_create(void (*entry)(void *), void *arg, int priority,
		   struct thread **result);
void thread_start(struct thread *thread);
void thread_exit(int status) __attribute__((noreturn));
int thread_wait(struct thread *thread, int *status);

#endif
