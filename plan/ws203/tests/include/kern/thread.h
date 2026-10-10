/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Opaque thread declarations for the GENET host hardware model.
 * The host does not share zedBSD's process and signal ABI.
 */

#ifndef WS203_HOST_THREAD_H
#define WS203_HOST_THREAD_H

struct thread;
int kthread_create(void (*entry)(void *), void *argument, int priority,
		   struct thread **thread);
void thread_start(struct thread *thread);

#endif
