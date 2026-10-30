/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The per-CPU run queue scheduler.
 *
 * Each CPU owns priority-ordered run queues, a sleep queue for timed
 * sleeps, an idle thread, and a lock.  Threads are pinned to a CPU and
 * migrate only through sched_set_cpu().  A thread sleeps by publishing
 * its sleeping state under the CPU lock and switching away without
 * enqueueing itself; wakeups and interrupts move it back to a run
 * queue.  The clock tick expires timed sleeps, accounts CPU time and
 * interval timers, and preempts an exhausted quantum.
 */

#include "kern/sleep.h"
#include "kern/sched.h"
#include "kern/sched_private.h"
#include "kern/atomic.h"
#include "kern/thread.h"
#include "kern/process.h"
#include "kern/resource-limit.h"
#include "kern/signal.h"
#include "kern/lock.h"
#include "kern/kmem.h"
#include "kern/klog.h"
#include "kern/clock.h"
#include "kern/vm-lock.h"
#include <kern/boot.h>
#include <kern/kcrt.h>
#include <kern/platform.h>

#include <uapi/errno.h>
#include <hal/hal.h>

#if SCHED_WAKE_LATENCY
#include "kern/clock.h"
#include "kern/klog.h"

/* Diagnostic: wake-to-run latency buckets (<0.1 ms, <1 ms, <5 ms, <12 ms, more), same CPU and cross CPU. */
static struct {
	uint64_t count[2][5];
	uint64_t total_ns[2];
	uint64_t max_ns[2];
	/* Wakeups whose CPU was running another thread, and of those, the ones made while some other CPU was idle. */
	uint64_t busy_home;
	uint64_t busy_home_idle_other;
} wake_latency;

static uint64_t
wake_latency_now(void)
{
	uint64_t counter;
	uint64_t frequency;

	if (!kern_rtc_read_counter(&counter, &frequency) || frequency == 0U)
		return 0U;
	return (counter / frequency) * 1000000000ULL + ((counter % frequency) * 1000000000ULL) / frequency;
}

static void
wake_latency_account(struct thread *next)
{
	uint64_t now;
	uint64_t latency;
	unsigned bucket;
	unsigned kind;

	if (next->sched.woken_at == 0U)
		return;
	now = wake_latency_now();
	latency = now - next->sched.woken_at;
	next->sched.woken_at = 0U;
	kind = next->sched.woken_same_cpu ? 0U : 1U;
	if (latency < 100000ULL)
		bucket = 0U;
	else if (latency < 1000000ULL)
		bucket = 1U;
	else if (latency < 5000000ULL)
		bucket = 2U;
	else if (latency < 12000000ULL)
		bucket = 3U;
	else
		bucket = 4U;
	wake_latency.count[kind][bucket]++;
	wake_latency.total_ns[kind] += latency;
	if (latency > wake_latency.max_ns[kind])
		wake_latency.max_ns[kind] = latency;
}

void
sched_wake_latency_report(void)
{
	unsigned kind;
	uint64_t n;

	for (kind = 0U; kind < 2U; kind++) {
		n = wake_latency.count[kind][0] + wake_latency.count[kind][1] + wake_latency.count[kind][2] +
		    wake_latency.count[kind][3] + wake_latency.count[kind][4];
		kern_logf("sched: wake latency %s: n=%llu avg=%llu us max=%llu us | <0.1ms %llu <1ms %llu <5ms %llu <12ms %llu more %llu\n",
		    kind == 0U ? "same-cpu" : "cross-cpu",
		    (unsigned long long)n,
		    (unsigned long long)(n ? wake_latency.total_ns[kind] / n / 1000U : 0U),
		    (unsigned long long)(wake_latency.max_ns[kind] / 1000U),
		    (unsigned long long)wake_latency.count[kind][0],
		    (unsigned long long)wake_latency.count[kind][1],
		    (unsigned long long)wake_latency.count[kind][2],
		    (unsigned long long)wake_latency.count[kind][3],
		    (unsigned long long)wake_latency.count[kind][4]);
	}
	kern_logf("sched: wake onto a busy CPU %llu, while another CPU was idle %llu\n",
	    (unsigned long long)wake_latency.busy_home,
	    (unsigned long long)wake_latency.busy_home_idle_other);
	kern_memset(&wake_latency, 0, sizeof(wake_latency));
}
#endif

#define SCHED_MIGRATING 0x00000001U
#define SCHED_WAKE_PENDING 0x00000002U
#define SCHED_ONLINE_TIMEOUT 10000000U

/* What a CPU's tick is charged to (struct sched_cpu's times, hw.cputimes). */
#define SCHED_TIME_USER		0U
#define SCHED_TIME_SYSTEM	1U
#define SCHED_TIME_IDLE		2U
#define SCHED_TIME_OTHER	3U
#define SCHED_TIMES		4U

/*
 * The latency diagnostic (ws213-p007): the boot token that turns it on,
 * how often it reports, and why a thread's wait to run is being measured
 * (struct sched's perf_runnable_kind).  A preempted thread is one a wakeup
 * took the CPU from (sched_preempt()); a quantum that ran out is not
 * measured.
 */
#define SCHED_PERF_TOKEN		"kern.perf=1"
#define SCHED_PERF_COMMAND_LINE		"boot.command-line"
#define SCHED_PERF_REPORT_MS		2000U
#define SCHED_PERF_NONE			0U
#define SCHED_PERF_WOKEN		1U
#define SCHED_PERF_PREEMPTED		2U

/*
 * The longest wait to run of one kind in a window of the latency
 * diagnostic.
 *
 * It holds how long the thread waited in counter counts, the uptime in
 * milliseconds when it ran, which thread it was, and how many other
 * threads its CPU switched to first (-1 when another CPU took it).  An
 * empty one has zero counts.
 */
struct sched_perf_longest {
	uint64_t counts;
	uint64_t end_ms;
	struct sched_perf_thread who;
	int ahead;
};

/*
 * One CPU's part of a window of the latency diagnostic.
 *
 * It counts the switches to another thread, the woken and the preempted
 * threads that ran, and the switches that left a woken thread of at least
 * the new thread's priority waiting after the mark asking for its turn was
 * cleared; and keeps the longest wait of each kind.  The CPU lock guards
 * it, and the report empties it.
 */
struct sched_perf_window {
	uint64_t switches;
	uint64_t woken;
	uint64_t preempted;
	uint64_t dropped;
	struct sched_perf_longest woken_longest;
	struct sched_perf_longest preempted_longest;
};

struct sched_cpu {
	struct spinlock lock;
	struct sched_queue run[SCHED_PRIOR_LEVELS];
	/*
	 * Threads woken to run before the thread they found running, in the
	 * order they woke.  Each level is served before the run queue of the
	 * same priority, so a preempted thread put back at the front of its
	 * run queue waits behind every thread woken to preempt it.
	 */
	struct sched_queue woken[SCHED_PRIOR_LEVELS];
	struct sched_queue sleep;
	struct thread *idle;
	struct thread *retired;
	struct thread *current;   /* the thread running on this CPU */
	unsigned need_resched;
	unsigned preempt;         /* a wakeup asked to run before the current thread */
	unsigned yield_due;       /* a quantum ended while preemption was held off */
	unsigned online;
	unsigned preempt_count;   /* kern_preempt_disable() nesting depth */

	/*
	 * The CPU's ticks by what it was doing (SCHED_TIME_*, hw.cputimes).
	 * Only the CPU's own tick adds to them; readers on other CPUs load
	 * them atomically (a u64 is not one access on i386).
	 */
	volatile uint64_t times[SCHED_TIMES];

	/*
	 * The latency diagnostic's window, and the CPU's switches to another
	 * thread since boot, which a waiting thread's stamp is compared with
	 * to tell how many ran before it.  Both only with the diagnostic on.
	 */
	struct sched_perf_window perf;
	uint64_t perf_switch_count;
};

static struct sched_cpu *scheduler_cpus;
static unsigned scheduler_cpu_count;
static struct hal_cpu_mask scheduler_online_mask;
static volatile uint64_t scheduler_ticks;
static volatile unsigned scheduler_round_robin;

/*
 * The CPUs halted in their idle loop with nothing queued.
 *
 * A CPU sets its bit before it halts and clears it when it leaves the idle
 * thread.  A CPU whose tick finds more runnable threads than it can run
 * notifies one of these so that it steals, and a new thread goes to one
 * when its creator's CPU already has work queued.  Read without a lock:
 * a stale bit costs one notification that finds nothing to take.
 */
static struct hal_cpu_mask scheduler_idle_mask;

/*
 * Whether the latency diagnostic is on (kern.perf=1), set once by
 * sched_init() before another thread runs and read without a lock.
 */
int sched_perf_active;

/*
 * The frequency of the counter the latency diagnostic measures with, read
 * once when it is turned on.  Zero keeps the diagnostic off.
 */
static uint64_t sched_perf_frequency;

/*
 * The global tick of the last report of the latency diagnostic.  The
 * thread that moves it forward with a compare-and-swap writes the report,
 * so two CPUs never write the same window.
 */
static volatile uint64_t sched_perf_report_tick;

/*
 * The counter at the last report, which measures the window's length.
 * Only the thread that won the report writes it.
 */
static uint64_t sched_perf_report_counter;

/*
 * How long the last report took to write, in counter units, from gathering
 * the windows to the end of its lines; the next report prints it.  The
 * report is written synchronously by whichever thread is returning to user
 * mode, so this is the time that thread lost to the diagnostic, including
 * any time it was preempted meanwhile.  Zero before the first report.
 */
static volatile uint64_t sched_perf_report_cost;

static void send_itimer_signal(struct process *process, int signo);
static struct sched_cpu *sched_cpu_state(hal_cpu_id_t cpu);
static void queue_append(struct sched_queue *queue, struct thread *thread, unsigned kind);
static void queue_remove(struct sched_queue *queue, struct thread *thread) __attribute__((noinline));
static void queue_report_underflow(const struct sched_queue *queue, const struct thread *thread, void *caller);
static void refuse_idle_sleep(const struct thread *thread, const char *entry, void *caller);
static void queue_remove_thread(struct sched_cpu *cpu, struct thread *thread);
static struct thread *pick_next_locked(struct sched_cpu *cpu);
static void complete_retired(struct sched_cpu *cpu);
static int cpu_online(hal_cpu_id_t cpu);
static hal_cpu_id_t choose_cpu(void);
static void notify_cpu(hal_cpu_id_t cpu);
static void switch_without_enqueue(void);
static void queue_prepend(struct sched_queue *queue, struct thread *thread, unsigned kind);
static void wake_enqueue_locked(struct sched_cpu *cpu, struct thread *thread);
static int best_priority_locked(struct sched_cpu *cpu);
static void sched_preempt(void);
static int cpu_has_backlog(const struct sched_cpu *cpu);
static hal_cpu_id_t choose_idle_cpu(hal_cpu_id_t except);
static void idle_mask_set(hal_cpu_id_t cpu);
static void idle_mask_clear(hal_cpu_id_t cpu);
static void leave_idle(struct sched_cpu *cpu, const struct thread *current, hal_cpu_id_t id);
static struct thread *take_stealable_locked(struct sched_cpu *victim);
static int steal_runnable(hal_cpu_id_t self);
static void retarget_migrating(struct thread *thread, struct sched_cpu *old_cpu, hal_cpu_id_t target);
static void charge_cpu_time(struct sched_cpu *cpu, const struct thread *thread);
static void perf_init(void);
static void perf_mark_runnable_locked(struct sched_cpu *cpu, struct thread *thread, unsigned kind);
static void perf_switch_locked(struct sched_cpu *cpu, struct thread *current, struct thread *next);
static void perf_note_longest(struct sched_perf_longest *longest, uint64_t counts, const struct thread *thread, int ahead);
static void perf_merge_longest(struct sched_perf_longest *total, const struct sched_perf_longest *longest);
static void perf_report_if_due(void);
static void perf_report(void);

/*
 * Marks the current user thread as executing in the kernel for accounting.
 */
