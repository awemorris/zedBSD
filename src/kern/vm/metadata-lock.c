/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The metadata lock that orders VM work, and its latency diagnostic.
 */

#include "kern/vm-lock.h"
#include "kern/lock.h"
#include "kern/sched.h"
#include "kern/thread.h"
#include "kern/clock.h"
#include <kern/kcrt.h>
#include <hal/hal.h>
#include <uapi/errno.h>
#include <stdint.h>

/*
 * The mutex that orders every piece of VM metadata work.
 */
static struct mutex metadata_lock;

/*
 * How deep the owning thread has entered the metadata lock.
 *
 * The lock is re-entrant for the thread that already holds it, and the
 * mutex is released only when the outermost entry leaves.
 */
static unsigned metadata_depth;

/*
 * Whether the metadata lock has been initialized.
 */
static int metadata_initialized;

/*
 * The metadata lock's window of the latency diagnostic (kern.perf=1,
 * ws213-p007).
 *
 * Only the thread that holds the metadata lock writes it, so the lock
 * itself guards it; the report takes the lock without waiting to read and
 * empty it.  It stays zero with the diagnostic off.
 */
static struct vm_metadata_perf metadata_perf;

/*
 * The measured hold of the metadata lock: the counter when the outermost
 * entry took it (zero when the hold is not measured), the address that
 * entry was called from, and how many times the holder had been switched
 * away from its CPU then.  The lock guards them.
 */
static uint64_t metadata_taken_at;
static const void *metadata_taken_from;
static uint64_t metadata_taken_switch_outs;

static void metadata_lock_measured(struct thread *thread, const void *caller);
static void metadata_hold_end(void);

/*
 * Initializes the metadata lock once.
 */
void
vm_metadata_init(
	void)
{
	/* Ignores a repeated initialization. */
	if (metadata_initialized)
		return;

	/* Creates the mutex; failure here leaves the VM unusable. */
	if (mutex_init(&metadata_lock, LOCK_RANK_VMSPACE, "VM metadata") != 0)
		HAL_FATAL("VM metadata lock initialization failed");

	metadata_initialized = 1;
}

/*
 * Enters the metadata lock, re-entering it when already owned.
 *
 * It is never inlined, so that the latency diagnostic's return address
 * names the function that entered the lock.
 */
__attribute__((noinline)) void
vm_metadata_enter(
	void)
{
	struct thread *thread;
	int measured;

	vm_metadata_init();

	/* Deepens an entry that the current thread already owns. */
	if (mutex_owned(&metadata_lock)) {
		if (metadata_depth == UINT_MAX)
			HAL_FATAL("VM metadata lock recursion overflow");
		metadata_depth++;
		return;
	}

	/* Takes the first entry, measured when the latency diagnostic is on. */
	measured = sched_perf_enabled();
	thread = thread_current();
	if (measured && thread != NULL) {
		metadata_lock_measured(thread, __builtin_return_address(0));
	} else {
		mutex_lock(&metadata_lock);
	}

	metadata_depth = 1;
}

/*
 * Leaves one level of the metadata lock.
 */
void
vm_metadata_leave(
	void)
{
	/* Traps on a release by a thread that does not own the lock. */
	if (!metadata_initialized ||
	    !mutex_owned(&metadata_lock) ||
	    metadata_depth == 0)
		HAL_FATAL("VM metadata lock ownership mismatch");

	/* Keeps the lock while an outer entry still holds it. */
	metadata_depth--;
	if (metadata_depth != 0)
		return;

	/* Ends a measured hold, then releases the mutex. */
	if (metadata_taken_at != 0)
		metadata_hold_end();
	mutex_unlock(&metadata_lock);
}

/*
 * Takes the metadata lock's window of the latency diagnostic and starts a
 * new one.
 *
 * The caller holds no VM lock.  It does not wait for the lock: a held lock
 * reports 0 and leaves the window to grow until the next report.
 */
int
vm_metadata_perf_take(
	struct vm_metadata_perf *window)
{
	int owned;
	int taken;

	/* An uninitialized lock has measured nothing yet. */
	if (!metadata_initialized)
		return 0;

	/* A caller that holds the lock would trap on taking it again. */
	owned = mutex_owned(&metadata_lock);
	if (owned)
		return 0;

	/* Takes the lock only when it is free. */
	taken = mutex_trylock(&metadata_lock);
	if (!taken)
		return 0;

	*window = metadata_perf;
	kern_memset(&metadata_perf, 0, sizeof(metadata_perf));

	mutex_unlock(&metadata_lock);

	/* Succeeded: the window is the caller's and a new one has begun. */
	return 1;
}

/*
 * Tests whether the current thread owns the metadata lock.
 */
int
vm_metadata_owned(
	void)
{
	int owned;

	/* An uninitialized lock is owned by nobody. */
	if (!metadata_initialized)
		return 0;

	/* Asks the mutex. */
	owned = mutex_owned(&metadata_lock);

	/* Reports the ownership. */
	return owned;
}

/*
 * Takes the metadata lock for the latency diagnostic, measuring the wait
 * when it is held and starting the measurement of the hold.
 *
 * A free lock is taken without waiting and counts no wait; a held one is
 * waited for as vm_metadata_enter() always does.
 */
static void
metadata_lock_measured(
	struct thread *thread,
	const void *caller)
{
	uint64_t started;
	uint64_t waited;
	int taken;

	/* Takes a free lock at once. */
	waited = 0;
	taken = mutex_trylock(&metadata_lock);
	if (!taken) {
		/* Waits for the holder, measuring how long. */
		started = sched_perf_counter();
		mutex_lock(&metadata_lock);
		waited = sched_perf_counter() - started;
	}

	/* Counts the entry, and a wait for it. */
	metadata_perf.takes++;
	if (!taken)
		metadata_perf.waits++;

	/* Keeps the longest wait: how long, when it ended, who waited, and from where. */
	if (waited > metadata_perf.wait_counts) {
		metadata_perf.wait_counts = waited;
		metadata_perf.wait_end_ms = kern_ticks_to_ms(clock_ticks());
		metadata_perf.wait_from = caller;
		sched_perf_identify(thread, &metadata_perf.waiter);
	}

	/* Starts measuring the hold. */
	metadata_taken_at = sched_perf_counter();
	metadata_taken_from = caller;
	metadata_taken_switch_outs = thread->sched.perf_switch_outs;
}

/*
 * Ends a measured hold of the metadata lock, keeping it when it is the
 * longest of the window.  The lock is still held.
 */
static void
metadata_hold_end(
	void)
{
	struct thread *thread;
	uint64_t held;

	/* Measures the hold and stops measuring it. */
	thread = thread_current();
	held = sched_perf_counter() - metadata_taken_at;
	metadata_taken_at = 0;

	/* A hold no longer than the kept one changes nothing. */
	if (held <= metadata_perf.hold_counts)
		return;

	/*
	 * Keeps the hold: how long, when it ended, who held it, where it was
	 * entered from, and how often the holder lost the CPU meanwhile.
	 */
	metadata_perf.hold_counts = held;
	metadata_perf.hold_end_ms = kern_ticks_to_ms(clock_ticks());
	metadata_perf.hold_from = metadata_taken_from;
	metadata_perf.hold_switched = 0;
	if (thread != NULL)
		metadata_perf.hold_switched = thread->sched.perf_switch_outs - metadata_taken_switch_outs;
	sched_perf_identify(thread, &metadata_perf.holder);
}
