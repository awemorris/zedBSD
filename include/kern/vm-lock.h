/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Cross-vmspace metadata serialization.
 */

#ifndef KERN_KERN_VM_LOCK_H
#define KERN_KERN_VM_LOCK_H

#include <stdint.h>
#include <kern/sched.h>

/*
 * The metadata lock's part of one window of the latency diagnostic
 * (kern.perf=1, ws213-p007).
 *
 * It counts the outermost entries and those that found the lock held, and
 * keeps the longest wait for it and the longest hold of it: how long in
 * counter counts (sched_perf_counts_to_us()), the uptime in milliseconds
 * when it ended, the thread, and the address vm_metadata_enter() was
 * called from.  A hold also says how many times its holder was switched
 * away from its CPU while it held the lock.
 */
struct vm_metadata_perf {
	uint64_t takes;
	uint64_t waits;
	uint64_t wait_counts;
	uint64_t wait_end_ms;
	const void *wait_from;
	struct sched_perf_thread waiter;
	uint64_t hold_counts;
	uint64_t hold_end_ms;
	const void *hold_from;
	uint64_t hold_switched;
	struct sched_perf_thread holder;
};

/*
 * Reverse mappings connect vmspace, private-page and VM-object ownership
 * domains.  Code traversing more than one domain enters this outer lock
 * before taking a vmspace, reclaim or object lock.  The lock is recursive
 * for calls which cross an internal public API boundary on the same thread.
 */
void
vm_metadata_init(void);

void
vm_metadata_enter(void);

void
vm_metadata_leave(void);

int
vm_metadata_owned(void);

/*
 * Takes the metadata lock's window of the latency diagnostic and starts a
 * new one.  Reports 0, leaving the window to the next report, when the
 * lock is held and the window cannot be read without waiting.
 */
int
vm_metadata_perf_take(
	struct vm_metadata_perf *window);

#endif