void
sched_accounting_kernel_enter(
	void)
{
	struct thread *thread;

	/* Only user threads are accounted. */
	thread = curthread;
	if (thread == NULL ||
	    (thread->flags & THREAD_FLAG_IDLE) != 0 ||
	    thread->proc == NULL ||
	    thread->proc == &process0)
		return;

	/* Nests the kernel entries. */
	if (thread->accounting_kernel_depth == (unsigned)-1)
		HAL_FATAL("kernel accounting depth overflow");
	thread->accounting_kernel_depth++;
}

/*
 * Marks the current user thread as leaving the kernel for accounting.
 */
void
sched_accounting_kernel_leave(
	void)
{
	struct thread *thread;

	/* Only user threads are accounted. */
	thread = curthread;
	if (thread == NULL ||
	    (thread->flags & THREAD_FLAG_IDLE) != 0 ||
	    thread->proc == NULL ||
	    thread->proc == &process0)
		return;

	/* Unnests the kernel entries. */
	if (thread->accounting_kernel_depth == 0)
		HAL_FATAL("kernel accounting depth underflow");
	thread->accounting_kernel_depth--;
}

/*
 * Initializes the scheduler on the boot CPU with thread0 as its idle thread.
 */
void
sched_init(
	void)
{
	unsigned count;
	unsigned cpu;

	/* The scheduler starts on the boot thread. */
	if (curthread == NULL || curthread != &thread0)
		HAL_FATAL("scheduler before process0");

	/*
	 * Allocates one state per CPU.  The count is published last: a timer
	 * tick can already arrive here (sched_clock_cpu), and it must see
	 * either no CPUs or every CPU's state.
	 */
	count = hal_cpu_count();
	if (count == 0 || count > HAL_CPU_MAX)
		HAL_FATAL("invalid scheduler CPU count");
	scheduler_cpus = kern_calloc(count, sizeof(*scheduler_cpus));
	if (scheduler_cpus == NULL)
		HAL_FATAL("scheduler allocation failed");
	for (cpu = 0; cpu < count; cpu++)
		spin_init(&scheduler_cpus[cpu].lock, LOCK_RANK_SCHEDULER,
		    "scheduler CPU");
	atomic_raw_store_release(&scheduler_cpu_count, count);

	/* Brings the boot CPU online with thread0 idling on it. */
	hal_cpu_mask_zero(&scheduler_online_mask);
	scheduler_cpus[0].idle = &thread0;
	scheduler_cpus[0].current = &thread0;
	atomic_raw_store_release(&scheduler_cpus[0].online, 1U);
	hal_cpu_mask_set(&scheduler_online_mask, 0);
	thread0.state = THREAD_RUNNING;
	thread0.sched.cpu = 0;
	thread0.sched.last_cpu = 0;
	thread0.sched.quantum = SCHED_QUANTUM_TICKS;
	atomic_u64_store_release(&scheduler_ticks, 0);

	/* Turns the latency diagnostic on when the boot line asks for it. */
	perf_init();
}

/*
 * Assigns a new thread to a CPU and moves its task there.
 */
int
sched_prepare_thread(
	struct thread *thread)
{
	hal_cpu_id_t cpu;
	int error;
	int usable;

	/* Only a new thread with a task can be prepared. */
	if (thread == NULL ||
	    thread->task == NULL ||
	    thread->state != THREAD_NEW)
		return EINVAL;

	/*
	 * Places the thread where its creator runs when nothing waits there:
	 * a child its parent waits for then runs on a warm cache without an
	 * interrupt to another CPU.  With work already queued there an idle
	 * CPU takes it, and failing that the round-robin choice does.
	 */
	cpu = hal_cpu_current();
	usable = 0;
	if (cpu < scheduler_cpu_count)
		usable = cpu_online(cpu);
	if (usable)
		usable = !cpu_has_backlog(sched_cpu_state(cpu));
	if (!usable)
		cpu = choose_idle_cpu(HAL_CPU_MAX);
	if (cpu == HAL_CPU_MAX)
		cpu = choose_cpu();
	if (cpu == HAL_CPU_MAX)
		return EAGAIN;
	error = hal_task_transfer(thread->task, cpu);
	if (error != HAL_OK) {
		if (error == HAL_ERR_NOMEM)
			return ENOMEM;
		return EBUSY;
	}

	thread->sched.cpu = cpu;
	thread->sched.last_cpu = cpu;

	/* Reports the prepared thread. */
	return 0;
}

/*
 * Makes a new or sleeping thread runnable on its CPU.
 */
void
sched_add(
	struct thread *thread)
{
	struct sched_cpu *cpu;
	unsigned long irq;

	/* The thread must be unqueued with a valid CPU and priority. */
	if (thread == NULL ||
	    thread->sched.cpu >= scheduler_cpu_count ||
	    thread->sched.queue_kind != SCHED_QUEUE_NONE ||
	    thread->sched.priority < SCHED_PRIOR_HIGH ||
	    thread->sched.priority > SCHED_PRIOR_LOW ||
	    (thread->state != THREAD_NEW && thread->state != THREAD_SLEEPING))
		HAL_FATAL("invalid sched_add");

	/* Queues the thread with a fresh quantum and pokes its CPU. */
	cpu = sched_cpu_state(thread->sched.cpu);
	irq = spin_lock_irqsave(&cpu->lock);

	thread->state = THREAD_RUNNABLE;
	thread->sched.quantum = SCHED_QUANTUM_TICKS;
	queue_append(&cpu->run[thread->sched.priority], thread,
	    SCHED_QUEUE_RUN);
	cpu->need_resched = 1;

	/* A new or added thread's first run is not a measured wait. */
	perf_mark_runnable_locked(cpu, thread, SCHED_PERF_NONE);

	spin_unlock_irqrestore(&cpu->lock, irq);

	notify_cpu(thread->sched.cpu);
}

/*
 * Removes a thread from whatever queue of its CPU it is on.
 */
void
sched_unlink(
	struct thread *thread)
{
	struct sched_cpu *cpu;
	unsigned long irq;
	hal_cpu_id_t id;

	/* Ignores a missing thread. */
	if (thread == NULL)
		return;

	/* Locks the CPU the thread is on, re-reading it under the lock. */
	for (;;) {
		id = (hal_cpu_id_t)atomic_raw_load_acquire(
		    (volatile unsigned *)&thread->sched.cpu);
		if (id >= scheduler_cpu_count)
			return;
		cpu = sched_cpu_state(id);
		irq = spin_lock_irqsave(&cpu->lock);
		if (id == thread->sched.cpu)
			break;
		spin_unlock_irqrestore(&cpu->lock, irq);
	}

	queue_remove_thread(cpu, thread);

	spin_unlock_irqrestore(&cpu->lock, irq);
}

/*
 * Wakes a sleeping thread.
 *
 * A thread in the middle of a migration remembers the wakeup for its
 * arrival; a thread that is not sleeping is left alone.
 */
void
sched_wakeup(
	struct thread *thread)
{
	struct sched_cpu *cpu;
	unsigned long irq;
	hal_cpu_id_t id;

	/* Ignores a missing thread. */
	if (thread == NULL)
		return;

	/* Locks the CPU the thread is on, re-reading it under the lock. */
	for (;;) {
		id = (hal_cpu_id_t)atomic_raw_load_acquire(
		    (volatile unsigned *)&thread->sched.cpu);
		if (id >= scheduler_cpu_count)
			return;
		cpu = sched_cpu_state(id);
		irq = spin_lock_irqsave(&cpu->lock);
		if (id == thread->sched.cpu)
			break;
		spin_unlock_irqrestore(&cpu->lock, irq);
	}

	/* A migrating thread takes the wakeup with it. */
	if ((thread->sched.need_migrate & SCHED_MIGRATING) != 0) {
		thread->sched.need_migrate |= SCHED_WAKE_PENDING;
		spin_unlock_irqrestore(&cpu->lock, irq);
		return;
	}

	/* Only a sleeping thread is woken. */
	if (thread->state != THREAD_SLEEPING) {
		spin_unlock_irqrestore(&cpu->lock, irq);
		return;
	}

	/* Moves the thread to its run queue and pokes the CPU. */
	queue_remove_thread(cpu, thread);
	thread->state = THREAD_RUNNABLE;
	thread->sched.wakeup_tick = 0;
#if SCHED_WAKE_LATENCY
	thread->sched.woken_at = wake_latency_now();
	thread->sched.woken_same_cpu = (id == hal_cpu_current());
	if (cpu->current != NULL && (cpu->current->flags & THREAD_FLAG_IDLE) == 0) {
		wake_latency.busy_home++;
		if (choose_idle_cpu(id) != HAL_CPU_MAX)
			wake_latency.busy_home_idle_other++;
	}
#endif
	wake_enqueue_locked(cpu, thread);

	spin_unlock_irqrestore(&cpu->lock, irq);

	notify_cpu(id);
}

/*
 * Wakes a thread from a sleep; the same as sched_wakeup().
 */
void
sched_awake_from_sleep(
	struct thread *thread)
{
	sched_wakeup(thread);
}

/*
 * Interrupts a thread so that it notices a signal or request soon.
 *
 * A sleeping thread is woken; a running one is notified so that it
 * reaches a return-to-user safe point.
 */
void
sched_interrupt(
	struct thread *thread)
{
	hal_cpu_id_t cpu;

	/* Ignores a missing thread. */
	if (thread == NULL)
		return;

	/*
	 * Publishes the interrupt before inspecting scheduler state.  A
	 * target which is still RUNNING can subsequently register a wait;
	 * the generation check in sched_sleep_locked_interruptible() closes
	 * that otherwise lost wakeup.
	 */
	(void)atomic_u64_fetch_add_release(&thread->interrupt_generation, 1U);

	/*
	 * sched_wakeup() is deliberately a no-op for RUNNING/RUNNABLE tasks.
	 * The explicit CPU notification below is what gives those tasks a
	 * prompt return-to-user safe point.  A duplicate notification after
	 * a real wake is harmless and keeps the migration race contained in
	 * the scheduler.
	 */
	sched_wakeup(thread);
	cpu = (hal_cpu_id_t)atomic_raw_load_acquire(
	    (volatile unsigned *)&thread->sched.cpu);
	if (cpu < scheduler_cpu_count && cpu_online(cpu))
		notify_cpu(cpu);
}

/*
 * Switches to the next runnable thread without requeueing the current one.
 */
void
sched_switch(
	void)
{
	switch_without_enqueue();
}

/*
 * Gives up the CPU to the next runnable thread, requeueing the current one.
 */
void
kern_preempt_disable(void)
{
	struct sched_cpu *cpu;
	bool enabled;

	/* Names the CPU with interrupts off, so the caller cannot move. */
	enabled = hal_irq_disable();
	cpu = sched_cpu_state(hal_cpu_current());
	if (cpu != NULL) {
		spin_lock(&cpu->lock);
		cpu->preempt_count++;
		spin_unlock(&cpu->lock);
	}

	/* Restores the caller's interrupt state. */
	if (enabled)
		hal_irq_enable();
}

void
kern_preempt_enable(void)
{
	struct sched_cpu *cpu;
	unsigned long irq;
	int yield = 0;
	bool enabled;

	/* Names the CPU with interrupts off, so the caller cannot move. */
	enabled = hal_irq_disable();
	cpu = sched_cpu_state(hal_cpu_current());
	if (cpu == NULL) {
		if (enabled)
			hal_irq_enable();
		return;
	}

	/* Leaves one level, noting a switch that waited for the last one. */
	irq = spin_lock_irqsave(&cpu->lock);
	if (cpu->preempt_count > 0u)
		cpu->preempt_count--;
	if (cpu->preempt_count == 0u && cpu->yield_due != 0u) {
		cpu->yield_due = 0;
		yield = 1;
	} else if (cpu->preempt_count == 0u && cpu->preempt != 0u) {
		yield = 2;
	}
	spin_unlock_irqrestore(&cpu->lock, irq);
	if (enabled)
		hal_irq_enable();

	/* A quantum that ended goes to the back; a wakeup that asked goes first. */
	if (yield == 1)
		sched_yield();
	else if (yield == 2)
		sched_preempt();
}

