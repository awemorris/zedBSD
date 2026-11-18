/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The desktop thread balancer's decisions that need nothing but numbers:
 * whether a thread is busy or heavy from the looks the balancer took at
 * it, which rule a thread falls under and the hint that rule gives, the
 * level a nice value gives, what the person at the machine is doing, and
 * how one CPU is described to the scheduler.
 *
 * They hold no lock and read no kernel object, so that they can be tested
 * on their own.
 */

#include <kern/desktop-balancer-policy.h>
#include <stddef.h>

/*
 * The levels the rules give.  A thread whose process presents images or
 * puts work on a GPU is raised above the user default unless it is heavy
 * on the CPU; a nice value moves the level a step or two either way; no
 * user thread goes above the kernel's default level.
 */
#define POLICY_LEVEL_PRESENTER		8U
#define POLICY_LEVEL_GPU		9U
#define POLICY_LEVEL_NICE_URGENT	8U	/* nice -20 .. -10 */
#define POLICY_LEVEL_NICE_EAGER		9U	/* nice -9 .. -1 */
#define POLICY_LEVEL_NICE_PATIENT	11U	/* nice 1 .. 9 */
#define POLICY_LEVEL_NICE_IDLE		12U	/* nice 10 .. 20 */

/* The nice values where the levels change. */
#define POLICY_NICE_URGENT		(-10)
#define POLICY_NICE_IDLE		10

static void observe_busy(struct desktop_balancer_history *history, const struct desktop_balancer_sample *sample, const struct desktop_balancer_limits *limits);
static void observe_heavy(struct desktop_balancer_history *history, const struct desktop_balancer_sample *sample);
static unsigned display_level(const struct desktop_balancer_traits *traits);
static void hint_fill(struct sched_hint *hint, unsigned core, unsigned strength, unsigned flags, unsigned level);

/*
 * Updates what a thread is from one look at it.
 *
 * The first look only notes the thread's run time, which the heavy
 * window needs a difference of; the busy test needs none.
 */
void
desktop_balancer_observe(
	struct desktop_balancer_history *history,
	const struct desktop_balancer_sample *sample,
	const struct desktop_balancer_limits *limits)
{
	/* Busy depends only on the streak the scheduler counts. */
	observe_busy(history, sample, limits);

	/* A thread seen the first time starts its window from now. */
	if (!history->seen) {
		history->seen = 1;
		history->run_ticks = sample->run_ticks;
		return;
	}

	/* Heavy depends on the share of the time it ran lately. */
	observe_heavy(history, sample);
	history->run_ticks = sample->run_ticks;
}

/*
 * Chooses a thread's hint by the first rule that matches.
 *
 * A nice value is the user's own word and comes first; a thread
 * computing without system calls goes to the efficient cores even in a
 * process that draws; the display and GPU processes go to the fast ones;
 * a terminal's threads go to a fast core only while someone is there and
 * only where the whole core is free, so that they never take the seat the
 * drawing processes need.
 */
unsigned
desktop_balancer_classify(
	const struct desktop_balancer_traits *traits,
	struct sched_hint *hint)
{
	unsigned level;
	unsigned display;

	/* A nice value below zero: a fast core, at the higher of its level and the display's. */
	if (traits->nice < 0) {
		level = desktop_balancer_nice_level(traits->nice);
		display = display_level(traits);
		if (display < level)
			level = display;
		hint_fill(hint, SCHED_HINT_CORE_FAST, SCHED_HINT_PLACE, 0U, level);
		return DESKTOP_BALANCER_RULE_NICE_FAST;
	}

	/* A nice value above zero: an efficient core it does not overflow from, at its lower level. */
	if (traits->nice > 0) {
		level = desktop_balancer_nice_level(traits->nice);
		hint_fill(hint, SCHED_HINT_CORE_EFFICIENT, SCHED_HINT_PLACE, SCHED_HINT_NO_OVERFLOW, level);
		return DESKTOP_BALANCER_RULE_NICE_EFFICIENT;
	}

	/* Computing without system calls: an efficient core, at the user default. */
	if (traits->busy) {
		hint_fill(hint, SCHED_HINT_CORE_EFFICIENT, SCHED_HINT_PLACE, 0U, SCHED_HINT_USER_DEFAULT);
		return DESKTOP_BALANCER_RULE_BUSY;
	}

	/* Putting images on a display: a fast core, raised unless heavy. */
	if (traits->presenter) {
		level = display_level(traits);
		hint_fill(hint, SCHED_HINT_CORE_FAST, SCHED_HINT_PLACE, 0U, level);
		return DESKTOP_BALANCER_RULE_PRESENTER;
	}

	/* Putting work on a GPU: a fast core, raised unless heavy. */
	if (traits->gpu) {
		level = display_level(traits);
		hint_fill(hint, SCHED_HINT_CORE_FAST, SCHED_HINT_PLACE, 0U, level);
		return DESKTOP_BALANCER_RULE_GPU;
	}

