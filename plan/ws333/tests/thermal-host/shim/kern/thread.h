/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test's stand-in for the kernel's thread header: only what the
 * thermal core uses, without the process and signal types that clash with
 * the host's C library.  A kernel thread is a host thread.
 */

#ifndef KERN_KERN_THREAD_H
#define KERN_KERN_THREAD_H

#include <kern/sched.h>

/* A kernel thread as the core sees it: an entry, its argument and whether it runs. */
struct thread {
	void (*entry)(void *);
	void *argument;
	int started;
};

int
kthread_create(
	void (*entry)(void *),
	void *arg,
	int priority,
	struct thread **result);

void
thread_start(
	struct thread *thread);

#endif