/*
 * Reports the number of the CPU the caller runs on.
 */
unsigned
kern_cpu_current(void)
{
	/* The kernel numbers CPUs as the platform does. */
	return (unsigned)hal_cpu_current();
}

/* Test-only: read the current CPU's preempt nesting depth. */
unsigned
sched_test_preempt_count(void)
{
	struct sched_cpu *cpu;
	unsigned n;
	bool enabled;

	/* Names the CPU with interrupts off, so the caller cannot move. */
	enabled = hal_irq_disable();
	n = 0u;
	cpu = sched_cpu_state(hal_cpu_current());
	if (cpu != NULL) {
		spin_lock(&cpu->lock);
		n = cpu->preempt_count;
		spin_unlock(&cpu->lock);
	}

	/* Restores the caller's interrupt state. */
	if (enabled)
		hal_irq_enable();
	return n;
}

void
sched_yield(
	void)
{
	struct thread *current;
	struct thread *next;
	struct sched_cpu *cpu;
	hal_cpu_id_t id;
	unsigned long ignored;
	bool enabled;

	/* Enters this CPU's run queue with interrupts disabled. */
	enabled = hal_irq_disable();
	current = curthread;
	id = hal_cpu_current();
	cpu = sched_cpu_state(id);
	ignored = spin_lock_irqsave(&cpu->lock);

	(void)ignored;

	/* An idle thread giving up the CPU takes the CPU off the idle mask. */
	leave_idle(cpu, current, id);

	/* Requeues a running non-idle thread behind its peers. */
	if (current != NULL &&
	    current->state == THREAD_RUNNING &&
	    (current->flags & THREAD_FLAG_IDLE) == 0) {
		if (current->sched.cpu != id)
			HAL_FATAL("running thread on wrong scheduler CPU");
		current->state = THREAD_RUNNABLE;
		current->sched.quantum = SCHED_QUANTUM_TICKS;
		queue_append(&cpu->run[current->sched.priority], current,
		    SCHED_QUEUE_RUN);
	}

	/* Picks the next thread, falling back to the current one or idle. */
	next = pick_next_locked(cpu);
	if (next == NULL) {
		if (current != NULL && current->state == THREAD_RUNNING)
			next = current;
		else
			next = cpu->idle;
	}

	if (next == NULL)
		HAL_FATAL("yield without idle thread");
	next->state = THREAD_RUNNING;
#if SCHED_WAKE_LATENCY
	wake_latency_account(next);
#endif
	perf_switch_locked(cpu, current, next);
	next->sched.last_cpu = id;
	if (next->sched.quantum == 0)
		next->sched.quantum = SCHED_QUANTUM_TICKS;
	cpu->current = next;
	cpu->need_resched = 0;
	cpu->preempt = 0;
	spin_unlock(&cpu->lock);

	/* Switches, then retires the thread that exited before us. */
	if (next != current)
		hal_task_context_switch(next->task);

	/*
	 * Resumed, perhaps on another CPU which stole this thread: the CPU
	 * state is taken afresh, and it is that CPU's retired thread which is
	 * completed.
	 */
	complete_retired(sched_cpu_state(hal_cpu_current()));
	if (enabled)
		hal_irq_enable();
}

/*
 * Switches to the thread a wakeup put at the front of the run queue.
 *
 * The running thread goes back to the front of its own run queue with
 * the rest of its quantum, behind the woken queue of its priority, so a
 * thread that keeps the CPU busy loses only the time the woken ones run
 * and not its place among its peers.  If nothing of at least its priority
 * is waiting, nothing changes.  The idle thread, or a thread no longer running, takes the
 * ordinary yield.
 */
static void
sched_preempt(
	void)
{
	struct thread *current;
	struct thread *next;
	struct sched_cpu *cpu;
	hal_cpu_id_t id;
	unsigned long ignored;
	int priority;
	bool enabled;

	/* Enters this CPU's run queue with interrupts disabled. */
	enabled = hal_irq_disable();
	current = curthread;
	id = hal_cpu_current();
	cpu = sched_cpu_state(id);
	ignored = spin_lock_irqsave(&cpu->lock);

	(void)ignored;
	cpu->preempt = 0;

	/* The idle thread and a thread on its way to sleep yield as usual. */
	if (current == NULL ||
	    current->state != THREAD_RUNNING ||
	    (current->flags & THREAD_FLAG_IDLE) != 0) {
		spin_unlock(&cpu->lock);
		if (enabled)
			hal_irq_enable();
		sched_yield();
		return;
	}
	if (current->sched.cpu != id)
		HAL_FATAL("running thread on wrong scheduler CPU");

	/* Keeps the CPU when nothing of at least its priority is waiting. */
	next = NULL;
	priority = best_priority_locked(cpu);
	if (priority >= 0 && priority <= current->sched.priority)
		next = pick_next_locked(cpu);
	if (next == NULL) {
		spin_unlock(&cpu->lock);
		if (enabled)
			hal_irq_enable();
		return;
	}

	/* Puts the running thread back at the front with the rest of its quantum. */
	current->state = THREAD_RUNNABLE;
	queue_prepend(&cpu->run[current->sched.priority], current,
	    SCHED_QUEUE_RUN);
	perf_mark_runnable_locked(cpu, current, SCHED_PERF_PREEMPTED);

	next->state = THREAD_RUNNING;
#if SCHED_WAKE_LATENCY
	wake_latency_account(next);
#endif
	perf_switch_locked(cpu, current, next);
	next->sched.last_cpu = id;
	if (next->sched.quantum == 0)
		next->sched.quantum = SCHED_QUANTUM_TICKS;
	cpu->current = next;
	cpu->need_resched = 0;
	spin_unlock(&cpu->lock);

	/* Switches, then retires the thread that exited before us. */
	hal_task_context_switch(next->task);

	/* Resumed, perhaps on another CPU: completes that CPU's retired thread. */
	complete_retired(sched_cpu_state(hal_cpu_current()));
	if (enabled)
		hal_irq_enable();
}

/*
 * Acts on a wakeup that asked to run before the current thread.
 *
 * The mark is read without the lock: a mark set just after the read is
 * acted on at the next point or tick, and one read as set is checked
 * again under the lock.
 */
void
sched_preempt_point(
	void)
{
	struct sched_cpu *cpu;

	/*
	 * Writes the latency diagnostic's report when one is due.  The way
	 * back to user mode is where a busy CPU's threads come with nothing
	 * held and may sleep.
	 */
	if (sched_perf_active)
		perf_report_if_due();

	cpu = sched_cpu_state(hal_cpu_current());
	if (cpu == NULL ||
	    atomic_raw_load_acquire((volatile unsigned *)&cpu->preempt) == 0U ||
	    cpu->preempt_count != 0u)
		return;
	sched_preempt();
}

/*
 * Retires the current thread and switches away for good.
 *
 * The thread becomes the CPU's retirement candidate, which the next
 * thread to run completes.
 */
void
sched_exit_current(
	void)
{
	struct thread *current;
	struct thread *next;
	struct sched_cpu *cpu;
	hal_cpu_id_t id;
	unsigned long ignored;

	/* Names the CPU with interrupts off, so the caller cannot move. */
	(void)hal_irq_disable();
	current = curthread;
	id = hal_cpu_current();
	cpu = sched_cpu_state(id);

	/* Only a non-idle thread on its own CPU can exit. */
	if (current == NULL ||
	    (current->flags & THREAD_FLAG_IDLE) != 0 ||
	    current->sched.cpu != id)
		HAL_FATAL("invalid scheduler exit");

	/*
	 * A newly started task does not return through the previous task's
	 * hal_task_context_switch() call.  Retire that predecessor before
	 * this task is allowed to become the next retirement candidate.
	 */
	complete_retired(cpu);

	/* Marks the thread exiting and picks its successor. */
	ignored = spin_lock_irqsave(&cpu->lock);

	(void)ignored;
	if (current->state != THREAD_RUNNING || cpu->retired != NULL)
		HAL_FATAL("invalid scheduler retirement");
	queue_remove_thread(cpu, current);
	current->state = THREAD_EXITING;
	next = pick_next_locked(cpu);
	if (next == NULL)
		next = cpu->idle;
	if (next == NULL || next == current)
		HAL_FATAL("scheduler exit without idle thread");
	next->state = THREAD_RUNNING;
#if SCHED_WAKE_LATENCY
	wake_latency_account(next);
#endif
	perf_switch_locked(cpu, current, next);
	next->sched.last_cpu = id;
	if (next->sched.quantum == 0)
		next->sched.quantum = SCHED_QUANTUM_TICKS;
	cpu->current = next;
	cpu->need_resched = 0;
	cpu->preempt = 0;
	cpu->retired = current;
	spin_unlock(&cpu->lock);

	/* Switches away; the successor retires us. */
	hal_task_context_switch(next->task);
	complete_retired(cpu);
	HAL_FATAL("retired thread resumed");
	__builtin_unreachable();
}

/*
 * Handles the clock tick on a CPU.
 *
 * Timed sleeps that expired become runnable, the running user thread is
 * charged a tick of CPU time and its interval timers advanced, and an
 * exhausted quantum yields.  CPU 0 also keeps the global tick count and
 * the real-time interval timers.
 */
void
sched_clock_cpu(
	hal_cpu_id_t id,
	uint64_t now)
{
	struct sched_cpu *cpu;
	struct thread *thread;
	struct thread *next;
	struct process *expired_process;
	struct process *accounted_process;
	uint64_t accounted_ticks;
	unsigned expired_signals;
	unsigned long irq;
	hal_cpu_id_t helper;
	int preempt;
	int wanted;
	int kernel;
	int backlog;
	int waiting;

	expired_process = NULL;
	accounted_process = NULL;
	accounted_ticks = 0;
	expired_signals = 0;
	preempt = 0;

	/* Only the CPU itself handles its tick, and only once online. */
	if (id != hal_cpu_current() || !cpu_online(id))
		return;
	if (id == 0)
		atomic_u64_store_release(&scheduler_ticks, now);

	/* Wakes the timed sleepers whose deadline passed. */
	cpu = sched_cpu_state(id);
	irq = spin_lock_irqsave(&cpu->lock);

	thread = cpu->sleep.head;
	while (thread != NULL) {
		next = thread->sched.next;
		if (thread->sched.wakeup_tick != 0 &&
		    thread->sched.wakeup_tick <= now) {
			queue_remove(&cpu->sleep, thread);
			thread->state = THREAD_RUNNABLE;
			thread->sched.wakeup_tick = 0;
			wake_enqueue_locked(cpu, thread);
		}

		thread = next;
	}

	/* Charges the tick to what the CPU was doing (hw.cputimes). */
	thread = curthread;
	charge_cpu_time(cpu, thread);

	/* Charges the running thread and decides on preemption. */
	if (thread != NULL && thread->state == THREAD_RUNNING) {
		if ((thread->flags & THREAD_FLAG_IDLE) == 0 &&
		    thread->proc != NULL &&
		    thread->proc != &process0) {
			kernel = thread->accounting_kernel_depth != 0;
			accounted_ticks = atomic_u64_fetch_add_relaxed(
			    &thread->proc->cpu_ticks, 1U) + 1U;
			if (kernel)
				(void)atomic_u64_fetch_add_relaxed(
				    &thread->proc->system_ticks, 1U);
			else
				(void)atomic_u64_fetch_add_relaxed(
				    &thread->proc->user_ticks, 1U);
			accounted_process = thread->proc;

			/*
			 * ITIMER_VIRTUAL advances in user mode only.
			 * ITIMER_PROF measures the complete user+system CPU
			 * time of the process.
			 */
			if (!kernel && process_itimer_tick(thread->proc, 1))
				expired_signals |= 1U << 1;
			if (process_itimer_tick(thread->proc, 2))
				expired_signals |= 1U << 2;
			if (expired_signals != 0)
				expired_process = thread->proc;
		}

		/* The idle thread yields to any work; others when their quantum ends. */
		if ((thread->flags & THREAD_FLAG_IDLE) != 0) {
			preempt = cpu->need_resched != 0;
		} else if (thread->sched.quantum != 0) {
			thread->sched.quantum--;
			if (thread->sched.quantum == 0)
				preempt = 1;
		}
	}

	/* Honour kern_preempt_disable(): defer the switch while preemption is off. */
	if (preempt && cpu->preempt_count != 0u) {
		cpu->need_resched = 1;
		cpu->yield_due = 1;
		preempt = 0;
	}

	/*
	 * A wakeup that asked to run first and was not acted on since (the
	 * current thread has not been back to user mode) is acted on now.
	 */
	wanted = !preempt &&
	    cpu->preempt != 0u &&
	    cpu->preempt_count == 0u &&
	    thread != NULL &&
	    (thread->flags & THREAD_FLAG_IDLE) == 0;

	/*
	 * A busy CPU with threads still waiting behind the running one hands
	 * one of them to an idle CPU, which steals it from the queue.  Once
	 * a tick is soon enough: a thread placed here for a parent that
	 * blocks at once is gone before the tick, and only a parent that
	 * keeps running leaves its child waiting this long.
	 */
	backlog = 0;
	if (thread != NULL && (thread->flags & THREAD_FLAG_IDLE) == 0) {
		waiting = best_priority_locked(cpu);
		if (waiting >= 0)
			backlog = 1;
	}

	spin_unlock_irqrestore(&cpu->lock, irq);

	/* Wakes one idle CPU to take a waiting thread. */
	if (backlog) {
		helper = choose_idle_cpu(id);
		if (helper != HAL_CPU_MAX)
			notify_cpu(helper);
	}

	/* Applies the CPU limit and sends the expired interval timer signals. */
	if (accounted_process != NULL)
		resource_limit_cpu_tick(accounted_process, accounted_ticks);
	if (expired_process != NULL) {
		if (expired_signals & 2U)
			send_itimer_signal(expired_process, SIGVTALRM);
		if (expired_signals & 4U)
			send_itimer_signal(expired_process, SIGPROF);
	}

	/*
	 * ITIMER_REAL is wall-clock based, so CPU 0 advances all armed state
	 * in one registry pass rather than repeatedly searching by PID.
	 */
	if (id == 0)
		process_itimer_real_tick_all();
	if (preempt)
		sched_yield();
	else if (wanted)
		sched_preempt();
}

