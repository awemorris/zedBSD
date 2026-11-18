/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host check of the scheduler's choice of an idle CPU by the cores and
 * speeds of the CPUs (include/kern/sched-place.h).  Busy threads are placed
 * one after another on models of machines: a hybrid processor with two SMT
 * performance cores and eight efficient cores, four SMT cores numbered with
 * the siblings side by side and numbered half a machine apart, four cores
 * without SMT, and no description at all.  Each placed thread makes its CPU
 * busy, so the order of the places is the order a machine fills.  It also
 * checks when an idle CPU leaves waiting work to a better idle CPU, also
 * when its sibling idles only for a moment between two runs of its thread;
 * where a waking thread moves, and that threads waking over and over on
 * siblings settle on cores of their own and then stay; and that a broken
 * ring of siblings cannot keep a walk going.
 *
 * cc -std=c99 -O1 -Wall -Wextra -Werror -Iinclude \
 *    -fsanitize=address,undefined -fno-sanitize-recover=all \
 *    tests/sched-place-host-test.c -o <out> && <out>
 *
 * The last line is "sched-place-host-test: PASS".
 */

#include <kern/sched-place.h>

#include <stdio.h>
#include <string.h>

/* The most CPUs a model has. */
#define MODEL_CPU_MAX	16U

/* The number of checks that failed. */
static unsigned failures;

/* The number of checks made. */
static unsigned checks;

static void idle_all(uint64_t *idle, unsigned count);
static void idle_clear(uint64_t *idle, unsigned cpu);
static void idle_set(uint64_t *idle, unsigned cpu);
static void check_fill(const char *name, const struct sched_cpu_place *places, unsigned count, const unsigned *expected, unsigned threads);
static void check_value(const char *name, unsigned got, unsigned expected);
static void model_hybrid(struct sched_cpu_place *places);
static void model_smt_adjacent(struct sched_cpu_place *places);
static void model_smt_halves(struct sched_cpu_place *places);
static void model_single(struct sched_cpu_place *places, unsigned count);
static void test_fill(void);
static void test_except(void);
static void test_better_idle(void);
static void test_quiet_steal(void);
static void test_wake(void);
static void test_wake_settles(void);
static void test_broken_ring(void);
static void test_stray_bits(void);

/*
 * Runs every check and reports the result.
 */
int
main(void)
{
	/* The order the machines fill with busy threads. */
	test_fill();

	/* The CPU a busy caller excludes. */
	test_except();

	/* When an idle CPU leaves waiting work to a better one. */
	test_better_idle();

	/* A sibling idle for a moment still uses the core. */
	test_quiet_steal();

	/* Where a waking thread goes. */
	test_wake();

	/* Threads that keep waking on siblings spread once and then stay. */
	test_wake_settles();

	/* A ring of siblings that does not close. */
	test_broken_ring();

	/* Idle bits beyond the CPUs a model has. */
	test_stray_bits();

	/* Reports the result. */
	if (failures != 0U) {
		printf("sched-place-host-test: FAIL (%u of %u checks)\n", failures, checks);
		return 1;
	}
	printf("sched-place-host-test: PASS (%u checks)\n", checks);
	return 0;
}

/* Marks every CPU of a model idle and every other bit clear. */
static void
idle_all(
	uint64_t *idle,
	unsigned count)
{
	unsigned cpu;

	/* Clears the set, then sets the model's CPUs. */
	memset(idle, 0, KERN_CPU_MASK_WORDS * sizeof(*idle));
	for (cpu = 0U; cpu < count; cpu++)
		idle_set(idle, cpu);
}

/* Marks one CPU busy. */
static void
idle_clear(
	uint64_t *idle,
	unsigned cpu)
{
	/* Clears the CPU's bit. */
	idle[cpu / 64U] &= ~((uint64_t)1U << (cpu % 64U));
}

/* Marks one CPU idle. */
static void
idle_set(
	uint64_t *idle,
	unsigned cpu)
{
	/* Sets the CPU's bit. */
	idle[cpu / 64U] |= (uint64_t)1U << (cpu % 64U);
}

/*
 * Places threads one after another on an idle machine, each making its
 * CPU busy, and compares the CPUs with the expected order.
 */
