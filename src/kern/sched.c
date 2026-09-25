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

#include "kern/sched.h"
#include "kern/atomic.h"
#include "kern/thread.h"
#include "kern/process.h"
#include "kern/resource-limit.h"
#include "kern/signal.h"
#include "kern/lock.h"
#include "kern/kmem.h"
#include <kern/kcrt.h>

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
	kern_memset(&wake_latency, 0, sizeof(wake_latency));
}
#endif

#define SCHED_MIGRATING 0x00000001U
#define SCHED_WAKE_PENDING 0x00000002U
#define SCHED_ONLINE_TIMEOUT 10000000U

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

static void send_itimer_signal(struct process *process, int signo);
static struct sched_cpu *sched_cpu_state(hal_cpu_id_t cpu);
static void queue_append(struct sched_queue *queue, struct thread *thread, unsigned kind);
static void queue_remove(struct sched_queue *queue, struct thread *thread);
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

	next->state = THREAD_RUNNING;
#if SCHED_WAKE_LATENCY
	wake_latency_account(next);
#endif
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

	/* Charges the running thread and decides on preemption. */
	thread = curthread;
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

		/* Halts as a CPU that others may hand work to. */
		idle_mask_set(cpu);
		hal_cpu_idle();
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
	if (queue->count == 0)
		HAL_FATAL("scheduler queue underflow");
	queue->count--;
	thread->sched.next = NULL;
	thread->sched.prev = NULL;
	thread->sched.queue_kind = SCHED_QUEUE_NONE;
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
	(void)__atomic_fetch_and(
	    &scheduler_idle_mask.bits[cpu / 64U],
	    ~((uint64_t)1U << (cpu % 64U)),
	    __ATOMIC_RELEASE);
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
