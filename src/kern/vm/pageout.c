/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The free-page estimate, the page-out worker and the out-of-memory
 * killer.
 */

#include "kern/vm-object.h"
#include "kern/cred.h"
#include "kern/file.h"
#include "kern/kmem.h"
#include "kern/page.h"
#include "kern/vm-reclaim.h"
#include "kern/vm-lock.h"
#include "kern/vmspace.h"
#include "kern/vm-commit.h"
#include "kern/io-pool.h"
#include "kern/disk.h"
#include "kern/cache-memory.h"
#include "kern/writeback.h"
#include <uapi/errno.h>
#include <uapi/mman.h>
#include "kern/lock.h"
#include "kern/swap.h"
#include <hal/hal.h>
#include <kern/pmem.h>
#include <limits.h>
#include "kern/sched.h"
#include "kern/clock.h"
#include "kern/thread.h"
#include "kern/process.h"
#include "kern/signal.h"
#include "kern/klog.h"
#include <uapi/signal.h>
#include <kern/kcrt.h>
#include "reclaim-internal.h"

#define stats				vm_reclaim_counters

/*
 * Guards the page-out worker's state and the two wait queues below.
 */
struct spinlock pageout_lock;

/*
 * Where the page-out worker sleeps until the free count falls below the
 * low mark.
 */
struct wait_queue pageout_waitq;

/*
 * Where a fault that found no frame above the reserve sleeps until the
 * worker or a free makes room.
 */
struct wait_queue free_waitq;

/*
 * Whether the page-out worker has been started, and whether it has been
 * asked to run since it last looked.
 */
int pageout_started;
int pageout_requested;

/*
 * The free-page count as the allocator last reported it, and how many
 * frames the fault path has asked for since.
 *
 * Reading the allocator takes its global lock and walks its extents, too
 * much for every frame.  Between readings the count is estimated as the
 * snapshot less the frames asked for since; frees are not counted, so the
 * estimate errs low.  The allocator is read again after a bounded number
 * of frames, or whenever the estimate comes near the reserve, where the
 * decision has to be exact.  Both are updated without a lock: a lost
 * increment only makes the next reading come a little later.
 */
static size_t free_snapshot_pages;
static size_t free_snapshot_taken;

static size_t vm_free_pages_exact(void);
static void pageout_worker(void *argument);

/*
 * Waits until memory has been freed or a short time has passed.
 *
 * Used by a fault that could not map a page for lack of page-table memory:
 * it wakes the page-out worker and sleeps on the same queue as a fault
 * that could not get a frame.
 */
void
vm_reclaim_wait_free(
	void)
{
	unsigned long irq;
	uint64_t sequence;

	/* Asks the worker to refill. */
	vm_reclaim_pageout_kick();

	/* Sleeps until memory is freed, briefly. */
	irq = spin_lock_irqsave(&pageout_lock);
	sequence = waitq_sequence(&free_waitq);
	(void)waitq_sleep(&free_waitq, &pageout_lock, sequence,
	    sched_ticks() + VM_FREE_WAIT_TICKS, 0);
	spin_unlock_irqrestore(&pageout_lock, irq);
}

/*
 * Reports the free physical memory in bytes for a caller about to take a page.
 *
 * The value is the fault path's estimate, which errs low and is read
 * exactly near the reserve; a cache deciding whether to grow needs no more.
 */
size_t
vm_free_bytes_estimate(
	void)
{
	size_t pages;

	/* Counts the caller's page against the current reading. */
	pages = vm_free_pages();

	/* Reports the estimate in bytes. */
	return pages * PAGE_SIZE;
}

/*
 * Reports the free physical pages for one frame the caller is about to take.
 *
 * The count is an estimate that errs low, see free_snapshot_pages; it is
 * read exactly whenever it matters for the reserve.
 */
size_t
vm_free_pages(
	void)
{
	size_t estimate;
	size_t minimum;
	size_t exact;

	/* The snapshot less the frames taken since bounds the free count from below. */
	minimum = vm_free_min_pages();
	estimate = 0;
	if (free_snapshot_pages > free_snapshot_taken)
		estimate = free_snapshot_pages - free_snapshot_taken;

	/* Trusts a fresh estimate that leaves the reserve well clear. */
	if (free_snapshot_taken < VM_FREE_SNAPSHOT_MAX_TAKEN &&
	    estimate >= minimum + VM_FREE_SNAPSHOT_MARGIN) {
		free_snapshot_taken++;
		return estimate;
	}

	/* Reads the allocator, counting the frame this caller takes next. */
	exact = vm_free_pages_exact();
	free_snapshot_taken = 1;

	/* Reports the fresh reading. */
	return exact;
}

/*
 * Reports the reserve of free pages a user frame may not take.
 */
size_t
vm_free_min_pages(
	void)
{
	static size_t cached;
	struct hal_memstat stats;
	size_t minimum;

	/* Managed memory does not change, so the reserve is computed once. */
	if (cached != 0)
		return cached;

	/* Takes a share of managed memory, with a floor for a small machine. */
	hal_get_memstat(&stats);
	minimum = stats.physical_total / PAGE_SIZE / VM_FREE_MIN_SHARE;
	if (minimum < VM_FREE_MIN_FLOOR)
		minimum = VM_FREE_MIN_FLOOR;
	cached = minimum;
	return minimum;
}

