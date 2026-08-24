/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Round-robin kernel scheduler
 */

#ifndef ZEDBSD_KERN_SCHED_H
#define ZEDBSD_KERN_SCHED_H

#include <hal/hal.h>
#include <stdint.h>

struct thread;
struct spinlock;

#define SCHED_PRIOR_LEVELS	16
#define SCHED_PRIOR_HIGH	0
#define SCHED_PRIOR_LOW		15
#define SCHED_PRIORITY_DEFAULT	8
#define SCHED_QUANTUM_TICKS	5U

enum sched_queue_kind {
	SCHED_QUEUE_NONE = 0,
	SCHED_QUEUE_RUN,
	SCHED_QUEUE_SLEEP,
};

struct sched {
	int priority;
	uint32_t quantum;
	uint64_t wakeup_tick;
	unsigned queue_kind;
	hal_cpu_id_t cpu;
	hal_cpu_id_t last_cpu;
	unsigned need_migrate;
	struct thread *next;
	struct thread *prev;
};

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
 * Like sched_sleep_locked(), then invokes notify after publishing SLEEPING.
 * notify informs an external observer only: it must not wake the current
 * thread itself.  A later observer action owns the corresponding wakeup.
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

#ifdef ZEDBSD_SCHED_TEST
int
sched_test_cpu_online(
	hal_cpu_id_t,
	struct thread *);
#endif

#endif
