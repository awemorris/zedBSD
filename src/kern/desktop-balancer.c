/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The desktop thread balancer.
 *
 * The input core reports every event a person makes and the GPU core
 * every request that puts work on a GPU or an image on a display.  The
 * notes only store a time and count, so they cost the reporting paths an
 * atomic operation or two and take no lock.
 *
 * A kernel thread looks at every user thread ten times a second while a
 * person uses the machine or did in the last minute, once a second while
 * nobody does.  It tells the scheduler what kind of core each CPU is,
 * decides from the thread's counters, its process's GPU times, nice value
 * and terminal and the person's input which kind of core and which level
 * the thread should have, hands the scheduler that hint, and asks it to
 * move a thread that sits on a CPU of the wrong kind.  Where and when a
 * thread runs stays the scheduler's choice; a move it cannot make now it
 * refuses, and a later look asks again.
 */

#include <kern/desktop-balancer.h>
#include <kern/desktop-balancer-policy.h>
#include <kern/sched-hint.h>
#include "kern/atomic.h"
#include "kern/clock.h"
#include "kern/process.h"
#include "kern/thread.h"
#include "kern/sched.h"
#include "kern/lock.h"
#include "kern/kmem.h"
#include "kern/klog.h"
#include "kern/boot.h"
#include "kern/kcrt.h"
#include "kern/platform.h"
#if CONFIG_KERN_THERMAL
#include "kern/cpuperf.h"
#endif

#include <hal/hal.h>
#include <uapi/errno.h>
#include <uapi/sysctl.h>

/* The boot line, and the word on it that keeps the balancer from starting. */
#define BALANCER_COMMAND_LINE		"boot.command-line"
#define BALANCER_OFF			"desktop_balancer=off"

/*
 * The balancer thread's level: above the kernel's default, so that its
 * short looks are not held back behind busy kernel workers.
 */
#define BALANCER_THREAD_PRIORITY	5

/* Nanoseconds in a second, for the looks' times. */
#define BALANCER_NS_PER_SECOND		1000000000ULL

/*
 * The rules, the person's states and the kinds of core are the report's:
 * each name has the same number in both.
 */
typedef char balancer_report_names_match[(DESKTOP_BALANCER_RULES <= KERN_DESKTOP_BALANCER_RULES &&
    DESKTOP_BALANCER_RULE_NICE_FAST == KERN_DESKTOP_BALANCER_RULE_NICE_FAST &&
    DESKTOP_BALANCER_RULE_NICE_EFFICIENT == KERN_DESKTOP_BALANCER_RULE_NICE_EFFICIENT &&
    DESKTOP_BALANCER_RULE_BUSY == KERN_DESKTOP_BALANCER_RULE_BUSY &&
    DESKTOP_BALANCER_RULE_PRESENTER == KERN_DESKTOP_BALANCER_RULE_PRESENTER &&
    DESKTOP_BALANCER_RULE_GPU == KERN_DESKTOP_BALANCER_RULE_GPU &&
    DESKTOP_BALANCER_RULE_TERMINAL == KERN_DESKTOP_BALANCER_RULE_TERMINAL &&
    DESKTOP_BALANCER_RULE_OTHER == KERN_DESKTOP_BALANCER_RULE_OTHER &&
    DESKTOP_BALANCER_STATE_AWAY == KERN_DESKTOP_BALANCER_STATE_AWAY &&
    DESKTOP_BALANCER_STATE_QUIET == KERN_DESKTOP_BALANCER_STATE_QUIET &&
    DESKTOP_BALANCER_STATE_ACTIVE == KERN_DESKTOP_BALANCER_STATE_ACTIVE &&
    SCHED_HINT_CORE_ANY == KERN_DESKTOP_BALANCER_CORE_ANY &&
    SCHED_HINT_CORE_FAST == KERN_DESKTOP_BALANCER_CORE_FAST &&
    SCHED_HINT_CORE_EFFICIENT == KERN_DESKTOP_BALANCER_CORE_EFFICIENT) ? 1 : -1];

#if CONFIG_KERN_THERMAL
/*
 * The value the CPUs' performance report gives for what it does not know
 * is the one the balancer's decisions take.
 */
typedef char balancer_unknown_matches[(KERN_CPUPERF_UNKNOWN == DESKTOP_BALANCER_UNKNOWN) ? 1 : -1];

