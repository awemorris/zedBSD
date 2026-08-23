/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_PROCESS_TIMER_THREAD_HOST_STUBS_H
#define ZEDBSD_PROCESS_TIMER_THREAD_HOST_STUBS_H

#include <stdint.h>

struct host_thread_handle {
	uintptr_t opaque[2];
};

int host_sync_init(void);
void host_timer_lock(void);
void host_timer_unlock(void);
void host_checkpoint_lock(void);
void host_checkpoint_unlock(void);
void host_checkpoint_wait(void);
void host_checkpoint_broadcast(void);
int host_thread_create(struct host_thread_handle *, int (*)(void *), void *);
int host_thread_join(struct host_thread_handle *);

#endif
