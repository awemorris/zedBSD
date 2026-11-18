/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A single-threaded stand-in for the kernel's locks, wait queues and ticks
 * (see host_kernel.h).
 */

#include "contract.h"
#include "host_kernel.h"

#include <kern/clock.h>
#include <kern/klog.h>
#include <kern/lock.h>
#include <kern/sched.h>
#include <kern/waitq.h>

#include <errno.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * How many sleeps one program may take before it is declared hung.
 *
 * Every sleep lets one tick pass, so any bounded wait ends long before
 * this; only a sleep without a deadline that nothing wakes reaches it.
 */
#define HOST_KERNEL_SLEEP_LIMIT	1000000U

/*
 * The current scheduler tick.
 *
 * It advances by one on every sleep and never otherwise; the program is
 * single-threaded.
 */
static uint64_t host_ticks;

/*
 * How many sleeps have been taken since the last reset.
 */
static unsigned host_sleeps;

/*
 * How many spinlocks are held right now.
 *
 * A contract group ends with it back at zero; a sleep releases the
 * sleeper's lock for its duration, as the kernel does.
 */
static int host_lock_depth;

/*
 * What a sleep runs before it returns, and the argument it receives.
 *
 * NULL means nothing happens while the sleeper is away.  The test sets it
 * and clears it around one wait.
 */
static void (*host_sleep_hook)(void *context);
static void *host_sleep_context;

/*
 * Starts the tick, the sleep count and the lock depth from zero and clears the hook.
 */
void
host_kernel_reset(void)
{
	/* Nothing has happened yet. */
	host_ticks = 0U;
	host_sleeps = 0U;
	host_lock_depth = 0;

	/* Nothing acts while a sleeper is away. */
	host_sleep_hook = NULL;
	host_sleep_context = NULL;
}

/*
 * Sets what each following sleep runs while the sleeper is away.
 */
void
host_kernel_set_sleep_hook(
	void (*hook)(void *context),
	void *context)
{
	/* Replaces the hook; NULL clears it. */
	host_sleep_hook = hook;
	host_sleep_context = context;
}

/*
 * Reports the current scheduler tick.
 */
uint64_t
host_kernel_ticks(void)
{
	/* Reports the tick the last sleep left. */
	return host_ticks;
}

/*
 * Reports how many sleeps have been taken since the last reset.
 */
unsigned
host_kernel_sleeps(void)
{
	/* Reports the sleep count. */
	return host_sleeps;
}

/*
 * Reports how many spinlocks are held right now.
 */
int
host_kernel_lock_depth(void)
{
	/* Reports the depth; zero between contract groups. */
	return host_lock_depth;
}

/*
 * Prepares a spinlock; the stand-in keeps only its name.
 */
void
spin_init(
	struct spinlock *lock,
	enum lock_rank rank,
	const char *name)
{
	/* Records what the kernel would, without any owner. */
	lock->rank = rank;
	lock->name = name;
	lock->owner_cpu = 0U;
	lock->owner_valid = 0U;
}

/*
 * Takes a spinlock; single-threaded, so it only counts the hold.
 */
void
spin_lock(
	struct spinlock *lock)
{
	UNUSED_PARAMETER(lock);

	/* One more hold. */
	host_lock_depth++;
}

/*
 * Gives back a spinlock taken with spin_lock().
 */
void
spin_unlock(
	struct spinlock *lock)
{
	UNUSED_PARAMETER(lock);

	/* One hold fewer. */
	host_lock_depth--;
}

/*
 * Takes a spinlock with interrupts off; there are no interrupts to turn off.
 */
unsigned long
spin_lock_irqsave(
	struct spinlock *lock)
{
	UNUSED_PARAMETER(lock);

	/* One more hold. */
	host_lock_depth++;

	/* Succeeded: reports interrupts as having been enabled. */
	return 1UL;
}

/*
 * Gives back a spinlock taken with spin_lock_irqsave().
 */
void
spin_unlock_irqrestore(
	struct spinlock *lock,
	unsigned long enabled)
{
	UNUSED_PARAMETER(lock);
	UNUSED_PARAMETER(enabled);

	/* One hold fewer. */
	host_lock_depth--;
}

/*
 * Prepares an empty wait queue.
 */
void
waitq_init(
	struct wait_queue *queue,
	const char *name)
{
	/* No sleeper and no wake-up yet. */
	queue->head = NULL;
	queue->tail = NULL;
	queue->sequence = 0U;
	queue->name = name;
}