/*
 * One CPU as the CPUs' performance report gave it at the last reading:
 * where it sits (CPUs with the same package, die and core are threads of
 * one core), its kind, its relative speed and the use the hardware asks
 * for.  known is zero for a CPU the report did not answer for.
 */
struct balancer_cpu_reading {
	uint32_t package;
	uint32_t die;
	uint32_t core;
	uint32_t known;
	uint32_t kind;			/* SCHED_HINT_CORE_* */
	uint32_t relative;
	uint32_t available;
};
#endif

/*
 * What one look saw and has left: its time, the person's state, the moves
 * it may still ask for (of them for running threads), the moves taken and
 * refused, how many threads each rule took, and the terminals' threads
 * kept, each held by a reference, for the moves left at the end of the
 * look.
 */
struct balancer_look {
	uint64_t now;
	uint64_t elapsed;
	unsigned state;
	unsigned moves_left;
	unsigned handoffs_left;
	unsigned deferred_count;
	uint64_t places;
	uint64_t places_refused;
	uint32_t rule_threads[DESKTOP_BALANCER_RULES];
	struct thread *deferred[DESKTOP_BALANCER_MOVES_PER_PASS];
};

/*
 * When a person last made an input event, in scheduler ticks, and how
 * many they made since boot.
 *
 * Any context writes them with atomic operations only, from whichever CPU
 * the input arrived on; readers load them atomically.  Zero time means no
 * input since boot.  They live as long as the kernel.
 */
static volatile uint64_t balancer_input_tick;
static volatile uint64_t balancer_input_count;

/*
 * The kind of every CPU as the balancer last saw it: what the scheduler
 * has published, or, while the CPUs are of one kind, the description the
 * balancer made of them.  balancer_cpu_count entries, allocated once at
 * the start and kept for the kernel's life; only the balancer thread
 * writes it after that.
 */
static struct sched_cpu_class *balancer_classes;
static unsigned balancer_cpu_count;

/*
 * Whether the scheduler has a table that names both kinds of core.  Only
 * the balancer thread writes and reads it; a thread is moved only while
 * it is set.
 */
static int balancer_hybrid;

#if CONFIG_KERN_THERMAL
/*
 * Each CPU as the performance report gave it, balancer_cpu_count entries
 * allocated with balancer_classes; and the report's generation the table
 * was made from, valid once balancer_generation_read is set.  Only the
 * balancer thread uses them.
 */
static struct balancer_cpu_reading *balancer_readings;
static uint32_t balancer_generation;
static int balancer_generation_read;
#endif

/*
 * The look under way.  It is reset at the start of each look and only the
 * balancer thread uses it; it is static because it is too large for the
 * thread's stack.
 */
static struct balancer_look balancer_current_look;

/*
 * The threads of the process the balancer looks at now, each held by a
 * reference taken under the process's lock and given up once the thread
 * is decided.  Only the balancer thread uses it.
 */
static struct thread *balancer_threads[DESKTOP_BALANCER_THREADS_PER_PROCESS];

/* The busy limits in scheduler ticks, set at the start and never changed. */
static struct desktop_balancer_limits balancer_limits;

/*
 * Whether the balancer thread was started: 0 until it is, then for good.
 * Written once by the boot thread, read by the report.
 */
static volatile unsigned balancer_running;

/*
 * The report kern.desktop_balancer gives, as of the last look.
 *
 * The balancer thread writes it under balancer_report_lock at the end of
 * each look and a reader copies it under the same lock; the scheduler's
 * counters are added at each read.  It lives as long as the kernel.
 */
static struct spinlock balancer_report_lock;
static struct desktop_balancer_info balancer_report;

static void balancer_thread(void *argument);
static unsigned balancer_pass(uint64_t elapsed);
static void balancer_refresh_classes(void);
#if CONFIG_KERN_THERMAL
static void balancer_read_cpuperf(void);
static void balancer_read_cpu(unsigned cpu);
static void balancer_describe_cpu(unsigned cpu);
static unsigned balancer_core_kind(uint32_t core_class);
#endif
static void balancer_look_process(struct process *process, struct balancer_look *look);
static unsigned balancer_take_threads(struct process *process);
static unsigned balancer_take_after_cursor(struct process *process);
static void balancer_look_thread(struct thread *thread, const struct desktop_balancer_traits *process_traits, struct balancer_look *look);
static int balancer_wants_move(const struct desktop_balancer_history *history, const struct sched_hint_activity *activity, uint64_t now);
static void balancer_place(struct thread *thread, int running, struct balancer_look *look);
static void balancer_place_deferred(struct balancer_look *look);
static void balancer_publish(const struct balancer_look *look, uint64_t pass_ns);
static uint64_t balancer_time_ns(void);

