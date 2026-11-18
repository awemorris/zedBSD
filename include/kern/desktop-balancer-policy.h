/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The desktop thread balancer's decisions: what a thread is (busy
 * computing, heavy on the CPU), what the person at the machine is doing,
 * and which kind of core and which level each user thread is given.
 *
 * They are pure functions of what the balancer saw, so that they can be
 * tested away from the kernel.  The balancer's thread keeps the history
 * each decision needs in the thread it is about.
 */

#ifndef KERN_KERN_DESKTOP_BALANCER_POLICY_H
#define KERN_KERN_DESKTOP_BALANCER_POLICY_H

#include <kern/sched-hint.h>
#include <stdint.h>

/*
 * How often the balancer looks: while a person uses the machine or did in
 * the last minute, and while nobody did.
 */
#define DESKTOP_BALANCER_PASS_MS		100U
#define DESKTOP_BALANCER_PASS_AWAY_MS		1000U

/*
 * A thread is busy once it has run this long in user mode without a
 * system call, and stops being busy after this many looks in a row found
 * it within the shorter time of its last call.
 */
#define DESKTOP_BALANCER_BUSY_ON_MS		200U
#define DESKTOP_BALANCER_BUSY_OFF_MS		20U
#define DESKTOP_BALANCER_BUSY_OFF_LOOKS		10U

/*
 * A thread is heavy once it ran at least the first share of the time over
 * the last looks of the window, and stops being heavy after this many
 * looks in a row found it under the second share.
 */
#define DESKTOP_BALANCER_HEAVY_WINDOW		10U
#define DESKTOP_BALANCER_HEAVY_ON_PERCENT	50U
#define DESKTOP_BALANCER_HEAVY_OFF_PERCENT	30U
#define DESKTOP_BALANCER_HEAVY_OFF_LOOKS	10U

/* How long after a GPU request a process still counts as using the GPU or presenting. */
#define DESKTOP_BALANCER_GPU_WINDOW_MS		1000U
#define DESKTOP_BALANCER_PRESENT_WINDOW_MS	1000U

/*
 * How long after the last input event a person counts as using the
 * machine, and after how long without one they count as away.
 */
#define DESKTOP_BALANCER_USER_ACTIVE_MS		5000U
#define DESKTOP_BALANCER_USER_AWAY_MS		60000U

/*
 * How seldom one thread is moved, and how many threads one look moves at
 * most, of them how many running ones (which costs their CPU a switch).
 */
#define DESKTOP_BALANCER_MOVE_INTERVAL_MS	500U
#define DESKTOP_BALANCER_MOVES_PER_PASS		32U
#define DESKTOP_BALANCER_HANDOFFS_PER_PASS	8U

/* The most threads of one process one look decides about. */
#define DESKTOP_BALANCER_THREADS_PER_PROCESS	64U

/*
 * The speeds the CPUs are described with when the hardware does not say
 * how fast each kind is (0..1000), and the share of its speed a CPU keeps
 * while its SMT sibling is busy, in thousandths.
 */
#define DESKTOP_BALANCER_FAST_RANK		1000U
#define DESKTOP_BALANCER_EFFICIENT_RANK		700U
#define DESKTOP_BALANCER_SMT_SHARE_PERMILLE	600U

/* A value the hardware did not report. */
#define DESKTOP_BALANCER_UNKNOWN		0xffffffffU

/* What the person at the machine is doing. */
#define DESKTOP_BALANCER_STATE_AWAY		0U	/* no input for a minute, or none since boot */
#define DESKTOP_BALANCER_STATE_QUIET		1U	/* input in the last minute */
#define DESKTOP_BALANCER_STATE_ACTIVE		2U	/* input in the last seconds */

/*
 * The rules, in the order they are tried: the first that matches gives the
 * thread its kind of core.
 */
#define DESKTOP_BALANCER_RULE_NICE_FAST		0U	/* nice below zero */
#define DESKTOP_BALANCER_RULE_NICE_EFFICIENT	1U	/* nice above zero */
#define DESKTOP_BALANCER_RULE_BUSY		2U	/* computing without system calls */
#define DESKTOP_BALANCER_RULE_PRESENTER		3U	/* its process puts images on a display */
#define DESKTOP_BALANCER_RULE_GPU		4U	/* its process puts work on a GPU */
#define DESKTOP_BALANCER_RULE_TERMINAL		5U	/* its process has a terminal and someone is there */
#define DESKTOP_BALANCER_RULE_OTHER		6U
#define DESKTOP_BALANCER_RULES			7U

