/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the scheduler's hint decisions (src/kern/sched-hint.c):
 * the choice of a CPU for a kind of core, checked against a reference
 * written apart from it on several shapes of machine and every (or many
 * random) idle and waiting states; and the levels a thread is queued at
 * and raised to.  Prints one line per part and "hint-test: PASS" or
 * "hint-test: FAIL" last.
 */

#include <kern/sched-hint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_CPUS	16U

/*
 * One shape of machine: the letters say each CPU's kind as the stress
 * test's boot parameter does (P fast, p fast and sibling of the CPU before
 * it, E efficient, X fast and to be avoided, A of no kind).
 */
struct shape {
	const char *name;
	const char *letters;
};

static const struct shape shapes[] = {
	{ "5330", "PpPpEEEEEEEE" },
	{ "PPEE", "PPEE" },
	{ "PpEE", "PpEE" },
	{ "allP", "PPPP" },
	{ "allA", "AAAA" },
	{ "XpEE", "XpEE" },
	{ "PXEEA", "PXEEA" },
};

static unsigned failures;

/* Builds the table of a shape. */
static unsigned
build(
	const char *letters,
	struct sched_cpu_class *classes)
{
	unsigned count;
	unsigned index;
	unsigned group;

	count = (unsigned)strlen(letters);
	group = 0;
	memset(classes, 0, sizeof(*classes) * MAX_CPUS);
	for (index = 0; index < count; index++) {
		if (letters[index] != 'p' || index == 0)
			group = index;
		classes[index].core_group = (uint16_t)group;
		classes[index].usable = 1;
		switch (letters[index]) {
		case 'P':
		case 'p':
		case 'X':
			classes[index].core = SCHED_HINT_CORE_FAST;
			classes[index].rank = 1000;
			classes[index].sibling_rank = 600;
			if (letters[index] == 'X')
				classes[index].usable = 0;
			break;
		case 'E':
			classes[index].core = SCHED_HINT_CORE_EFFICIENT;
			classes[index].rank = 700;
			classes[index].sibling_rank = 700;
			break;
		default:
			classes[index].core = SCHED_HINT_CORE_ANY;
			break;
		}
	}
	return count;
}

/* The reference: whether a CPU is free (idle and nothing waiting). */
static int
ref_free(
	unsigned state_idle,
	unsigned state_backlog,
	unsigned index)
{
	if ((state_backlog >> index) & 1U)
		return 0;
	return (state_idle >> index) & 1U;
}

/* The reference: whether every sibling of a CPU is free. */
static int
ref_whole(
	const struct sched_cpu_class *classes,
	unsigned count,
	unsigned idle,
	unsigned backlog,
	unsigned index)
{
	unsigned other;

	for (other = 0; other < count; other++)
		if (other != index && classes[other].core_group == classes[index].core_group &&
		    !ref_free(idle, backlog, other))
			return 0;
	return 1;
}

/*
 * The reference choice, written from the rules rather than from the code:
 * a list of candidates, each scored, the best score with the lowest number.
 */
static unsigned
ref_choose(
	const struct sched_cpu_class *classes,
	unsigned count,
	unsigned idle,
	unsigned backlog,
	unsigned core,
	unsigned mode)
{
	unsigned best;
	unsigned best_score;
	unsigned index;
	unsigned score;
	unsigned pass;
	int whole;
	int ok;

	if (core == SCHED_HINT_CORE_ANY)
		return KERN_CPU_MAX;
	for (pass = 0; pass < 2; pass++) {
		best = KERN_CPU_MAX;
		best_score = 0;
		for (index = 0; index < count; index++) {
			if (!classes[index].usable || classes[index].core == SCHED_HINT_CORE_ANY)
				continue;
			if (!ref_free(idle, backlog, index))
				continue;
			whole = ref_whole(classes, count, idle, backlog, index);
			score = whole ? classes[index].rank : classes[index].sibling_rank;
			ok = 0;
			if (mode == SCHED_HINT_CHOOSE_MOVE)
				ok = pass == 0 && classes[index].core == core;
			else if (mode == SCHED_HINT_CHOOSE_MOVE_IDLE_CORE)
				ok = pass == 0 && classes[index].core == core && whole;
			else if (core == SCHED_HINT_CORE_EFFICIENT)
				ok = pass == 0 && classes[index].core == SCHED_HINT_CORE_EFFICIENT;
			else if (pass == 0)
				ok = classes[index].core == SCHED_HINT_CORE_FAST && whole;
			else
				ok = classes[index].core == SCHED_HINT_CORE_EFFICIENT ||
				    (classes[index].core == SCHED_HINT_CORE_FAST && !whole);
			if (!ok)
				continue;
			if (best == KERN_CPU_MAX || score > best_score) {
				best = index;
				best_score = score;
			}
		}
		if (best != KERN_CPU_MAX)
			return best;
	}
	return KERN_CPU_MAX;
}