/*
 * Yields the CPU on behalf of the HAL.
 */
void
kernel_yield_task(
	void)
{
	sched_yield();
}

/*
 * Sleeps the current thread until a task notification arrives.
 *
 * A notification that arrived before the wait returns at once.
 */
void
sched_wait_task(
	void)
{
	struct thread *thread;
	struct sched_cpu *cpu;
	unsigned long ignored;
	bool enabled;

	enabled = hal_irq_disable();

	/* Only a non-idle thread on its own CPU can wait. */
	thread = curthread;
	if (thread == NULL ||
	    (thread->flags & THREAD_FLAG_IDLE) != 0 ||
	    thread->sched.cpu != hal_cpu_current())
		HAL_FATAL("invalid task wait");

	/* Consumes an earlier notification without sleeping. */
	cpu = sched_cpu_state(thread->sched.cpu);
	ignored = spin_lock_irqsave(&cpu->lock);

	(void)ignored;
	if (thread->notify_pending != 0) {
		thread->notify_pending = 0;
		spin_unlock(&cpu->lock);
		if (enabled)
			hal_irq_enable();
		return;
	}

	/* Sleeps without a deadline. */
	if (thread->state != THREAD_RUNNING)
		HAL_FATAL("waiting task is not running");
	thread->state = THREAD_SLEEPING;
	thread->sched.wakeup_tick = 0;
	spin_unlock(&cpu->lock);
	switch_without_enqueue();
	if (enabled)
		hal_irq_enable();
}

/*
 * Notifies a task: wakes it from a task wait, or remembers the notification.
 */
void
sched_notify_task(
	hal_task_t task)
{
	struct thread *thread;
	struct sched_cpu *cpu;
	unsigned long irq;
	hal_cpu_id_t id;
	int runnable;

	runnable = 0;

	/* Ignores a missing task or one without a thread. */
	if (task == NULL)
		return;
	thread = hal_task_get_private(task);
	if (thread == NULL)
		return;

	/* Locks the CPU the thread is on, re-reading it under the lock. */
	for (;;) {
		id = (hal_cpu_id_t)atomic_raw_load_acquire(
		    (volatile unsigned *)&thread->sched.cpu);
		if (id >= scheduler_cpu_count)
			return;
		cpu = sched_cpu_state(id);
		irq = spin_lock_irqsave(&cpu->lock);
		if (id == thread->sched.cpu)
			break;
		spin_unlock_irqrestore(&cpu->lock, irq);
	}

	/* Wakes a sleeper; otherwise remembers the notification. */
	if ((thread->sched.need_migrate & SCHED_MIGRATING) != 0) {
		thread->sched.need_migrate |= SCHED_WAKE_PENDING;
		thread->notify_pending = 1;
	} else if (thread->state == THREAD_SLEEPING) {
		queue_remove_thread(cpu, thread);
		thread->state = THREAD_RUNNABLE;
		thread->sched.wakeup_tick = 0;
		wake_enqueue_locked(cpu, thread);
		runnable = 1;
	} else if (thread->state == THREAD_RUNNING ||
	    thread->state == THREAD_RUNNABLE) {
		/* Preserves a notification delivered before kernel_wait_task(). */
		thread->notify_pending = 1;
	}

	spin_unlock_irqrestore(&cpu->lock, irq);

	if (runnable)
		notify_cpu(id);
}

/*
 * Waits for a task notification on behalf of the HAL.
 */
void
kernel_wait_task(
	void)
{
	sched_wait_task();
}

/*
 * Notifies a task on behalf of the HAL.
 */
void
kernel_notify_task(
	hal_task_t task)
{
	sched_notify_task(task);
}

/*
 * Sleeps the current thread until a wakeup or a tick deadline.
 *
 * A zero deadline sleeps until woken.
 */
void
sched_sleep(
	uint64_t timeout_tick)
{
	struct thread *thread;
	struct sched_cpu *cpu;
	unsigned long ignored;
	bool enabled;

	enabled = hal_irq_disable();

	/* Only a thread on its own CPU can sleep. */
	thread = curthread;
	if (thread == NULL || thread->sched.cpu != hal_cpu_current())
		HAL_FATAL("invalid scheduler sleep");

	/* Not the idle thread, which has nothing to switch to and would stay on the sleep queue (BUG-202). */
	refuse_idle_sleep(thread, "sched_sleep", __builtin_return_address(0));

	/* Publishes the sleep, with the deadline on the sleep queue. */
	cpu = sched_cpu_state(thread->sched.cpu);
	ignored = spin_lock_irqsave(&cpu->lock);

	(void)ignored;
	thread->state = THREAD_SLEEPING;
	thread->sched.wakeup_tick = timeout_tick;
	if (timeout_tick != 0)
		queue_append(&cpu->sleep, thread, SCHED_QUEUE_SLEEP);
	spin_unlock(&cpu->lock);
	switch_without_enqueue();
	if (enabled)
		hal_irq_enable();
}

/*
 * Sleeps the current thread, releasing a condition lock while it sleeps.
 *
 * The lock is held again on return, and the caller's interrupt state is
 * restored: the context switch preserves RFLAGS, so without this a thread
 * would resume with interrupts disabled and keep running that way until its
 * next sleep -- on the BSP that starves the global tick (kernel_ticks) and
 * every tick-deadline sleep in the system.
 */
void
sched_sleep_locked(
	uint64_t timeout_tick,
	struct spinlock *condition_lock)
{
	struct thread *thread;
	struct sched_cpu *cpu;
	bool enabled;

	thread = curthread;

	enabled = hal_irq_disable();

	/* Only a thread on its own CPU can sleep, and it needs a lock. */
	if (thread == NULL ||
	    condition_lock == NULL ||
	    thread->sched.cpu != hal_cpu_current())
		HAL_FATAL("invalid locked sleep");

	/* Not the idle thread, which has nothing to switch to and would stay on the sleep queue (BUG-202). */
	refuse_idle_sleep(thread, "sched_sleep_locked", __builtin_return_address(0));

	/* Publishes the sleep before dropping the condition lock. */
	cpu = sched_cpu_state(thread->sched.cpu);
	spin_lock(&cpu->lock);
	thread->state = THREAD_SLEEPING;
	thread->sched.wakeup_tick = timeout_tick;
	if (timeout_tick != 0)
		queue_append(&cpu->sleep, thread, SCHED_QUEUE_SLEEP);
	spin_unlock(condition_lock);
	spin_unlock(&cpu->lock);
	switch_without_enqueue();
	spin_lock(condition_lock);
	if (enabled)
		hal_irq_enable();
}

/*
 * Sleeps like sched_sleep_locked() unless an interrupt arrived meanwhile.
 *
 * The caller passes the interrupt generation it observed before deciding
 * to sleep; a newer generation refuses the sleep and returns 1.
 */
int
sched_sleep_locked_interruptible(
	uint64_t timeout_tick,
	struct spinlock *condition_lock,
	uint64_t observed_generation)
{
	struct thread *thread;
	struct sched_cpu *cpu;
	bool enabled;

	thread = curthread;

	enabled = hal_irq_disable();

	/* Only a thread on its own CPU can sleep, and it needs a lock. */
	if (thread == NULL ||
	    condition_lock == NULL ||
	    thread->sched.cpu != hal_cpu_current())
		HAL_FATAL("invalid interruptible locked sleep");

	/* Not the idle thread, which has nothing to switch to and would stay on the sleep queue (BUG-202). */
	refuse_idle_sleep(thread, "sched_sleep_locked_interruptible", __builtin_return_address(0));
	cpu = sched_cpu_state(thread->sched.cpu);
	spin_lock(&cpu->lock);

	/*
	 * sched_interrupt() advances the latch before attempting
	 * sched_wakeup().  Holding the scheduler lock makes this comparison
	 * and publication of the sleeping state one atomic handoff from the
	 * waker's point of view.
	 */
	if (atomic_u64_load_acquire(&thread->interrupt_generation) !=
	    observed_generation) {
		spin_unlock(&cpu->lock);
		if (enabled)
			hal_irq_enable();
		return 1;
	}

	/* Publishes the sleep before dropping the condition lock. */
	thread->state = THREAD_SLEEPING;
	thread->sched.wakeup_tick = timeout_tick;
	if (timeout_tick != 0)
		queue_append(&cpu->sleep, thread, SCHED_QUEUE_SLEEP);
	spin_unlock(condition_lock);
	spin_unlock(&cpu->lock);
	switch_without_enqueue();
	spin_lock(condition_lock);
	if (enabled)
		hal_irq_enable();

	/* Reports a completed sleep. */
	return 0;
}

/*
 * Sleeps like sched_sleep_locked(), calling a notifier once asleep.
 */
void
sched_sleep_locked_notify(
	uint64_t timeout_tick,
	struct spinlock *condition_lock,
	void (*notify)(void *),
	void *argument)
{
	struct thread *thread;
	struct sched_cpu *cpu;
	bool enabled;

	thread = curthread;

	enabled = hal_irq_disable();

	/* Only a thread on its own CPU can sleep, and it needs a lock and notifier. */
	if (thread == NULL ||
	    condition_lock == NULL ||
	    notify == NULL ||
	    thread->sched.cpu != hal_cpu_current())
		HAL_FATAL("invalid notifying locked sleep");

	/* Not the idle thread, which has nothing to switch to and would stay on the sleep queue (BUG-202). */
	refuse_idle_sleep(thread, "sched_sleep_locked_notify", __builtin_return_address(0));

	/* Publishes the sleep before dropping the condition lock. */
	cpu = sched_cpu_state(thread->sched.cpu);
	spin_lock(&cpu->lock);
	thread->state = THREAD_SLEEPING;
	thread->sched.wakeup_tick = timeout_tick;
	if (timeout_tick != 0)
		queue_append(&cpu->sleep, thread, SCHED_QUEUE_SLEEP);
	spin_unlock(condition_lock);
	spin_unlock(&cpu->lock);

	/*
	 * The scheduler already regards this task as sleeping, so an
	 * observer cannot see the notification while the final user thread
	 * is runnable.  This callback only publishes that fact; it must not
	 * wake thread itself, because switch_without_enqueue() still owns
	 * the pending context switch.
	 */
	notify(argument);
	switch_without_enqueue();
	spin_lock(condition_lock);
	if (enabled)
		hal_irq_enable();
}

