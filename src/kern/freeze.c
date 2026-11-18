/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Freezing the user processes for a system sleep (ws052-p006).
 *
 * While the freeze holds, a thread that would return to user mode parks
 * in kern_freeze_user_return() (called from the return-to-user policy)
 * until kern_thaw_user().  A thread running user code reaches that path at
 * its next interrupt (kern_freeze_user() notifies every CPU, and the tick
 * follows); a thread sleeping in the kernel parks when its system call
 * ends.  Kernel threads never return to user mode and keep running: the
 * ACPI events, the devices' work.  A thread being terminated exits before
 * it would park.  Signals posted meanwhile are delivered after the thaw.
 */

#include "kern/freeze.h"
#include "kern/atomic.h"
#include "kern/lock.h"
#include "kern/waitq.h"
#include <hal/hal.h>

#include <stdint.h>

/*
 * Whether user threads park, and the queue they park on.  freeze_active is
 * set and cleared by the one system sleep under way (system_sleeping keeps
 * it one at a time); it is read without the lock on the return path and
 * again under it before parking, so a thaw is never missed.
 */
static volatile unsigned freeze_active;
static struct spinlock freeze_lock;
static struct wait_queue freeze_waitq;
static volatile unsigned freeze_ready;

static void freeze_init(void);
static void freeze_notify_all(void);

/*
 * Freezes the user processes: no user code runs from the next interrupt of
 * each CPU on, until kern_thaw_user().
 */
void
kern_freeze_user(
	void)
{
	unsigned long irq;

	/* The lock and the queue, made the first time. */
	freeze_init();

	/* User threads park from now on. */
	irq = spin_lock_irqsave(&freeze_lock);

	freeze_active = 1U;

	spin_unlock_irqrestore(&freeze_lock, irq);

	/* Every other CPU is interrupted, so that the user code it runs reaches the return path. */
	freeze_notify_all();
}

/*
 * Thaws the user processes: every parked thread returns to user mode.
 */
void
kern_thaw_user(
	void)
{
	unsigned long irq;

	/* Nothing was frozen before the first freeze. */
	if (!freeze_ready)
		return;

	/* The parked threads wake and find the freeze over. */
	irq = spin_lock_irqsave(&freeze_lock);

	freeze_active = 0U;
	waitq_wake_all(&freeze_waitq);

	spin_unlock_irqrestore(&freeze_lock, irq);
}

/*
 * Parks the current thread on its way back to user mode while the freeze
 * holds (the return-to-user policy calls it with interrupts enabled).
 */
void
kern_freeze_user_return(
	void)
{
	unsigned long irq;
	uint64_t observed;

	/* Usually nothing is frozen. */
	if (!freeze_active)
		return;

	/* Parks until the thaw; each wake checks again. */
	irq = spin_lock_irqsave(&freeze_lock);

	while (freeze_active) {
		observed = waitq_sequence(&freeze_waitq);
		(void)waitq_sleep(&freeze_waitq, &freeze_lock, observed, 0, 0);
	}

	spin_unlock_irqrestore(&freeze_lock, irq);
}

/* Makes the lock and the queue once (the first freeze, from the one sleep under way). */
static void
freeze_init(
	void)
{
	/* Made already. */
	if (freeze_ready)
		return;

	/* The lock and the queue, then published for the return path. */
	spin_init(&freeze_lock, LOCK_RANK_DEVICE, "freeze");
	waitq_init(&freeze_waitq, "freeze");
	__atomic_store_n(&freeze_ready, 1U, __ATOMIC_RELEASE);
}

/* Interrupts every online CPU but this one (a CPU running user code then takes the return path). */
static void
freeze_notify_all(
	void)
{
	struct hal_cpu_mask targets;
	hal_cpu_id_t self;

	/* The online CPUs but this one. */
	hal_cpu_ready_mask(&targets);
	self = hal_cpu_current();
	hal_cpu_mask_clear(&targets, self);

	/* A machine without notifications relies on the tick alone. */
	(void)hal_cpu_notify_mask(&targets);
}
