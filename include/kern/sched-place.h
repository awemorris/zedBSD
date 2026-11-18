/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Where the scheduler puts a thread among the idle CPUs, by the cores the
 * CPUs belong to and how fast each CPU is.
 *
 * Two CPUs of one core (the SMT threads of a physical core) share its
 * execution units, so a thread placed beside a busy sibling runs at a
 * fraction of the core's speed while a whole core may stand idle.  A
 * hybrid processor also has cores of different speeds.  The choice
 * therefore takes, among the idle CPUs, first one whose whole core is
 * idle, the fastest of those first, and only then a CPU whose sibling is
 * busy; equals go to the lowest number.  The same order as FreeBSD's ULE
 * and Linux's idle-sibling search and asymmetric packing, which also try a
 * fully idle core before an SMT sibling.
 *
 * A core counts as in use while any of its CPUs runs a thread, and for a
 * short while after one leaves its thread for idle: the thread is likely
 * to come back to the CPU it ran on, so a sibling idle for a moment between
 * two runs of its thread is not taken for a free core.  The CPUs out of use
 * are the quiet ones.  A core whose siblings are quiet comes first; one
 * whose siblings are halted but only just left their threads still comes
 * before one beside a sibling running a thread.
 *
 * A thread that wakes on a CPU whose core a sibling uses moves to an idle
 * CPU whose whole core is quiet, the fastest first, as FreeBSD's ULE moves
 * a waking thread off its last CPU unless every SMT thread of that core is
 * idle.  A thread with a core of its own moves only to a whole quiet core
 * that is clearly faster, as Linux moves work to a free faster core of a
 * hybrid processor.  Otherwise it wakes where it ran, on a warm cache.
 * Each move goes to a strictly better place than the one it leaves, so the
 * threads settle instead of chasing one another.
 *
 * The description of the CPUs comes from whoever knows the machine (the
 * CPU performance framework on a machine with a provider); without one,
 * every CPU is its own core of one speed and the choice is the lowest
 * numbered idle CPU.
 *
 * The decisions here read a snapshot and return an answer, holding no lock
 * and reading no kernel object, so that they can be tested on their own.
 */

#ifndef KERN_KERN_SCHED_PLACE_H
#define KERN_KERN_SCHED_PLACE_H

#include <kern/types.h>
#include <stddef.h>
#include <stdint.h>

/* The rank of the fastest CPU, and the most any CPU is given. */
#define SCHED_PLACE_RANK_FASTEST	1000U

/*
 * The rank of an efficient core when no measured speed is known: below
 * any performance core, as the kinds alone say nothing finer.
 */
#define SCHED_PLACE_RANK_EFFICIENT	700U

/*
 * The weights of a core in a CPU's score, each above any rank, so that
 * whatever their speeds a CPU whose core no sibling has used for a while
 * comes first, then one whose siblings are halted but only just left their
 * threads (which are likely to come back), and last one beside a sibling
 * running a thread.
 */
#define SCHED_PLACE_WHOLE_CORE		0x10000U
#define SCHED_PLACE_QUIET_CORE		0x20000U

/*
 * How much faster a whole core must be for a thread with a core of its own
 * to move there when it wakes: a rank at least 9/8 of its own.  An
 * efficient core (700) gives way to a performance core (1000); cores of
 * one kind whose measured speeds differ a little do not trade threads.
 */
#define SCHED_PLACE_FASTER_NUM		9U
#define SCHED_PLACE_FASTER_DEN		8U

/*
 * What one CPU is, for placing threads.
 *
 * sibling names the next CPU of the same core, so that the CPUs of one
 * core form a ring; a CPU alone in its core names itself.  rank is the
 * speed one thread gets on the CPU while the rest of its core is idle,
 * 0..SCHED_PLACE_RANK_FASTEST, comparable across the kinds of core.
 */
struct sched_cpu_place {
	uint16_t sibling;
	uint16_t rank;
};

/*
 * Publishes the description of every CPU for placing threads.
 *
 * Thread context; one caller at a time (the caller serializes); takes no
 * CPU lock.  count is the number of CPUs the scheduler runs.  The table is
 * copied.  A NULL table, one of another length, or one whose siblings do
 * not form rings withdraws the published description, so that every CPU
 * is again its own core of one speed.
 */
void
sched_cpu_places_set(
	const struct sched_cpu_place *places,
	unsigned count);

