/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Kernel process objects
 */

#ifndef ZEDBSD_KERN_PROCESS_H
#define ZEDBSD_KERN_PROCESS_H

#include <sys/types.h>
#include <stdint.h>
#include <uapi/zedbsd/signal.h>
#include <kern/signal.h>
#include <kern/atomic.h>
#include <kern/lock.h>
#include <kern/waitq.h>
#include <kern/resource-limit.h>

struct cwdinfo;
struct filedesc;
struct thread;
struct vmspace;
struct ucred;
struct process_retired_cred;
struct process_cred_reservation;
struct tty;

#define PROCESS_AUTOREAP	0x00000001U
#define PROCESS_PGRP_ORPHANED	0x00000002U
#define PROCESS_PGRP_NOTIFY	0x00000004U

enum process_state {
	PROCESS_NEW = 0,
	PROCESS_RUNNING,
	PROCESS_STOPPED,
	PROCESS_EXITING,
	PROCESS_ZOMBIE,
	PROCESS_DEAD,
};

enum process_wait_kind {
	PROCESS_WAIT_NONE = 0,
	PROCESS_WAIT_EXITED,
	PROCESS_WAIT_STOPPED,
	PROCESS_WAIT_CONTINUED,
};

struct process_wait_event {
	struct process *parent;
	struct process *child;
	pid_t pid;
	int status;
	uid_t uid;
	enum process_wait_kind kind;
};

#define PROCESS_WAIT_EVENT_EXITED	0x01U
#define PROCESS_WAIT_EVENT_STOPPED	0x02U
#define PROCESS_WAIT_EVENT_CONTINUED	0x04U

struct process {
	refcount_t refs;
	struct spinlock lock;
	struct mutex resource_lock;
	struct wait_queue child_waitq;
	pid_t pid;
	pid_t pgrp;
	pid_t session;
	mode_t umask;
	struct process_limits limits;
	struct ucred *cred;
	struct process_retired_cred *retired_creds;
	unsigned cred_readers;
	struct signal_action signal_actions[NSIG];
	sigset_t signal_pending;

	/*
	 * Subset represented by signal_info rather than signal_queue.  Keeping
	 * it separate prevents a queued RT/timer instance from erasing a
	 * coalesced classic instance of the same signal when the queue entry is
	 * consumed.
	 */
	sigset_t signal_unqueued_pending;

	struct signal_info signal_info[NSIG];
	struct queued_signal signal_queue[SIGNAL_QUEUE_MAX];
	unsigned signal_queue_count;
	uint64_t signal_queue_sequence;
	enum process_state state;
	unsigned flags;
	unsigned did_exec;
	unsigned execing;
	unsigned thread_count;

	/*
	 * Stop notification is committed only after all live threads
	 * acknowledge the same generation at scheduler safe points.
	 */
	volatile unsigned stop_requested;

	unsigned stop_generation;
	unsigned stop_target_count;
	unsigned stop_ack_count;
	int stop_signo;
	volatile uint64_t cpu_ticks;
	volatile uint64_t child_cpu_ticks;
	volatile uint64_t user_ticks;
	volatile uint64_t system_ticks;
	volatile uint64_t child_user_ticks;
	volatile uint64_t child_system_ticks;
	uint64_t cpu_limit_signal_second;
	int nice_value;
	uint64_t itimer_remaining[3];
	uint64_t itimer_interval[3];
	volatile unsigned itimer_sequence[3];
	int exit_status;
	int wait_status;
	unsigned wait_stopped;
	unsigned wait_continued;
	enum process_wait_kind wait_reserved;
	struct process *parent;
	struct process *children;
	struct process *sibling;
	struct process *all_next;
	struct thread *threads;
	struct thread *waiter;
	struct thread *child_waiters;
	struct vmspace *vmspace;
	struct filedesc *fd;
	struct cwdinfo *cwdi;
	struct tty *controlling_tty;
	uint64_t controlling_tty_generation;
	char command[64];
};

extern struct process process0;

void
process_init(void);

int
process_reaper_start(void);

struct process *
process_find_ref(
	pid_t pid);

struct process *
process_find_next_ref(
	pid_t after);

void
process_ref(
	struct process *process);

void
process_release(
	struct process *process);

/*
 * Parent links are protected by the process-tree lock.  The reference form
 * pins the returned parent across concurrent reparenting and reaping.
 */
