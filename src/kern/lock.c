/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <kern/lock.h>
#include <kern/waitq.h>
#include <kern/signal.h>
#include <kern/thread.h>

#include <errno.h>
#include <hal/hal.h>

#ifdef ZEDBSD_LOCKDEP
#define LOCKDEP_MAX_DEPTH 32U
struct lockdep_cpu_state {
	struct spinlock *held[LOCKDEP_MAX_DEPTH];
	unsigned depth;
};
static struct lockdep_cpu_state lockdep_cpus[HAL_CPU_MAX];

static void
lockdep_acquire(struct spinlock *lock, unsigned cpu)
{
	struct lockdep_cpu_state *state;

	if (cpu >= HAL_CPU_MAX)
		__builtin_trap();
	state = &lockdep_cpus[cpu];
	if (state->depth >= LOCKDEP_MAX_DEPTH)
		__builtin_trap();
	if (state->depth != 0 &&
	    lock->rank < state->held[state->depth - 1U]->rank)
		__builtin_trap();
	state->held[state->depth++] = lock;
}

static void
lockdep_release(struct spinlock *lock, unsigned cpu)
{
	struct lockdep_cpu_state *state;

	if (cpu >= HAL_CPU_MAX)
		__builtin_trap();
	state = &lockdep_cpus[cpu];
	if (state->depth == 0 || state->held[state->depth - 1U] != lock)
		__builtin_trap();
	state->held[--state->depth] = NULL;
}
#else
static void lockdep_acquire(struct spinlock *lock, unsigned cpu)
{ (void)lock; (void)cpu; }
static void lockdep_release(struct spinlock *lock, unsigned cpu)
{ (void)lock; (void)cpu; }
#endif

void spin_init(struct spinlock *lock, enum lock_rank rank, const char *name)
{
	lock->held.value = 0;
	lock->rank = rank;
	lock->name = name;
	lock->owner_cpu = 0;
	lock->owner_valid = 0;
}
int spin_trylock(struct spinlock *lock)
{
	unsigned cpu = hal_cpu_current();
	if (atomic_load_acquire(&lock->held) != 0 && lock->owner_valid &&
	    lock->owner_cpu == cpu)
		__builtin_trap();
	if (!atomic_try_acquire_zero(&lock->held))
		return 0;
	lockdep_acquire(lock, cpu);
	lock->owner_cpu = cpu;
	__atomic_store_n(&lock->owner_valid, 1U, __ATOMIC_RELEASE);
	return 1;
}
void spin_lock(struct spinlock *lock)
{ while (!spin_trylock(lock)) __asm__ volatile("" ::: "memory"); }
void spin_unlock(struct spinlock *lock)
{
	unsigned cpu = hal_cpu_current();
	if (atomic_load_acquire(&lock->held) == 0 || !lock->owner_valid ||
	    lock->owner_cpu != cpu)
		__builtin_trap();
	lockdep_release(lock, cpu);
	__atomic_store_n(&lock->owner_valid, 0U, __ATOMIC_RELEASE);
	atomic_store_release(&lock->held, 0);
}
unsigned long spin_lock_irqsave(struct spinlock *lock)
{ unsigned long enabled = hal_irq_disable() ? 1UL : 0UL; spin_lock(lock); return enabled; }
void spin_unlock_irqrestore(struct spinlock *lock, unsigned long enabled)
{ spin_unlock(lock); if (enabled != 0) hal_irq_enable(); }

int mutex_init(struct mutex *mutex, enum lock_rank rank, const char *name)
{
	if (mutex == NULL) return EINVAL;
	spin_init(&mutex->guard, rank, name);
	mutex->owner = NULL; mutex->locked = 0;
	waitq_init(&mutex->waiters, name);
	return 0;
}
int mutex_lock_interruptible(struct mutex *mutex)
{
	unsigned long irq;
	struct thread *thread = thread_current();
	if (mutex == NULL || thread == NULL) return EINVAL;
	irq = spin_lock_irqsave(&mutex->guard);
	if (mutex->owner == thread) __builtin_trap();
	while (mutex->locked) {
		uint64_t sequence = waitq_sequence(&mutex->waiters);
		int error = waitq_sleep(&mutex->waiters, &mutex->guard,
		    sequence, 0, WAITQ_INTERRUPTIBLE);
		if (error == EINTR) {
			spin_unlock_irqrestore(&mutex->guard, irq);
			return EINTR;
		}
	}
	mutex->locked = 1; mutex->owner = thread;
	spin_unlock_irqrestore(&mutex->guard, irq);
	return 0;
}
void mutex_lock(struct mutex *mutex)
{ while (mutex_lock_interruptible(mutex) == EINTR) ; }
void mutex_unlock(struct mutex *mutex)
{
	unsigned long irq;
	if (mutex == NULL) __builtin_trap();
	irq = spin_lock_irqsave(&mutex->guard);
	if (!mutex->locked || mutex->owner != thread_current()) __builtin_trap();
	mutex->owner = NULL; mutex->locked = 0;
	waitq_wake_one(&mutex->waiters);
	spin_unlock_irqrestore(&mutex->guard, irq);
}
