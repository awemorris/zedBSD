/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A stand-in for kernel thread creation that never runs the thread.
 *
 * The work queue creates its worker through kthread_create().  On the host
 * there is no scheduler to run it, so the worker is created, marked started
 * and left idle: queued work stays queued, which is exactly what the
 * contract checks that remain on the host observe.  This file sees the
 * kernel's struct thread, so run.sh compiles it, like workqueue.c, against
 * the zedBSD C library headers.
 */

#include "contract.h"
#include "host_kernel.h"

#include <kern/thread.h>

#include <stddef.h>
#include <string.h>

/*
 * The thread every creation hands out.
 *
 * Only its detached flag is ever written; nothing runs on it.
 */
static struct thread host_thread;

/*
 * How many threads were created and started since the program began.
 */
static unsigned host_threads_created;
static unsigned host_threads_started;

/*
 * Reports how many kernel threads were created.
 */
unsigned
host_thread_created(void)
{
	/* Reports the creation count. */
	return host_threads_created;
}

/*
 * Reports how many kernel threads were started.
 */
unsigned
host_thread_started(void)
{
	/* Reports the start count. */
	return host_threads_started;
}

/*
 * Creates a kernel thread that will never be scheduled.
 */
int
kthread_create(
	void (*entry)(void *),
	void *arg,
	int priority,
	struct thread **result)
{
	UNUSED_PARAMETER(entry);
	UNUSED_PARAMETER(arg);
	UNUSED_PARAMETER(priority);

	/* Hands out the idle thread in a clean state. */
	memset(&host_thread, 0, sizeof(host_thread));
	host_threads_created++;
	*result = &host_thread;

	/* Succeeded: the caller owns a thread that never runs. */
	return 0;
}

/*
 * Starts a kernel thread; on the host it is only counted.
 */
void
thread_start(
	struct thread *thread)
{
	UNUSED_PARAMETER(thread);

	/* Counts the start; the entry is never called. */
	host_threads_started++;
}
