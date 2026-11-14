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
 * Only a kernel built with the desktop thread balancer
 * (CONFIG_KERN_DESKTOP_BALANCER) keeps any of this; without it the types
 * exist and the calls are empty.
 */

#ifndef KERN_KERN_SCHED_HINT_H
#define KERN_KERN_SCHED_HINT_H

#include <kern/types.h>
#include <stdint.h>

/*
 * Whether the balancer is built.  The build defines it to 1 when it is;
 * otherwise it is 0 here.
 */
#ifndef CONFIG_KERN_DESKTOP_BALANCER
#define CONFIG_KERN_DESKTOP_BALANCER 0
#endif

/*
 * Whether the scheduler hints' stress test is built, which only a test
 * kernel with the balancer is.
 */
#ifndef CONFIG_KERN_DESKTOP_BALANCER_TEST
#define CONFIG_KERN_DESKTOP_BALANCER_TEST 0
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
 * The levels a hint may give a user thread: from the kernel's default
 * level (SCHED_PRIORITY_DEFAULT), which no hint goes above so that the
 * kernel's workers at that level keep their turn, down to the lowest.
 */
#define SCHED_HINT_LEVEL_HIGHEST	8
#define SCHED_HINT_LEVEL_LOWEST		15

/*
 * The level of a user thread the balancer has no reason to raise.  A user
 * thread inside the kernel, which may hold a lock a more urgent thread
 * waits for, is never queued below it.
 */
#define SCHED_HINT_USER_DEFAULT		10

/*
 * The longest a user thread waits behind higher levels before it runs one
 * quantum at the running thread's level (never above the kernel's
 * default), and the wait after which a CPU of another kind may take a
 * thread that asked not to overflow.
 */
#define SCHED_HINT_STARVE_MS		100U

/*
 * How sched_hint_choose() picks: a move to an idle CPU of the kind, the
 * same with the CPU's whole core idle, or the placement of a new or
 * waiting thread.
 */
#define SCHED_HINT_CHOOSE_MOVE		1U
#define SCHED_HINT_CHOOSE_MOVE_IDLE_CORE	2U
#define SCHED_HINT_CHOOSE_PLACE		3U

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
	uint8_t priority;		/* SCHED_HINT_LEVEL_HIGHEST .. SCHED_HINT_LEVEL_LOWEST */
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

#if CONFIG_KERN_DESKTOP_BALANCER

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

/*
 * Publishes the kind of every CPU.
 *
 * Thread context; one caller at a time; takes no CPU lock.  count is the
 * number of CPUs the scheduler runs.  A table of another length, or one
 * that does not name both a FAST and an EFFICIENT CPU, turns every core
 * preference off and leaves the levels.
 */
void
sched_cpu_classes_set(
	const struct sched_cpu_class *classes,
	unsigned count);

/*
 * Stores a user thread's hint and gives it the hint's level.
 *
 * A thread waiting on a run queue moves to its new level there; a sleeping
 * thread keeps its place on the sleep queue and takes the level when it
 * wakes; a running thread whose level drops below a waiting one yields at
 * its next safe point.  A thread raised for one quantum keeps that level
 * until it is queued again.
 *
 * Thread context; takes the thread's CPU lock.  Returns 0, ESRCH for a
 * kernel, idle, new, exiting or bound thread, or EINVAL for a value out of
 * range.
 */
int
sched_hint_set(
	struct thread *thread,
	const struct sched_hint *hint);

/*
 * Moves a user thread now to an idle CPU of the kind its hint names.
 *
 * Thread context; does not sleep (it takes CPU locks and moves a stopped
 * task).  A thread that is not running moves before the call returns,
 * after its state is checked again under its CPU's lock; a running one is
 * marked, and its CPU hands it off at its next notification or tick if an
 * idle CPU of the kind is still there.  Returns 0, EALREADY when it is on
 * a CPU of that kind, EAGAIN when no CPU of the kind is idle (or the CPUs
 * have no kinds), EBUSY when the thread is moving or its CPU's handoff is
 * taken, ESRCH for a thread that cannot move (kernel, new, exiting,
 * bound), or EINVAL for a hint without a kind and PLACE or PLACE_IDLE.
 */
int
sched_hint_place(
	struct thread *thread);

/* Sums the scheduler's hint counters.  Any context; no lock. */
void
sched_hint_counters(
	struct sched_hint_counters *counters);

/*
 * Chooses the CPU a thread of a kind should go to, or KERN_CPU_MAX.
 *
 * A pure function over a snapshot: idle and backlog are
 * KERN_CPU_MASK_WORDS words with one bit per CPU (idle: halted; backlog:
 * has threads waiting).  mode is SCHED_HINT_CHOOSE_*.
 */
kern_cpu_id_t
sched_hint_choose(
	const struct sched_cpu_class *classes,
	unsigned count,
	const uint64_t *idle,
	const uint64_t *backlog,
	unsigned core,
	unsigned mode);

/*
 * Reports the level a user thread with the given base level is queued at,
 * inside the kernel or not.  A pure function.
 */
int
sched_hint_queue_level(
	int base,
	int in_kernel);

/*
 * Reports the level a user thread waiting at one level behind a thread
 * running at another is raised to after waiting so long, or -1 when it is
 * not raised.  A pure function; waited and limit are in ticks.
 */
int
sched_hint_starve_level(
	int running,
	int waiting,
	uint64_t waited,
	uint64_t limit);

#else

static __inline void
sched_note_syscall(void)
{
}

#endif

#if CONFIG_KERN_DESKTOP_BALANCER && CONFIG_KERN_DESKTOP_BALANCER_TEST

/*
 * Starts the hints' stress test when the boot line asks for it: a false
 * table of CPU kinds (desktop_balancer.classes=) and a thread that sets
 * random hints on random user threads and moves them
 * (desktop_balancer.stress=1).  Called once by the boot thread.
 */
void
sched_hint_test_start(void);

#endif

#endif
