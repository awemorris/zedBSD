/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Spinlocks and sleeping mutexes.
 *
 * A spinlock records its owning CPU so that recursive acquisition and a
 * release by another CPU trap immediately.  A mutex is a spinlock-guarded
 * ownership flag whose waiters sleep on a wait queue; mutex_wait() lets the
 * holder sleep on a condition while temporarily yielding the mutex.
 */

#include <kern/lock.h>
#include <kern/waitq.h>
#include <kern/signal.h>
#include <kern/thread.h>

#include <errno.h>
#include <hal/hal.h>

/*
 * Initializes an unlocked spinlock.
 */
void
spin_init(
	struct spinlock *lock,
	enum lock_rank rank,
	const char *name)
{
	/* Starts unlocked with no recorded owner. */
	lock->held.value = 0;
	lock->rank = rank;
	lock->name = name;
	lock->owner_cpu = 0;
	lock->owner_valid = 0;
}

/*
 * Acquires a spinlock without waiting.
 *
 * Recursive acquisition by the owning CPU is a fatal programming error and
 * traps instead of deadlocking.
 */
int
spin_trylock(
	struct spinlock *lock)
{
	unsigned cpu;

	cpu = hal_cpu_current();

	/* Traps on a recursive acquisition by the owning CPU. */
	if (atomic_load_acquire(&lock->held) != 0 &&
	    lock->owner_valid &&
	    lock->owner_cpu == cpu)
		__builtin_trap();

	/* Reports a lock held by another CPU. */
	if (!atomic_try_acquire_zero(&lock->held))
		return 0;

	/* Records this CPU as the owner. */
	lock->owner_cpu = cpu;
	atomic_raw_store_release(&lock->owner_valid, 1U);

	/* Reports the acquisition. */
	return 1;
}

/*
 * Spins until the spinlock is acquired.
 */
void
spin_lock(
	struct spinlock *lock)
{
	/* Retries the acquisition while relaxing the CPU between attempts. */
	while (!spin_trylock(lock))
		hal_atomic_relax();
}

/*
 * Releases a spinlock held by the current CPU.
 *
 * Releasing a lock that is not held, or is held by another CPU, traps.
 */
void
spin_unlock(
	struct spinlock *lock)
{
	unsigned cpu;

	cpu = hal_cpu_current();

	/* Traps unless this CPU is the recorded owner. */
	if (atomic_load_acquire(&lock->held) == 0 ||
	    !lock->owner_valid ||
	    lock->owner_cpu != cpu)
		__builtin_trap();

	/* Clears the owner before publishing the release. */
	atomic_raw_store_release(&lock->owner_valid, 0U);
	atomic_store_release(&lock->held, 0);
}

/*
 * Disables interrupts and acquires a spinlock.
 *
 * The returned value tells spin_unlock_irqrestore() whether interrupts were
 * enabled before the call.
 */
unsigned long
spin_lock_irqsave(
	struct spinlock *lock)
{
	unsigned long enabled;

	/* Disables interrupts and remembers whether they were enabled. */
	enabled = 0UL;
	if (hal_irq_disable())
		enabled = 1UL;

	spin_lock(lock);

	/* Reports the previous interrupt state. */
	return enabled;
}

/*
 * Releases a spinlock and restores the previous interrupt state.
 */
void
spin_unlock_irqrestore(
	struct spinlock *lock,
	unsigned long enabled)
{
	spin_unlock(lock);

	/* Re-enables interrupts only when they were enabled before. */
	if (enabled != 0)
		hal_irq_enable();
}

/*
 * Initializes an unlocked mutex.
 */
int
mutex_init(
	struct mutex *mutex,
	enum lock_rank rank,
	const char *name)
{
	/* Rejects a missing mutex. */
	if (mutex == NULL)
		return EINVAL;

	/* Starts unlocked with an empty waiter queue. */
	spin_init(&mutex->guard, rank, name);
	mutex->owner = NULL;
	mutex->locked = 0;
	waitq_init(&mutex->waiters, name);

	/* Reports the initialized mutex. */
	return 0;
}

/*
 * Acquires a mutex without sleeping.
 *
 * A thread that already owns the mutex traps.
 */
int
mutex_trylock(
	struct mutex *mutex)
{
	struct thread *thread;
	unsigned long irq;
	int acquired;

	thread = thread_current();
	acquired = 0;

	/* Reports failure without a mutex or a current thread. */
	if (mutex == NULL || thread == NULL)
		return 0;

	irq = spin_lock_irqsave(&mutex->guard);

	/* Traps on a recursive acquisition. */
	if (mutex->owner == thread)
		__builtin_trap();

	/* Takes ownership only while the mutex is free. */
	if (!mutex->locked) {
		mutex->locked = 1;
		mutex->owner = thread;
		acquired = 1;
	}

