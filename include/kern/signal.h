/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_KERN_SIGNAL_H
#define ZEDBSD_KERN_SIGNAL_H
#include <uapi/zedbsd/signal.h>
#include <stdint.h>
#include <sys/types.h>
struct process;
struct thread;
struct signal_action {
	uintptr_t handler, restorer;
	sigset_t mask;
	unsigned flags;
};
void signal_init(void);
int signal_send_process(struct process *, int);
int signal_kill(struct process *, pid_t, int);
int signal_pending_unblocked(const struct thread *);
void signal_deliver_on_user_return(void);
void signal_fork(struct process *, const struct process *, struct thread *,
		 const struct thread *);
void signal_exec(struct process *);
#endif
