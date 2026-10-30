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

#include <kern/clock.h>
#include <kern/types.h>
#include <stdint.h>

struct thread;
struct spinlock;

#define SCHED_PRIOR_LEVELS	16
#define SCHED_PRIOR_HIGH	0
#define SCHED_PRIOR_LOW		15
#define SCHED_PRIORITY_DEFAULT	8
/*
 * The time a thread runs before another of its priority gets a turn.  It
 * is set in milliseconds and counted in ticks, at least one: the
 * conversion rounds up.
 */
#define SCHED_QUANTUM_MS	10U
#define SCHED_QUANTUM_TICKS	((uint32_t)kern_ms_to_ticks(SCHED_QUANTUM_MS))

enum sched_queue_kind {
	SCHED_QUEUE_NONE = 0,
	SCHED_QUEUE_RUN,
	SCHED_QUEUE_SLEEP,
	/* Runnable, woken to run before the thread it found running. */
	SCHED_QUEUE_WOKEN,
};

/*
 * The scheduler's state of one thread.
 *
 * It lives inside the thread, so it exists as long as the thread does.
 */
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

	/*
	 * The latency diagnostic (kern.perf=1).  When a wakeup or
	 * a preemption made the thread runnable, the counter then (zero while
	 * no wait is being measured), why (SCHED_PERF_*), the CPU it waits on
	 * and that CPU's switch count then, which together say how long it
	 * waited and how many threads ran before it.  The CPU lock of the
	 * thread's CPU guards them.
	 */
	uint64_t perf_runnable_at;
	uint64_t perf_runnable_switches;
	unsigned perf_runnable_kind;
	kern_cpu_id_t perf_runnable_cpu;

	/*
	 * How many times the thread has been switched away from its CPU, with
	 * the diagnostic on.  Only the thread's own CPU adds to it, while the
	 * thread is the one running there, so the thread reads it without a
	 * lock: the metadata lock's hold reports how often its holder lost the
	 * CPU while holding it.
	 */
	uint64_t perf_switch_outs;
	kern_cpu_id_t cpu;
	kern_cpu_id_t last_cpu;
	unsigned need_migrate;
	struct thread *next;
	struct thread *prev;
};

#if SCHED_WAKE_LATENCY
/* Diagnostic: logs the distribution of wake-to-run latencies since the last report. */
void sched_wake_latency_report(void);
#endif

/*
 * Which thread a sample of the latency diagnostic belongs to: its process
 * and thread ids, and the start of its process's command (a kernel thread
 * shows pid 0).
 */
struct sched_perf_thread {
	int pid;
	int tid;
	char name[16];
};

/*
 * Whether the boot line turned the latency diagnostic on (kern.perf=1).
 *
 * It is set once by sched_init(), before any other thread runs, and never
 * changes; zero leaves every measured path exactly as it is without it.
 */
extern int sched_perf_active;

/*
 * Reports whether the latency diagnostic is on.
 */
static __inline int
sched_perf_enabled(
	void)
{
	/* Reports the switch the boot line set. */
	return sched_perf_active;
}

/*
 * Reads the counter the latency diagnostic measures with.
 */
uint64_t
sched_perf_counter(void);

/*
 * Converts a difference of sched_perf_counter() readings to microseconds.
 */
uint64_t
sched_perf_counts_to_us(
	uint64_t counts);

/*
 * Names a thread for a sample of the latency diagnostic.
 */
void
sched_perf_identify(
	const struct thread *thread,
	struct sched_perf_thread *who);

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

/*
 * Report the number of the CPU the caller runs on.
 *
 * The answer is stable only while the caller cannot move: with preemption
 * or interrupts disabled, or in interrupt context.
 */
unsigned
kern_cpu_current(void);

/* Test-only: current CPU's kern_preempt_disable() nesting depth. */
unsigned
sched_test_preempt_count(void);

void
sched_notify_task(
	kern_task_handle_t task);

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

/*
 * One CPU's clock ticks since boot by what it was doing when each came
 * (hw.cputimes, ws134-p005): a user thread in user mode, a thread in the
 * kernel or a kernel thread, its idle thread, or between threads.
 */
struct sched_cpu_time {
	uint64_t user;
	uint64_t system;
	uint64_t idle;
	uint64_t other;
};

int
sched_cpu_time(
	kern_cpu_id_t cpu,
	struct sched_cpu_time *time);

int
sched_has_runnable(void);

int
sched_set_cpu(
	struct thread *thread,
	kern_cpu_id_t cpu);

#endif
