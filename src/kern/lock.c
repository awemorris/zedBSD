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

static int mutex_take_free(struct mutex *mutex);
static int mutex_spin_take(struct mutex *mutex);
static void mutex_sleep_take(struct mutex *mutex, unsigned flags, int *error);
static void mutex_sleep_give_up(struct mutex *mutex);
static void mutex_check_owner(struct mutex *mutex, struct thread *thread);

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
	unsigned held;
	unsigned owner_valid;
	int acquired;

	/*
	 * Traps on a recursive acquisition by the owning CPU, and reports a
	 * lock held by another CPU without the exchange, which would take
	 * the line away from the holder on every spin.
	 *
	 * The owner is read with an acquire on owner_valid, which the owner
	 * publishes with a release after it writes owner_cpu.  Two plain
	 * loads may be satisfied in either order on a weakly ordered CPU
	 * (arm64): a CPU that held the lock just before another one could
	 * then see the new owner's owner_valid with its own old owner_cpu and
	 * trap on an acquisition that is not recursive (ws208-p007).
	 */
	cpu = hal_cpu_current();
	held = atomic_load_acquire(&lock->held);
	if (held != 0) {
		owner_valid = atomic_raw_load_acquire(&lock->owner_valid);
		if (owner_valid && lock->owner_cpu == cpu)
			__builtin_trap();
		return 0;
	}

	/* Reports a lock taken by another CPU since the load. */
	acquired = atomic_try_acquire_zero(&lock->held);
	if (!acquired)
		return 0;

	/* Records this CPU as the owner. */
	lock->owner_cpu = cpu;
	atomic_raw_store_release(&lock->owner_valid, 1U);

	/* Succeeded: this CPU holds the lock. */
	return 1;
}

/*
 * Spins until the spinlock is acquired.
 *
 * It does not disable interrupts or preemption.  A thread uses it only where
 * interrupts are already disabled; elsewhere spin_lock_irqsave() is used,
 * since a holder preempted by the clock tick may resume on another CPU, and
 * its release there traps.
 */
void
spin_lock(
	struct spinlock *lock)
{
	int acquired;

	/* Retries the acquisition while relaxing the CPU between attempts. */
	for (;;) {
		acquired = spin_trylock(lock);
		if (acquired)
			break;
		hal_atomic_relax();
	}
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
	unsigned held;

	/* Traps unless this CPU is the recorded owner. */
	cpu = hal_cpu_current();
	held = atomic_load_acquire(&lock->held);
	if (held == 0 ||
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
	bool was_enabled;

	/* Disables interrupts and remembers whether they were enabled. */
	enabled = 0UL;
	was_enabled = hal_irq_disable();
	if (was_enabled)
		enabled = 1UL;

	/* Acquires the lock with interrupts off. */
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
	/* Releases the lock before interrupts can come in again. */
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
	struct thread *owner;
	int acquired;

	/* Reports failure without a mutex or a current thread. */
	thread = thread_current();
	if (mutex == NULL || thread == NULL)
		return 0;

	/* Traps on a recursive acquisition. */
	owner = __atomic_load_n(&mutex->owner, __ATOMIC_RELAXED);
	if (owner == thread)
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
	struct thread *owner;
	unsigned long irq;
	int acquired;
	int error;

	/* Rejects a missing mutex or current thread. */
	thread = thread_current();
	if (mutex == NULL || thread == NULL)
		return EINVAL;

	/* Traps on a recursive acquisition. */
	owner = __atomic_load_n(&mutex->owner, __ATOMIC_RELAXED);
	if (owner == thread)
		__builtin_trap();

	/* Takes a free mutex, or one that its holder lets go of soon. */
	acquired = mutex_spin_take(mutex);
	if (acquired) {
		__atomic_store_n(&mutex->owner, thread, __ATOMIC_RELAXED);
		return 0;
	}

	/*
	 * Marks the mutex contended, which makes the holder's release wake
	 * a sleeper, and sleeps until the release; an interruption gives up.
	 */
	irq = spin_lock_irqsave(&mutex->guard);

	/* Sleeps until the mutex is taken, or an interruption gives up. */
	mutex_sleep_take(mutex, WAITQ_INTERRUPTIBLE, &error);

	/* Leaves the guard; the mutex, when taken, stays held. */
	spin_unlock_irqrestore(&mutex->guard, irq);

	/* Reports an interrupted wait, which left the mutex untouched. */
	if (error == EINTR)
		return EINTR;

	/* Takes ownership. */
	__atomic_store_n(&mutex->owner, thread, __ATOMIC_RELAXED);

	/* Succeeded: the caller holds the mutex. */
	return 0;
}

/*
 * Acquires a mutex, sleeping uninterruptibly while it is held.
 *
 * It takes the mutex the way mutex_lock_interruptible() does, but nothing
 * ends the sleep except the release.  Retrying the interruptible form
 * instead would never sleep while a signal, stop, or termination request
 * is pending, and would spin on the CPU the holder needs to let go.
 */
void
mutex_lock(
	struct mutex *mutex)
{
	struct thread *thread;
	struct thread *owner;
	unsigned long irq;
	int acquired;
	int error;

