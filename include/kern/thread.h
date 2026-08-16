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
#include <kern/waitq.h>
#include <kern/atomic.h>
#include <sys/types.h>
#include <stdint.h>

struct process;

#define THREAD_FLAG_IDLE 0x00000001U
#define SIGNAL_NEST_MAX HAL_SIGNAL_NEST_MAX

struct thread_signal_level {
	uint32_t token;
	uint32_t saved_mask;
	uint32_t restart_number;
	uintptr_t restart_args[HAL_SYSCALL_ARGS];
	unsigned restart_on_return;
};

enum thread_state {
	THREAD_NEW = 0,
	THREAD_RUNNABLE,
	THREAD_RUNNING,
	THREAD_SLEEPING,
	THREAD_EXITING,
	THREAD_ZOMBIE,
	THREAD_REAPING,
	THREAD_DEAD,
};

struct thread {
	refcount_t refs;
	tid_t tid;
	struct process *proc;
	hal_task_t task;
	enum thread_state state;
	unsigned flags;
	unsigned state_generation;
	int exit_status;
	struct sched sched;
	struct thread *proc_next;
	/* Intrusive link used only while sleeping on a process child event. */
	struct thread *wait_next;
	struct wait_token wait_token;
	void (*kernel_entry)(void *);
	void *kernel_arg;
	uint32_t signal_mask;
	uint32_t signal_pending;
	uint32_t signal_suspend_mask;
	uint32_t signal_token;
	uint32_t signal_token_counter;
	unsigned signal_depth;
	struct thread_signal_level signal_levels[SIGNAL_NEST_MAX];
	unsigned signal_suspended;
	uint32_t syscall_restart_number;
	uintptr_t syscall_restart_args[HAL_SYSCALL_ARGS];
	unsigned syscall_restart_valid;
	unsigned syscall_restart_on_return;
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
int thread_prepare_secondaries(unsigned cpu_count);
void thread_init_secondary(hal_cpu_id_t cpu);
void thread_attach_secondaries(void);
void thread_start(struct thread *thread);
void thread_ref(struct thread *thread);
void thread_release(struct thread *thread);
void thread_sched_retired(struct thread *thread);
void thread_exit(int status) __attribute__((noreturn));
int thread_wait(struct thread *thread, int *status);

#endif