static void
check_fill(
	const char *name,
	const struct sched_cpu_place *places,
	unsigned count,
	const unsigned *expected,
	unsigned threads)
{
	uint64_t idle[KERN_CPU_MASK_WORDS];
	kern_cpu_id_t cpu;
	unsigned thread;
	char label[96];

	/* Starts from an idle machine. */
	idle_all(idle, count);

	/* Places each thread where the choice says and makes that CPU busy. */
	for (thread = 0U; thread < threads; thread++) {
		cpu = sched_place_choose(places, count, idle, idle, KERN_CPU_MAX);
		snprintf(label, sizeof(label), "%s thread %u", name, thread + 1U);
		check_value(label, cpu, expected[thread]);
		if (cpu < count)
			idle_clear(idle, cpu);
	}

	/* A full machine has no idle CPU left. */
	if (threads == count) {
		cpu = sched_place_choose(places, count, idle, idle, KERN_CPU_MAX);
		snprintf(label, sizeof(label), "%s full", name);
		check_value(label, cpu, KERN_CPU_MAX);
	}
}

/* Counts one check and reports it when it fails. */
static void
check_value(
	const char *name,
	unsigned got,
	unsigned expected)
{
	/* Counts the check. */
	checks++;
	if (got == expected)
		return;

	/* Reports the failure. */
	failures++;
	printf("FAIL %s: got %u, expected %u\n", name, got, expected);
}

/*
 * The model of a hybrid processor like the Alder Lake-U of a Latitude 5330:
 * CPUs 0 and 1 are the two threads of the first performance core, 2 and 3
 * of the second, and 4 to 11 are efficient cores without SMT, slower.
 */
static void
model_hybrid(
	struct sched_cpu_place *places)
{
	unsigned cpu;

	/* The two performance cores, each a ring of two. */
	places[0].sibling = 1U;
	places[1].sibling = 0U;
	places[2].sibling = 3U;
	places[3].sibling = 2U;
	for (cpu = 0U; cpu < 4U; cpu++)
		places[cpu].rank = SCHED_PLACE_RANK_FASTEST;

	/* The efficient cores, each alone and slower. */
	for (cpu = 4U; cpu < 12U; cpu++) {
		places[cpu].sibling = (uint16_t)cpu;
		places[cpu].rank = 750U;
	}
}

/* Four SMT cores whose two threads have adjacent numbers (0 and 1, ...). */
static void
model_smt_adjacent(
	struct sched_cpu_place *places)
{
	unsigned cpu;

	/* Each even CPU and the odd one after it form a core. */
	for (cpu = 0U; cpu < 8U; cpu++) {
		places[cpu].sibling = (uint16_t)(cpu ^ 1U);
		places[cpu].rank = SCHED_PLACE_RANK_FASTEST;
	}
}

/* Four SMT cores whose two threads are numbered four apart (0 and 4, ...). */
static void
model_smt_halves(
	struct sched_cpu_place *places)
{
	unsigned cpu;

	/* CPU n and CPU n + 4 form a core. */
	for (cpu = 0U; cpu < 8U; cpu++) {
		places[cpu].sibling = (uint16_t)((cpu + 4U) % 8U);
		places[cpu].rank = SCHED_PLACE_RANK_FASTEST;
	}
}

/* A machine of single-thread cores of one speed, as a Pi 5 or a VisionFive 2. */
static void
model_single(
	struct sched_cpu_place *places,
	unsigned count)
{
	unsigned cpu;

	/* Each CPU is its own core. */
	for (cpu = 0U; cpu < count; cpu++) {
		places[cpu].sibling = (uint16_t)cpu;
		places[cpu].rank = SCHED_PLACE_RANK_FASTEST;
	}
}

/*
 * Checks the order the models fill in: whole fast cores, then whole slow
 * cores, then the SMT siblings of busy cores.
 */
