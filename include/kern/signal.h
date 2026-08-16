/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_KERN_SIGNAL_H
#define ZEDBSD_KERN_SIGNAL_H
#include <uapi/zedbsd/signal.h>
#include <stdint.h>
#include <sys/types.h>
struct process;
struct thread;
struct timespec;
struct signal_info {
	int code;
	int error;
	pid_t pid;
	uid_t uid;
	int status;
	uintptr_t address;
	uint64_t value;
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
void signal_init(void);
int signal_send_process(struct process *, int);
int signal_send_process_info(struct process *, int,
			     const struct signal_info *);
int signal_send_thread(struct thread *, int);
int signal_kill(struct process *, pid_t, int);
int signal_pending_unblocked(const struct thread *);
void signal_deliver_on_user_return(void);
void signal_fork(struct process *, const struct process *, struct thread *,
		 const struct thread *);
void signal_exec(struct process *);
int signal_timedwait(struct thread *, sigset_t, const struct timespec *,
	struct signal_info *, int *);
#endif