/*
 * Reads the global tick count.
 */
uint64_t
sched_ticks(
	void)
{
	uint64_t ticks;

	ticks = atomic_u64_load_acquire(&scheduler_ticks);

	/* Reports the tick count. */
	return ticks;
}

/*
 * Reads one CPU's ticks since boot by what it was doing (hw.cputimes).
 *
 * A CPU that never came online reads all zero.  Returns 0, or ENOENT for a
 * CPU the scheduler does not have.
 */
int
sched_cpu_time(
	hal_cpu_id_t cpu,
	struct sched_cpu_time *time)
{
	struct sched_cpu *state;
	unsigned count;

	/* A CPU the scheduler does not have (or none yet, before it starts). */
	count = atomic_raw_load_acquire(&scheduler_cpu_count);
	if (scheduler_cpus == NULL || cpu >= count)
		return ENOENT;

	/* Each count, loaded whole. */
	state = &scheduler_cpus[cpu];
	time->user = atomic_u64_load_acquire(&state->times[SCHED_TIME_USER]);
	time->system = atomic_u64_load_acquire(&state->times[SCHED_TIME_SYSTEM]);
	time->idle = atomic_u64_load_acquire(&state->times[SCHED_TIME_IDLE]);
	time->other = atomic_u64_load_acquire(&state->times[SCHED_TIME_OTHER]);

	/* Succeeded: the CPU's ticks. */
	return 0;
}

/*
 * Tests whether the current CPU has a runnable thread queued.
 */
int
sched_has_runnable(
	void)
{
	struct sched_cpu *cpu;
	unsigned long irq;
	int priority;
	int found;

	found = 0;

	/* Checks every priority level under the CPU lock. */
	cpu = sched_cpu_state(hal_cpu_current());
	irq = spin_lock_irqsave(&cpu->lock);

	for (priority = SCHED_PRIOR_HIGH; priority <= SCHED_PRIOR_LOW;
	     priority++) {
		if (cpu->woken[priority].head != NULL ||
		    cpu->run[priority].head != NULL) {
			found = 1;
			break;
		}
	}

	spin_unlock_irqrestore(&cpu->lock, irq);

	/* Reports whether a thread is queued. */
	return found;
}

/*
 * Runs the idle loop of the current CPU forever.
 */
void
sched_idle(
	void)
{
	struct thread *idle;
	hal_cpu_id_t cpu;
	int stolen;

	idle = curthread;
	stolen = 0;

	/* Only the CPU's idle thread idles. */
	cpu = hal_cpu_current();
	if (idle == NULL ||
	    (idle->flags & THREAD_FLAG_IDLE) == 0 ||
	    idle->sched.cpu != cpu)
		HAL_FATAL("invalid idle context");

	/* Runs queued work, then halts until the next interrupt. */
	for (;;) {
		(void)hal_irq_disable();
		if (sched_has_runnable())
			sched_switch();
		if (curthread != idle)
			HAL_FATAL("idle resumed on foreign task");

		/* Takes waiting work from a busier CPU rather than halting. */
		stolen = steal_runnable(cpu);
		if (stolen) {
			sched_switch();
			continue;
		}

		/* Halts as a CPU that others may hand work to (suspended to idle during a system sleep, ws052-p006). */
		idle_mask_set(cpu);
		kern_sleep_idle(cpu);
		idle_mask_clear(cpu);
		sched_switch();
	}
}

/*
 * Brings a secondary CPU online with the current thread as its idle thread.
 */
void
sched_secondary_init(
	hal_cpu_id_t id)
{
	struct sched_cpu *cpu;
	struct thread *idle;

	/* Only a secondary CPU's own idle thread may enter. */
	idle = curthread;
	if (id == 0 ||
	    id != hal_cpu_current() ||
	    id >= scheduler_cpu_count ||
	    idle == NULL ||
	    (idle->flags & THREAD_FLAG_IDLE) == 0 ||
	    idle->sched.cpu != id)
		HAL_FATAL("invalid secondary scheduler entry");

	/* Publishes the CPU as online. */
	cpu = sched_cpu_state(id);
	spin_lock(&cpu->lock);
	if (cpu->idle != NULL || cpu->online)
		HAL_FATAL("secondary scheduler initialized twice");
	cpu->idle = idle;
	cpu->current = idle;
	atomic_raw_store_release(&cpu->online, 1U);
	(void)atomic_u64_fetch_or_release(
	    &scheduler_online_mask.bits[id / 64U],
	    (uint64_t)1U << (id % 64U));
	spin_unlock(&cpu->lock);

	/* Idles from now on. */
	hal_irq_enable();
	sched_idle();
}

/*
 * Waits until every CPU the HAL reports ready is online in the scheduler.
 */
int
sched_wait_others_online(
	void)
{
	struct hal_cpu_mask ready;
	unsigned timeout;
	hal_cpu_id_t cpu;

	/* Polls the online mask against the ready mask for a bounded time. */
	hal_cpu_ready_mask(&ready);
	for (timeout = 0; timeout < SCHED_ONLINE_TIMEOUT; timeout++) {
		for (cpu = 0; cpu < scheduler_cpu_count; cpu++) {
			if (hal_cpu_mask_test(&ready, cpu) &&
			    !hal_cpu_mask_test(&scheduler_online_mask, cpu))
				break;
		}

		if (cpu == scheduler_cpu_count)
			return 0;
		hal_compiler_barrier();
	}

	/* Reports a CPU that never came online. */
	return ETIMEDOUT;
}

/*
 * Migrates a thread that is not running to another CPU.
 *
 * A wakeup that arrives during the migration is applied on the new CPU,
 * or on the old one when the task transfer fails.
 */
int
sched_set_cpu(
	struct thread *thread,
	hal_cpu_id_t target)
{
	struct sched_cpu *old_cpu;
	struct sched_cpu *new_cpu;
	hal_cpu_id_t old;
	unsigned old_kind;
	unsigned pending;
	unsigned long irq;
	int error;

	/* Only a non-running live thread can move to an online CPU. */
	if (thread == NULL ||
	    thread->task == NULL ||
	    !cpu_online(target) ||
	    thread->state == THREAD_RUNNING ||
	    thread->state == THREAD_ZOMBIE ||
	    thread->state == THREAD_DEAD)
		return EINVAL;
	old = thread->sched.cpu;
	if (old == target)
		return 0;
	if (old >= scheduler_cpu_count)
		return EINVAL;

	/* Takes the thread off its old CPU's queues and marks it migrating. */
	old_cpu = sched_cpu_state(old);
	irq = spin_lock_irqsave(&old_cpu->lock);

	if (thread->sched.cpu != old || thread->sched.need_migrate != 0) {
		spin_unlock_irqrestore(&old_cpu->lock, irq);
		return EBUSY;
	}

	old_kind = thread->sched.queue_kind;
	if (old_kind == SCHED_QUEUE_WOKEN)
		old_kind = SCHED_QUEUE_RUN;
	queue_remove_thread(old_cpu, thread);
	thread->sched.need_migrate = SCHED_MIGRATING;

	spin_unlock_irqrestore(&old_cpu->lock, irq);

	/* Moves the task; a failure puts the thread back on the old CPU. */
	error = hal_task_transfer(thread->task, target);
	if (error != HAL_OK) {
		irq = spin_lock_irqsave(&old_cpu->lock);
		pending = thread->sched.need_migrate & SCHED_WAKE_PENDING;
		thread->sched.need_migrate = 0;
		if (pending && thread->state == THREAD_SLEEPING)
			old_kind = SCHED_QUEUE_RUN;
		if (old_kind == SCHED_QUEUE_RUN) {
			thread->state = THREAD_RUNNABLE;
			queue_append(&old_cpu->run[thread->sched.priority], thread,
			    SCHED_QUEUE_RUN);
		} else if (old_kind == SCHED_QUEUE_SLEEP) {
			queue_append(&old_cpu->sleep, thread, SCHED_QUEUE_SLEEP);
		}

		spin_unlock_irqrestore(&old_cpu->lock, irq);
		if (error == HAL_ERR_BUSY)
			return EBUSY;
		return EIO;
	}

	/* Sends later wakers to the new CPU, under the lock they now take. */
	retarget_migrating(thread, old_cpu, target);

	/* Queues the thread on the new CPU, applying a pending wakeup. */
	new_cpu = sched_cpu_state(target);
	irq = spin_lock_irqsave(&new_cpu->lock);

	pending = thread->sched.need_migrate & SCHED_WAKE_PENDING;
	thread->sched.last_cpu = old;
	thread->sched.need_migrate = 0;
	if (pending && thread->state == THREAD_SLEEPING)
		old_kind = SCHED_QUEUE_RUN;
	if (old_kind == SCHED_QUEUE_RUN) {
		thread->state = THREAD_RUNNABLE;
		queue_append(&new_cpu->run[thread->sched.priority], thread,
		    SCHED_QUEUE_RUN);
		new_cpu->need_resched = 1;
	} else if (old_kind == SCHED_QUEUE_SLEEP) {
		queue_append(&new_cpu->sleep, thread, SCHED_QUEUE_SLEEP);
	}

	spin_unlock_irqrestore(&new_cpu->lock, irq);

	if (old_kind == SCHED_QUEUE_RUN)
		notify_cpu(target);

	/* Reports the migrated thread. */
	return 0;
}

/*
 * Handles a scheduler notification on the current CPU by yielding if needed.
 */
void
sched_cpu_notify(
	hal_cpu_id_t id)
{
	struct sched_cpu *cpu;
	unsigned long irq;
	int runnable;

	/* Only the CPU itself handles its notification, and only once online. */
	if (id != hal_cpu_current() || !cpu_online(id))
		return;

	/*
	 * A wakeup that asked to run first preempts; otherwise only the idle
	 * thread gives way to new work.
	 */
	cpu = sched_cpu_state(id);
	irq = spin_lock_irqsave(&cpu->lock);

	runnable = 0;
	if (cpu->preempt != 0 && cpu->preempt_count == 0u)
		runnable = 2;
	else if (cpu->need_resched != 0 && cpu->current == cpu->idle)
		runnable = 1;

	spin_unlock_irqrestore(&cpu->lock, irq);

	if (runnable == 2)
		sched_preempt();
	else if (runnable == 1)
		sched_yield();
}

/*
 * Reads the counter the latency diagnostic measures with.
 *
 * Reports zero without a counter; the diagnostic is then never on, so no
 * caller compares a zero reading.
 */
uint64_t
sched_perf_counter(
	void)
{
	uint64_t counter;
	uint64_t frequency;
	bool available;

	/* Asks the platform for the monotonic counter. */
	available = kern_rtc_read_counter(&counter, &frequency);
	if (!available)
		return 0;

	/* Reports the reading. */
	return counter;
}

/*
 * Converts a difference of sched_perf_counter() readings to microseconds.
 *
 * Whole seconds and the remainder are scaled apart, so a long difference
 * does not overflow on a counter of several gigahertz.
 */
uint64_t
sched_perf_counts_to_us(
	uint64_t counts)
{
	uint64_t frequency;
	uint64_t seconds;
	uint64_t remainder;

	/* Without a frequency there is nothing to convert. */
	frequency = sched_perf_frequency;
	if (frequency == 0)
		return 0;

	/* Scales the whole seconds and the part of a second separately. */
	seconds = counts / frequency;
	remainder = counts % frequency;

	/* Reports the microseconds. */
	return seconds * 1000000ULL + (remainder * 1000000ULL) / frequency;
}

/*
 * Names a thread for a sample of the latency diagnostic.
 *
 * The command is read without the process lock: a title changed at the
 * same moment can only garble the diagnostic's own line.
 */
