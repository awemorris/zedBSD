/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Kernel thread objects backed by opaque HAL tasks
 */

#ifndef ZEDBSD_KERN_THREAD_H
#define ZEDBSD_KERN_THREAD_H

#include <hal/hal.h>
#include <kern/sched.h>
#include <kern/waitq.h>
#include <kern/atomic.h>
#include <kern/signal.h>
#include <uapi/zedbsd/signal.h>
#include <sys/types.h>
#include <stdint.h>

struct process;

#define THREAD_FLAG_IDLE	0x00000001U
#define SIGNAL_NEST_MAX		HAL_SIGNAL_NEST_MAX

struct thread_signal_level {
	uint32_t token;
	sigset_t saved_mask;
	uint32_t restart_number;
	uintptr_t restart_args[HAL_SYSCALL_ARGS];
	unsigned restart_on_return;
	unsigned used_altstack;
	uintptr_t user_ucontext;
	ucontext_t saved_ucontext;
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
	unsigned notify_pending;
	unsigned state_generation;

	/*
	 * Monotonic wakeup latch for interruptible kernel waits.  A signal or
	 * kernel retirement request advances this before touching scheduler
	 * state, so a waiter can detect an interrupt delivered immediately
	 * before it publishes THREAD_SLEEPING.
	 */
	volatile uint64_t interrupt_generation;

	int exit_status;
	uintptr_t user_exit_value;
	unsigned detached;
	unsigned join_claimed;

	/*
	 * TID of the joining thread which owns join_claimed (zero means none).
	 * Protected by proc->lock; STOP redispatch may re-enter only for this
	 * owner.
	 */
	tid_t join_owner_tid;

	/*
	 * Sticky pthread-cancellation request.  Wait registration can use a
	 * lock other than proc->lock, so readers outside that lock use atomic
	 * access.
	 */
	volatile unsigned cancel_pending;

	unsigned terminate_requested;

	/*
	 * Published under proc->lock before scheduler retirement begins.
	 * Process exit ownership counts this flag rather than racing
	 * THREAD_EXITING state, which belongs to the scheduler lock domain.
	 */
	unsigned exit_committed;

	unsigned stop_generation;
	struct wait_queue join_waitq;
	struct sched sched;
	struct thread *proc_next;

	/*
	 * Protected by the thread ID registry lock.
	 */
	struct thread *tid_next;

	/*
	 * Intrusive link used only while sleeping on a process child event.
	 */
	struct thread *wait_next;

	struct wait_token wait_token;
	void (
		*kernel_entry)(
		void *);
	void *kernel_arg;
	sigset_t signal_mask;
	sigset_t signal_pending;
	struct signal_info signal_info[NSIG];
	sigset_t signal_suspend_mask;
	uint32_t signal_token;
	uint32_t signal_token_counter;
	unsigned signal_depth;
	struct thread_signal_level signal_levels[SIGNAL_NEST_MAX];
	unsigned signal_suspended;
	uintptr_t signal_altstack_base;
	size_t signal_altstack_size;
	unsigned signal_altstack_flags;
	unsigned signal_on_altstack_depth;
	sigset_t signal_wait_set;
	unsigned signal_waiting;
	uint32_t syscall_restart_number;
	uintptr_t syscall_restart_args[HAL_SYSCALL_ARGS];
	unsigned syscall_restart_valid;

	/*
	 * A successful sigreturn can request an in-kernel redispatch of the
	 * interrupted syscall.  The HAL only restores the generic user context;
	 * syscall policy remains entirely in the kernel dispatcher.
	 */
	unsigned syscall_redispatch_valid;

	/*
	 * True only while syscall_dispatch_body() is re-entering a syscall that
	 * was interrupted by a transparent process stop.
	 */
	unsigned syscall_stop_redispatch;

	/*
	 * Absolute deadline retained only across a transparent STOP/CONT
	 * redispatch.  Ordinary signal-handler restarts begin a fresh syscall
	 * attempt and therefore initialize this state again.
	 */
	uint64_t syscall_wait_deadline;

	unsigned syscall_wait_deadline_valid;

	/*
	 * Set when an interruptible kernel wait unwinds solely to acknowledge a
	 * process stop generation.  syscall_dispatch() consumes it and retries
	 * without exposing EINTR after SIGCONT.
	 */
	unsigned stop_interrupted;

	/*
	 * Nonzero from a user-origin syscall/fault entry until the architecture
	 * is ready to return to user mode.  A depth, rather than a boolean,
	 * keeps the accounting contract valid across nested kernel execution.
	 */
	unsigned accounting_kernel_depth;

	uint32_t fault_vector;
	uintptr_t fault_eip;
	uintptr_t fault_address;
};

extern struct thread thread0;

struct thread *
thread_current(void);

#define curthread	(thread_current())

int
thread_create(
	struct process *,
	uintptr_t entry,
	uintptr_t user_sp,
	struct thread **result);

int
thread_abort_new(
	struct thread *thread);

int
thread_fork(
	struct process *,
	hal_task_t,
	struct thread **);

int
kthread_create(
	void (*entry)(void *),
	void *arg,
	int priority,
	struct thread **result);

int
thread_prepare_secondaries(
	unsigned cpu_count);

void
thread_init_secondary(
	hal_cpu_id_t cpu);

void
thread_attach_secondaries(void);

void
thread_start(
	struct thread *thread);

void
thread_ref(
	struct thread *thread);

void
thread_release(
	struct thread *thread);

void
thread_sched_retired(
	struct thread *thread);

void
thread_exit(
	int status) __attribute__((noreturn));

int
thread_wait(
	struct thread *thread,
	int *status);

#endif
