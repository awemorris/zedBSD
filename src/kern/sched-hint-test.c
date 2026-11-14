/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The stress test of the scheduler's hints.
 *
 * On a machine whose CPUs are all of one kind, the boot line can give the
 * scheduler a false table of kinds (desktop_balancer.classes=PPEE: one
 * letter a CPU, P fast, p fast and the SMT sibling of the CPU before it, E
 * efficient, X fast but to be avoided, anything else of no kind).  With
 * desktop_balancer.stress=1 a kernel thread then sets random hints on
 * random user threads every few milliseconds and asks for them to be
 * moved, so that the moves, the handoffs of running threads and the level
 * changes race everything else the threads do.
 *
 * What the thread did is kept in sched_hint_test_report, which a debugger
 * reads from the running kernel.
 */

#include <kern/sched-hint.h>
#include <kern/sched.h>
#include <kern/thread.h>
#include <kern/process.h>
#include <kern/lock.h>
#include <kern/kmem.h>
#include <kern/klog.h>
#include <kern/clock.h>
#include <kern/boot.h>
#include <kern/kcrt.h>
#include <kern/platform.h>

#include <uapi/errno.h>

#if CONFIG_KERN_DESKTOP_BALANCER && CONFIG_KERN_DESKTOP_BALANCER_TEST

/* The boot parameters the test reads. */
#define HINT_TEST_COMMAND_LINE		"boot.command-line"
#define HINT_TEST_CLASSES		"desktop_balancer.classes="
#define HINT_TEST_STRESS		"desktop_balancer.stress=1"

/* How often the test thread acts, and on how many threads each time. */
#define HINT_TEST_PERIOD_MS		10U
#define HINT_TEST_THREADS_PER_PASS	4U

/* The speeds the false table gives, on the scale of struct sched_cpu_class. */
#define HINT_TEST_FAST_RANK		1000U
#define HINT_TEST_FAST_SIBLING_RANK	600U
#define HINT_TEST_EFFICIENT_RANK	700U

/* What the report starts with, so that a reader knows it found it: "HINTSTRS". */
#define HINT_TEST_MAGIC			0x48494e5453545253ULL

/* The answers of sched_hint_set() and sched_hint_place() the report counts apart. */
#define HINT_TEST_ANSWER_OK		0U
#define HINT_TEST_ANSWER_EALREADY	1U
#define HINT_TEST_ANSWER_EAGAIN		2U
#define HINT_TEST_ANSWER_EBUSY		3U
#define HINT_TEST_ANSWER_ESRCH		4U
#define HINT_TEST_ANSWER_EINVAL		5U
#define HINT_TEST_ANSWER_OTHER		6U
#define HINT_TEST_ANSWERS		7U

/*
 * What the test thread did and what the scheduler counted, for a debugger.
 *
 * Only the test thread writes it, and it lives as long as the kernel.  A
 * reader stops the machine and reads it whole; passes grows with every
 * pass, so two readings tell whether the thread still runs.
 */
struct sched_hint_test_report {
	uint64_t magic;
	uint64_t passes;
	uint64_t threads_hinted;
	uint64_t classes_cpus;
	uint64_t set_answers[HINT_TEST_ANSWERS];
	uint64_t place_answers[HINT_TEST_ANSWERS];
	struct sched_hint_counters counters;
};

/*
 * The report.  It is global and kept so that the debugger finds it by
 * name in the kernel image.
 */
volatile struct sched_hint_test_report sched_hint_test_report __attribute__((used));

/*
 * The test thread's random state, a 32-bit xorshift.  Only the test thread
 * uses it after the start.
 */
static uint32_t hint_test_random_state;

static const char *test_parameter(const char *line, const char *name, size_t *length);
static void test_classes(const char *value, size_t length);
static uint32_t test_random(void);
static unsigned test_answer(int error);
static struct thread *test_pick_thread(pid_t *cursor);
static void test_hint_one(struct thread *thread);
static void test_publish(void);
static void test_thread(void *argument);

/*
 * Starts the hints' stress test when the boot line asks for it.
 */