void
sched_perf_identify(
	const struct thread *thread,
	struct sched_perf_thread *who)
{
	const struct process *process;

	/* Starts as nobody. */
	kern_memset(who, 0, sizeof(*who));
	who->pid = -1;
	who->tid = -1;
	if (thread == NULL)
		return;

	/* The thread id, and the process id; a kernel thread belongs to process 0. */
	who->tid = (int)thread->tid;
	process = thread->proc;
	if (process == NULL)
		return;
	who->pid = (int)process->pid;

	/* The start of the command, always terminated. */
	kern_memcpy(who->name, process->command, sizeof(who->name) - 1U);
	who->name[sizeof(who->name) - 1U] = '\0';
}

#ifdef KERN_SCHED_TEST
/*
 * Brings a CPU online under test with a fake idle thread.
 */
int
sched_test_cpu_online(
	hal_cpu_id_t id,
	struct thread *idle)
{
	struct sched_cpu *cpu;

	/* Rejects the boot CPU, a CPU out of range, or a missing idle thread. */
	if (id == 0 || id >= scheduler_cpu_count || idle == NULL)
		return EINVAL;

	/* Installs the idle thread and marks the CPU online. */
	cpu = sched_cpu_state(id);
	cpu->idle = idle;
	idle->flags |= THREAD_FLAG_IDLE;
	idle->sched.cpu = id;
	idle->sched.last_cpu = id;
	idle->state = THREAD_RUNNING;
	atomic_raw_store_release(&cpu->online, 1U);
	(void)atomic_u64_fetch_or_release(
	    &scheduler_online_mask.bits[id / 64U],
	    (uint64_t)1U << (id % 64U));

	/* Reports the online CPU. */
	return 0;
}
#endif

/* Sends an interval timer signal to a process. */
static void
send_itimer_signal(
	struct process *process,
	int signo)
{
	struct signal_info info;

	/* Describes the signal as a timer notification. */
	kern_memset(&info, 0, sizeof(info));
	info.code = SI_TIMER;
	(void)signal_send_process_info(process, signo, &info);
}

/*
 * Charges a CPU's tick to what it was doing: its idle thread, a user
 * thread in user mode, a thread in the kernel or a kernel thread (process0's
 * or none), or neither (no thread, or one that is not running: it is going
 * to sleep or leaving).  An interrupt is the time of the thread it stopped.
 */
static void
charge_cpu_time(
	struct sched_cpu *cpu,
	const struct thread *thread)
{
	unsigned kind;

	/* No thread, or the idle thread. */
	if (thread == NULL) {
		kind = SCHED_TIME_OTHER;
	} else if ((thread->flags & THREAD_FLAG_IDLE) != 0) {
		kind = SCHED_TIME_IDLE;
	} else if (thread->state != THREAD_RUNNING) {
		/* Between threads: going to sleep or leaving. */
		kind = SCHED_TIME_OTHER;
	} else if (thread->proc == NULL) {
		/* A thread of no process works for the kernel. */
		kind = SCHED_TIME_SYSTEM;
	} else if (thread->proc == &process0) {
		/* The kernel's own threads. */
		kind = SCHED_TIME_SYSTEM;
	} else if (thread->accounting_kernel_depth != 0) {
		/* A user thread inside a system call or a fault. */
		kind = SCHED_TIME_SYSTEM;
	} else {
		kind = SCHED_TIME_USER;
	}

	/* One more tick of that kind; only this CPU writes its counts. */
	(void)atomic_u64_fetch_add_relaxed(&cpu->times[kind], 1U);
}

/* Finds the state of a CPU, which must exist. */
static struct sched_cpu *
sched_cpu_state(
	hal_cpu_id_t cpu)
{
	/* An unknown CPU is a fatal programming error. */
	if (scheduler_cpus == NULL || cpu >= scheduler_cpu_count)
		HAL_FATAL("invalid scheduler CPU");

	/* Reports the state. */
	return &scheduler_cpus[cpu];
}

/* Appends a thread to the tail of a queue. */
static void
queue_append(
	struct sched_queue *queue,
	struct thread *thread,
	unsigned kind)
{
	thread->sched.prev = queue->tail;
	thread->sched.next = NULL;
	if (queue->tail != NULL)
		queue->tail->sched.next = thread;
	else
		queue->head = thread;
	queue->tail = thread;
	queue->count++;
	thread->sched.queued_tick = atomic_u64_load_acquire(&scheduler_ticks);
	thread->sched.queue_kind = kind;
}

/* Inserts a thread at the front of a queue. */
static void
queue_prepend(
	struct sched_queue *queue,
	struct thread *thread,
	unsigned kind)
{
	thread->sched.prev = NULL;
	thread->sched.next = queue->head;
	if (queue->head != NULL)
		queue->head->sched.prev = thread;
	else
		queue->tail = thread;
	queue->head = thread;
	queue->count++;
	thread->sched.queued_tick = atomic_u64_load_acquire(&scheduler_ticks);
	thread->sched.queue_kind = kind;
}

/*
 * Queues a thread that a wakeup has just made runnable on its CPU.
 *
 * A thread of the same or a higher priority than the one running there
 * joins the woken queue of its priority, served before its run queue, and
 * marks the CPU for preemption, so it runs at the next safe point instead
 * of after the running thread's quantum.  A lower one waits its turn at the back.  The quantum is not
 * refilled: a thread that wakes and sleeps over and over still uses it up
 * and then goes to the back behind threads that keep the CPU busy.  The
 * CPU lock is held.
 */
static void
wake_enqueue_locked(
	struct sched_cpu *cpu,
	struct thread *thread)
{
	struct thread *running;

	/* A quantum used up while preemption was held off starts afresh. */
	if (thread->sched.quantum == 0)
		thread->sched.quantum = SCHED_QUANTUM_TICKS;

	/* The latency diagnostic measures the wait from here to the thread's run. */
	perf_mark_runnable_locked(cpu, thread, SCHED_PERF_WOKEN);

	/* Places the thread by its priority against the running one. */
	running = cpu->current;
	if (running == NULL ||
	    (running->flags & THREAD_FLAG_IDLE) != 0 ||
	    thread->sched.priority <= running->sched.priority) {
		queue_append(&cpu->woken[thread->sched.priority], thread,
		    SCHED_QUEUE_WOKEN);
		cpu->preempt = 1;
	} else {
		queue_append(&cpu->run[thread->sched.priority], thread,
		    SCHED_QUEUE_RUN);
	}
	cpu->need_resched = 1;
}

/* Unlinks a thread from a queue. */
static void
queue_remove(
	struct sched_queue *queue,
	struct thread *thread)
{
	/* Splices the thread out of the doubly linked list. */
	if (thread->sched.prev != NULL)
		thread->sched.prev->sched.next = thread->sched.next;
	else
		queue->head = thread->sched.next;
	if (thread->sched.next != NULL)
		thread->sched.next->sched.prev = thread->sched.prev;
	else
		queue->tail = thread->sched.prev;
	if (queue->count == 0) {
		/* The queue and the thread, and where the removal came from, for the screen before the fatal (BUG-202). */
		queue_report_underflow(queue, thread, __builtin_return_address(0));
		HAL_FATAL("scheduler queue underflow");
	}
	queue->count--;
	thread->sched.next = NULL;
	thread->sched.prev = NULL;
	thread->sched.queue_kind = SCHED_QUEUE_NONE;
}

/*
 * Writes what a removal from an empty queue found, before the fatal
 * (BUG-202): which queue of which CPU it is (run or woken at a priority,
 * the sleep queue, or none of the thread's CPU), its count and ends, the
 * thread's id, state, flags, queue kind and priority, the CPU running, and
 * the address the removal was called from (the kernel is not stripped, so
 * the address names the function).
 */
static void
queue_report_underflow(
	const struct sched_queue *queue,
	const struct thread *thread,
	void *caller)
{
	const struct sched_cpu *cpu;
	const char *kind;
	int level;
	int priority;

	/* The queue among those of the thread's CPU. */
	kind = "foreign";
	level = -1;
	cpu = NULL;
	if (thread->sched.cpu < scheduler_cpu_count)
		cpu = &scheduler_cpus[thread->sched.cpu];
	if (cpu != NULL && queue == &cpu->sleep)
		kind = "sleep";

	/* A run or woken queue, by its priority. */
	for (priority = 0; cpu != NULL && priority < SCHED_PRIOR_LEVELS; priority++) {
		if (queue == &cpu->run[priority]) {
			kind = "run";
			level = priority;
		} else if (queue == &cpu->woken[priority]) {
			kind = "woken";
			level = priority;
		}
	}

	/* The line on the screen. */
	kern_logf("sched: underflow queue=%s[%d] cpu=%u count=%u head=%p tail=%p thread=%p tid=%d state=%d flags=0x%x kind=%d priority=%d on=%u caller=%p\n",
	    kind, level, (unsigned)thread->sched.cpu, queue->count, (void *)queue->head, (void *)queue->tail,
	    (const void *)thread, (int)thread->tid, (int)thread->state, (unsigned)thread->flags,
	    (int)thread->sched.queue_kind, thread->sched.priority, (unsigned)hal_cpu_current(), caller);
}

/*
 * Stops the kernel when the idle thread enters a sleep (BUG-202).
 *
 * switch_without_enqueue() returns at once to an idle thread that finds
 * nothing to run, so a sleep with a deadline would leave it linked on the
 * sleep queue, and its next sleep would link it twice and break the queue
 * (the "scheduler queue underflow" later).  The idle loop never sleeps; an
 * interrupt handler that sleeps while the idle thread is current does.  The
 * line names the entry and the address it was called from.
 */
static void
refuse_idle_sleep(
	const struct thread *thread,
	const char *entry,
	void *caller)
{
	/* Any other thread may sleep. */
	if ((thread->flags & THREAD_FLAG_IDLE) == 0)
		return;

	/* The line on the screen, then the fatal. */
	kern_logf("sched: the idle thread of CPU %u entered %s from %p\n", (unsigned)thread->sched.cpu, entry, caller);
	HAL_FATAL("idle thread sleeps");
}

/* Unlinks a thread from whichever queue of a CPU it is on. */
static void
queue_remove_thread(
	struct sched_cpu *cpu,
	struct thread *thread)
{
	if (thread->sched.queue_kind == SCHED_QUEUE_RUN)
		queue_remove(&cpu->run[thread->sched.priority], thread);
	else if (thread->sched.queue_kind == SCHED_QUEUE_WOKEN)
		queue_remove(&cpu->woken[thread->sched.priority], thread);
	else if (thread->sched.queue_kind == SCHED_QUEUE_SLEEP)
		queue_remove(&cpu->sleep, thread);
}

/* Takes the highest-priority runnable thread of a CPU, or none. */
static struct thread *
pick_next_locked(
	struct sched_cpu *cpu)
{
	struct thread *next;
	int priority;

	/* Searches the run queues from the highest priority down. */
	for (priority = SCHED_PRIOR_HIGH; priority <= SCHED_PRIOR_LOW;
	     priority++) {
		next = cpu->woken[priority].head;
		if (next != NULL) {
			queue_remove(&cpu->woken[priority], next);
			return next;
		}
		next = cpu->run[priority].head;
		if (next != NULL) {
			queue_remove(&cpu->run[priority], next);
			return next;
		}
	}

	/* Reports an empty CPU. */
	return NULL;
}

/* Reports the highest priority with a runnable thread on a CPU, or -1. */
static int
best_priority_locked(
	struct sched_cpu *cpu)
{
	int priority;

	for (priority = SCHED_PRIOR_HIGH; priority <= SCHED_PRIOR_LOW;
	     priority++) {
		if (cpu->woken[priority].head != NULL ||
		    cpu->run[priority].head != NULL)
			return priority;
	}

	/* Nothing is runnable. */
	return -1;
}

/* Retires the thread that exited on a CPU before the current one ran. */
static void
complete_retired(
	struct sched_cpu *cpu)
{
	struct thread *thread;