/*
 * Notes one input event a person made on any device.
 */
void
kern_desktop_balancer_note_input(
	void)
{
	uint64_t now;

	/* Records when, and counts the event. */
	now = clock_ticks();
	atomic_u64_store_release(&balancer_input_tick, now);
	(void)atomic_u64_fetch_add_relaxed(&balancer_input_count, 1U);
}

/*
 * Notes a successful GPU request of the calling thread's process.
 */
void
kern_desktop_balancer_note_gpu(
	uint32_t what)
{
	struct process *process;
	struct thread *thread;
	uint64_t now;

	/* Only a user process's request says anything about how it behaves. */
	thread = curthread;
	if (thread == NULL)
		return;
	process = thread->proc;
	if (process == NULL || process == &process0)
		return;

	/* Records the time against what the request did. */
	now = clock_ticks();
	if (what == KERN_DESKTOP_BALANCER_GPU_PRESENT) {
		atomic_u64_store_release(&process->desktop_balancer.present_tick, now);
	} else if (what == KERN_DESKTOP_BALANCER_GPU_SUBMIT) {
		atomic_u64_store_release(&process->desktop_balancer.gpu_submit_tick, now);
	}
}

/*
 * Reports when a person last used an input device and how many events
 * they made.
 */
void
kern_desktop_balancer_input_activity(
	uint64_t *tick,
	uint64_t *count)
{
	/* Loads the time and the count; each is recent, not one snapshot. */
	*tick = atomic_u64_load_acquire(&balancer_input_tick);
	*count = atomic_u64_load_acquire(&balancer_input_count);
}

/*
 * Starts the balancer's thread unless the boot line says
 * desktop_balancer=off.
 */
void
kern_desktop_balancer_init(
	void)
{
	struct thread *thread;
	const char *line;
	unsigned count;
	int off;
	int error;

	/* The report exists whether the balancer runs or not. */
	spin_init(&balancer_report_lock, LOCK_RANK_DEVICE, "desktop balancer report");

	/* The busy limits, in the scheduler's ticks. */
	balancer_limits.busy_on = kern_ms_to_ticks(DESKTOP_BALANCER_BUSY_ON_MS);
	balancer_limits.busy_off = kern_ms_to_ticks(DESKTOP_BALANCER_BUSY_OFF_MS);

	/* The table of the CPUs' kinds the balancer keeps, an entry a CPU. */
	count = kern_cpu_count();
	balancer_classes = kern_calloc(count, sizeof(*balancer_classes));
	if (balancer_classes == NULL) {
		kern_logf("desktop balancer: no memory for the table of CPU kinds\n");
		return;
	}
#if CONFIG_KERN_THERMAL

	/* Each CPU as the performance report gives it, read with the kinds. */
	balancer_readings = kern_calloc(count, sizeof(*balancer_readings));
	if (balancer_readings == NULL) {
		kern_logf("desktop balancer: no memory for the CPUs' readings\n");
		return;
	}
#endif
	balancer_cpu_count = count;

	/* desktop_balancer=off leaves the scheduler as it is without the balancer. */
	line = kern_boot_handoff(BALANCER_COMMAND_LINE);
	if (line != NULL) {
		off = kern_boot_parameters_token_present(line, BALANCER_OFF);
		if (off) {
			kern_logf("desktop balancer: off\n");
			return;
		}
	}

	/* Starts the thread that looks at the threads. */
	error = kthread_create(balancer_thread, NULL, BALANCER_THREAD_PRIORITY, &thread);
	if (error != 0) {
		kern_logf("desktop balancer: the thread was not made (%d)\n", error);
		return;
	}

	/* Marks the balancer as running for the report, then lets its thread go. */
	atomic_raw_store_release(&balancer_running, 1U);
	thread_start(thread);
	kern_logf("desktop balancer: on, a look every %u ms\n", DESKTOP_BALANCER_PASS_MS);
}