void
sched_hint_test_start(
	void)
{
	struct thread *thread;
	const char *line;
	const char *value;
	size_t length;
	int stress;
	int error;

	/* Reads the boot line; without one there is nothing to start. */
	line = kern_boot_handoff(HINT_TEST_COMMAND_LINE);
	if (line == NULL)
		return;

	/* Publishes the false table of kinds the boot line gives. */
	value = test_parameter(line, HINT_TEST_CLASSES, &length);
	if (value != NULL)
		test_classes(value, length);

	/* Starts the thread that sets the hints, if asked. */
	stress = kern_boot_parameters_token_present(line, HINT_TEST_STRESS);
	if (!stress)
		return;
	sched_hint_test_report.magic = HINT_TEST_MAGIC;
	hint_test_random_state = (uint32_t)clock_ticks() | 1U;
	error = kthread_create(test_thread, NULL, SCHED_PRIORITY_DEFAULT, &thread);
	if (error != 0) {
		kern_logf("sched: the hint stress thread was not made (%d)\n", error);
		return;
	}
	thread_start(thread);
	kern_logf("sched: hint stress on, %u threads every %u ms\n", HINT_TEST_THREADS_PER_PASS,
	    HINT_TEST_PERIOD_MS);
}

/*
 * Finds a parameter given as name=value on the boot line and reports its
 * value and the value's length, or NULL.
 */
static const char *
test_parameter(
	const char *line,
	const char *name,
	size_t *length)
{
	size_t position;
	size_t start;
	size_t name_length;
	size_t index;
	int same;

	/* Walks the parameters, separated as the parser separates them. */
	name_length = kern_strlen(name);
	position = 0;
	while (position < KERN_BOOT_PARAMETERS_STORAGE_SIZE && line[position] != '\0') {
		/* Steps over the separators. */
		if (line[position] == ' ' || line[position] == '\t' ||
		    line[position] == '\n' || line[position] == '\r') {
			position++;
			continue;
		}

		/* The parameter's extent. */
		start = position;
		while (position < KERN_BOOT_PARAMETERS_STORAGE_SIZE &&
		       line[position] != '\0' &&
		       line[position] != ' ' && line[position] != '\t' &&
		       line[position] != '\n' && line[position] != '\r')
			position++;

		/* A parameter that starts with the name. */
		if (position - start < name_length)
			continue;
		same = 1;
		for (index = 0; index < name_length; index++) {
			if (line[start + index] != name[index]) {
				same = 0;
				break;
			}
		}
		if (same) {
			*length = position - start - name_length;
			return line + start + name_length;
		}
	}

	/* Not there. */
	return NULL;
}

/* Publishes the false table of kinds a boot parameter's value describes. */
static void
test_classes(
	const char *value,
	size_t length)
{
	struct sched_cpu_class *classes;
	unsigned count;
	unsigned index;
	uint16_t group;
	char letter;

	/* One entry a CPU; CPUs past the letters have no kind. */
	count = kern_cpu_count();
	classes = kern_calloc(count, sizeof(*classes));
	if (classes == NULL) {
		kern_logf("sched: no memory for the test table of kinds\n");
		return;
	}

	/* Fills the entries, one letter a CPU, a new core for each but a sibling. */
	group = 0;
	for (index = 0; index < count; index++) {
		letter = 'A';
		if (index < length)
			letter = value[index];
		if (letter != 'p' || index == 0)
			group = (uint16_t)index;
		classes[index].core_group = group;
		classes[index].usable = 1;

		/* The letter's kind and speed. */
		switch (letter) {
		case 'P':
		case 'p':
		case 'X':
			classes[index].core = SCHED_HINT_CORE_FAST;
			classes[index].rank = HINT_TEST_FAST_RANK;
			classes[index].sibling_rank = HINT_TEST_FAST_SIBLING_RANK;
			if (letter == 'X')
				classes[index].usable = 0;
			break;
		case 'E':
			classes[index].core = SCHED_HINT_CORE_EFFICIENT;
			classes[index].rank = HINT_TEST_EFFICIENT_RANK;
			classes[index].sibling_rank = HINT_TEST_EFFICIENT_RANK;
			break;
		default:
			classes[index].core = SCHED_HINT_CORE_ANY;
			break;
		}
	}

	/* Hands the table to the scheduler, which copies it. */
	sched_cpu_classes_set(classes, count);
	sched_hint_test_report.classes_cpus = count;
	kern_free(classes);
	kern_logf("sched: test table of CPU kinds for %u CPUs\n", count);
}

