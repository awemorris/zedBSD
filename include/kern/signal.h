/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * signal
 *
 * SPDX-License-Identifier: Zlib
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

#define SIGNAL_QUEUE_MAX 32U

struct queued_signal {
	int signo;
	struct signal_info info;
	uint64_t sequence;
};

void
signal_init(void);

int
signal_send_process(
	struct process *,
	int);

int
signal_send_process_info(
	struct process *,
	int,
	const struct signal_info *);

int
signal_send_thread(
	struct thread *,
	int);

int
signal_send_thread_info(
	struct thread *,
	int,
	const struct signal_info *);

/*
 * Atomically snapshot and optionally replace one disposition.
 * Installing an ignored disposition also discards every process- and
 * thread-directed instance of that signal before releasing the
 * process lock.
 */
int
signal_action_set(
	struct process *,
	int,
	const struct signal_action *,
	struct signal_action *);

int
signal_kill(
	struct process *,
	pid_t,
	int);

int
signal_pending_unblocked(
	const struct thread *);

/* Caller holds thread->proc->lock. */
int
signal_pending_unblocked_locked(
	const struct thread *);

/*
 * Consume and perform one pending default job-control stop.  Returns
 * nonzero after the caller has resumed through SIGCONT.
 */
int
signal_stop_before_return(
	struct thread *);

/*
 * Job-control generation decision: 0=default stop, EINTR=caught,
 * EIO=the calling thread blocks it or the process ignores it.
 */
int
signal_job_control_decision(
	const struct thread *,
	int);

void
signal_deliver_on_user_return(void);

void
signal_fork(
	struct process *,
	const struct process *,
	struct thread *,
	const struct thread *);

void
signal_exec(
	struct process *);

int
signal_timedwait(
	struct thread *,
	sigset_t,
	uint64_t,
	int,
	struct signal_info *,
	int *);

#endif