	/* A terminal's thread while someone is there: a fast core whose whole core is free. */
	if (traits->terminal && traits->state != DESKTOP_BALANCER_STATE_AWAY) {
		hint_fill(hint, SCHED_HINT_CORE_FAST, SCHED_HINT_PLACE_IDLE, 0U, SCHED_HINT_USER_DEFAULT);
		return DESKTOP_BALANCER_RULE_TERMINAL;
	}

	/* Anything else: no kind of core, at the user default. */
	hint_fill(hint, SCHED_HINT_CORE_ANY, 0U, 0U, SCHED_HINT_USER_DEFAULT);
	return DESKTOP_BALANCER_RULE_OTHER;
}

/*
 * Reports the level a nice value gives a user thread.
 */
unsigned
desktop_balancer_nice_level(
	int nice)
{
	/* The most urgent tenth of the range goes to the kernel's default level. */
	if (nice <= POLICY_NICE_URGENT)
		return POLICY_LEVEL_NICE_URGENT;

	/* The rest of the urgent half, one level above the user default. */
	if (nice < 0)
		return POLICY_LEVEL_NICE_EAGER;

	/* Zero keeps the user default. */
	if (nice == 0)
		return SCHED_HINT_USER_DEFAULT;

	/* The milder half of the patient side, one level below. */
	if (nice < POLICY_NICE_IDLE)
		return POLICY_LEVEL_NICE_PATIENT;

	/* The most patient, two levels below. */
	return POLICY_LEVEL_NICE_IDLE;
}

/*
 * Reports what the person at the machine is doing.
 */
unsigned
desktop_balancer_user_state(
	uint64_t now,
	uint64_t input_tick,
	uint64_t active,
	uint64_t away)
{
	uint64_t quiet;

	/* No input since boot: nobody has been there. */
	if (input_tick == 0U)
		return DESKTOP_BALANCER_STATE_AWAY;

	/* An input time ahead of the reader's clock is a moment ago. */
	quiet = 0U;
	if (now > input_tick)
		quiet = now - input_tick;

	/* Input within the active time: someone is using the machine. */
	if (quiet <= active)
		return DESKTOP_BALANCER_STATE_ACTIVE;

	/* Input within the last minute: someone is reading or thinking. */
	if (quiet < away)
		return DESKTOP_BALANCER_STATE_QUIET;

	/* Nobody touched it for long. */
	return DESKTOP_BALANCER_STATE_AWAY;
}

/*
 * Tests whether something that happened at a tick is no older than a
 * window.
 */
int
desktop_balancer_recent(
	uint64_t now,
	uint64_t tick,
	uint64_t window)
{
	/* Never happened. */
	if (tick == 0U)
		return 0;

	/* A time ahead of the reader's clock is a moment ago. */
	if (tick >= now)
		return 1;

	/* Within the window. */
	if (now - tick <= window)
		return 1;

	/* Too long ago. */
	return 0;
}

/*
 * Tests whether a thread last moved at a tick may be moved again.
 */
int
desktop_balancer_move_due(
	uint64_t now,
	uint64_t moved_tick,
	uint64_t interval)
{
	/* A thread never moved may move. */
	if (moved_tick == 0U)
		return 1;

	/* A move time ahead of the reader's clock is a moment ago. */
	if (moved_tick >= now)
		return 0;

	/* Once the interval has passed. */
	if (now - moved_tick >= interval)
		return 1;

	/* Too soon after the last move. */
	return 0;
}

/*
 * Describes one CPU to the scheduler.
 *
 * A reported relative speed gives the rank, ten to a percent; without one
 * the kind gives it.  A CPU with an SMT sibling keeps a share of its rank
 * while the sibling is busy.  A CPU the hardware reports no capacity for
 * is not usable.
 */
void
desktop_balancer_describe_cpu(
	unsigned core,
	uint32_t relative,
	uint32_t available,
	int has_sibling,
	struct sched_cpu_class *description)
{
	uint32_t rank;

	/* The kind; a kind the scheduler does not know is no kind. */
	description->core = SCHED_HINT_CORE_ANY;
	if (core == SCHED_HINT_CORE_FAST || core == SCHED_HINT_CORE_EFFICIENT)
		description->core = (uint8_t)core;

	/* The speed with the sibling idle: the reported share, or the kind's. */
	if (relative != DESKTOP_BALANCER_UNKNOWN) {
		rank = relative * 10U;
		if (relative > 100U)
			rank = 1000U;
	} else if (description->core == SCHED_HINT_CORE_FAST) {
		rank = DESKTOP_BALANCER_FAST_RANK;
	} else if (description->core == SCHED_HINT_CORE_EFFICIENT) {
		rank = DESKTOP_BALANCER_EFFICIENT_RANK;
	} else {
		rank = 0U;
	}

	/* The rank the scheduler compares CPUs by. */
	description->rank = (uint16_t)rank;

	/* The speed with the sibling busy. */
	description->sibling_rank = (uint16_t)rank;
	if (has_sibling)
		description->sibling_rank = (uint16_t)(rank * DESKTOP_BALANCER_SMT_SHARE_PERMILLE / 1000U);

