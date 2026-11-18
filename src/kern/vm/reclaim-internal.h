/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Declarations private to the anonymous-page side of the VM: the private
 * pages, their mapping tracking, reclaim and page-out.  These connect
 * private-page.c, page-track.c, reclaim.c, pageout.c and the object-*.c
 * files and are not part of the kernel's public VM API.
 *
 * Internal VM API: used only between the files in src/kern/vm/.  Its
 * compatibility is not guaranteed and it may change at any time.  None of
 * these declarations belongs in the public headers under include/kern.
 *
 * reclaim_lock guards the reclaim queue; pageout_lock guards the page-out
 * worker's state and its two wait queues.  Function bodies and the order
 * they take these locks are as they were in vm.c.
 */

#ifndef KERN_VM_RECLAIM_INTERNAL_H
#define KERN_VM_RECLAIM_INTERNAL_H

#include "kern/lock.h"
#include "kern/waitq.h"
#include <stddef.h>

struct vm_private_page;

#define PAGE_SIZE			KERN_PAGE_SIZE

/*
 * How many queued pages one reclaim examines at most.
 *
 * The clock below clears the accessed bit of each page it passes over, so
 * a second sweep finds a victim; the bound keeps one reclaim from walking
 * all of memory when everything was touched at once.
 */
#define VM_RECLAIM_SCAN_MAX		4096U

/*
 * The free-page watermarks, in pages, as a share of managed memory.
 *
 * Below the low mark the page-out worker is woken and reclaims until the
 * high mark.  The min mark is a reserve: a user frame is never taken from
 * it, so page tables, kernel records and the memory swap I/O needs can
 * still be allocated while user faults wait for the worker.  Each mark
 * has a floor for a small machine.
 */
#define VM_FREE_MIN_SHARE		512U
#define VM_FREE_MIN_FLOOR		256U

/* How long one wait for free memory lasts, in milliseconds. */
#define VM_FREE_WAIT_MS		10U

/*
 * How many frames the fault path takes on one reading of the allocator's
 * free count, and how far above the reserve the estimate must stay for
 * the reading to be trusted at all.
 */
#define VM_FREE_SNAPSHOT_MAX_TAKEN	64U
#define VM_FREE_SNAPSHOT_MARGIN		256U

/* Marks a private page that is on the reclaim queue or the swapped queue. */
#define VM_PAGE_TRACKED			0x0010U

/*
 * Guards the page-out worker's state and the two wait queues below
 * (defined in pageout.c).
 */
extern struct spinlock pageout_lock;

/*
 * Where the page-out worker sleeps until the free count falls below the
 * low mark.
 */
extern struct wait_queue pageout_waitq;

/*
 * Where a fault that found no frame above the reserve sleeps until the
 * worker or a free makes room.
 */
extern struct wait_queue free_waitq;

/*
 * Whether the page-out worker has been started, and whether it has been
 * asked to run since it last looked.
 */
extern int pageout_started;
extern int pageout_requested;

/*
 * The mutex that guards the private page queue in reclaim.c.
 */
extern struct mutex reclaim_lock;

/*
 * Defined in reclaim.c, part of the reclaim queue of private pages and the
 * reclaim of one page to swap or to its file.
 */
void vm_reclaim_queue_insert(struct vm_private_page *backing);
void vm_reclaim_queue_remove(struct vm_private_page *backing);
void vm_reclaim_swapped_insert(struct vm_private_page *backing);
unsigned vm_reclaim_object_fault_reserve(void);

/*
 * Defined in pageout.c, part of the free-page estimate, the page-out
 * worker and the out-of-memory killer.
 */
size_t vm_free_pages(void);
size_t vm_free_min_pages(void);
void vm_reclaim_pageout_kick(void);
int vm_oom_kill(void);

/*
 * Defined in private-page.c, part of the private pages that back anonymous
 * memory: their reference, I/O, pin and operation state.
 */
void vm_reclaim_private_page_advance_locked(struct vm_private_page *backing);
int vm_private_page_wait_idle_cancelable(struct vm_private_page *backing, int (*cancel)(void *), void *cancel_argument);

#endif