/* Tests one CPU's bit in a set of KERN_CPU_MASK_WORDS words. */
static inline int
sched_place_bit(
	const uint64_t *bits,
	unsigned cpu)
{
	uint64_t word;

	/* The word of the CPU, and its bit in it. */
	word = bits[cpu / 64U];
	if ((word & ((uint64_t)1U << (cpu % 64U))) != 0U)
		return 1;

	/* The bit is clear. */
	return 0;
}

/*
 * Tests whether every other CPU of a CPU's core is quiet.
 *
 * Walks the CPU's ring of siblings, at most count steps, so a ring that
 * does not close cannot keep the walk going.
 */
static inline int
sched_place_core_idle(
	const struct sched_cpu_place *places,
	unsigned count,
	const uint64_t *quiet,
	unsigned cpu)
{
	unsigned other;
	unsigned steps;
	int set;

	/* Visits the siblings until the ring comes back to the CPU. */
	other = places[cpu].sibling;
	steps = 0U;
	while (other != cpu && other < count && steps < count) {
		/* A sibling that is not quiet uses the core. */
		set = sched_place_bit(quiet, other);
		if (!set)
			return 0;

		/* The next sibling of the ring. */
		other = places[other].sibling;
		steps++;
	}

	/* No sibling uses the core. */
	return 1;
}

/*
 * Reports how good a place a CPU is: a core whose siblings are quiet, then
 * one whose siblings are halted, then a shared one (each above any rank),
 * and among those the CPU's rank.  Without a description every CPU scores
 * the same.
 */
static inline unsigned
sched_place_score(
	const struct sched_cpu_place *places,
	unsigned count,
	const uint64_t *idle,
	const uint64_t *quiet,
	unsigned cpu)
{
	unsigned score;
	int whole;

	/* Every CPU is alike without a description. */
	if (places == NULL)
		return 0U;

	/* The CPU's speed with its core to itself. */
	score = places[cpu].rank;

	/* A core no sibling has used for a while comes first. */
	whole = sched_place_core_idle(places, count, quiet, cpu);
	if (whole) {
		score += SCHED_PLACE_QUIET_CORE;
		return score;
	}

	/* A core whose siblings are halted comes before one a running sibling shares. */
	whole = sched_place_core_idle(places, count, idle, cpu);
	if (whole)
		score += SCHED_PLACE_WHOLE_CORE;

	/* Reports the score. */
	return score;
}

/*
 * Chooses the idle CPU a thread should go to, other than except, or
 * KERN_CPU_MAX when none is idle.
 *
 * idle has one bit per halted CPU and quiet one bit per CPU out of use
 * (KERN_CPU_MASK_WORDS words each).  The idle CPU with the highest score
 * wins, the lowest number among equals; without a description that is the
 * lowest numbered idle CPU.
 */
static inline kern_cpu_id_t
sched_place_choose(
	const struct sched_cpu_place *places,
	unsigned count,
	const uint64_t *idle,
	const uint64_t *quiet,
	kern_cpu_id_t except)
{
	kern_cpu_id_t best;
	unsigned best_score;
	unsigned score;
	unsigned index;
	unsigned bit;
	unsigned cpu;
	uint64_t word;

	/* Walks the idle CPUs in number order, so the first of equals stays. */
	best = KERN_CPU_MAX;
	best_score = 0U;
	for (index = 0U; index < KERN_CPU_MASK_WORDS; index++) {
		word = idle[index];
		while (word != 0U) {
			bit = (unsigned)__builtin_ctzll(word);
			word &= word - 1U;
			cpu = index * 64U + bit;

			/* The excluded CPU, and a bit beyond the scheduler's CPUs. */
			if (cpu == except || cpu >= count)
				continue;

			/* Keeps the best place so far. */
			score = sched_place_score(places, count, idle, quiet, cpu);
			if (best == KERN_CPU_MAX || score > best_score) {
				best = (kern_cpu_id_t)cpu;
				best_score = score;
			}
		}
	}

	/* Reports the CPU, or none. */
	return best;
}

/*
 * Tests whether an idle CPU other than self is a better place than self
 * for a thread self is about to take from another CPU.
 *
 * self is idle but not in idle (it is looking for work).  Only a strictly
 * higher score counts, so among equals any CPU takes the work.  Without a
 * description no CPU is better than another.
 */
