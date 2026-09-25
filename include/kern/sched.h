/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Round-robin kernel scheduler
 */

#ifndef KERN_KERN_SCHED_H
#define KERN_KERN_SCHED_H

#include <hal/hal.h>
#include <kern/clock.h>
#include <stdint.h>

struct thread;
struct spinlock;

#define SCHED_PRIOR_LEVELS	16
#define SCHED_PRIOR_HIGH	0
#define SCHED_PRIOR_LOW		15
#define SCHED_PRIORITY_DEFAULT	8
/*
 * The time a thread runs before another of its priority gets a turn.  It
 * is set in milliseconds and counted in ticks of this platform's timer,
 * at least one.
 */
#define SCHED_QUANTUM_MS	10U
#define SCHED_QUANTUM_TICKS						\
	((uint32_t)(KERN_MS_TO_TICKS(SCHED_QUANTUM_MS) != 0U ?		\
	    KERN_MS_TO_TICKS(SCHED_QUANTUM_MS) : 1U))

enum sched_queue_kind {
	SCHED_QUEUE_NONE = 0,
	SCHED_QUEUE_RUN,
	SCHED_QUEUE_SLEEP,
	/* Runnable, woken to run before the thread it found running. */
	SCHED_QUEUE_WOKEN,
};

struct sched {
	int priority;
	uint32_t quantum;
	uint64_t wakeup_tick;
#if SCHED_WAKE_LATENCY
	/* Diagnostic: the counter value when the thread was last woken, and whether the waker shared its CPU. */
	uint64_t woken_at;
	unsigned woken_same_cpu;
#endif
	unsigned queue_kind;
	/* The scheduler tick at which the thread last joined a run queue. */
	uint64_t queued_tick;
	hal_cpu_id_t cpu;
	hal_cpu_id_t last_cpu;
	unsigned need_migrate;
	struct thread *next;
	struct thread *prev;
};

#if SCHED_WAKE_LATENCY
/* Diagnostic: logs the distribution of wake-to-run latencies since the last report. */
void sched_wake_latency_report(void);
#endif

struct sched_queue {
	struct thread *head;
	struct thread *tail;
	unsigned count;
};

void
sched_init(void);

int
sched_prepare_thread(
	struct thread *thread);

void
sched_add(
	struct thread *thread);

void
sched_unlink(
	struct thread *thread);

void
sched_wakeup(
	struct thread *thread);

/*
 * Interrupt an interruptible kernel wait, or force a running remote thread
 * through an IRQ return safe point.
 */
void
sched_interrupt(
	struct thread *thread);

void
sched_switch(void);

void
sched_yield(void);

/*
 * Switches to a thread whose wakeup asked to run before the current one,
 * if any did.  Called where the current thread may give up the CPU, such
 * as on its way back to user mode.
 */
void
sched_preempt_point(void);

/*
 * Disable/enable preemption on the current CPU (nesting).  While disabled the
 * scheduler tick defers the context switch; enable runs any deferred resched.
 */
void
kern_preempt_disable(void);
void
kern_preempt_enable(void);

/* Test-only: current CPU's kern_preempt_disable() nesting depth. */
unsigned
sched_test_preempt_count(void);

void
sched_wait_task(void);

void
sched_notify_task(
	hal_task_t task);

void
sched_exit_current(void)
__attribute__((noreturn));

void
sched_clock_cpu(
	hal_cpu_id_t cpu,
	uint64_t now);

/*
 * Mark kernel execution entered from, and returning to, user mode.  These
 * calls classify scheduler ticks without exposing architecture context
 * details through the HAL task interface.
 */
void
sched_accounting_kernel_enter(void);

void
sched_accounting_kernel_leave(void);

void
sched_sleep(
	uint64_t timeout_tick);

/*
 * Atomically transitions the current thread to sleep, releases an IRQ-safe
 * condition lock, switches, and reacquires that lock before returning.
 */
void
sched_sleep_locked(
	uint64_t timeout_tick,
	struct spinlock *condition_lock);

/*
 * Interruptible wait handoff.  Returns nonzero without sleeping when an
 * interrupt newer than observed_generation was latched before the scheduler
 * could publish THREAD_SLEEPING.
 */
int
sched_sleep_locked_interruptible(
	uint64_t timeout_tick,
	struct spinlock *condition_lock,
	uint64_t observed_generation);

/*
 * Like sched_sleep_locked(), then synchronously invokes notify after
 * publishing THREAD_SLEEPING and before switching away.  The argument is
 * borrowed only for the duration of that call and must not be retained.
 *
 * The callback informs an external observer only: it must not wake the
 * current sleeper, block, or acquire a lock ordered before condition_lock.
 * A later observer action owns the corresponding wakeup.
 */
void
sched_sleep_locked_notify(
	uint64_t timeout_tick,
	struct spinlock *condition_lock,
	void (*notify)(void *),
	void *argument);

void
sched_awake_from_sleep(
	struct thread *thread);

uint64_t
sched_ticks(void);

int
sched_has_runnable(void);

void
sched_idle(void) __attribute__((noreturn));

void
sched_secondary_init(
	hal_cpu_id_t cpu) __attribute__((noreturn));

int
sched_wait_others_online(void);

int
sched_set_cpu(
	struct thread *thread,
	hal_cpu_id_t cpu);

void
sched_cpu_notify(
	hal_cpu_id_t cpu);

#ifdef KERN_SCHED_TEST
int
sched_test_cpu_online(
	hal_cpu_id_t,
	struct thread *);
#endif

#endif
