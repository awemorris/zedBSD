/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A single-threaded stand-in for the kernel's locks, wait queues and ticks.
 *
 * It lets sync.c and workqueue.c run on the host without the kernel
 * scheduler.  A spinlock only counts how deeply it is held.  A wait queue
 * only counts its wake-ups.  A sleep does not block: it runs the test's
 * sleep hook, which stands for an interrupt or another thread acting while
 * the sleeper is off the CPU, and then lets one scheduler tick pass, so a
 * deadline always arrives.  Worker threads are created but never run (see
 * host_thread.c).
 */

#ifndef DRIVERS_GPU_I915_TESTS_CONTRACTS_HOST_KERNEL_H
#define DRIVERS_GPU_I915_TESTS_CONTRACTS_HOST_KERNEL_H

#include <stdint.h>

void host_kernel_reset(void);
void host_kernel_set_sleep_hook(void (*hook)(void *context), void *context);
uint64_t host_kernel_ticks(void);
unsigned host_kernel_sleeps(void);
int host_kernel_lock_depth(void);

unsigned host_thread_created(void);
unsigned host_thread_started(void);

#endif