static inline int
sched_place_better_idle(
	const struct sched_cpu_place *places,
	unsigned count,
	const uint64_t *idle,
	const uint64_t *quiet,
	kern_cpu_id_t self)
{
	unsigned self_score;
	unsigned score;
	unsigned index;
	unsigned bit;
	unsigned cpu;
	uint64_t word;

	/* Every CPU is alike without a description, or for a CPU not described. */
	if (places == NULL || self >= count)
		return 0;

	/* How good a place self is now. */
	self_score = sched_place_score(places, count, idle, quiet, self);

	/* Looks for an idle CPU that scores higher. */
	for (index = 0U; index < KERN_CPU_MASK_WORDS; index++) {
		word = idle[index];
		while (word != 0U) {
			bit = (unsigned)__builtin_ctzll(word);
			word &= word - 1U;
			cpu = index * 64U + bit;

			/* Self, and a bit beyond the scheduler's CPUs. */
			if (cpu == self || cpu >= count)
				continue;

			/* A better place is idle. */
			score = sched_place_score(places, count, idle, quiet, cpu);
			if (score > self_score)
				return 1;
		}
	}

	/* Self is as good a place as any idle CPU. */
	return 0;
}

/*
 * Tests whether a whole core is clearly faster than another place: by at
 * least SCHED_PLACE_FASTER_NUM / SCHED_PLACE_FASTER_DEN of its rank, so that
 * cores of one kind whose measured speeds differ a little never trade
 * threads, while an efficient core gives way to a performance core.
 */
static inline int
sched_place_clearly_faster(
	unsigned rank,
	unsigned than)
{
	/* A rank no higher is never faster. */
	if (rank <= than)
		return 0;

	/* A small difference is not worth a move. */
	if ((uint32_t)rank * SCHED_PLACE_FASTER_DEN < (uint32_t)than * SCHED_PLACE_FASTER_NUM)
		return 0;

	/* The rank is clearly higher. */
	return 1;
}

/*
 * Chooses where a thread waking on home, or one whose quantum there has
 * ended, should run instead, or KERN_CPU_MAX to stay.
 *
 * Home's own state is not read: the caller wakes a thread there that runs
 * nowhere yet, or ends the quantum of the thread running there.  The
 * thread moves only to an idle quiet CPU whose whole core is quiet (the
 * CPUs of a core just left by their threads are not taken, as those
 * threads come back): the fastest such CPU, the lowest number among
 * equals, when a sibling uses home's core; or one clearly faster than home
 * when home's core is its own.  Without a description, or with home
 * outside it, the thread stays.
 */
static inline kern_cpu_id_t
sched_place_wake(
	const struct sched_cpu_place *places,
	unsigned count,
	const uint64_t *idle,
	const uint64_t *quiet,
	kern_cpu_id_t home)
{
	kern_cpu_id_t best;
	unsigned best_rank;
	unsigned index;
	unsigned bit;
	unsigned cpu;
	uint64_t word;
	int whole;
	int faster;

	/* Every CPU is alike without a description, or for a CPU not described. */
	if (places == NULL || home >= count)
		return KERN_CPU_MAX;

	/* Finds the fastest idle CPU whose whole core is quiet. */
	best = KERN_CPU_MAX;
	best_rank = 0U;
	for (index = 0U; index < KERN_CPU_MASK_WORDS; index++) {
		word = idle[index] & quiet[index];
		while (word != 0U) {
			bit = (unsigned)__builtin_ctzll(word);
			word &= word - 1U;
			cpu = index * 64U + bit;

			/* Home, and a bit beyond the scheduler's CPUs. */
			if (cpu == home || cpu >= count)
				continue;

			/* A CPU beside a sibling in use is no better than home. */
			whole = sched_place_core_idle(places, count, quiet, cpu);
			if (!whole)
				continue;

			/* Keeps the fastest so far, the first of equals. */
			if (best == KERN_CPU_MAX || places[cpu].rank > best_rank) {
				best = (kern_cpu_id_t)cpu;
				best_rank = places[cpu].rank;
			}
		}
	}

	/* Stays when no whole core is free. */
	if (best == KERN_CPU_MAX)
		return KERN_CPU_MAX;

	/* Leaves a core shared with a sibling in use for the whole one. */
	whole = sched_place_core_idle(places, count, quiet, home);
	if (!whole)
		return best;

	/* With home's core its own, moves only to a clearly faster one. */
	faster = sched_place_clearly_faster(best_rank, places[home].rank);
	if (faster)
		return best;

	/* Home is as good a place as any: the thread stays on its warm cache. */
	return KERN_CPU_MAX;
}

#endif