/*
 * Fills the balancer's report.
 */
void
kern_desktop_balancer_info(
	struct desktop_balancer_info *info)
{
	struct sched_hint_counters counters;
	unsigned long irq;
	unsigned running;

	/* What the last look left, copied whole. */
	irq = spin_lock_irqsave(&balancer_report_lock);

	*info = balancer_report;

	spin_unlock_irqrestore(&balancer_report_lock, irq);

	/* The layout, and whether the balancer is built and runs. */
	info->version = KERN_DESKTOP_BALANCER_VERSION;
	info->struct_size = sizeof(*info);
	info->flags |= KERN_DESKTOP_BALANCER_BUILT;
	running = atomic_raw_load_acquire(&balancer_running);
	if (running != 0U)
		info->flags |= KERN_DESKTOP_BALANCER_RUNNING;

	/* The scheduler's counters as they stand now. */
	sched_hint_counters(&counters);
	info->hints = counters.hints;
	info->moves = counters.moves;
	info->refused = counters.refused;
	info->handoffs = counters.handoffs;
	info->handoff_retries = counters.handoff_retries;
	info->handoff_failures = counters.handoff_failures;
	info->starved = counters.starved;
	info->kernel_held = counters.kernel_held;
}

/* Looks at the threads at the period the person's state asks for, until the machine stops. */
static void
balancer_thread(
	void *argument)
{
	uint64_t last;
	uint64_t now;
	uint64_t period;
	unsigned state;

	/* A look, then a sleep, forever; the first look's time runs from the start. */
	(void)argument;
	last = clock_ticks();
	for (;;) {
		now = clock_ticks();
		state = balancer_pass(now - last);
		last = now;

		/* Sleeps longer while nobody uses the machine. */
		period = kern_ms_to_ticks(DESKTOP_BALANCER_PASS_MS);
		if (state == DESKTOP_BALANCER_STATE_AWAY)
			period = kern_ms_to_ticks(DESKTOP_BALANCER_PASS_AWAY_MS);
		sched_sleep(sched_ticks() + period);
	}
}

/*
 * Looks at every user thread once: describes the CPUs, reads the person's
 * state, decides every thread, moves what it may, and publishes the
 * report.  Reports the person's state.
 */
static unsigned
balancer_pass(
	uint64_t elapsed)
{
	struct balancer_look *look;
	struct process *process;
	uint64_t input_tick;
	uint64_t input_count;
	uint64_t started;
	uint64_t finished;
	pid_t cursor;

	/* The look's start, for its time. */
	started = balancer_time_ns();

	/* The CPUs' kinds, made again when the hardware's report changed. */
	balancer_refresh_classes();

	/* What the person is doing. */
	look = &balancer_current_look;
	kern_memset(look, 0, sizeof(*look));
	look->now = clock_ticks();
	look->elapsed = elapsed;
	kern_desktop_balancer_input_activity(&input_tick, &input_count);
	look->state = desktop_balancer_user_state(look->now, input_tick,
	    kern_ms_to_ticks(DESKTOP_BALANCER_USER_ACTIVE_MS),
	    kern_ms_to_ticks(DESKTOP_BALANCER_USER_AWAY_MS));

	/* What this look may move. */
	look->moves_left = DESKTOP_BALANCER_MOVES_PER_PASS;
	look->handoffs_left = DESKTOP_BALANCER_HANDOFFS_PER_PASS;

	/* Every process in the order of its ID, each held while its threads are decided. */
	cursor = 0;
	for (;;) {
		process = process_find_next_ref(cursor);
		if (process == NULL)
			break;
		cursor = process->pid;
		balancer_look_process(process, look);
		process_release(process);
	}

	/* The terminals' threads get the moves the others left. */
	balancer_place_deferred(look);

	/* The look's time, and the report. */
	finished = balancer_time_ns();
	balancer_publish(look, finished - started);

	/* Reports the person's state, which sets the next look's time. */
	return look->state;
}

/*
 * Makes the table of the CPUs' kinds the balancer works with: from the
 * hardware's report when there is one and the boot line gave no test
 * table, then as the scheduler has it published.
 */