	/* Whether the hardware lets it be used: zero capacity means avoid it. */
	description->usable = 1;
	if (available == 0U)
		description->usable = 0;
}

/*
 * Updates the busy mark from the thread's streak in user mode.
 *
 * The streak starts again at every system call, so a streak under the off
 * limit means a call came within that time of the thread's running; a
 * thread that sleeps in a call shows a streak of zero.
 */
static void
observe_busy(
	struct desktop_balancer_history *history,
	const struct desktop_balancer_sample *sample,
	const struct desktop_balancer_limits *limits)
{
	/* A long streak makes the thread busy at once. */
	if ((uint64_t)sample->user_streak >= limits->busy_on) {
		history->busy = 1;
		history->busy_calm = 0;
		return;
	}

	/* A thread that is not busy stays so until a long streak. */
	if (!history->busy)
		return;

	/* A look between the limits breaks a run of calm looks. */
	if ((uint64_t)sample->user_streak >= limits->busy_off) {
		history->busy_calm = 0;
		return;
	}

	/* Enough calm looks in a row end the busy mark. */
	history->busy_calm++;
	if (history->busy_calm >= DESKTOP_BALANCER_BUSY_OFF_LOOKS) {
		history->busy = 0;
		history->busy_calm = 0;
	}
}

/*
 * Updates the heavy mark from the share of the time the thread ran over
 * the window of the last looks.
 */
static void
observe_heavy(
	struct desktop_balancer_history *history,
	const struct desktop_balancer_sample *sample)
{
	uint64_t ran;
	uint64_t run_sum;
	uint64_t elapsed_sum;
	unsigned index;

	/*
	 * What it ran since the last look, at most the time that passed: run
	 * time is charged by the tick, so a look can see a little more.
	 */
	ran = 0U;
	if (sample->run_ticks > history->run_ticks)
		ran = sample->run_ticks - history->run_ticks;
	if (ran > sample->elapsed)
		ran = sample->elapsed;

	/* Records the look over the oldest one in the window. */
	index = history->window_next;
	if (index >= DESKTOP_BALANCER_HEAVY_WINDOW)
		index = 0U;
	history->window_run[index] = (uint32_t)ran;
	history->window_elapsed[index] = (uint32_t)sample->elapsed;
	if (sample->elapsed > 0xffffffffU) {
		history->window_run[index] = (uint32_t)(ran * 0xffffffffU / sample->elapsed);
		history->window_elapsed[index] = 0xffffffffU;
	}

	/* The next look overwrites the following entry. */
	history->window_next = (uint8_t)(index + 1U);

	/* Sums the window. */
	run_sum = 0U;
	elapsed_sum = 0U;
	for (index = 0U; index < DESKTOP_BALANCER_HEAVY_WINDOW; index++) {
		run_sum += history->window_run[index];
		elapsed_sum += history->window_elapsed[index];
	}

	/* No time in the window tells nothing. */
	if (elapsed_sum == 0U)
		return;

	/* Running half the time or more makes the thread heavy at once. */
	if (run_sum * 100U >= elapsed_sum * DESKTOP_BALANCER_HEAVY_ON_PERCENT) {
		history->heavy = 1;
		history->heavy_calm = 0;
		return;
	}

	/* A thread that is not heavy stays so. */
	if (!history->heavy)
		return;

	/* A look between the shares breaks a run of light looks. */
	if (run_sum * 100U >= elapsed_sum * DESKTOP_BALANCER_HEAVY_OFF_PERCENT) {
		history->heavy_calm = 0;
		return;
	}

	/* Enough light looks in a row end the heavy mark. */
	history->heavy_calm++;
	if (history->heavy_calm >= DESKTOP_BALANCER_HEAVY_OFF_LOOKS) {
		history->heavy = 0;
		history->heavy_calm = 0;
	}
}

/* Reports the level a display or GPU process's thread has: raised unless heavy. */
static unsigned
display_level(
	const struct desktop_balancer_traits *traits)
{
	/* A thread heavy on the CPU is not raised, so that it cannot hold the others back. */
	if (traits->heavy)
		return SCHED_HINT_USER_DEFAULT;

	/* The process that puts images on a display, above any other user thread. */
	if (traits->presenter)
		return POLICY_LEVEL_PRESENTER;

	/* A process that puts work on a GPU, one level below. */
	if (traits->gpu)
		return POLICY_LEVEL_GPU;

	/* Neither: the user default. */
	return SCHED_HINT_USER_DEFAULT;
}

/* Fills a hint. */
static void
hint_fill(
	struct sched_hint *hint,
	unsigned core,
	unsigned strength,
	unsigned flags,
	unsigned level)
{
	/* Each field as the rule gives it. */
	hint->core = (uint8_t)core;
	hint->strength = (uint8_t)strength;
	hint->flags = (uint8_t)flags;
	hint->priority = (uint8_t)level;
}
