/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test's stand-in for the kernel's thread header: only what the
 * fan's driver uses, without the process and signal types that clash with
 * the host's C library.  The test records the thread and never runs it.
 */

#ifndef KERN_KERN_THREAD_H
#define KERN_KERN_THREAD_H

#include <kern/sched.h>

struct thread;

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