static void
test_fill(void)
{
	static const unsigned hybrid[12] = { 0, 2, 4, 5, 6, 7, 8, 9, 10, 11, 1, 3 };
	static const unsigned adjacent[8] = { 0, 2, 4, 6, 1, 3, 5, 7 };
	static const unsigned halves[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
	static const unsigned single[4] = { 0, 1, 2, 3 };
	struct sched_cpu_place places[MODEL_CPU_MAX];

	/* The hybrid machine, by 1, 2, 4, 8 and all 12 threads. */
	model_hybrid(places);
	check_fill("hybrid 1", places, 12U, hybrid, 1U);
	check_fill("hybrid 2", places, 12U, hybrid, 2U);
	check_fill("hybrid 4", places, 12U, hybrid, 4U);
	check_fill("hybrid 8", places, 12U, hybrid, 8U);
	check_fill("hybrid 12", places, 12U, hybrid, 12U);

	/* SMT cores numbered side by side, by 2, 4 and 8 threads. */
	model_smt_adjacent(places);
	check_fill("adjacent 2", places, 8U, adjacent, 2U);
	check_fill("adjacent 4", places, 8U, adjacent, 4U);
	check_fill("adjacent 8", places, 8U, adjacent, 8U);

	/* SMT cores numbered half a machine apart: the first half fills first. */
	model_smt_halves(places);
	check_fill("halves 4", places, 8U, halves, 4U);
	check_fill("halves 8", places, 8U, halves, 8U);

	/* Four cores without SMT, described and not described: number order. */
	model_single(places, 4U);
	check_fill("single 1", places, 4U, single, 1U);
	check_fill("single 2", places, 4U, single, 2U);
	check_fill("single 4", places, 4U, single, 4U);
	check_fill("none 1", NULL, 4U, single, 1U);
	check_fill("none 2", NULL, 4U, single, 2U);
	check_fill("none 4", NULL, 4U, single, 4U);
}

/* Checks that the CPU a caller excludes is never chosen. */
static void
test_except(void)
{
	struct sched_cpu_place places[MODEL_CPU_MAX];
	uint64_t idle[KERN_CPU_MASK_WORDS];
	kern_cpu_id_t cpu;

	/*
	 * An idle CPU 0 excluded: CPU 1's core still counts as idle, so CPU 1
	 * is the best of the rest.  The scheduler excludes its own busy CPU,
	 * which is not in the idle set; then the second core comes next.
	 */
	model_hybrid(places);
	idle_all(idle, 12U);
	cpu = sched_place_choose(places, 12U, idle, idle, 0U);
	check_value("except idle best", cpu, 1U);
	idle_clear(idle, 0U);
	cpu = sched_place_choose(places, 12U, idle, idle, 0U);
	check_value("except busy best", cpu, 2U);
	idle_set(idle, 0U);

	/* Without a description the next number. */
	cpu = sched_place_choose(NULL, 12U, idle, idle, 0U);
	check_value("except none", cpu, 1U);

	/* Excluding the only idle CPU leaves none. */
	memset(idle, 0, sizeof(idle));
	idle_set(idle, 5U);
	cpu = sched_place_choose(places, 12U, idle, idle, 5U);
	check_value("except only", cpu, KERN_CPU_MAX);
}

/*
 * Checks when an idle CPU looking for work leaves it to a better idle CPU.
 * The looking CPU is not in the idle set, as in the scheduler.
 */
static void
test_better_idle(void)
{
	struct sched_cpu_place places[MODEL_CPU_MAX];
	uint64_t idle[KERN_CPU_MASK_WORDS];
	int better;
	unsigned cpu;

	/* CPU 0 busy: CPU 1, beside it, leaves the work to the idle second core. */
	model_hybrid(places);
	idle_all(idle, 12U);
	idle_clear(idle, 0U);
	idle_clear(idle, 1U);
	better = sched_place_better_idle(places, 12U, idle, idle, 1U);
	check_value("sibling of busy defers", (unsigned)better, 1U);

	/* CPU 2 with its core idle is the best place: it takes the work. */
	idle_set(idle, 1U);
	idle_clear(idle, 2U);
	better = sched_place_better_idle(places, 12U, idle, idle, 2U);
	check_value("whole fast core takes", (unsigned)better, 0U);

	/* An efficient core leaves the work to an idle whole fast core. */
	idle_set(idle, 2U);
	idle_clear(idle, 4U);
	better = sched_place_better_idle(places, 12U, idle, idle, 4U);
	check_value("efficient defers to fast", (unsigned)better, 1U);

	/* With both fast cores busy an efficient core is as good as any: it takes the work. */
	idle_all(idle, 12U);
	idle_clear(idle, 0U);
	idle_clear(idle, 2U);
	idle_clear(idle, 5U);
	better = sched_place_better_idle(places, 12U, idle, idle, 5U);
	check_value("efficient among equals takes", (unsigned)better, 0U);

	/* A fast sibling leaves the work to an idle whole efficient core. */
	idle_clear(idle, 3U);
	better = sched_place_better_idle(places, 12U, idle, idle, 3U);
	check_value("fast sibling defers to whole efficient", (unsigned)better, 1U);

	/* With every whole core busy, a sibling takes the work. */
	idle_all(idle, 12U);
	for (cpu = 0U; cpu < 12U; cpu++) {
		if (cpu == 1U || cpu == 3U)
			continue;
		idle_clear(idle, cpu);
	}
	idle_clear(idle, 1U);
	better = sched_place_better_idle(places, 12U, idle, idle, 1U);
	check_value("last sibling takes", (unsigned)better, 0U);

	/* Without a description no CPU defers. */
	idle_all(idle, 12U);
	idle_clear(idle, 0U);
	idle_clear(idle, 1U);
	better = sched_place_better_idle(NULL, 12U, idle, idle, 1U);
	check_value("none never defers", (unsigned)better, 0U);
}

/*
 * Checks that a CPU whose sibling left its thread a moment ago (halted, but
 * not yet quiet) does not count its core as free when it looks for work.
 */
static void
test_quiet_steal(void)
{
	struct sched_cpu_place places[MODEL_CPU_MAX];
	uint64_t idle[KERN_CPU_MASK_WORDS];
	uint64_t quiet[KERN_CPU_MASK_WORDS];
	kern_cpu_id_t cpu;
	int better;

	/*
	 * CPU 0 halted between two runs of its thread, CPU 2 busy, CPU 1
	 * looking: an idle efficient core with its core free is the better
	 * place, so CPU 1 leaves the work to it.
	 */
	model_hybrid(places);
	idle_all(idle, 12U);
	idle_clear(idle, 1U);
	idle_clear(idle, 2U);
	idle_all(quiet, 12U);
	idle_clear(quiet, 0U);
	idle_clear(quiet, 1U);
	idle_clear(quiet, 2U);
	better = sched_place_better_idle(places, 12U, idle, quiet, 1U);
	check_value("moment idle sibling defers", (unsigned)better, 1U);

	/* Once CPU 0 is quiet, CPU 1 has its fast core to itself and takes the work. */
	idle_set(quiet, 0U);
	better = sched_place_better_idle(places, 12U, idle, quiet, 1U);
	check_value("quiet sibling takes", (unsigned)better, 0U);

	/* The choice of a helper passes over the core of the moment idle CPU too. */
	idle_clear(quiet, 0U);
	idle_set(idle, 1U);
	cpu = sched_place_choose(places, 12U, idle, quiet, 2U);
	check_value("helper avoids moment idle core", cpu, 4U);

	/*
	 * With no quiet core left, a CPU beside a halted sibling still comes
	 * before one beside a running sibling: CPU 0 runs, CPUs 2 and 3 are
	 * halted between two runs of their threads, the efficient cores run.
	 */
	idle_all(idle, 12U);
	memset(quiet, 0, sizeof(quiet));
	idle_clear(idle, 0U);
	for (cpu = 4U; cpu < 12U; cpu++)
		idle_clear(idle, cpu);
	cpu = sched_place_choose(places, 12U, idle, quiet, KERN_CPU_MAX);
	check_value("halted sibling before running sibling", cpu, 2U);
}

/* Checks where a thread waking on a CPU running no thread goes. */
static void
test_wake(void)
{
	struct sched_cpu_place places[MODEL_CPU_MAX];
	uint64_t idle[KERN_CPU_MASK_WORDS];
	uint64_t quiet[KERN_CPU_MASK_WORDS];
	kern_cpu_id_t cpu;
	unsigned other;

	/* Woken on CPU 1 beside a busy CPU 0: the idle second fast core. */
	model_hybrid(places);
	idle_all(idle, 12U);
	idle_all(quiet, 12U);
	idle_clear(idle, 0U);
	idle_clear(quiet, 0U);
	idle_clear(quiet, 1U);
	cpu = sched_place_wake(places, 12U, idle, quiet, 1U);
	check_value("wake beside busy to fast core", cpu, 2U);

	/* The second fast core busy too: the first idle efficient core. */
	idle_clear(idle, 2U);
	idle_clear(quiet, 2U);
	cpu = sched_place_wake(places, 12U, idle, quiet, 1U);
	check_value("wake beside busy to efficient core", cpu, 4U);

	/* Every efficient core busy as well: stays beside the busy sibling. */
	for (other = 4U; other < 12U; other++) {
		idle_clear(idle, other);
		idle_clear(quiet, other);
	}
	cpu = sched_place_wake(places, 12U, idle, quiet, 1U);
	check_value("wake beside busy stays when full", cpu, KERN_CPU_MAX);

	/* A sibling halted between two runs of its thread still uses the core. */
	idle_all(idle, 12U);
	idle_all(quiet, 12U);
	idle_clear(quiet, 0U);
	idle_clear(quiet, 1U);
	cpu = sched_place_wake(places, 12U, idle, quiet, 1U);
	check_value("wake beside moment idle moves", cpu, 2U);

	/* With the sibling quiet the fast core is the thread's own: it stays. */
	idle_set(quiet, 0U);
	cpu = sched_place_wake(places, 12U, idle, quiet, 1U);
	check_value("wake on whole fast core stays", cpu, KERN_CPU_MAX);

	/* A core its threads left a moment ago is not taken: the efficient core. */
	idle_clear(quiet, 0U);
	idle_clear(quiet, 2U);
	cpu = sched_place_wake(places, 12U, idle, quiet, 1U);
	check_value("wake passes over moment idle core", cpu, 4U);

	/* On a whole efficient core with a whole fast core quiet: moves to the fast one. */
	idle_all(quiet, 12U);
	idle_clear(quiet, 4U);
	idle_clear(idle, 0U);
	idle_clear(quiet, 0U);
	cpu = sched_place_wake(places, 12U, idle, quiet, 4U);
	check_value("wake efficient to faster core", cpu, 2U);

	/* Only a fast CPU beside a busy sibling free: the efficient core stays. */
	idle_clear(idle, 3U);
	idle_clear(quiet, 3U);
	cpu = sched_place_wake(places, 12U, idle, quiet, 4U);
	check_value("wake efficient stays beside shared fast", cpu, KERN_CPU_MAX);

	/* Fast cores whose measured speeds differ a little do not trade threads. */
	places[0].rank = 980U;
	places[1].rank = 980U;
	idle_all(idle, 12U);
	idle_all(quiet, 12U);
	idle_clear(quiet, 0U);
	cpu = sched_place_wake(places, 12U, idle, quiet, 0U);
	check_value("wake near speeds stays", cpu, KERN_CPU_MAX);

	/* SMT cores side by side: from beside a busy CPU 0 to the next core. */
	model_smt_adjacent(places);
	idle_all(idle, 8U);
	idle_all(quiet, 8U);
	idle_clear(idle, 0U);
	idle_clear(quiet, 0U);
	idle_clear(quiet, 1U);
	cpu = sched_place_wake(places, 8U, idle, quiet, 1U);
	check_value("wake adjacent to next core", cpu, 2U);

	/* Cores without SMT of one speed: a thread always wakes where it ran. */
	model_single(places, 4U);
	idle_all(idle, 4U);
	idle_all(quiet, 4U);
	idle_clear(idle, 0U);
	idle_clear(quiet, 0U);
	idle_clear(quiet, 1U);
	cpu = sched_place_wake(places, 4U, idle, quiet, 1U);
	check_value("wake single stays", cpu, KERN_CPU_MAX);

	/* Without a description, or for a CPU not described, nothing moves. */
	cpu = sched_place_wake(NULL, 4U, idle, quiet, 1U);
	check_value("wake none stays", cpu, KERN_CPU_MAX);
	cpu = sched_place_wake(places, 4U, idle, quiet, 7U);
	check_value("wake undescribed stays", cpu, KERN_CPU_MAX);
}

/*
 * Runs threads that work and sleep in turn on the hybrid model, starting
 * them on the SMT siblings of busy cores, and counts the moves: each
 * thread moves at most once, to a core of its own, and then stays.  A CPU
 * a thread left stays out of use for two rounds (the settling time),
 * unless the thread moved away from it for good, as the scheduler marks it.
 */
static void
test_wake_settles(void)
{
	static const unsigned start[4] = { 0U, 1U, 2U, 3U };
	struct sched_cpu_place places[MODEL_CPU_MAX];
	uint64_t idle[KERN_CPU_MASK_WORDS];
	uint64_t quiet[KERN_CPU_MASK_WORDS];
	unsigned home[4];
	unsigned left[MODEL_CPU_MAX];
	unsigned moves[4];
	unsigned round;
	unsigned thread;
	unsigned other;
	unsigned cpu;
	unsigned core;
	kern_cpu_id_t target;
	char label[96];

	/* Four threads on the four CPUs of the two fast cores, all running. */
	model_hybrid(places);
	for (thread = 0U; thread < 4U; thread++) {
		home[thread] = start[thread];
		moves[thread] = 0U;
	}
	memset(left, 0, sizeof(left));

	/* Each round one thread sleeps a moment and wakes while the others run. */
	for (round = 1U; round <= 40U; round++) {
		thread = round % 4U;

		/* Its CPU halts, out of use until the settling time passes. */
		left[home[thread]] = round;

		/* The halted and the quiet CPUs: no other thread runs there. */
		idle_all(idle, 12U);
		idle_all(quiet, 12U);
		for (other = 0U; other < 4U; other++) {
			if (other == thread)
				continue;
			idle_clear(idle, home[other]);
			idle_clear(quiet, home[other]);
		}
		for (cpu = 0U; cpu < 12U; cpu++) {
			if (left[cpu] != 0U && round - left[cpu] < 2U)
				idle_clear(quiet, cpu);
		}

		/* The thread wakes, perhaps elsewhere. */
		target = sched_place_wake(places, 12U, idle, quiet, home[thread]);
		if (target != KERN_CPU_MAX) {
			left[home[thread]] = 0U;
			home[thread] = target;
			moves[thread]++;
		}
	}

	/* Each thread moved at most once. */
	for (thread = 0U; thread < 4U; thread++) {
		snprintf(label, sizeof(label), "settle moves of thread %u", thread);
		check_value(label, moves[thread] <= 1U, 1U);
	}

	/* The four threads hold four cores: the two fast ones and two efficient ones. */
	core = 0U;
	for (thread = 0U; thread < 4U; thread++) {
		for (other = 0U; other < thread; other++) {
			if (places[home[thread]].sibling == home[other])
				core++;
		}
	}
	check_value("settle no shared core", core, 0U);
}

/* Checks that a ring of siblings that does not close ends the walk. */
static void
test_broken_ring(void)
{
	struct sched_cpu_place places[MODEL_CPU_MAX];
	uint64_t idle[KERN_CPU_MASK_WORDS];
	int whole;

	/* CPU 0 names 1, and 1 and 2 name each other: a walk from 0 never returns. */
	model_single(places, 4U);
	places[0].sibling = 1U;
	places[1].sibling = 2U;
	places[2].sibling = 1U;
	idle_all(idle, 4U);
	whole = sched_place_core_idle(places, 4U, idle, 0U);
	check_value("broken ring ends", (unsigned)whole, 1U);

	/* A sibling beyond the CPUs ends the walk too. */
	places[0].sibling = 9U;
	whole = sched_place_core_idle(places, 4U, idle, 0U);
	check_value("sibling beyond ends", (unsigned)whole, 1U);
}

/* Checks that idle bits beyond the model's CPUs are never chosen. */
static void
test_stray_bits(void)
{
	struct sched_cpu_place places[MODEL_CPU_MAX];
	uint64_t idle[KERN_CPU_MASK_WORDS];
	kern_cpu_id_t cpu;
	int better;

	/* Only bits 70 and 4 are set; the model has 4 CPUs. */
	model_single(places, 4U);
	memset(idle, 0, sizeof(idle));
	idle_set(idle, 70U);
	cpu = sched_place_choose(places, 4U, idle, idle, KERN_CPU_MAX);
	check_value("stray bit", cpu, KERN_CPU_MAX);
	idle_set(idle, 4U);
	cpu = sched_place_choose(NULL, 4U, idle, idle, KERN_CPU_MAX);
	check_value("stray bit none", cpu, KERN_CPU_MAX);

	/* A stray bit is not a better place either. */
	better = sched_place_better_idle(places, 4U, idle, idle, 1U);
	check_value("stray bit better", (unsigned)better, 0U);
}