pid_t
process_parent_pid(
	struct process *process);

struct process *
process_parent_ref(
	struct process *process);

int
process_controlling_tty_snapshot(
	struct process *process,
	struct tty **tty,
	uint64_t *generation);

int
process_controlling_tty_matches(
	struct process *process,
	struct tty *tty,
	uint64_t generation);

int
process_controlling_tty_attach(
	struct process *process,
	struct tty *tty,
	uint64_t generation);

void
process_controlling_tty_detach_one(
	struct process *process,
	struct tty *tty,
	uint64_t generation);

void
process_controlling_tty_detach_session(
	pid_t session,

	struct tty *tty,
	uint64_t generation);

/*
 * Acquire a stable reference to the address space currently published by a
 * process.  Callers inspecting a process other than curthread->proc must not
 * dereference process->vmspace directly.
 */
struct vmspace *
process_vmspace_ref(
	struct process *process);

void
process_cred_read_enter(
	struct process *process);

void
process_cred_read_leave(
	struct process *process);

int
process_cred_replace(
	struct process *process,
	struct ucred *replacement);

/*
 * Reserve the only allocation needed by a credential replacement.  Exec
 * uses this before its one-way commit point, then publishes the already-built
 * credential without a possible allocation failure.
 */
int
process_cred_reserve(
	struct process *process,
	struct process_cred_reservation **result);

void
process_cred_reservation_abort(
	struct process_cred_reservation *reservation);

void
process_cred_commit_reserved(
	struct process *process,
	struct ucred *replacement,
	struct process_cred_reservation *reservation);

struct thread *
thread_find_ref(
	tid_t tid);

int
process_setpgid(
	struct process *caller,
	pid_t pid,
	pid_t pgid);

pid_t
process_setsid(
	struct process *process);

int
process_signal_pgrp(
	pid_t session,
	pid_t pgrp,
	int signo);

int
process_signal_pgrp_except(
	pid_t session,
	pid_t pgrp,
	int signo,
	struct process *excluded);

int process_pgrp_in_session(
	pid_t session,
	pid_t pgrp);

int
process_pgrp_is_orphaned(
	const struct process *process);

int
process_create(
	struct process *parent,
	pid_t requested_pid,
	struct process **result);

int
process_fork(
	struct process *parent,
	struct process **result);

void
process_publish(
	struct process *process);

void
process_attach_boot_cwd(
	struct cwdinfo *cwd);

void
process_free_mem(
	struct process *process);

int
process_wait(
	struct process *process,
	int *status);

pid_t
process_waitpid(
	struct process *parent,
	pid_t selector,
	int *status,
	int options);

pid_t
process_wait_select(
	struct process *parent,
	pid_t selector,
	int options,
	struct process_wait_event *event);

pid_t
process_wait_select_mask(
	struct process *parent,
	pid_t selector,
	int options,
	unsigned event_mask,
	struct process_wait_event *event);

int
process_wait_commit(
	struct process_wait_event *event);

void
process_wait_abort(
	struct process_wait_event *event);

void
process_stop_current(
	int signo);

int
process_stop_requested(
	const struct thread *thread);

int
process_continue(
	struct process *process,
	int report_continued);

int
process_itimer_get(
	struct process *process,
	int which,
	uint64_t *remaining,
	uint64_t *interval);

int
process_itimer_set(
	struct process *process,
	int which,
	uint64_t remaining,
	uint64_t interval,
	uint64_t *old_remaining,
	uint64_t *old_interval);

int
process_itimer_tick(
	struct process *process,
	int which);

void
process_itimer_real_tick_all(void);

void
process_thread_retired(
	struct thread *thread);

/*
 * Returns only when other live threads keep the process alive.
 */
void
process_exit_if_last_thread(
	int status);

void
process_resource_count(
	uint64_t *processes,
	uint64_t *threads);
void
exit1(
	int status) __attribute__((noreturn));

void
exit1_signal(
	int signo) __attribute__((noreturn));

#ifdef ZEDBSD_PROCESS_TEST
void
process_test_reparent(
	struct process *,
	struct process *);

void
process_test_set_registry(
	struct process *);

int
process_test_commit_thread_exit(
	struct thread *,
	int);

int
process_test_autoreap_once(
	struct process *);
#endif

#endif
