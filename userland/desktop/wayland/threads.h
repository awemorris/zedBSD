/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The compositor's threads: each has a fixed name in one register, is made
 * with every signal blocked (the main thread alone takes the signals), and
 * tells the main thread of its results with one byte on the main thread's
 * wake pipe.  The register is what the startup line KWL THREADS reports.
 *
 * The main thread's deadlines are timers: a feature arms its timer only
 * while it has something planned, and the timer thread writes the wake
 * pipe when the earliest one is due; the main thread then fires the due
 * ones.
 */

#ifndef KWL_THREADS_H
#define KWL_THREADS_H

#include <pthread.h>
#include <stdint.h>

/* The most threads the register holds at once (resident, pool and ad hoc together). */
#define KWL_THREADS_MAX		48U

/* The longest name of a thread, with its terminating zero. */
#define KWL_THREAD_NAME		16U

/*
 * One thread of the compositor's own: its handle, its name in the
 * register, and whether it was started (and so is to be joined).  The
 * record belongs to the thread that started it.
 */
struct kwl_thread {
	pthread_t thread;
	char name[KWL_THREAD_NAME];
	unsigned started;
};

/* The most timers armed at once: every timer is one feature's own record, so this is the program's count of them. */
#define KWL_TIMERS_MAX		128U

/* The most timers told apart by name in one window of the fired timers' counts (KWL PERF timers); the rest are "other". */
#define KWL_TIMERS_TOLD		8U

struct kwl_timer;

/*
 * What a timer does when its time has come: called on the main thread by
 * kwl_timers_take, with the context the take was given.  It may arm the
 * timer again, for a time to come.
 */
typedef void kwl_timer_fire_fn(void *context, struct kwl_timer *timer);

/*
 * One deadline of the main thread's: the time it is due (kwl_milliseconds'
 * clock), its place in the heap of armed timers (1 + its index; 0 while it
 * is not armed), the take that fired it last, and what it does.  The record
 * belongs to the feature that arms it and is written by the main thread
 * only.
 */
struct kwl_timer {
	uint64_t due_ms;
	unsigned place;
	uint64_t taken;
	const char *name;
	kwl_timer_fire_fn *fire;
};

/*
 * The main thread's timers and the timer thread that wakes the main thread
 * when the earliest is due.
 *
 * heap, count, take and published belong to the main thread alone: heap is
 * a min-heap of the armed timers by due_ms, and published is the deadline
 * last given to the thread.  lock guards the rest, which the timer thread
 * reads and writes: next_ms is the deadline the thread waits for
 * (UINT64_MAX: none; the thread sets it to none when it fires, and the
 * main thread's take gives the next), fired is 1 from the thread's firing
 * until the take (so the wake pipe gets one byte for any number of
 * firings), stop ends the thread, and wakes counts the bytes the thread
 * wrote.  wake is the main thread's wake pipe; running says the thread was
 * started and not yet stopped.
 *
 * told_names and told_counts count the timers fired since the last report
 * (kwl_timers_report), by name, the first KWL_TIMERS_TOLD names apart and
 * the rest in told_other; told_total counts them all.  The main thread's
 * alone.
 */
struct kwl_timers {
	struct kwl_timer *heap[KWL_TIMERS_MAX];
	unsigned count;
	uint64_t take;
	uint64_t published;
	pthread_mutex_t lock;
	pthread_cond_t changed;
	uint64_t next_ms;
	unsigned fired;
	unsigned stop;
	unsigned wakes;
	int wake;
	unsigned ready;
	unsigned running;
	struct kwl_thread thread;
	const char *told_names[KWL_TIMERS_TOLD];
	unsigned told_counts[KWL_TIMERS_TOLD];
	unsigned told_kinds;
	unsigned told_other;
	unsigned told_total;
};

int kwl_thread_start(struct kwl_thread *thread, const char *name, void *(*run)(void *), void *argument);
void kwl_thread_join(struct kwl_thread *thread);
int kwl_thread_start_lent(const char *name, void *(*run)(void *), void *argument, pthread_t *handle);
int kwl_thread_start_detached(const char *name, void *(*run)(void *), void *argument);
void kwl_thread_ended(const char *name);
unsigned kwl_threads_count(void);
void kwl_threads_report(void);
unsigned kwl_cpu_count(void);
void kwl_thread_wake(int descriptor);
void kwl_timer_init(struct kwl_timer *timer, const char *name, kwl_timer_fire_fn *fire);
int kwl_timer_armed(const struct kwl_timer *timer);
int kwl_timers_init(struct kwl_timers *timers);
int kwl_timers_start(struct kwl_timers *timers, int wake);
void kwl_timers_stop(struct kwl_timers *timers);
void kwl_timers_destroy(struct kwl_timers *timers);
int kwl_timer_arm(struct kwl_timers *timers, struct kwl_timer *timer, uint64_t due_ms);
void kwl_timer_disarm(struct kwl_timers *timers, struct kwl_timer *timer);
unsigned kwl_timers_take(struct kwl_timers *timers, uint64_t now_ms, void *context);
uint64_t kwl_timers_next(const struct kwl_timers *timers);
uint64_t kwl_timers_now(void);
void kwl_timers_report(struct kwl_timers *timers);

#endif