	/* Nothing retired since the last switch. */
	thread = cpu->retired;
	if (thread == NULL)
		return;

	/* Hands the thread to the thread layer. */
	cpu->retired = NULL;
	thread_sched_retired(thread);
}

/* Tests whether a CPU is known and online. */
static int
cpu_online(
	hal_cpu_id_t cpu)
{
	/* The CPU must exist and have published itself online. */
	if (cpu >= atomic_raw_load_acquire(&scheduler_cpu_count))
		return 0;
	if (atomic_raw_load_acquire(&scheduler_cpus[cpu].online) == 0)
		return 0;

	/* Reports an online CPU. */
	return 1;
}

/* Picks an online CPU round-robin for a new thread. */
static hal_cpu_id_t
choose_cpu(
	void)
{
	unsigned start;
	unsigned offset;
	hal_cpu_id_t cpu;

	/* Starts after the last choice and takes the first online CPU. */
	start = atomic_raw_fetch_add_relaxed(&scheduler_round_robin, 1U);
	for (offset = 0; offset < scheduler_cpu_count; offset++) {
		cpu = (start + offset) % scheduler_cpu_count;
		if (cpu_online(cpu))
			return cpu;
	}

	/* Reports no online CPU. */
	return HAL_CPU_MAX;
}

/* Pokes another CPU so that it reschedules. */
static void
notify_cpu(
	hal_cpu_id_t cpu)
{
	int error;

	/* The current CPU reschedules on its own. */
	if (cpu == hal_cpu_current())
		return;

	/* Sends the notification. */
	error = hal_cpu_notify(cpu);
	if (error != HAL_OK)
		HAL_FATAL("scheduler CPU notification failed");
}

/* Switches to the next runnable thread, leaving the current one unqueued. */
static void
switch_without_enqueue(
	void)
{
	struct thread *current;
	struct thread *next;
	struct sched_cpu *cpu;
	hal_cpu_id_t id;
	unsigned long irq;
	bool enabled;

	/* Names the CPU with interrupts off, so the caller cannot move. */
	enabled = hal_irq_disable();
	current = curthread;
	id = hal_cpu_current();
	cpu = sched_cpu_state(id);
	irq = spin_lock_irqsave(&cpu->lock);

	/* Without queued work a running thread keeps the CPU; else idle runs. */
	next = pick_next_locked(cpu);
	if (next == NULL) {
		if (current != NULL && current->state == THREAD_RUNNING) {
			spin_unlock_irqrestore(&cpu->lock, irq);
			if (enabled)
				hal_irq_enable();
			return;
		}

		next = cpu->idle;
		if (next == NULL)
			HAL_FATAL("scheduler CPU has no idle thread");
	}

	/* An idle thread giving up the CPU takes the CPU off the idle mask. */
	leave_idle(cpu, current, id);

	next->state = THREAD_RUNNING;
#if SCHED_WAKE_LATENCY
	wake_latency_account(next);
#endif
	perf_switch_locked(cpu, current, next);
	next->sched.last_cpu = id;
	if (next->sched.quantum == 0)
		next->sched.quantum = SCHED_QUANTUM_TICKS;
	cpu->current = next;
	cpu->need_resched = 0;
	cpu->preempt = 0;

	spin_unlock_irqrestore(&cpu->lock, irq);

	/* Switches, then retires the thread that exited before us. */
	if (next != current)
		hal_task_context_switch(next->task);

	/* Resumed, perhaps on another CPU: completes that CPU's retired thread. */
	complete_retired(sched_cpu_state(hal_cpu_current()));

	/* Restores the caller's interrupt state. */
	if (enabled)
		hal_irq_enable();
}

/* Tests, without the lock, whether a CPU has a thread queued to run. */
static int
cpu_has_backlog(
	const struct sched_cpu *cpu)
{
	int priority;

	/* Any non-empty run or woken queue is a backlog. */
	for (priority = SCHED_PRIOR_HIGH; priority <= SCHED_PRIOR_LOW;
	     priority++) {
		if (cpu->woken[priority].head != NULL)
			return 1;
		if (cpu->run[priority].head != NULL)
			return 1;
	}

	/* Nothing waits on the CPU. */
	return 0;
}

/* Picks a halted idle CPU other than the given one, or HAL_CPU_MAX. */
static hal_cpu_id_t
choose_idle_cpu(
	hal_cpu_id_t except)
{
	uint64_t word;
	unsigned index;
	unsigned bit;
	hal_cpu_id_t cpu;

	/* Takes the first idle CPU in the mask that is not excluded. */
	for (index = 0; index < HAL_CPU_MASK_WORDS; index++) {
		word = atomic_u64_load_acquire(&scheduler_idle_mask.bits[index]);
		while (word != 0) {
			bit = (unsigned)__builtin_ctzll(word);
			word &= word - 1U;
			cpu = (hal_cpu_id_t)(index * 64U + bit);
			if (cpu == except || cpu >= scheduler_cpu_count)
				continue;
			return cpu;
		}
	}

	/* No CPU is idle. */
	return HAL_CPU_MAX;
}

/* Publishes that a CPU is about to halt with nothing to run. */
static void
idle_mask_set(
	hal_cpu_id_t cpu)
{
	(void)atomic_u64_fetch_or_release(
	    &scheduler_idle_mask.bits[cpu / 64U],
	    (uint64_t)1U << (cpu % 64U));
}

/* Withdraws a CPU from the idle mask. */
static void
idle_mask_clear(
	hal_cpu_id_t cpu)
{
	volatile uint64_t *word;
	uint64_t expected;
	uint64_t desired;
	int exchanged;

	/*
	 * Clears the bit by compare and exchange.  The atomic interface has
	 * no "and" (a 32-bit machine emulates 64-bit atomics), so the word is
	 * read and replaced until no other CPU changed it in between.
	 */
	word = &scheduler_idle_mask.bits[cpu / 64U];
	expected = atomic_u64_load_acquire(word);
	do {
		desired = expected & ~((uint64_t)1U << (cpu % 64U));
		exchanged = atomic_u64_compare_exchange(word, &expected,
		    desired);
	} while (!exchanged);
}

/*
 * Withdraws a CPU from the idle mask when its idle thread gives up the CPU.
 *
 * A notification that arrives while the idle thread halts may switch away
 * from inside the interrupt, before sched_idle() clears the bit itself;
 * clearing it here keeps the mask from naming a CPU that is busy.
 */
static void
leave_idle(
	struct sched_cpu *cpu,
	const struct thread *current,
	hal_cpu_id_t id)
{
	/* Only the idle thread's departure changes the mask. */
	if (current == NULL || current != cpu->idle)
		return;

	/* The CPU is busy from here on, whatever sched_idle() has not yet cleared. */
	idle_mask_clear(id);
}

/*
 * Takes one queued thread off a CPU for another CPU to run; the caller
 * holds the victim's lock.
 *
 * The thread leaves the queue marked as migrating, as sched_set_cpu()
 * marks one, so a wakeup or an explicit migration meanwhile sees it in
 * transit.  The thread that would run next on the victim is taken first.
 */
static struct thread *
take_stealable_locked(
	struct sched_cpu *victim)
{
	struct thread *thread;
	uint64_t now;
	int priority;

	/* Searches the queues from the highest priority down. */
	for (priority = SCHED_PRIOR_HIGH; priority <= SCHED_PRIOR_LOW;
	     priority++) {
		thread = victim->woken[priority].head;
		if (thread == NULL)
			thread = victim->run[priority].head;
		if (thread == NULL)
			continue;

		/* Leaves a thread already in transit or not simply runnable. */
		if (thread->sched.need_migrate != 0 ||
		    thread->state != THREAD_RUNNABLE ||
		    thread->task == NULL ||
		    (thread->flags & THREAD_FLAG_IDLE) != 0)
			continue;

		/*
		 * Leaves a thread queued during this tick where it is.  A child
		 * placed beside a parent that is about to wait runs there on a
		 * warm cache within the tick; taking it would move the child
		 * and leave the parent's CPU to halt and be woken again.  A
		 * thread still queued at the next tick has waited long enough.
		 */
		now = atomic_u64_load_acquire(&scheduler_ticks);
		if (thread->sched.queued_tick >= now)
			continue;

		/* Takes the thread out of the queue as a migrating one. */
		queue_remove_thread(victim, thread);
		thread->sched.need_migrate = SCHED_MIGRATING;
		return thread;
	}

	/* Nothing on the CPU can be taken. */
	return NULL;
}

/*
 * Names a migrating thread's new CPU where its wakers look for it.
 *
 * A waker locks the CPU the thread names and marks a migrating thread's
 * wakeup pending under that lock.  The name changes under the old CPU's
 * lock, so every waker holding it has finished, and every later one
 * re-reads the name and takes the new CPU's lock: the migration then ends
 * under that lock without racing a waker's update of the migration word.
 */
static void
retarget_migrating(
	struct thread *thread,
	struct sched_cpu *old_cpu,
	hal_cpu_id_t target)
{
	unsigned long irq;

	/* Publishes the new CPU while no waker holds the old one's lock. */
	irq = spin_lock_irqsave(&old_cpu->lock);
	atomic_raw_store_release((volatile unsigned *)&thread->sched.cpu,
	    (unsigned)target);
	spin_unlock_irqrestore(&old_cpu->lock, irq);
}

/*
 * Moves one runnable thread from a busier CPU to this idle one.
 *
 * The HAL refuses a task whose stack is still being left on its old CPU,
 * so a thread that has only just been descheduled goes back where it was.
 */
static int
steal_runnable(
	hal_cpu_id_t self)
{
	struct sched_cpu *mine;
	struct sched_cpu *victim;
	struct thread *thread;
	unsigned offset;
	hal_cpu_id_t id;
	unsigned long irq;
	int error;
	int online;
	int backlog;

	/* Visits the other online CPUs, starting after this one. */
	mine = sched_cpu_state(self);
	thread = NULL;
	victim = NULL;
	for (offset = 1;
	     offset < scheduler_cpu_count && thread == NULL;
	     offset++) {
		id = (hal_cpu_id_t)((self + offset) % scheduler_cpu_count);
		online = cpu_online(id);
		if (!online)
			continue;

		/* Skips a CPU with nothing queued, without taking its lock. */
		victim = sched_cpu_state(id);
		backlog = cpu_has_backlog(victim);
		if (!backlog)
			continue;

		/* Takes a queued thread off the victim as a migrating one. */
		irq = spin_lock_irqsave(&victim->lock);
		thread = take_stealable_locked(victim);
		spin_unlock_irqrestore(&victim->lock, irq);

		/* Tries the next CPU when this one had nothing to give. */
		if (thread == NULL)
			continue;

		/* Moves the task here; a refusal puts the thread back where it was. */
		error = hal_task_transfer(thread->task, self);
		if (error != HAL_OK) {
			irq = spin_lock_irqsave(&victim->lock);
			thread->sched.need_migrate = 0;
			queue_append(&victim->run[thread->sched.priority], thread,
			    SCHED_QUEUE_RUN);
			victim->need_resched = 1;
			spin_unlock_irqrestore(&victim->lock, irq);
			thread = NULL;
		}
	}

	/* Reports that no CPU had a thread to give. */
	if (thread == NULL)
		return 0;

	/* Sends later wakers to this CPU, under the lock they now take. */
	retarget_migrating(thread, victim, self);

	/* Queues the thread on this CPU, ending its migration. */
	irq = spin_lock_irqsave(&mine->lock);
	thread->sched.last_cpu = id;
	thread->sched.need_migrate = 0;
	queue_append(&mine->run[thread->sched.priority], thread,
	    SCHED_QUEUE_RUN);
	mine->need_resched = 1;
	spin_unlock_irqrestore(&mine->lock, irq);

	/* Succeeded: this CPU has work to run. */
	return 1;
}

/*
 * Turns the latency diagnostic on when the boot line holds kern.perf=1.
 *
 * Off by default: it adds a counter reading to every switch and to every
 * entry of the VM metadata lock, and a line to the kernel log every two
 * seconds that would push the boot messages out of the ring.
 */
