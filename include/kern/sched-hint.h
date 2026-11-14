/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The scheduler's hints: the kind of core a user thread would rather run
 * on, how hard the scheduler should try, and the priority level the thread
 * returns to; the description of the CPUs' kinds the hints are matched
 * against; and the counters a balancer reads to decide what to hint.
 *
 * The scheduler keeps the hints and the counters.  Whoever writes the
 * hints decides nothing about where a thread runs next; a hint the CPUs
 * cannot honour now is refused, not queued.
 *
 * Only a kernel built with the desktop thread balancer (CONFIG_DTB) keeps
 * any of this; without it the types exist and the calls are empty.
 */

#ifndef KERN_KERN_SCHED_HINT_H
#define KERN_KERN_SCHED_HINT_H

#include <kern/types.h>
#include <stdint.h>

/*
 * Whether the balancer is built.  The build defines it to 1 when it is;
 * otherwise it is 0 here.
 */
#ifndef CONFIG_DTB
#define CONFIG_DTB 0
#endif

struct thread;

/* The kinds of core a hint names and a CPU is. */
#define SCHED_HINT_CORE_ANY		0U	/* no preference; a CPU of unknown kind */
#define SCHED_HINT_CORE_FAST		1U
#define SCHED_HINT_CORE_EFFICIENT	2U

/*
 * How hard a hint is followed.  PREFER: placement and stealing favour the
 * kind, nothing is moved for it.  PLACE: the thread may be moved, running
 * or not, to an idle CPU of the kind.  PLACE_IDLE: the same, but only to a
 * CPU whose SMT sibling is idle too.
 */
#define SCHED_HINT_PREFER		1U
#define SCHED_HINT_PLACE		2U
#define SCHED_HINT_PLACE_IDLE		3U

/*
 * A CPU of another kind steals the thread only once it has waited longer
 * than the starvation limit.
 */
#define SCHED_HINT_NO_OVERFLOW		0x1U

/*
 * What a balancer asks of one user thread.
 *
 * The priority is the level the thread returns to whenever it is queued.
 * A hint with strength 0 asks for no kind of core.
 */
struct sched_hint {
	uint8_t core;			/* SCHED_HINT_CORE_* */
	uint8_t strength;		/* 0 or SCHED_HINT_PREFER, _PLACE, _PLACE_IDLE */
	uint8_t flags;			/* SCHED_HINT_NO_OVERFLOW */
	uint8_t priority;		/* a scheduler level, SCHED_PRIOR_HIGH .. SCHED_PRIOR_LOW */
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
 * What one thread did, as its counters stand, and where it is.
 *
 * The counters only grow (the 32-bit ones wrap); a reader keeps the values
 * it saw and subtracts.  The times are in scheduler ticks.  user_streak is
 * the time the thread ran in user mode since its last system call; it
 * drops to zero at the next one and stops at its maximum instead of
 * wrapping.  Only user threads have their run time counted.
 */
struct sched_hint_activity {
	uint64_t run_ticks;
	uint32_t user_streak;
	uint32_t syscalls;
	uint32_t wakeups;
	kern_cpu_id_t cpu;		/* where it is queued or runs now */
	uint8_t running;		/* it was running when read */
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
	uint64_t kernel_held;		/* user threads queued at the user default while in the kernel */
};

/*
 * The hint state a thread carries in its scheduler record.
 *
 * The thread's CPU lock guards the hint, the base level, move_pending and
 * raised.  base_priority starts equal to the thread's priority when the
 * thread is made.  move_pending lives only while the thread runs: queueing
 * it or switching away without queueing it clears the mark.
 *
 * run_ticks and user_streak are written by the tick of the CPU the thread
 * runs on, user_streak and syscalls by the thread's own system call entry,
 * both with interrupts masked on that CPU, so the two never overlap.
 * wakeups is written under the lock of whichever CPU holds the thread when
 * it wakes.  Readers load them without a lock and get a recent value.
 */
struct sched_hint_state {
	struct sched_hint hint;
	uint8_t base_priority;
	uint8_t move_pending;		/* a running thread is to be handed off */
	uint8_t raised;			/* priority is above base for one quantum */
	uint8_t reserved;
	volatile uint64_t run_ticks;
	volatile unsigned user_streak;
	volatile unsigned syscalls;
	volatile unsigned wakeups;
};

#if CONFIG_DTB

/*
 * Reads a thread's counters and where it is.
 *
 * Any context; takes no lock.  The values are each recent, not one
 * snapshot taken at once.
 */
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

#else

static __inline void
sched_note_syscall(void)
{
}

#endif

#endif