/* Draws the next random number. */
static uint32_t
test_random(
	void)
{
	uint32_t state;

	/* One step of the xorshift. */
	state = hint_test_random_state;
	state ^= state << 13;
	state ^= state >> 17;
	state ^= state << 5;
	hint_test_random_state = state;

	/* Reports the number. */
	return state;
}

/* Reports which counter of the report an answer goes to. */
static unsigned
test_answer(
	int error)
{
	/* The answers the calls are documented to give. */
	switch (error) {
	case 0:
		return HINT_TEST_ANSWER_OK;
	case EALREADY:
		return HINT_TEST_ANSWER_EALREADY;
	case EAGAIN:
		return HINT_TEST_ANSWER_EAGAIN;
	case EBUSY:
		return HINT_TEST_ANSWER_EBUSY;
	case ESRCH:
		return HINT_TEST_ANSWER_ESRCH;
	case EINVAL:
		return HINT_TEST_ANSWER_EINVAL;
	default:
		return HINT_TEST_ANSWER_OTHER;
	}
}

/*
 * Picks a random thread of the next user process after a cursor and holds
 * a reference to it, or reports none.
 */
static struct thread *
test_pick_thread(
	pid_t *cursor)
{
	struct process *process;
	struct thread *thread;
	struct thread *picked;
	unsigned long irq;
	unsigned count;
	unsigned choice;

	/* The next process, starting over from the first at the end. */
	process = process_find_next_ref(*cursor);
	if (process == NULL) {
		*cursor = 0;
		process = process_find_next_ref(*cursor);
		if (process == NULL)
			return NULL;
	}
	*cursor = process->pid;

	/* The kernel's own process and a finished one have no thread to hint. */
	if (process == &process0 || process->state == PROCESS_DEAD) {
		process_release(process);
		return NULL;
	}

	/* One of its threads at random, held past the process lock. */
	picked = NULL;
	irq = spin_lock_irqsave(&process->lock);

	count = 0;
	for (thread = process->threads; thread != NULL; thread = thread->proc_next)
		count++;
	if (count != 0) {
		choice = test_random() % count;
		for (thread = process->threads; choice != 0; thread = thread->proc_next)
			choice--;
		picked = thread;
		thread_ref(picked);
	}

	spin_unlock_irqrestore(&process->lock, irq);
	process_release(process);

	/* Reports the thread, or none. */
	return picked;
}

/* Gives one thread a random hint and asks for a move. */
static void
test_hint_one(
	struct thread *thread)
{
	struct sched_hint hint;
	uint32_t bits;
	int error;

	/* A random kind, strength, flag and level, now and then one out of range. */
	bits = test_random();
	hint.core = (uint8_t)(bits % 3U);
	hint.strength = (uint8_t)((bits >> 2) % 4U);
	hint.flags = (uint8_t)((bits >> 4) & SCHED_HINT_NO_OVERFLOW);
	hint.priority = (uint8_t)(SCHED_HINT_LEVEL_HIGHEST + (bits >> 5) % 5U);
	if ((bits >> 8) % 64U == 0U)
		hint.priority = SCHED_HINT_LEVEL_HIGHEST - 1;

	/* Sets it, and asks for a move as the balancer would. */
	error = sched_hint_set(thread, &hint);
	sched_hint_test_report.set_answers[test_answer(error)]++;
	error = sched_hint_place(thread);
	sched_hint_test_report.place_answers[test_answer(error)]++;
	sched_hint_test_report.threads_hinted++;
}

/* Copies the scheduler's counters into the report. */
static void
test_publish(
	void)
{
	struct sched_hint_counters counters;

	/* The sums as they stand. */
	sched_hint_counters(&counters);
	sched_hint_test_report.counters = counters;
}

/* Sets random hints on random user threads until the machine stops. */
static void
test_thread(
	void *argument)
{
	struct thread *thread;
	pid_t cursor;
	unsigned index;

	/* Hints a few threads each period, forever. */
	(void)argument;
	cursor = 0;
	for (;;) {
		sched_sleep(sched_ticks() + kern_ms_to_ticks(HINT_TEST_PERIOD_MS));
		for (index = 0; index < HINT_TEST_THREADS_PER_PASS; index++) {
			thread = test_pick_thread(&cursor);
			if (thread == NULL)
				continue;
			test_hint_one(thread);
			thread_release(thread);
		}
		sched_hint_test_report.passes++;
		test_publish();
	}
}

#endif