/*
 * What the balancer remembers of one thread between its looks.
 *
 * It lives in the thread, starts at zero with it, and only the balancer's
 * thread reads or writes it.  The window holds the time the thread ran and
 * the time that passed for each of the last looks, oldest overwritten
 * first.
 */
struct desktop_balancer_history {
	uint64_t run_ticks;		/* the thread's run time at the last look */
	uint64_t moved_tick;		/* when the balancer last moved it, zero never */
	uint32_t window_run[DESKTOP_BALANCER_HEAVY_WINDOW];
	uint32_t window_elapsed[DESKTOP_BALANCER_HEAVY_WINDOW];
	uint8_t window_next;		/* the entry the next look writes */
	uint8_t seen;			/* the balancer has looked at it before */
	uint8_t busy;
	uint8_t heavy;
	uint8_t busy_calm;		/* calm looks in a row while busy */
	uint8_t heavy_calm;		/* light looks in a row while heavy */
	uint8_t hinted;			/* the scheduler holds the hint below */
	uint8_t rule;			/* DESKTOP_BALANCER_RULE_* of the last look */
	struct sched_hint hint;		/* the hint the scheduler holds */
};

/*
 * One look at a thread: its counters as the scheduler keeps them, and the
 * time since the previous look, in scheduler ticks.
 */
struct desktop_balancer_sample {
	uint64_t run_ticks;
	uint64_t elapsed;
	uint32_t user_streak;
};

/* The busy limits of struct desktop_balancer_history, in scheduler ticks. */
struct desktop_balancer_limits {
	uint64_t busy_on;
	uint64_t busy_off;
};

/* What the rules know of one thread and its process. */
struct desktop_balancer_traits {
	int nice;
	unsigned state;			/* DESKTOP_BALANCER_STATE_* */
	uint8_t busy;
	uint8_t heavy;
	uint8_t presenter;
	uint8_t gpu;
	uint8_t terminal;		/* the process has a controlling terminal */
};

/*
 * Updates what a thread is from one look at it: busy and heavy, each
 * turning on fast and off slowly.
 */
void
desktop_balancer_observe(
	struct desktop_balancer_history *history,
	const struct desktop_balancer_sample *sample,
	const struct desktop_balancer_limits *limits);

/*
 * Chooses a thread's hint by the first rule that matches and reports the
 * rule (DESKTOP_BALANCER_RULE_*).
 */
unsigned
desktop_balancer_classify(
	const struct desktop_balancer_traits *traits,
	struct sched_hint *hint);

/* Reports the level a nice value gives a user thread. */
unsigned
desktop_balancer_nice_level(
	int nice);

/*
 * Reports what the person at the machine is doing (DESKTOP_BALANCER_STATE_*)
 * from the time of the last input event (zero: none since boot); all in
 * scheduler ticks.
 */
unsigned
desktop_balancer_user_state(
	uint64_t now,
	uint64_t input_tick,
	uint64_t active,
	uint64_t away);

/*
 * Tests whether something that happened at a tick (zero: never) is no
 * older than a window.
 */
int
desktop_balancer_recent(
	uint64_t now,
	uint64_t tick,
	uint64_t window);

/*
 * Tests whether a thread last moved at a tick (zero: never) may be moved
 * again.
 */
int
desktop_balancer_move_due(
	uint64_t now,
	uint64_t moved_tick,
	uint64_t interval);

/*
 * Describes one CPU to the scheduler from its kind (SCHED_HINT_CORE_*), its
 * speed relative to the fastest CPU (0..100) and how much the hardware
 * asks for it to be used (0..255, 0: avoid it), either one
 * DESKTOP_BALANCER_UNKNOWN when not reported, and whether it has an SMT
 * sibling.  The core group is left to the caller.
 */
void
desktop_balancer_describe_cpu(
	unsigned core,
	uint32_t relative,
	uint32_t available,
	int has_sibling,
	struct sched_cpu_class *description);

#endif
