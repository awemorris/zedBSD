/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The start of the GHCI device's service thread, which recovers from
 * faults and hangs and gives deferred memory back (ghci_service_main in
 * recover.c), and of the poll thread of a device that takes no interrupt
 * (ghci_poll_main in recover.c).
 */

#include <stddef.h>
#include <stdint.h>

#include <kern/sched.h>
#include <kern/thread.h>

#include "ghci.h"

/*
 * Starts the service thread of the device.
 */
int
ghci_service_start(
	struct ghci_device *device)
{
	struct thread *thread;
	int error;

	/* The thread; nobody joins it, it says when it has left. */
	error = kthread_create(ghci_service_main, device, SCHED_PRIORITY_DEFAULT, &thread);
	if (error != 0)
		return error;

	/* service_running is cleared by the thread as it leaves. */
	device->service_running = 1;
	thread->detached = 1;
	thread_start(thread);

	/* Succeeded: the thread runs. */
	return 0;
}

/*
 * Starts the poll thread of a polled device; a device with an interrupt has none.
 */
int
ghci_poll_start(
	struct ghci_device *device)
{
	struct thread *thread;
	int error;

	/* A device with an interrupt is served by its handler. */
	if (!device->polled)
		return 0;

	/* The thread; nobody joins it, it says when it has left. */
	device->poll_stop = 0;
	error = kthread_create(ghci_poll_main, device, SCHED_PRIORITY_DEFAULT, &thread);
	if (error != 0)
		return error;

	/* poll_running is cleared by the thread as it leaves. */
	device->poll_running = 1;
	thread->detached = 1;
	thread_start(thread);

	/* Succeeded: the thread looks at the device every GHCI_POLL_MS. */
	return 0;
}
