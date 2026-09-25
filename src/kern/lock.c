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

#include <uapi/errno.h>
#include <hal/hal.h>

/*
 * How many times a thread that finds a mutex held looks at it again,
 * relaxing the CPU between looks, before it sleeps.  A holder on another
 * CPU usually lets go within this, and the waiter is spared a sleep, a
 * wakeup and the interrupt that sends it.
 */
#define MUTEX_SPIN_LIMIT 256U

/*
 * The states of a mutex's locked word: free, held, and held with a
 * thread that may be asleep for it (whose release must wake one).
 */
#define MUTEX_FREE      0U
#define MUTEX_HELD      1U
#define MUTEX_CONTENDED 2U

/*
 * Takes a free mutex with one compare-and-swap; reports whether it did.
 */
static int
mutex_take_free(
	struct mutex *mutex)
{
	unsigned expected;
	int taken;

	/* Free becomes held, and nothing else does. */
	expected = MUTEX_FREE;
	taken = __atomic_compare_exchange_n(&mutex->locked, &expected,
	    MUTEX_HELD, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);

	/* Reports the take. */
	return taken;
}

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

	/*
	 * Traps on a recursive acquisition by the owning CPU, and reports a
	 * lock held by another CPU without the exchange, which would take
	 * the line away from the holder on every spin.
	 */
	cpu = hal_cpu_current();
	if (atomic_load_acquire(&lock->held) != 0) {
		if (lock->owner_valid && lock->owner_cpu == cpu)
			__builtin_trap();
		return 0;
	}

	/* Reports a lock taken by another CPU since the load. */
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

	/* Traps unless this CPU is the recorded owner. */
	cpu = hal_cpu_current();
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
	mutex->locked = MUTEX_FREE;
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
	int acquired;

	/* Reports failure without a mutex or a current thread. */
	thread = thread_current();
	if (mutex == NULL || thread == NULL)
		return 0;

	/* Traps on a recursive acquisition. */
	if (__atomic_load_n(&mutex->owner, __ATOMIC_RELAXED) == thread)
		__builtin_trap();

	/* Takes ownership only while the mutex is free. */
	acquired = mutex_take_free(mutex);
	if (acquired)
		__atomic_store_n(&mutex->owner, thread, __ATOMIC_RELAXED);

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
	struct thread *owner;
	int owned;

	/* Reports no ownership without a mutex or a current thread. */
	thread = thread_current();
	if (mutex == NULL || thread == NULL)
		return 0;

	/*
	 * Samples the owner without the guard.  Only the owner sets and
	 * clears the field, so a thread that reads itself owns the mutex,
	 * and a thread that reads anything else does not.  Assertions call
	 * this on hot paths.
	 */
	owner = __atomic_load_n(&mutex->owner, __ATOMIC_ACQUIRE);

	/* Reports whether the sampled owner is the caller. */
	owned = 0;
	if (owner == thread)
		owned = 1;

	/* Returns the sampled ownership. */
	return owned;
}

/*
 * Acquires a mutex, sleeping interruptibly while it is held.
 *
 * A free mutex is taken with one compare-and-swap.  A held one is looked
 * at a little longer, then the thread marks it contended under the
 * guard and sleeps until the holder's release wakes it.  A pending
 * signal, stop, or termination request ends the wait with EINTR and
 * leaves the mutex untouched.
 */
int
mutex_lock_interruptible(
	struct mutex *mutex)
{
	struct thread *thread;
	unsigned long irq;
	uint64_t sequence;
	unsigned spins;
	int acquired;
	int error;

	/* Rejects a missing mutex or current thread. */
	thread = thread_current();
	if (mutex == NULL || thread == NULL)
		return EINVAL;

	/* Traps on a recursive acquisition. */
	if (__atomic_load_n(&mutex->owner, __ATOMIC_RELAXED) == thread)
		__builtin_trap();

	/* A free mutex, or one that its holder lets go of soon. */
	for (spins = 0; spins <= MUTEX_SPIN_LIMIT; spins++) {
		if (__atomic_load_n(&mutex->locked, __ATOMIC_RELAXED) == MUTEX_FREE) {
			acquired = mutex_take_free(mutex);
			if (acquired) {
				__atomic_store_n(&mutex->owner, thread, __ATOMIC_RELAXED);
				return 0;
			}
		}
		hal_atomic_relax();
	}