static void
balancer_refresh_classes(
	void)
{
	int given;
	int hybrid;

	/* A test table on the boot line stands for the hardware's. */
	given = 0;
#if CONFIG_KERN_DESKTOP_BALANCER_TEST
	given = sched_hint_test_classes_given();
#endif
#if CONFIG_KERN_THERMAL

	/* The hardware's kinds, read again when its report changed. */
	if (!given)
		balancer_read_cpuperf();
#endif
	(void)given;

	/* What the scheduler moves by: a table naming both kinds, or none. */
	hybrid = sched_cpu_classes_get(balancer_classes, balancer_cpu_count);
	balancer_hybrid = hybrid;
}

#if CONFIG_KERN_THERMAL
/*
 * Reads every CPU's kind, speed and place from the CPUs' performance
 * report, when it changed since the last reading, and gives the scheduler
 * the table made of them.
 */
static void
balancer_read_cpuperf(
	void)
{
	uint32_t generation;
	unsigned cpu;

	/* Nothing changed since the last reading. */
	generation = kern_cpuperf_generation();
	if (balancer_generation_read && generation == balancer_generation)
		return;

	/* Each CPU as the report gives it, without waking any CPU to measure. */
	for (cpu = 0; cpu < balancer_cpu_count; cpu++)
		balancer_read_cpu(cpu);

	/* Each CPU's description, with the first CPU of its core as its group. */
	for (cpu = 0; cpu < balancer_cpu_count; cpu++)
		balancer_describe_cpu(cpu);

	/*
	 * Hands the table to the scheduler, which keeps it only if it names
	 * both kinds; one kind alone leaves only the levels in force.
	 */
	sched_cpu_classes_set(balancer_classes, balancer_cpu_count);
	balancer_generation = generation;
	balancer_generation_read = 1;
}

/* Reads one CPU from the performance report. */
static void
balancer_read_cpu(
	unsigned cpu)
{
	struct balancer_cpu_reading *reading;
	struct kern_cpuperf_cpu info;
	int error;

	/* What the report says without waking the CPU; a CPU it does not answer for is of no kind. */
	reading = &balancer_readings[cpu];
	kern_memset(reading, 0, sizeof(*reading));
	reading->kind = SCHED_HINT_CORE_ANY;
	reading->relative = DESKTOP_BALANCER_UNKNOWN;
	reading->available = DESKTOP_BALANCER_UNKNOWN;
	error = kern_cpuperf_cpu((kern_cpu_id_t)cpu, 0, &info);
	if (error != 0)
		return;

	/* Its place, kind, speed and the use the hardware asks for. */
	reading->package = info.topology.package;
	reading->die = info.topology.die;
	reading->core = info.topology.core;
	reading->known = 1;
	reading->kind = balancer_core_kind(info.topology.core_class);
	reading->relative = info.relative;
	reading->available = info.available;
}

/* Describes one CPU from its reading and its core's other CPUs. */
static void
balancer_describe_cpu(
	unsigned cpu)
{
	const struct balancer_cpu_reading *reading;
	const struct balancer_cpu_reading *other;
	unsigned index;
	unsigned group;
	int sibling;

	/* The CPUs of the same core: the first names the group, any other makes a sibling. */
	reading = &balancer_readings[cpu];
	group = cpu;
	sibling = 0;
	for (index = 0; index < balancer_cpu_count && reading->known; index++) {
		other = &balancer_readings[index];
		if (index == cpu || !other->known)
			continue;
		if (other->package != reading->package ||
		    other->die != reading->die ||
		    other->core != reading->core)
			continue;
		sibling = 1;
		if (index < group)
			group = index;
	}

	/* The description the scheduler matches hints against. */
	desktop_balancer_describe_cpu(reading->kind, reading->relative, reading->available, sibling,
	    &balancer_classes[cpu]);
	balancer_classes[cpu].core_group = (uint16_t)group;
}

/* Reports the scheduler's kind of core for the performance report's kind. */
static unsigned
balancer_core_kind(
	uint32_t core_class)
{
	/* The two kinds the report tells apart. */
	if (core_class == KERN_CPUPERF_CORE_PERFORMANCE)
		return SCHED_HINT_CORE_FAST;
	if (core_class == KERN_CPUPERF_CORE_EFFICIENCY)
		return SCHED_HINT_CORE_EFFICIENT;

	/* A kind nobody reported. */
	return SCHED_HINT_CORE_ANY;
}
#endif