static void
perf_init(
	void)
{
	const char *line;
	uint64_t counter;
	uint64_t frequency;
	bool available;
	int asked;

	/* Looks for kern.perf=1 among the boot parameters. */
	line = kern_boot_handoff(SCHED_PERF_COMMAND_LINE);
	asked = kern_boot_parameters_token_present(line, SCHED_PERF_TOKEN);
	if (!asked)
		return;

	/* The diagnostic measures with the platform's monotonic counter. */
	available = kern_rtc_read_counter(&counter, &frequency);
	if (!available) {
		kern_logf("sched: kern.perf=1 asked, but the platform has no counter\n");
		return;
	}
	if (frequency == 0) {
		kern_logf("sched: kern.perf=1 asked, but the counter has no frequency\n");
		return;
	}

	/* Starts the first window now, then lets every path measure. */
	sched_perf_frequency = frequency;
	sched_perf_report_counter = counter;
	atomic_u64_store_release(&sched_perf_report_tick, clock_ticks());
	sched_perf_active = 1;
	kern_logf("sched: latency diagnostic on (kern.perf=1), one report every %u ms\n", SCHED_PERF_REPORT_MS);
}

/*
 * Stamps a thread that a wakeup or a preemption made runnable, so that the
 * latency diagnostic measures its wait to run.  SCHED_PERF_NONE clears the
 * stamp.  The CPU lock is held.
 */
static void
perf_mark_runnable_locked(
	struct sched_cpu *cpu,
	struct thread *thread,
	unsigned kind)
{
	/* Nothing is measured with the diagnostic off. */
	if (!sched_perf_active)
		return;

	/* Clears a stamp a reused thread may still carry. */
	if (kind == SCHED_PERF_NONE) {
		thread->sched.perf_runnable_at = 0;
		thread->sched.perf_runnable_kind = SCHED_PERF_NONE;
		return;
	}

	/* Records when, why and on which CPU the wait began, with that CPU's switch count. */
	thread->sched.perf_runnable_at = sched_perf_counter();
	thread->sched.perf_runnable_kind = kind;
	thread->sched.perf_runnable_cpu = (hal_cpu_id_t)(cpu - scheduler_cpus);
	thread->sched.perf_runnable_switches = cpu->perf_switch_count;
}

/*
 * Accounts a switch for the latency diagnostic.
 *
 * A stamped thread that now runs ends its wait, the switch is counted for
 * the CPU and against the thread switched away from, and a woken thread of
 * at least the new thread's priority that is still waiting is counted as
 * dropped: every caller clears the CPU's preemption mark with this switch,
 * so that thread now waits for the new one to sleep, go back to user mode
 * after another wakeup, or use up its quantum.  The CPU lock is held.
 */
static void
perf_switch_locked(
	struct sched_cpu *cpu,
	struct thread *current,
	struct thread *next)
{
	hal_cpu_id_t id;
	uint64_t counts;
	int ahead;
	int priority;
	int last_priority;
	int waiting;

	/* Nothing is measured with the diagnostic off. */
	if (!sched_perf_active)
		return;

	/* Keeping the same thread is not a switch. */
	if (next == current)
		return;

	/* Ends the measured wait of a thread a wakeup or a preemption made runnable. */
	id = (hal_cpu_id_t)(cpu - scheduler_cpus);
	if (next->sched.perf_runnable_at != 0) {
		counts = sched_perf_counter() - next->sched.perf_runnable_at;

		/* The threads its CPU ran first; unknown when another CPU took it. */
		ahead = -1;
		if (next->sched.perf_runnable_cpu == id)
			ahead = (int)(cpu->perf_switch_count - next->sched.perf_runnable_switches);

		/* Counts the wait as a preempted or a woken one. */
		if (next->sched.perf_runnable_kind == SCHED_PERF_PREEMPTED) {
			cpu->perf.preempted++;
			perf_note_longest(&cpu->perf.preempted_longest, counts, next, ahead);
		} else {
			cpu->perf.woken++;
			perf_note_longest(&cpu->perf.woken_longest, counts, next, ahead);
		}

		next->sched.perf_runnable_at = 0;
		next->sched.perf_runnable_kind = SCHED_PERF_NONE;
	}

	/*
	 * Counts the switch.  The CPU's count since boot is what a waiting
	 * thread's stamp is compared with; the thread's own count tells the
	 * metadata lock how often its holder lost the CPU.
	 */
	cpu->perf.switches++;
	cpu->perf_switch_count++;
	if (current != NULL)
		current->sched.perf_switch_outs++;

	/* The idle thread runs only when nothing is queued. */
	if ((next->flags & THREAD_FLAG_IDLE) != 0)
		return;

	/* Looks for a woken thread of at least the new thread's priority. */
	last_priority = next->sched.priority;
	if (last_priority > SCHED_PRIOR_LOW)
		last_priority = SCHED_PRIOR_LOW;
	waiting = 0;
	for (priority = SCHED_PRIOR_HIGH; priority <= last_priority; priority++) {
		if (cpu->woken[priority].head != NULL)
			waiting = 1;
	}

	/* Counts the switch that left it waiting without the mark. */
	if (waiting)
		cpu->perf.dropped++;
}

/*
 * Keeps a wait as the longest of its kind when it is longer than the one
 * kept: how long, when it ended, which thread, and how many ran first.
 */
static void
perf_note_longest(
	struct sched_perf_longest *longest,
	uint64_t counts,
	const struct thread *thread,
	int ahead)
{
	/* A wait no longer than the kept one changes nothing. */
	if (counts <= longest->counts)
		return;

	/* Replaces the kept wait. */
	longest->counts = counts;
	longest->end_ms = kern_ticks_to_ms(clock_ticks());
	sched_perf_identify(thread, &longest->who);
	longest->ahead = ahead;
}

/*
 * Keeps one CPU's longest wait of a kind when it is longer than the total's.
 */
static void
perf_merge_longest(
	struct sched_perf_longest *total,
	const struct sched_perf_longest *longest)
{
	/* A wait no longer than the kept one changes nothing. */
	if (longest->counts <= total->counts)
		return;

	/* Takes the CPU's wait. */
	*total = *longest;
}

/*
 * Writes the latency diagnostic's report when its interval has passed.
 *
 * The thread that moves the report tick forward writes it; one that loses
 * the race to another CPU goes on.
 */
static void
perf_report_if_due(
	void)
{
	uint64_t now;
	uint64_t last;
	uint64_t interval;
	int claimed;

	/* Waits for the interval to pass since the last report. */
	now = clock_ticks();
	last = atomic_u64_load_acquire(&sched_perf_report_tick);
	interval = KERN_MS_TO_TICKS(SCHED_PERF_REPORT_MS);
	if (now - last < interval)
		return;

	/* Claims the report. */
	claimed = atomic_u64_compare_exchange(&sched_perf_report_tick, &last, now);
	if (!claimed)
		return;

	/* Writes it. */
	perf_report();
}

/*
 * Writes one line of the latency diagnostic and starts a new window.
 *
 * The line gives the uptime and the window's length, the switches, the
 * woken and the preempted threads that ran with the longest wait of each
 * (microseconds, the uptime when it ended, pid/tid/command, and how many
 * threads ran first), the dropped preemption marks, and the metadata
 * lock's part: its entries, the waits for it and the longest one, and its
 * longest hold, each with the address vm_metadata_enter() was called from.
 * report_us is what the previous report cost the thread that wrote it.
 * The caller is on its way back to user mode and holds nothing.
 */
static void
perf_report(
	void)
{
	struct sched_perf_window total;
	struct sched_perf_window window;
	struct vm_metadata_perf meta;
	struct sched_cpu *cpu;
	char meta_text[256];
	uint64_t started;
	uint64_t finished;
	uint64_t previous_cost;
	uint64_t counter;
	uint64_t elapsed_ms;
	unsigned long irq;
	unsigned online;
	hal_cpu_id_t id;
	int online_cpu;
	int taken;

	/* Starts measuring what this report costs the thread that writes it. */
	started = sched_perf_counter();
	previous_cost = atomic_u64_load_acquire(&sched_perf_report_cost);

	/* Gathers every online CPU's window and starts a new one there. */
	kern_memset(&total, 0, sizeof(total));
	online = 0;
	for (id = 0; id < scheduler_cpu_count; id++) {
		online_cpu = cpu_online(id);
		if (!online_cpu)
			continue;

		/* Takes the CPU's window and empties it. */
		cpu = sched_cpu_state(id);
		irq = spin_lock_irqsave(&cpu->lock);

		window = cpu->perf;
		kern_memset(&cpu->perf, 0, sizeof(cpu->perf));

		spin_unlock_irqrestore(&cpu->lock, irq);

		/* Adds the CPU's counts and keeps the longest waits. */
		online++;
		total.switches += window.switches;
		total.woken += window.woken;
		total.preempted += window.preempted;
		total.dropped += window.dropped;
		perf_merge_longest(&total.woken_longest, &window.woken_longest);
		perf_merge_longest(&total.preempted_longest, &window.preempted_longest);
	}

	/* Measures the window. */
	counter = sched_perf_counter();
	elapsed_ms = sched_perf_counts_to_us(counter - sched_perf_report_counter) / 1000U;
	sched_perf_report_counter = counter;

	/* Writes the metadata lock's part, which a held lock leaves to the next report. */
	taken = vm_metadata_perf_take(&meta);
	if (taken) {
		kern_snprintf(meta_text, sizeof(meta_text),
		    "meta takes=%llu waits=%llu max_us=%llu end_ms=%llu who=%d/%d/%s at=%p"
		    " | hold max_us=%llu end_ms=%llu who=%d/%d/%s at=%p switched=%llu",
		    (unsigned long long)meta.takes,
		    (unsigned long long)meta.waits,
		    (unsigned long long)sched_perf_counts_to_us(meta.wait_counts),
		    (unsigned long long)meta.wait_end_ms,
		    meta.waiter.pid,
		    meta.waiter.tid,
		    meta.waiter.name,
		    meta.wait_from,
		    (unsigned long long)sched_perf_counts_to_us(meta.hold_counts),
		    (unsigned long long)meta.hold_end_ms,
		    meta.holder.pid,
		    meta.holder.tid,
		    meta.holder.name,
		    meta.hold_from,
		    (unsigned long long)meta.hold_switched);
	} else {
		kern_snprintf(meta_text, sizeof(meta_text), "meta held (left to the next report)");
	}

	/* Writes the line, with what the previous report cost its writer. */
	kern_logf("kern: perf at_ms=%llu ms=%llu cpus=%u switches=%llu report_us=%llu"
	    " | woken=%llu max_us=%llu end_ms=%llu who=%d/%d/%s ahead=%d"
	    " | preempted=%llu max_us=%llu end_ms=%llu who=%d/%d/%s ahead=%d dropped=%llu"
	    " | %s\n",
	    (unsigned long long)kern_ticks_to_ms(clock_ticks()),
	    (unsigned long long)elapsed_ms,
	    online,
	    (unsigned long long)total.switches,
	    (unsigned long long)sched_perf_counts_to_us(previous_cost),
	    (unsigned long long)total.woken,
	    (unsigned long long)sched_perf_counts_to_us(total.woken_longest.counts),
	    (unsigned long long)total.woken_longest.end_ms,
	    total.woken_longest.who.pid,
	    total.woken_longest.who.tid,
	    total.woken_longest.who.name,
	    total.woken_longest.ahead,
	    (unsigned long long)total.preempted,
	    (unsigned long long)sched_perf_counts_to_us(total.preempted_longest.counts),
	    (unsigned long long)total.preempted_longest.end_ms,
	    total.preempted_longest.who.pid,
	    total.preempted_longest.who.tid,
	    total.preempted_longest.who.name,
	    total.preempted_longest.ahead,
	    (unsigned long long)total.dropped,
	    meta_text);

#if SCHED_WAKE_LATENCY
	/* The build with the wake histogram writes it with the same window. */
	sched_wake_latency_report();
#endif

	/* Keeps this report's cost for the next line. */
	finished = sched_perf_counter();
	atomic_u64_store_release(&sched_perf_report_cost, finished - started);
}