/*
 * Reports how many wake-ups the queue has seen.
 */
uint64_t
waitq_sequence(
	const struct wait_queue *queue)
{
	/* A sleeper compares this against the value it saw before sleeping. */
	return queue->sequence;
}

/*
 * Sleeps until a wake-up after observed, or until the deadline.
 *
 * Runs the sleep hook with the condition lock released, then lets one tick
 * pass.  Returns 0 after a wake-up or a spurious return, or ETIMEDOUT once
 * the deadline has passed.
 */
int
waitq_sleep(
	struct wait_queue *queue,
	struct spinlock *condition_lock,
	uint64_t observed,
	uint64_t deadline,
	unsigned flags)
{
	UNUSED_PARAMETER(condition_lock);
	UNUSED_PARAMETER(flags);

	/* A wait nothing ever ends would spin the program forever. */
	host_sleeps++;
	if (host_sleeps > HOST_KERNEL_SLEEP_LIMIT) {
		printf("host: sleep on %s never ended\n", queue->name);
		abort();
	}

	/* The sleeper is off the CPU and has let go of its lock while the hook acts. */
	host_lock_depth--;
	if (host_sleep_hook != NULL)
		host_sleep_hook(host_sleep_context);
	host_lock_depth++;

	/* One scheduler tick passes during the sleep. */
	host_ticks++;

	/* A wake-up during the sleep ends it. */
	if (queue->sequence != observed)
		return 0;

	/* The deadline ends it once it has passed; zero means no deadline. */
	if (deadline != 0U && host_ticks >= deadline)
		return ETIMEDOUT;

	/* Succeeded: a spurious return, which the caller re-checks. */
	return 0;
}

/*
 * Wakes one sleeper; the stand-in wakes them all.
 */
void
waitq_wake_one(
	struct wait_queue *queue)
{
	/* Each wake-up moves the sequence a sleeper compares against. */
	queue->sequence++;
}

/*
 * Wakes every sleeper.
 */
void
waitq_wake_all(
	struct wait_queue *queue)
{
	/* Each wake-up moves the sequence a sleeper compares against. */
	queue->sequence++;
}

/*
 * Reports the current scheduler tick.
 */
uint64_t
sched_ticks(void)
{
	/* Reports the tick the last sleep left. */
	return host_ticks;
}

/*
 * Converts milliseconds to ticks as the kernel does (src/kern/clock.c),
 * rounding up; the host's tick is a millisecond.
 */
uint64_t
kern_ms_to_ticks(
	uint64_t milliseconds)
{
	/* Reports the ticks of the host clock. */
	return milliseconds;
}

/*
 * Converts nanoseconds to ticks, rounding up.
 */
uint64_t
kern_ns_to_ticks(
	uint64_t nanoseconds)
{
	/* Reports the whole milliseconds that cover the time. */
	return (nanoseconds + 999999U) / 1000000U;
}

/*
 * Converts ticks to milliseconds, rounding down.
 */
uint64_t
kern_ticks_to_ms(
	uint64_t ticks)
{
	/* Reports the milliseconds the ticks counted. */
	return ticks;
}

/*
 * Converts ticks to nanoseconds, rounding down.
 */
uint64_t
kern_ticks_to_ns(
	uint64_t ticks)
{
	/* Reports the nanoseconds the ticks counted. */
	return ticks * 1000000U;
}

/*
 * Computes a tick deadline as the kernel does (src/kern/clock.c): the sum,
 * or EOVERFLOW past the tick range.  The delayed works compute theirs with
 * it (the RPS idle check arms one).
 */
int
kern_deadline_after(
	uint64_t now,
	uint64_t delta,
	uint64_t *deadline)
{
	/* Rejects a missing result. */
	if (deadline == NULL)
		return EINVAL;

	/* Rejects a deadline beyond the tick range. */
	if (now > UINT64_MAX - delta)
		return EOVERFLOW;
	*deadline = now + delta;

	/* Reports the computed deadline. */
	return 0;
}

/*
 * Prints a kernel log line on the host's standard output.
 */
void
kern_logf(
	const char *format,
	...)
{
	va_list arguments;

	/* Marks the line as coming from the driver under test. */
	printf("kern: ");

	/* Prints the line as the kernel log would. */
	va_start(arguments, format);
	vprintf(format, arguments);
	va_end(arguments);
}