	/* Gives up without a mutex or a current thread, which has no owner to record. */
	thread = thread_current();
	if (mutex == NULL || thread == NULL)
		return;

	/* Traps on a recursive acquisition. */
	owner = __atomic_load_n(&mutex->owner, __ATOMIC_RELAXED);
	if (owner == thread)
		__builtin_trap();

	/* Takes a free mutex, or one that its holder lets go of soon. */
	acquired = mutex_spin_take(mutex);
	if (acquired) {
		__atomic_store_n(&mutex->owner, thread, __ATOMIC_RELAXED);
		return;
	}

	/*
	 * Marks the mutex contended, which makes the holder's release wake
	 * a sleeper, and sleeps until the release lets this thread take it.
	 */
	irq = spin_lock_irqsave(&mutex->guard);

	/* Sleeps until the mutex is taken; no request ends this sleep. */
	mutex_sleep_take(mutex, 0, &error);

	/* Leaves the guard, holding the mutex. */
	spin_unlock_irqrestore(&mutex->guard, irq);

	/* Takes ownership. */
	__atomic_store_n(&mutex->owner, thread, __ATOMIC_RELAXED);
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
	struct thread *thread;
	unsigned long irq;
	unsigned previous;

	/* Traps on a missing mutex. */
	if (mutex == NULL)
		__builtin_trap();

	/* Traps unless the current thread owns the mutex. */
	thread = thread_current();
	mutex_check_owner(mutex, thread);

	/*
	 * Releases ownership.  The exchange tells whether a thread may be
	 * asleep for the mutex (MUTEX_CONTENDED).
	 */
	__atomic_store_n(&mutex->owner, NULL, __ATOMIC_RELAXED);
	previous = __atomic_exchange_n(&mutex->locked, MUTEX_FREE, __ATOMIC_RELEASE);

	/* A contended mutex hands the release to one sleeper. */
	if (previous == MUTEX_CONTENDED) {
		irq = spin_lock_irqsave(&mutex->guard);

		/* Wakes one thread asleep for the mutex. */
		waitq_wake_one(&mutex->waiters);

		/* Leaves the guard. */
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
	unsigned previous;
	int reacquired;
	int error;

	/* Rejects a missing mutex, condition, or current thread. */
	thread = thread_current();
	if (mutex == NULL || condition == NULL || thread == NULL)
		return EINVAL;

	/* Yields the mutex and sleeps on the condition, then takes the mutex back. */
	irq = spin_lock_irqsave(&mutex->guard);

	/* Traps unless the current thread owns the mutex. */
	mutex_check_owner(mutex, thread);

	/* Yields the mutex, waking one sleeper, before sleeping on the condition. */
	__atomic_store_n(&mutex->owner, NULL, __ATOMIC_RELAXED);
	previous = __atomic_exchange_n(&mutex->locked, MUTEX_FREE, __ATOMIC_RELEASE);
	if (previous == MUTEX_CONTENDED)
		waitq_wake_one(&mutex->waiters);
	error = waitq_sleep(condition, &mutex->guard, observed, deadline, flags);

	/* Reacquiring the mutex is not itself an interruptible operation. */
	mutex_sleep_take(mutex, 0, &reacquired);

	/* Leaves the guard, holding the mutex again. */
	spin_unlock_irqrestore(&mutex->guard, irq);

	/* Takes ownership back. */
	__atomic_store_n(&mutex->owner, thread, __ATOMIC_RELAXED);

	/* Reports why the condition wait failed. */
	if (error != 0)
		return error;

	/* Succeeded: the condition wait ended and the caller holds the mutex. */
	return 0;
}

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
	taken = __atomic_compare_exchange_n(&mutex->locked,
					    &expected,
					    MUTEX_HELD,
					    0,
					    __ATOMIC_ACQUIRE,
					    __ATOMIC_RELAXED);

