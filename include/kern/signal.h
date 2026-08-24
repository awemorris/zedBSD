/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * signal
 */

#ifndef ZEDBSD_KERN_SIGNAL_H
#define ZEDBSD_KERN_SIGNAL_H

#include <uapi/zedbsd/signal.h>
#include <stdint.h>
#include <sys/types.h>

struct process;

struct thread;

struct signal_info {
	int code;
	int error;
	pid_t pid;
	uid_t uid;
	int status;
	uintptr_t address;
	uint64_t value;

	/*
	 * Nonzero generation identifies one in-kernel POSIX timer
	 * notification.  These fields are kernel-private and are not
	 * copied into the public siginfo_t layout.
	 */
	unsigned timer_slot;
	uint32_t timer_generation;
};

struct signal_action {
	uintptr_t handler, restorer;
	sigset_t mask;
	unsigned flags;
};

#define SIGNAL_QUEUE_MAX	32U

struct queued_signal {
	int signo;
	struct signal_info info;
	uint64_t sequence;
};

void
signal_init(void);

int
signal_send_process(
	struct process *process,
	int signo);

int
signal_send_process_info(
	struct process *process,
	int signo,
	const struct signal_info *info);

int
signal_send_thread(
	struct thread *thread,
	int signo);

int
signal_send_thread_info(
	struct thread *thread,
	int signo,
	const struct signal_info *info);

/*
 * Atomically snapshot and optionally replace one disposition.
 * Installing an ignored disposition also discards every process- and
 * thread-directed instance of that signal before releasing the
 * process lock.
 */
int
signal_action_set(
	struct process *process,
	int signo,
	const struct signal_action *requested,
	struct signal_action *previous);

int
signal_kill(
	struct process *sender,
	pid_t selector,
	int signo);

int
signal_pending_unblocked(
	const struct thread *thread);

/*
 * Caller holds thread->proc->lock.
 */
int
signal_pending_unblocked_locked(
	const struct thread *thread);

/*
 * Consume and perform one pending default job-control stop.  Returns
 * nonzero after the caller has resumed through SIGCONT.
 */
int
signal_stop_before_return(
	struct thread *thread);

/*
 * Job-control generation decision: 0=default stop, EINTR=caught,
 * EIO=the calling thread blocks it or the process ignores it.
 */
int
signal_job_control_decision(
	const struct thread *thread,
	int signo);

void
signal_deliver_on_user_return(void);

void
signal_fork(
	struct process *child,
	const struct process *parent,
	struct thread *child_thread,
	const struct thread *parent_thread);

void
signal_exec(
	struct process *process);

int
signal_timedwait(
	struct thread *thread,
	sigset_t set,
	uint64_t deadline,
	int timed,
	struct signal_info *info,
	int *signo_out);

#endif
