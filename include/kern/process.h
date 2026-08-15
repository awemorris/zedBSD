/*
 * Kernel process objects
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
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

struct cwdinfo;
struct filedesc;
struct thread;
struct vmspace;
struct ucred;

#define PROCESS_RESULT_MAX 256U
#define PROCESS_AUTOREAP 0x00000001U

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
	enum process_wait_kind kind;
};

struct process {
	refcount_t refs;
	struct spinlock lock;
	struct wait_queue child_waitq;
	pid_t pid;
	pid_t pgrp;
	pid_t session;
	mode_t umask;
	struct ucred *cred;
	struct signal_action signal_actions[NSIG];
	sigset_t signal_pending;
	enum process_state state;
	unsigned flags;
	unsigned did_exec;
	unsigned thread_count;
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
	char result[PROCESS_RESULT_MAX];
	size_t result_length;
};

extern struct process process0;
void process_init(void);
int process_reaper_start(void);
struct process *process_find_ref(pid_t pid);
struct process *process_find_next_ref(pid_t after);
void process_ref(struct process *);
void process_release(struct process *);
struct thread *thread_find_ref(tid_t tid);
int process_setpgid(struct process *, pid_t, pid_t);
pid_t process_setsid(struct process *);
int process_create(struct process *parent, pid_t requested_pid,
		   struct process **result);
int process_fork(struct process *, struct process **);
void process_publish(struct process *process);
void process_attach_boot_cwd(struct cwdinfo *cwd);
void process_free_mem(struct process *process);
int process_wait(struct process *, int *status, char *result,
		 size_t result_capacity);
pid_t process_waitpid(struct process *, pid_t, int *, int);
pid_t process_wait_select(struct process *, pid_t, int,
			  struct process_wait_event *);
int process_wait_commit(struct process_wait_event *);
void process_wait_abort(struct process_wait_event *);
void process_note_stopped(struct process *, int);
void process_note_continued(struct process *);
void process_thread_retired(struct thread *);
void exit1(int status) __attribute__((noreturn));
void exit1_signal(int signo) __attribute__((noreturn));

#endif
