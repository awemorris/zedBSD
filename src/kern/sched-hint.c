/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The scheduler's decisions about hints that need nothing but numbers:
 * which CPU a thread of a kind of core should go to, given which CPUs are
 * idle and what kind each one is; the level a thread is queued at; and
 * whether a thread that waited too long is raised.
 *
 * They take a snapshot and return an answer, holding no lock and reading
 * no kernel object, so that they can be tested on their own.
 */

#include <kern/sched-hint.h>
#include <stddef.h>

/* The answer of an index that names no CPU. */
#define SCHED_HINT_NO_CPU	KERN_CPU_MAX

static int bit_test(const uint64_t *bits, unsigned index);
static int cpu_free(unsigned index, const uint64_t *idle, const uint64_t *backlog);
static int core_idle(const struct sched_cpu_class *classes, unsigned count, unsigned index, const uint64_t *idle, const uint64_t *backlog);
static unsigned cpu_speed(const struct sched_cpu_class *classes, unsigned count, unsigned index, const uint64_t *idle, const uint64_t *backlog);
static kern_cpu_id_t choose_fastest(const struct sched_cpu_class *classes, unsigned count, const uint64_t *idle, const uint64_t *backlog, unsigned core, int whole_core);

/*
 * Chooses the CPU a thread of the given kind should go to.
 *
 * MOVE takes the fastest free usable CPU of the kind, where a CPU whose
 * SMT sibling is busy counts at its sibling_rank; it never answers a CPU
 * of another kind, so a thread is not moved somewhere it would only be
 * moved from again.  MOVE_IDLE_CORE takes the same, among the CPUs whose
 * whole core is free.  PLACE, for a new or waiting thread, takes for FAST
 * a usable FAST CPU whose whole core is free, and failing that the
 * fastest of the free usable EFFICIENT CPUs and FAST CPUs with a busy
 * sibling; for EFFICIENT a free usable EFFICIENT CPU.  Ties go to the
 * lowest number.  A CPU is free when it is idle with nothing queued.
 */
kern_cpu_id_t
sched_hint_choose(
	const struct sched_cpu_class *classes,
	unsigned count,
	const uint64_t *idle,
	const uint64_t *backlog,
	unsigned core,
	unsigned mode)
{
	kern_cpu_id_t cpu;

	/* A table that names no CPU, or a thread that asks for no kind, has no place. */
	if (classes == NULL || count == 0U || count > KERN_CPU_MAX)
		return SCHED_HINT_NO_CPU;
	if (core != SCHED_HINT_CORE_FAST && core != SCHED_HINT_CORE_EFFICIENT)
		return SCHED_HINT_NO_CPU;

	/* A move goes only to a CPU of the kind, its whole core free if asked. */
	if (mode == SCHED_HINT_CHOOSE_MOVE) {
		cpu = choose_fastest(classes, count, idle, backlog, core, 0);
		return cpu;
	}
	if (mode == SCHED_HINT_CHOOSE_MOVE_IDLE_CORE) {
		cpu = choose_fastest(classes, count, idle, backlog, core, 1);
		return cpu;
	}

	/* A placement that is not one of the three modes has no answer. */
	if (mode != SCHED_HINT_CHOOSE_PLACE)
		return SCHED_HINT_NO_CPU;

	/* An efficient thread is placed only on a free efficient CPU. */
	if (core == SCHED_HINT_CORE_EFFICIENT) {
		cpu = choose_fastest(classes, count, idle, backlog, SCHED_HINT_CORE_EFFICIENT, 0);
		return cpu;
	}

	/* A fast thread takes a fast core of its own first. */
	cpu = choose_fastest(classes, count, idle, backlog, SCHED_HINT_CORE_FAST, 1);
	if (cpu != SCHED_HINT_NO_CPU)
		return cpu;

	/*
	 * Then the faster of what is left: an efficient CPU, or a fast one
	 * that shares its core with a busy sibling.
	 */
	cpu = choose_fastest(classes, count, idle, backlog, SCHED_HINT_CORE_ANY, 0);

	/* Reports the CPU, or none when nothing fits. */
	return cpu;
}

/*
 * Reports the level a thread is queued at.
 *
 * It is the thread's base level, except that a thread inside the kernel,
 * which may hold a lock others wait for, is not queued below the user
 * default.
 */
int
sched_hint_queue_level(
	int base,
	int in_kernel)
{
	/* A thread in the kernel keeps at least the user default. */
	if (in_kernel && base > SCHED_HINT_USER_DEFAULT)
		return SCHED_HINT_USER_DEFAULT;

	/* Elsewhere the thread's own level. */
	return base;
}