/* Compares the code's choice with the reference's, and checks the rules every choice keeps. */
static void
check_one(
	const struct shape *shape,
	const struct sched_cpu_class *classes,
	unsigned count,
	unsigned idle,
	unsigned backlog,
	unsigned core,
	unsigned mode)
{
	uint64_t idle_bits[KERN_CPU_MASK_WORDS];
	uint64_t backlog_bits[KERN_CPU_MASK_WORDS];
	kern_cpu_id_t got;
	unsigned want;

	memset(idle_bits, 0, sizeof(idle_bits));
	memset(backlog_bits, 0, sizeof(backlog_bits));
	idle_bits[0] = idle;
	backlog_bits[0] = backlog;
	got = sched_hint_choose(classes, count, idle_bits, backlog_bits, core, mode);
	want = ref_choose(classes, count, idle, backlog, core, mode);
	if (got != want) {
		if (failures < 10)
			printf("  %s core=%u mode=%u idle=%#x backlog=%#x: got %u want %u\n", shape->name, core, mode,
			    idle, backlog, (unsigned)got, want);
		failures++;
		return;
	}
	if (got == KERN_CPU_MAX)
		return;

	/* The rules any answer keeps: free and usable, of the kind for a move, a whole core for an idle-core move. */
	if (!ref_free(idle, backlog, got) || !classes[got].usable) {
		printf("  %s: chose a busy or unusable CPU %u\n", shape->name, (unsigned)got);
		failures++;
	}
	if ((mode == SCHED_HINT_CHOOSE_MOVE || mode == SCHED_HINT_CHOOSE_MOVE_IDLE_CORE) &&
	    classes[got].core != core) {
		printf("  %s: a move to CPU %u of another kind\n", shape->name, (unsigned)got);
		failures++;
	}
	if (mode == SCHED_HINT_CHOOSE_MOVE_IDLE_CORE && !ref_whole(classes, count, idle, backlog, got)) {
		printf("  %s: an idle-core move to CPU %u with a busy sibling\n", shape->name, (unsigned)got);
		failures++;
	}
	if (mode == SCHED_HINT_CHOOSE_PLACE && core == SCHED_HINT_CORE_EFFICIENT &&
	    classes[got].core != SCHED_HINT_CORE_EFFICIENT) {
		printf("  %s: an efficient thread placed on CPU %u\n", shape->name, (unsigned)got);
		failures++;
	}
}