/* Decides every thread of one process the look takes. */
static void
balancer_look_process(
	struct process *process,
	struct balancer_look *look)
{
	struct desktop_balancer_traits traits;
	unsigned long irq;
	unsigned count;
	unsigned index;
	uint64_t gpu_tick;
	uint64_t present_tick;

	/* The kernel's own threads, and a process not running or stopped, take no hints. */
	if (process == &process0)
		return;
	if (process->state != PROCESS_RUNNING && process->state != PROCESS_STOPPED)
		return;

	/* Whether the process put work on a GPU or an image on a display lately. */
	kern_memset(&traits, 0, sizeof(traits));
	traits.state = look->state;
	gpu_tick = atomic_u64_load_acquire(&process->desktop_balancer.gpu_submit_tick);
	present_tick = atomic_u64_load_acquire(&process->desktop_balancer.present_tick);
	traits.gpu = (uint8_t)desktop_balancer_recent(look->now, gpu_tick,
	    kern_ms_to_ticks(DESKTOP_BALANCER_GPU_WINDOW_MS));
	traits.presenter = (uint8_t)desktop_balancer_recent(look->now, present_tick,
	    kern_ms_to_ticks(DESKTOP_BALANCER_PRESENT_WINDOW_MS));

	/* Its nice value and terminal, and its threads, each held past the lock. */
	irq = spin_lock_irqsave(&process->lock);

	traits.nice = process->nice_value;
	traits.terminal = 0;
	if (process->controlling_tty != NULL)
		traits.terminal = 1;
	count = balancer_take_threads(process);

	spin_unlock_irqrestore(&process->lock, irq);

	/* Decides each thread; each reference is given up there or kept for the end of the look. */
	for (index = 0; index < count; index++) {
		balancer_look_thread(balancer_threads[index], &traits, look);
		balancer_threads[index] = NULL;
	}
}

/*
 * Takes a reference to each thread of a process the look decides, into
 * balancer_threads, and reports how many.  A process with more threads
 * than one look takes is taken in turns by thread ID.  The process's lock
 * is held.
 */
static unsigned
balancer_take_threads(
	struct process *process)
{
	struct thread *thread;
	unsigned count;
	unsigned index;

	/* A process with more threads than a look takes goes on from its cursor. */
	if (process->thread_count > DESKTOP_BALANCER_THREADS_PER_PROCESS) {
		count = balancer_take_after_cursor(process);
	} else {
		/* Every thread, in the order of the list. */
		count = 0;
		for (thread = process->threads;
		     thread != NULL && count < DESKTOP_BALANCER_THREADS_PER_PROCESS;
		     thread = thread->proc_next) {
			balancer_threads[count] = thread;
			count++;
		}

		/* A process that fits in one look has no place to go on from. */
		process->desktop_balancer.thread_cursor = 0;
	}

	/* Holds every thread taken, so that it outlives the lock. */
	for (index = 0; index < count; index++)
		thread_ref(balancer_threads[index]);

	/* Reports how many the look decides. */
	return count;
}

/*
 * Takes the threads with the lowest IDs above the process's cursor, as
 * many as a look takes, into balancer_threads in the order of their IDs;
 * moves the cursor past them, or back to the start when fewer were left;
 * and reports how many.  The process's lock is held.
 */
static unsigned
balancer_take_after_cursor(
	struct process *process)
{
	struct thread *thread;
	int32_t cursor;
	unsigned count;
	unsigned position;
	unsigned last;

	/* Keeps the lowest IDs above the cursor, in order, as the list is walked. */
	cursor = process->desktop_balancer.thread_cursor;
	count = 0;
	for (thread = process->threads; thread != NULL; thread = thread->proc_next) {
		/* A thread the last look took, or one before it, waits for the next round. */
		if (thread->tid <= cursor)
			continue;

		/* Where it goes among those kept, the lowest first; past the end it is not kept. */
		position = count;
		while (position > 0U && balancer_threads[position - 1U]->tid > thread->tid)
			position--;
		if (position >= DESKTOP_BALANCER_THREADS_PER_PROCESS)
			continue;

		/* Makes room for it, dropping the highest once the array is full. */
		if (count < DESKTOP_BALANCER_THREADS_PER_PROCESS)
			count++;
		for (last = count - 1U; last > position; last--)
			balancer_threads[last] = balancer_threads[last - 1U];
		balancer_threads[position] = thread;
	}