	/*
	 * Marks the mutex contended, which makes the holder's release wake
	 * a sleeper, and sleeps until the release; the exchange that finds
	 * it free takes it.
	 */
	irq = spin_lock_irqsave(&mutex->guard);
	for (;;) {
		sequence = waitq_sequence(&mutex->waiters);
		if (__atomic_exchange_n(&mutex->locked, MUTEX_CONTENDED,
		    __ATOMIC_ACQUIRE) == MUTEX_FREE)
			break;

		/* Gives up the wait on an interruption. */
		error = waitq_sleep(
			&mutex->waiters,
			&mutex->guard,
			sequence,
			0,
			WAITQ_INTERRUPTIBLE);
		if (error == EINTR) {
			spin_unlock_irqrestore(&mutex->guard, irq);
			return EINTR;
		}
	}
	spin_unlock_irqrestore(&mutex->guard, irq);

	/* Takes ownership. */
	__atomic_store_n(&mutex->owner, thread, __ATOMIC_RELAXED);

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
 * Releases a mutex owned by the current thread and wakes one waiter
 * when a thread may be asleep for it.
 *
 * Releasing a mutex that is not held by the current thread traps.
 */
void
mutex_unlock(
	struct mutex *mutex)
{
	unsigned long irq;
	unsigned previous;

	/* Traps on a missing mutex. */
	if (mutex == NULL)
		__builtin_trap();

	/* Traps unless the current thread owns the mutex. */
	if (__atomic_load_n(&mutex->locked, __ATOMIC_RELAXED) == MUTEX_FREE ||
	    __atomic_load_n(&mutex->owner, __ATOMIC_RELAXED) != thread_current())
		__builtin_trap();

	/* Releases ownership. */
	__atomic_store_n(&mutex->owner, NULL, __ATOMIC_RELAXED);
	previous = __atomic_exchange_n(&mutex->locked, MUTEX_FREE,
	    __ATOMIC_RELEASE);

	/* A contended mutex hands the release to one sleeper. */
	if (previous == MUTEX_CONTENDED) {
		irq = spin_lock_irqsave(&mutex->guard);
		waitq_wake_one(&mutex->waiters);
		spin_unlock_irqrestore(&mutex->guard, irq);
	}
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
	unsigned previous;
	int error;

	/* Rejects a missing mutex, condition, or current thread. */
	thread = thread_current();
	if (mutex == NULL || condition == NULL || thread == NULL)
		return EINVAL;

	irq = spin_lock_irqsave(&mutex->guard);

	/* Traps unless the current thread owns the mutex. */
	if (__atomic_load_n(&mutex->locked, __ATOMIC_RELAXED) == MUTEX_FREE ||
	    __atomic_load_n(&mutex->owner, __ATOMIC_RELAXED) != thread)
		__builtin_trap();

	/* Yields the mutex, waking one sleeper, before sleeping on the condition. */
	__atomic_store_n(&mutex->owner, NULL, __ATOMIC_RELAXED);
	previous = __atomic_exchange_n(&mutex->locked, MUTEX_FREE,
	    __ATOMIC_RELEASE);
	if (previous == MUTEX_CONTENDED)
		waitq_wake_one(&mutex->waiters);
	error = waitq_sleep(condition, &mutex->guard, observed, deadline, flags);

	/* Reacquiring the mutex is not itself an interruptible operation. */
	for (;;) {
		sequence = waitq_sequence(&mutex->waiters);
		if (__atomic_exchange_n(&mutex->locked, MUTEX_CONTENDED,
		    __ATOMIC_ACQUIRE) == MUTEX_FREE)
			break;
		(void)waitq_sleep(&mutex->waiters, &mutex->guard, sequence, 0, 0);
	}

	spin_unlock_irqrestore(&mutex->guard, irq);

	/* Takes ownership back. */
	__atomic_store_n(&mutex->owner, thread, __ATOMIC_RELAXED);

	/* Reports why the condition wait failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}
