/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Host fixture declares only the existing opaque thread APIs used by the worker. */
#ifndef WS141_HOST_KERN_THREAD_H
#define WS141_HOST_KERN_THREAD_H

struct thread;
int kthread_create(void (*entry)(void *), void *argument, int priority, struct thread **result);
void thread_start(struct thread *thread);

#endif