	/* The next look goes on after the highest taken, or starts over when the end was reached. */
	process->desktop_balancer.thread_cursor = 0;
	if (count == DESKTOP_BALANCER_THREADS_PER_PROCESS)
		process->desktop_balancer.thread_cursor = balancer_threads[count - 1U]->tid;

	/* Reports how many were taken. */
	return count;
}

/*
 * Decides one thread: updates its marks, gives the scheduler the hint of
 * the rule it falls under, and asks for a move when it sits on a CPU of
 * the wrong kind.  Gives up the thread's reference, or keeps it for the
 * end of the look when the thread is a terminal's.
 */
static void
balancer_look_thread(
	struct thread *thread,
	const struct desktop_balancer_traits *process_traits,
	struct balancer_look *look)
{
	struct desktop_balancer_history *history;
	struct desktop_balancer_traits traits;
	struct desktop_balancer_sample sample;
	struct sched_hint_activity activity;
	struct sched_hint hint;
	unsigned rule;
	int wanted;
	int error;

	/* What the thread did since the last look. */
	history = &thread->desktop_balancer;
	sched_hint_activity(thread, &activity);
	sample.run_ticks = activity.run_ticks;
	sample.elapsed = look->elapsed;
	sample.user_streak = activity.user_streak;
	desktop_balancer_observe(history, &sample, &balancer_limits);

	/* The rule it falls under and the hint that rule gives. */
	traits = *process_traits;
	traits.busy = history->busy;
	traits.heavy = history->heavy;
	rule = desktop_balancer_classify(&traits, &hint);
	history->rule = (uint8_t)rule;
	look->rule_threads[rule]++;

	/*
	 * Gives the scheduler a hint it does not hold yet.  hinted says the
	 * scheduler holds the hint kept here; a refused hint (a thread not
	 * started or leaving) is offered again at the next look.
	 */
	if (!history->hinted ||
	    history->hint.core != hint.core ||
	    history->hint.strength != hint.strength ||
	    history->hint.flags != hint.flags ||
	    history->hint.priority != hint.priority) {
		error = sched_hint_set(thread, &hint);
		history->hinted = 0;
		if (error == 0) {
			history->hint = hint;
			history->hinted = 1;
		}
	}

	/* A thread whose hint the scheduler does not hold, or that sits where it should, stays. */
	wanted = 0;
	if (history->hinted)
		wanted = balancer_wants_move(history, &activity, look->now);
	if (!wanted) {
		thread_release(thread);
		return;
	}

	/* A terminal's thread waits for the moves the others leave. */
	if (rule == DESKTOP_BALANCER_RULE_TERMINAL) {
		if (look->deferred_count < DESKTOP_BALANCER_MOVES_PER_PASS) {
			look->deferred[look->deferred_count] = thread;
			look->deferred_count++;
			return;
		}

		/* No room to keep it: it is asked again at a later look. */
		thread_release(thread);
		return;
	}

	/* Asks for the move now. */
	balancer_place(thread, activity.running, look);
	thread_release(thread);
}

/*
 * Tests whether a thread should be asked to move: the CPUs are of two
 * kinds, its hint asks for moves to a kind of core, it sits on a CPU of
 * the other kind, and it was not moved too recently.
 */
static int
balancer_wants_move(
	const struct desktop_balancer_history *history,
	const struct sched_hint_activity *activity,
	uint64_t now)
{
	int due;

	/* Only a table of two kinds gives anywhere to move to. */
	if (!balancer_hybrid)
		return 0;

	/* Only a hint that asks for moves, to a kind of core. */
	if (history->hint.strength != SCHED_HINT_PLACE && history->hint.strength != SCHED_HINT_PLACE_IDLE)
		return 0;
	if (history->hint.core == SCHED_HINT_CORE_ANY)
		return 0;

	/* A thread already on a CPU of its kind stays. */
	if (activity->cpu >= balancer_cpu_count)
		return 0;
	if (balancer_classes[activity->cpu].core == history->hint.core)
		return 0;

	/* Not again before the interval, so that a thread is not tossed between kinds. */
	due = desktop_balancer_move_due(now, history->moved_tick,
	    kern_ms_to_ticks(DESKTOP_BALANCER_MOVE_INTERVAL_MS));
	if (!due)
		return 0;

	/* The thread should move. */
	return 1;
}

