/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test's stand-in for the kernel's thread header: only what the
 * CPU performance framework uses, without the process and signal types
 * that clash with the host's C library.
 */

#ifndef KERN_KERN_THREAD_H
#define KERN_KERN_THREAD_H

#include <kern/sched.h>

/* A kernel thread as the framework sees it, with what the test records. */
struct thread {
	unsigned detached;
	void (*entry)(void *);
	void *argument;
	unsigned bound_cpu;
	int started;
};

int
kthread_create(
	void (*entry)(void *),
	void *arg,
	int priority,
	struct thread **result);

int
thread_abort_new(
	struct thread *thread);

void
thread_start(
	struct thread *thread);

#endif