/*
 * Reports the level a waiting user thread is raised to, or -1.
 *
 * A thread at a lower level than the running one (a larger number) that
 * has waited at least limit ticks runs one quantum at the running
 * thread's level, but never above the kernel's default, so the kernel's
 * workers at that level keep their turn.  A raise that would not lift the
 * thread at all is not made.
 */
int
sched_hint_starve_level(
	int running,
	int waiting,
	uint64_t waited,
	uint64_t limit)
{
	int level;

	/* A thread at or above the running level is not starved by it. */
	if (waiting <= running)
		return -1;

	/* A thread that has not waited the limit waits on. */
	if (waited < limit)
		return -1;

	/* The running level, at most the kernel's default. */
	level = running;
	if (level < SCHED_HINT_LEVEL_HIGHEST)
		level = SCHED_HINT_LEVEL_HIGHEST;

	/* A raise that leaves the thread where it is would be none. */
	if (level >= waiting)
		return -1;

	/* Succeeded: the level of the one quantum. */
	return level;
}

/* Tests one bit of a set of CPUs. */
static int
bit_test(
	const uint64_t *bits,
	unsigned index)
{
	uint64_t word;

	/* The word of the CPU, and its bit in it. */
	word = bits[index / 64U];
	if ((word & ((uint64_t)1U << (index % 64U))) != 0U)
		return 1;

	/* The bit is clear. */
	return 0;
}

/* Tests whether a CPU is idle with nothing queued. */
static int
cpu_free(
	unsigned index,
	const uint64_t *idle,
	const uint64_t *backlog)
{
	int set;

	/* A CPU with work queued is about to run it. */
	set = bit_test(backlog, index);
	if (set)
		return 0;

	/* A CPU that is not halted runs a thread. */
	set = bit_test(idle, index);
	if (!set)
		return 0;

	/* The CPU is free. */
	return 1;
}

/* Tests whether every other CPU of a CPU's core is free. */
static int
core_idle(
	const struct sched_cpu_class *classes,
	unsigned count,
	unsigned index,
	const uint64_t *idle,
	const uint64_t *backlog)
{
	unsigned other;
	int free;

	/* Looks at the siblings, the CPUs that share the core's group. */
	for (other = 0U; other < count; other++) {
		if (other == index)
			continue;
		if (classes[other].core_group != classes[index].core_group)
			continue;
		free = cpu_free(other, idle, backlog);
		if (!free)
			return 0;
	}

	/* No sibling is busy. */
	return 1;
}

/* Reports how fast one thread would run on a CPU now. */
static unsigned
cpu_speed(
	const struct sched_cpu_class *classes,
	unsigned count,
	unsigned index,
	const uint64_t *idle,
	const uint64_t *backlog)
{
	int whole;

	/* A CPU whose sibling is busy shares the core. */
	whole = core_idle(classes, count, index, idle, backlog);
	if (!whole)
		return classes[index].sibling_rank;

	/* The core is the thread's alone. */
	return classes[index].rank;
}

/*
 * Picks the fastest free usable CPU of a kind (ANY: of either known kind),
 * only among the CPUs whose whole core is free when whole_core is set.
 */
static kern_cpu_id_t
choose_fastest(
	const struct sched_cpu_class *classes,
	unsigned count,
	const uint64_t *idle,
	const uint64_t *backlog,
	unsigned core,
	int whole_core)
{
	kern_cpu_id_t best;
	unsigned best_speed;
	unsigned speed;
	unsigned index;
	int free;
	int whole;

	/* Walks the CPUs in number order, so the first of equals stays. */
	best = SCHED_HINT_NO_CPU;
	best_speed = 0U;
	for (index = 0U; index < count; index++) {
		/* A CPU of another kind, of no known kind, or that the hardware asks to avoid. */
		if (classes[index].core == SCHED_HINT_CORE_ANY)
			continue;
		if (core != SCHED_HINT_CORE_ANY && classes[index].core != core)
			continue;
		if (!classes[index].usable)
			continue;

		/* A CPU running or about to run a thread. */
		free = cpu_free(index, idle, backlog);
		if (!free)
			continue;

		/* A CPU whose core is shared, when only a whole core will do. */
		whole = core_idle(classes, count, index, idle, backlog);
		if (whole_core && !whole)
			continue;

		/* Keeps the fastest. */
		speed = cpu_speed(classes, count, index, idle, backlog);
		if (best == SCHED_HINT_NO_CPU || speed > best_speed) {
			best = (kern_cpu_id_t)index;
			best_speed = speed;
		}
	}

	/* Reports the CPU, or none. */
	return best;
}