/*
 * Wakes the page-out worker, starting it on first use.
 */
void
vm_reclaim_pageout_kick(
	void)
{
	struct thread *thread;
	unsigned long irq;
	int start;
	int error;

	/* Marks the request and notes whether the worker still has to start. */
	irq = spin_lock_irqsave(&pageout_lock);
	pageout_requested = 1;
	start = !pageout_started;
	if (start)
		pageout_started = 1;
	waitq_wake_all(&pageout_waitq);
	spin_unlock_irqrestore(&pageout_lock, irq);

	/* Starts the worker once; a failure leaves faults to reclaim directly. */
	if (!start)
		return;
	error = kthread_create(pageout_worker, NULL, SCHED_PRIORITY_DEFAULT, &thread);
	if (error != 0) {
		irq = spin_lock_irqsave(&pageout_lock);
		pageout_started = 0;
		spin_unlock_irqrestore(&pageout_lock, irq);
		return;
	}

	/* Lets the worker run. */
	thread_start(thread);
}

/*
 * Kills the process with the most mapped memory, sparing init and the
 * kernel.  Reports whether one was killed.
 */
int
vm_oom_kill(
	void)
{
	struct process *process;
	struct process *victim;
	uint64_t largest;
	uint64_t mapped;
	pid_t cursor;

	/* Walks every process for the largest address space. */
	victim = NULL;
	largest = 0;
	cursor = 1;
	process = process_find_next_ref(cursor);
	while (process != NULL) {
		cursor = process->pid;
		mapped = 0;
		if (process->vmspace != NULL)
			mapped = process->vmspace->mapped_virtual_bytes;
		if (mapped > largest) {
			if (victim != NULL)
				process_release(victim);
			victim = process;
			largest = mapped;
		} else {
			process_release(process);
		}

		/* Moves to the next process by PID. */
		process = process_find_next_ref(cursor);
	}

	/* Reports that no process could be chosen. */
	if (victim == NULL)
		return 0;

	/* Kills the victim and says so. */
	kern_logf("kern: out of memory: killed pid %ld (%llu bytes mapped)\n",
	    (long)victim->pid, (unsigned long long)largest);
	(void)signal_send_process(victim, SIGKILL);
	process_release(victim);
	return 1;
}

/* Reads the allocator's free page count and starts a fresh estimate from it. */
static size_t
vm_free_pages_exact(
	void)
{
	struct hal_memstat stats;

	/* Reads the allocator's free count. */
	hal_get_memstat(&stats);

	/* The reading becomes the snapshot the estimate counts down from. */
	free_snapshot_pages = stats.physical_free / PAGE_SIZE;
	free_snapshot_taken = 0;

	/* Reports the exact count. */
	return free_snapshot_pages;
}

/*
 * Keeps free memory between the low and high marks.
 *
 * It sleeps until woken or a tenth of a second passes, reclaims until the
 * high mark (three times the reserve) or until nothing more can be taken,
 * and wakes the faults waiting for memory.
 */
static void
pageout_worker(
	void *argument)
{
	unsigned long irq;
	uint64_t sequence;
	size_t minimum;
	size_t free_pages;
	unsigned taken;
	int error;
	int failed;

	/* The worker takes no argument and keeps memory above the same reserve. */
	(void)argument;
	minimum = vm_free_min_pages();

	/* Runs for the life of the kernel. */
	for (;;) {
		/* Sleeps until asked, or checks again after a tenth of a second. */
		irq = spin_lock_irqsave(&pageout_lock);
		sequence = waitq_sequence(&pageout_waitq);
		if (!pageout_requested) {
			(void)waitq_sleep(&pageout_waitq, &pageout_lock, sequence,
			    sched_ticks() + kern_ms_to_ticks(100U), 0);
		}

		/* Consumes the request it is about to serve. */
		pageout_requested = 0;
		spin_unlock_irqrestore(&pageout_lock, irq);

		/* Reclaims to the high mark, waking waiters as memory frees. */
		taken = 0;
		failed = 0;
		for (;;) {
			free_pages = vm_free_pages_exact();
			if (free_pages >= 3U * minimum)
				break;
			error = vm_reclaim_one(NULL);
			if (error != 0) {
				failed = 1;
				break;
			}
			taken++;
			if ((taken % 16U) == 0) {
				irq = spin_lock_irqsave(&pageout_lock);
				waitq_wake_all(&free_waitq);
				spin_unlock_irqrestore(&pageout_lock, irq);
			}
		}

		/* Wakes every fault waiting for memory. */
		irq = spin_lock_irqsave(&pageout_lock);
		waitq_wake_all(&free_waitq);
		spin_unlock_irqrestore(&pageout_lock, irq);

		/* Leaves the CPU alone for a moment when nothing could be reclaimed. */
		if (failed)
			kern_usleep_range(5000U, 10000U);
	}
}