	spin_unlock_irqrestore(&mutex->guard, irq);

	/* Reports whether the mutex was acquired. */
	return acquired;
}

/*
 * Tests whether the current thread owns a mutex.
 */
int
mutex_owned(
	struct mutex *mutex)
{
	struct thread *thread;
	unsigned long irq;
	int owned;

	thread = thread_current();

	/* Reports no ownership without a mutex or a current thread. */
	if (mutex == NULL || thread == NULL)
		return 0;

	/* Samples the ownership under the guard. */
	irq = spin_lock_irqsave(&mutex->guard);
	owned = mutex->locked && mutex->owner == thread;
	spin_unlock_irqrestore(&mutex->guard, irq);

	/* Reports the sampled ownership. */
	return owned;
}

/*
 * Acquires a mutex, sleeping interruptibly while it is held.
 *
 * A pending signal, stop, or termination request ends the wait with EINTR
 * and leaves the mutex untouched.
 */
int
mutex_lock_interruptible(
	struct mutex *mutex)
{
	struct thread *thread;
	unsigned long irq;
	uint64_t sequence;
	int error;

	thread = thread_current();

	/* Rejects a missing mutex or current thread. */
	if (mutex == NULL || thread == NULL)
		return EINVAL;

	irq = spin_lock_irqsave(&mutex->guard);

	/* Traps on a recursive acquisition. */
	if (mutex->owner == thread)
		__builtin_trap();

	/* Sleeps on the waiter queue until the mutex becomes free. */
	while (mutex->locked) {
		sequence = waitq_sequence(&mutex->waiters);
		error = waitq_sleep(
			&mutex->waiters,
			&mutex->guard,
			sequence,
			0,
			WAITQ_INTERRUPTIBLE);

		/* Gives up the wait on an interruption. */
		if (error == EINTR) {
			spin_unlock_irqrestore(&mutex->guard, irq);
			return EINTR;
		}
	}

	/* Takes ownership. */
	mutex->locked = 1;
	mutex->owner = thread;
	spin_unlock_irqrestore(&mutex->guard, irq);

	/* Reports the acquisition. */
	return 0;
}

/*
 * Acquires a mutex, sleeping uninterruptibly while it is held.
 */
void
mutex_lock(
	struct mutex *mutex)
{
	int error;

	/* Retries the interruptible acquisition until it is not interrupted. */
	do {
		error = mutex_lock_interruptible(mutex);
	} while (error == EINTR);
}

/*
 * Releases a mutex owned by the current thread and wakes one waiter.
 *
 * Releasing a mutex that is not held by the current thread traps.
 */
void
mutex_unlock(
	struct mutex *mutex)
{
	unsigned long irq;

	/* Traps on a missing mutex. */
	if (mutex == NULL)
		__builtin_trap();

	irq = spin_lock_irqsave(&mutex->guard);

	/* Traps unless the current thread owns the mutex. */
	if (!mutex->locked || mutex->owner != thread_current())
		__builtin_trap();

	/* Releases ownership and hands the mutex to one waiter. */
	mutex->owner = NULL;
	mutex->locked = 0;
	waitq_wake_one(&mutex->waiters);

	spin_unlock_irqrestore(&mutex->guard, irq);
}

/*
 * Sleeps on a condition while yielding an owned mutex.
 *
 * The mutex is released before the sleep and reacquired afterwards, so the
 * caller holds it again whatever the condition wait reported.
 */
int
mutex_wait(
	struct mutex *mutex,
	struct wait_queue *condition,
	uint64_t observed,
	uint64_t deadline,
	unsigned flags)
{
	struct thread *thread;
	unsigned long irq;
	uint64_t sequence;
	int error;

	thread = thread_current();

	/* Rejects a missing mutex, condition, or current thread. */
	if (mutex == NULL || condition == NULL || thread == NULL)
		return EINVAL;

	irq = spin_lock_irqsave(&mutex->guard);

	/* Traps unless the current thread owns the mutex. */
	if (!mutex->locked || mutex->owner != thread)
		__builtin_trap();

	/* Yields the mutex to one waiter before sleeping on the condition. */
	mutex->owner = NULL;
	mutex->locked = 0;
	waitq_wake_one(&mutex->waiters);
	error = waitq_sleep(condition, &mutex->guard, observed, deadline, flags);

	/* Reacquiring the mutex is not itself an interruptible operation. */
	while (mutex->locked) {
		sequence = waitq_sequence(&mutex->waiters);
		(void)waitq_sleep(&mutex->waiters, &mutex->guard, sequence, 0, 0);
	}

	/* Takes ownership back. */
	mutex->locked = 1;
	mutex->owner = thread;
	spin_unlock_irqrestore(&mutex->guard, irq);

	/* Reports the condition wait result. */
	return error;
}