/*
 * Asks the scheduler to move a thread to a CPU of its kind, if the look
 * has a move left, and a handoff for a running thread.
 */
static void
balancer_place(
	struct thread *thread,
	int running,
	struct balancer_look *look)
{
	int error;

	/* The look has spent its moves. */
	if (look->moves_left == 0U)
		return;
	if (running && look->handoffs_left == 0U)
		return;

	/* Asks for the move; a refusal is asked again at a later look. */
	error = sched_hint_place(thread);
	if (error == EAGAIN)
		look->places_refused++;
	if (error != 0)
		return;

	/* The move was taken: it counts against the look and starts the thread's interval. */
	look->moves_left--;
	if (running)
		look->handoffs_left--;
	look->places++;
	thread->desktop_balancer.moved_tick = look->now;
}

/* Asks for the terminals' threads' moves with what the look has left, and gives up their references. */
static void
balancer_place_deferred(
	struct balancer_look *look)
{
	struct sched_hint_activity activity;
	struct thread *thread;
	unsigned index;

	/* Each kept thread, looked at again for whether it runs now. */
	for (index = 0; index < look->deferred_count; index++) {
		thread = look->deferred[index];
		sched_hint_activity(thread, &activity);
		balancer_place(thread, activity.running, look);
		thread_release(thread);
		look->deferred[index] = NULL;
	}

	/* Nothing is kept for the end of the look any more. */
	look->deferred_count = 0;
}

/* Publishes what one look saw and did in the report. */
static void
balancer_publish(
	const struct balancer_look *look,
	uint64_t pass_ns)
{
	unsigned long irq;
	unsigned count;
	unsigned index;

	/* The CPUs the report has room for. */
	count = balancer_cpu_count;
	if (count > KERN_DESKTOP_BALANCER_CPU_MAX)
		count = KERN_DESKTOP_BALANCER_CPU_MAX;

	/* Writes the look's numbers and the CPUs into the report. */
	irq = spin_lock_irqsave(&balancer_report_lock);

	balancer_report.flags = 0;
	if (balancer_hybrid)
		balancer_report.flags |= KERN_DESKTOP_BALANCER_HYBRID;
	balancer_report.state = look->state;
	balancer_report.passes++;
	balancer_report.pass_ns_total += pass_ns;
	if (pass_ns > balancer_report.pass_ns_max)
		balancer_report.pass_ns_max = pass_ns;
	for (index = 0; index < DESKTOP_BALANCER_RULES; index++)
		balancer_report.rule_threads[index] = look->rule_threads[index];
	balancer_report.places += look->places;
	balancer_report.places_refused += look->places_refused;
	balancer_report.cpu_count = count;
	for (index = 0; index < count; index++) {
		balancer_report.cpus[index].core = balancer_classes[index].core;
		balancer_report.cpus[index].usable = balancer_classes[index].usable;
		balancer_report.cpus[index].rank = balancer_classes[index].rank;
		balancer_report.cpus[index].sibling_rank = balancer_classes[index].sibling_rank;
		balancer_report.cpus[index].core_group = balancer_classes[index].core_group;
	}

	spin_unlock_irqrestore(&balancer_report_lock, irq);
}

/* Reports a monotonic time in nanoseconds, as finely as the hardware counts. */
static uint64_t
balancer_time_ns(
	void)
{
	uint64_t counter;
	uint64_t frequency;
	uint64_t seconds;
	uint64_t rest;
	int counted;

	/* The hardware's fixed-rate counter; without one, the scheduler's ticks. */
	counted = hal_rtc_read_counter(&counter, &frequency);
	if (!counted || frequency == 0U)
		return kern_ticks_to_ns(clock_ticks());

	/* Whole seconds and the rest apart, so that the product does not overflow. */
	seconds = counter / frequency;
	rest = counter % frequency;
	return seconds * BALANCER_NS_PER_SECOND + rest * BALANCER_NS_PER_SECOND / frequency;
}