/* Tests the choice on every shape, all states for small ones and random ones for large. */
static void
test_choose(
	void)
{
	struct sched_cpu_class classes[MAX_CPUS];
	unsigned shape;
	unsigned count;
	unsigned states;
	unsigned idle;
	unsigned backlog;
	unsigned core;
	unsigned mode;
	unsigned trial;
	unsigned checks;
	unsigned before;

	before = failures;
	checks = 0;
	srand(302);
	for (shape = 0; shape < sizeof(shapes) / sizeof(shapes[0]); shape++) {
		count = build(shapes[shape].letters, classes);
		states = 1U << count;
		for (core = 0; core <= SCHED_HINT_CORE_EFFICIENT; core++) {
			for (mode = SCHED_HINT_CHOOSE_MOVE; mode <= SCHED_HINT_CHOOSE_PLACE; mode++) {
				if (count <= 6) {
					for (idle = 0; idle < states; idle++)
						for (backlog = 0; backlog < states; backlog++) {
							check_one(&shapes[shape], classes, count, idle, backlog, core, mode);
							checks++;
						}
				} else {
					for (trial = 0; trial < 200000; trial++) {
						idle = (unsigned)rand() & (states - 1U);
						backlog = (unsigned)rand() & (unsigned)rand() & (states - 1U);
						check_one(&shapes[shape], classes, count, idle, backlog, core, mode);
						checks++;
					}
				}
			}
		}
	}

	/* A table naming no CPU, and a mode that is none of the three. */
	count = build("PPEE", classes);
	{
		uint64_t all[KERN_CPU_MASK_WORDS];
		uint64_t none[KERN_CPU_MASK_WORDS];

		memset(all, 0xff, sizeof(all));
		memset(none, 0, sizeof(none));
		if (sched_hint_choose(NULL, count, all, none, SCHED_HINT_CORE_FAST, SCHED_HINT_CHOOSE_MOVE) != KERN_CPU_MAX ||
		    sched_hint_choose(classes, 0, all, none, SCHED_HINT_CORE_FAST, SCHED_HINT_CHOOSE_MOVE) != KERN_CPU_MAX ||
		    sched_hint_choose(classes, count, all, none, SCHED_HINT_CORE_FAST, 9) != KERN_CPU_MAX) {
			printf("  an empty table or a bad mode chose a CPU\n");
			failures++;
		}
		/* The 5330 shape with every CPU free: a fast thread takes CPU 0, an efficient one CPU 4. */
		count = build("PpPpEEEEEEEE", classes);
		if (sched_hint_choose(classes, count, all, none, SCHED_HINT_CORE_FAST, SCHED_HINT_CHOOSE_PLACE) != 0 ||
		    sched_hint_choose(classes, count, all, none, SCHED_HINT_CORE_EFFICIENT, SCHED_HINT_CHOOSE_MOVE) != 4) {
			printf("  the 5330 shape with every CPU free chose wrongly\n");
			failures++;
		}
	}
	printf("choose: %u checks, %s\n", checks, failures == before ? "PASS" : "FAIL");
}

/* Tests the queued level and the raise. */
static void
test_levels(
	void)
{
	unsigned before;
	int base;
	int in_kernel;
	int running;
	int waiting;
	int level;
	int want;
	static const uint64_t waits[] = { 0, 50, 99, 100, 101, 200 };
	uint64_t limit;
	uint64_t waited;
	unsigned step;

	before = failures;

	/* Queued: the base, held at the user default while in the kernel. */
	for (base = SCHED_HINT_LEVEL_HIGHEST; base <= SCHED_HINT_LEVEL_LOWEST; base++)
		for (in_kernel = 0; in_kernel <= 1; in_kernel++) {
			level = sched_hint_queue_level(base, in_kernel);
			want = base;
			if (in_kernel && base > SCHED_HINT_USER_DEFAULT)
				want = SCHED_HINT_USER_DEFAULT;
			if (level != want) {
				printf("  queue level base=%d kernel=%d: %d, want %d\n", base, in_kernel, level, want);
				failures++;
			}
		}

	/* Raised: below the running level, after the limit, to the running level but never above 8. */
	limit = 100;
	for (running = 0; running <= 15; running++)
		for (waiting = 0; waiting <= 15; waiting++)
			for (step = 0; step < sizeof(waits) / sizeof(waits[0]); step++) {
				waited = waits[step];
				level = sched_hint_starve_level(running, waiting, waited, limit);
				want = -1;
				if (waiting > running && waited >= limit) {
					want = running < SCHED_HINT_LEVEL_HIGHEST ? SCHED_HINT_LEVEL_HIGHEST : running;
					if (want >= waiting)
						want = -1;
				}
				if (level != want) {
					printf("  raise running=%d waiting=%d waited=%llu: %d, want %d\n", running, waiting,
					    (unsigned long long)waited, level, want);
					failures++;
				}
				if (level >= 0 && level < SCHED_HINT_LEVEL_HIGHEST) {
					printf("  raise above the kernel's default\n");
					failures++;
				}
			}
	printf("levels: %s\n", failures == before ? "PASS" : "FAIL");
}

int
main(void)
{
	test_choose();
	test_levels();
	printf("hint-test: %s\n", failures == 0 ? "PASS" : "FAIL");
	return failures == 0 ? 0 : 1;
}