	/* Reports the take. */
	return taken;
}

/*
 * Looks at a held mutex up to MUTEX_SPIN_LIMIT more times, relaxing the
 * CPU between looks, and takes it when it is free.  Reports whether it
 * did.  With one CPU the holder cannot run while this thread looks, so
 * one look decides.
 */
static int
mutex_spin_take(
	struct mutex *mutex)
{
	unsigned spins;
	unsigned limit;
	unsigned cpus;
	unsigned locked;
	int acquired;

	/*
	 * Looks again only when another CPU may be running the holder; on
	 * a single CPU the holder is off the CPU until this thread sleeps.
	 */
	limit = MUTEX_SPIN_LIMIT;
	cpus = hal_cpu_count();
	if (cpus <= 1)
		limit = 0;

	/* Takes the mutex as soon as a look finds it free. */
	spins = 0;
	for (;;) {
		locked = __atomic_load_n(&mutex->locked, __ATOMIC_RELAXED);
		if (locked == MUTEX_FREE) {
			acquired = mutex_take_free(mutex);
			if (acquired)
				return 1;
		}

		/* Stops looking once the looks are used up. */
		if (spins == limit)
			break;

		/* Lets the holder's CPU have the line before the next look. */
		hal_atomic_relax();
		spins++;
	}

	/* The mutex stayed held. */
	return 0;
}

/*
 * Takes a mutex under its guard, sleeping on its waiters until a release
 * lets it: the locked word is exchanged for MUTEX_CONTENDED, which makes
 * the next release wake a sleeper, and an exchange that finds it free
 * takes it.  With WAITQ_INTERRUPTIBLE in flags, an interruption gives up
 * with *error set to EINTR and the mutex not taken; otherwise *error is
 * 0.  The caller holds the guard.
 */
static void
mutex_sleep_take(
	struct mutex *mutex,
	unsigned flags,
	int *error)
{
	uint64_t sequence;
	unsigned previous;
	int slept;

	/* Sleeps until an exchange finds the mutex free. */
	*error = 0;
	for (;;) {
		sequence = waitq_sequence(&mutex->waiters);
		previous = __atomic_exchange_n(&mutex->locked, MUTEX_CONTENDED, __ATOMIC_ACQUIRE);
		if (previous == MUTEX_FREE)
			break;

		/* Sleeps until a release wakes this thread, or an interruption ends the wait. */
		slept = waitq_sleep(&mutex->waiters, &mutex->guard, sequence, 0, flags);

		/* Gives up the wait on an interruption, when it may be interrupted. */
		if (slept == EINTR && (flags & WAITQ_INTERRUPTIBLE) != 0) {
			mutex_sleep_give_up(mutex);
			*error = EINTR;
			return;
		}
	}
}

/*
 * Passes the wakeup on when an interrupted thread leaves a mutex's
 * waiters, so that a waiter left asleep is not stranded.
 *
 * A release wakes the waiter at the head, and that waiter can still end its
 * wait with EINTR (a termination or stop request is checked after the
 * wakeup), taking the release's wakeup with it.  The mutex may then be free,
 * or held by a thread that took it free and whose release wakes nobody, with
 * other threads still asleep.  Waking one of them lets it take the mutex or
 * mark it contended again.  The caller holds the guard.
 */
static void
mutex_sleep_give_up(
	struct mutex *mutex)
{
	/* Leaves the mutex alone when nobody else is asleep for it. */
	if (mutex->waiters.head == NULL)
		return;

	/* Wakes the next sleeper, which looks at the mutex again. */
	waitq_wake_one(&mutex->waiters);
}

/*
 * Traps unless a thread owns a mutex: the mutex must be held and its
 * owner must be the thread.
 */
static void
mutex_check_owner(
	struct mutex *mutex,
	struct thread *thread)
{
	struct thread *owner;
	unsigned locked;

	/* Traps on a mutex that is not held. */
	locked = __atomic_load_n(&mutex->locked, __ATOMIC_RELAXED);
	if (locked == MUTEX_FREE)
		__builtin_trap();

	/* Traps on a mutex another thread holds. */
	owner = __atomic_load_n(&mutex->owner, __ATOMIC_RELAXED);
	if (owner != thread)
		__builtin_trap();
}
