/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The scheduler's hints: the kind of core a thread would rather run on,
 * how hard the scheduler should try, and the priority level the thread
 * returns to; the table of the CPUs' kinds the hints are matched against;
 * and the counters a balancer reads to decide what to hint.
 *
 * The scheduler keeps the hints and acts on them where it already places
 * threads: a new thread, an idle CPU woken for a waiting thread, a steal,
 * and a move asked for explicitly.  Whoever writes the hints decides
 * nothing about where a thread runs next; a hint the CPUs cannot honour
 * now is refused, not queued.
 *
 * Only a kernel built with the desktop thread balancer has any of this.
 */

#ifndef KERN_KERN_SCHED_HINT_H
#define KERN_KERN_SCHED_HINT_H

#include <kern/types.h>
#include <stdbool.h>
#include <stdint.h>

struct thread;

/* The kinds of core a hint names and a CPU is. */
#define SCHED_HINT_CORE_ANY		0U	/* no preference; a CPU of unknown kind */
#define SCHED_HINT_CORE_FAST		1U
#define SCHED_HINT_CORE_EFFICIENT	2U

/*
 * How hard a hint is followed.  PREFER: placement and stealing favour the
 * kind, nothing is moved for it.  PLACE: sched_hint_place() may move the
 * thread, running or not, to a CPU of the kind.
 */
#define SCHED_HINT_PREFER		1U
#define SCHED_HINT_PLACE		2U

/*
 * A CPU of another kind steals the thread only once it has waited longer
 * than the starvation limit.
 */
#define SCHED_HINT_NO_OVERFLOW		0x1U

/* The longest a thread lowered below the default waits before one quantum at the running level. */
#define SCHED_HINT_STARVE_MS		100U

/*
 * What a balancer asks of one thread.
 *
 * The priority is the level the thread returns to whenever it is queued;
 * the scheduler may raise it above that for one quantum to keep it from
 * starving.  A hint with strength 0 asks for no kind of core.
 */
struct sched_hint {
	uint8_t core;			/* SCHED_HINT_CORE_* */
	uint8_t strength;		/* 0, SCHED_HINT_PREFER or SCHED_HINT_PLACE */
	uint8_t flags;			/* SCHED_HINT_NO_OVERFLOW */
	uint8_t priority;		/* SCHED_PRIOR_HIGH .. SCHED_PRIOR_LOW */
};

/*
 * What one CPU is, as the balancer tells the scheduler.
 *
 * rank is the speed one thread gets on the CPU while its SMT sibling is
 * idle, sibling_rank the speed while the sibling is busy, both 0..1000 and
 * comparable across kinds.  CPUs of one core share core_group.  A CPU the
 * hardware asks to avoid is not usable and is never chosen for a move.
 */
struct sched_cpu_class {
	uint8_t core;			/* SCHED_HINT_CORE_* */
	uint8_t usable;
	uint16_t rank;
	uint16_t sibling_rank;
	uint16_t core_group;
};

/*
 * What one thread did, as its counters stand.
 *
 * The counters only grow (the 32-bit ones wrap); a reader keeps the values
 * it saw and subtracts.  user_streak is the ticks the thread ran in user
 * mode since its last system call, and drops to zero at the next one.
 */
struct sched_hint_activity {
	uint64_t run_ticks;
	uint32_t user_streak;
	uint32_t syscalls;
	uint32_t wakeups;
	kern_cpu_id_t cpu;		/* where it is queued or runs now */
	uint8_t cpu_core;		/* that CPU's SCHED_HINT_CORE_* */
	uint8_t running;
	uint16_t reserved;
};

/*
 * The scheduler's record of what it did with the hints, summed over the
 * CPUs when read.  Every counter only grows.
 */
struct sched_hint_counters {
	uint64_t hints;			/* hints that changed a thread */
	uint64_t moves;			/* threads moved while not running */
	uint64_t refused;		/* moves refused for want of a CPU of the kind */
	uint64_t handoffs;		/* running threads handed to another CPU */
	uint64_t handoff_retries;
	uint64_t handoff_failures;	/* handed back to their old CPU */
	uint64_t starved;		/* threads raised for one quantum */
};

/*
 * The hint state a thread carries, in its scheduler record.
 *
 * The thread's CPU lock guards the hint, the base level and move_pending.
 * The counters have one writer each (the thread's own CPU at its tick and
 * at its system call entry, the waker under the CPU lock) and are read
 * without a lock.
 */
struct sched_hint_state {
	struct sched_hint hint;
	uint8_t base_priority;
	uint8_t move_pending;		/* a running thread is to be handed off */
	uint8_t raised;			/* priority is above base for one quantum */
	uint8_t reserved;
	volatile uint64_t run_ticks;
	volatile uint32_t user_streak;
	volatile uint32_t syscalls;
	volatile uint32_t wakeups;
};

/*
 * Publishes the kind of every CPU.
 *
 * Thread context; takes no CPU lock.  count is the number of CPUs the
 * scheduler runs; NULL or a table naming one kind only turns every core
 * preference off, leaving the priorities.
 */
void
sched_cpu_classes_set(
	const struct sched_cpu_class *classes,
	unsigned count);

/*
 * Stores a thread's hint and moves it to its new level if it is queued.
 *
 * Any thread context; takes the thread's CPU lock.  Returns 0, or EINVAL
 * for an idle, exiting or dead thread or a value out of range.
 */
int
sched_hint_set(
	struct thread *thread,
	const struct sched_hint *hint);

/*
 * Moves a thread now to a CPU of the kind its hint names.
 *
 * Thread context; may sleep only as sched_set_cpu() does.  A thread that
 * is not running moves before the call returns; a running one is marked
 * and its CPU hands it off at its next notification or tick.  Returns 0,
 * EALREADY when it is on a CPU of that kind, EAGAIN when no CPU of the
 * kind is idle or free of waiting threads, EBUSY when the thread is
 * already moving, or EINVAL for a hint without PLACE.
 */
int
sched_hint_place(
	struct thread *thread);

/* Reads a thread's counters and where it is.  Any context; no lock. */
void
sched_hint_activity(
	const struct thread *thread,
	struct sched_hint_activity *activity);

/*
 * Counts a system call of the current thread and ends its user streak.
 *
 * Called from the system call entry with interrupts masked, so that the
 * thread's own tick does not race it.
 */
void
sched_note_syscall(void);

/* Sums the scheduler's hint counters.  Any context; no lock. */
void
sched_hint_counters(
	struct sched_hint_counters *counters);

/*
 * Chooses the CPU a thread of the given kind should move to.
 *
 * A pure function over a snapshot, so that it can be tested alone:
 * idle and backlog hold one bit per CPU (idle: halted with nothing
 * queued; backlog: has threads waiting).  For FAST: a usable FAST CPU
 * whose core is idle, then the idle usable CPUs ranked by rank (EFFICIENT)
 * or sibling_rank (FAST with a busy sibling), then a FAST CPU without a
 * backlog.  For EFFICIENT: an idle usable EFFICIENT CPU, then one without
 * a backlog.  Ties go to the lowest number.  Returns the CPU, or
 * KERN_CPU_MAX when none fits.
 */
kern_cpu_id_t
sched_hint_choose(
	const struct sched_cpu_class *classes,
	unsigned count,
	const uint64_t *idle,
	const uint64_t *backlog,
	unsigned core);

#endif
