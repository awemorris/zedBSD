/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include "kern/sched.h"
#include "kern/atomic.h"
#include "kern/thread.h"
#include "kern/lock.h"
#include "kern/kmem.h"

#include <errno.h>
#include <hal/hal.h>
#include <string.h>

#define SCHED_MIGRATING 0x00000001U
#define SCHED_WAKE_PENDING 0x00000002U
#define SCHED_ONLINE_TIMEOUT 10000000U

struct sched_cpu {
	struct spinlock lock;
	struct sched_queue run[SCHED_PRIOR_LEVELS];
	struct sched_queue sleep;
	struct thread *idle;
	struct thread *retired;
	unsigned need_resched;
	unsigned online;
};

static struct sched_cpu *scheduler_cpus;
static unsigned scheduler_cpu_count;
static struct hal_cpu_mask scheduler_online_mask;
static volatile uint64_t scheduler_ticks;
static volatile unsigned scheduler_round_robin;

static struct sched_cpu *
sched_cpu_state(hal_cpu_id_t cpu)
{
	if (scheduler_cpus == NULL || cpu >= scheduler_cpu_count)
		HAL_FATAL("invalid scheduler CPU");
	return &scheduler_cpus[cpu];
}

static void
queue_append(struct sched_queue *queue, struct thread *thread, unsigned kind)
{
	thread->sched.prev = queue->tail;
	thread->sched.next = NULL;
	if (queue->tail != NULL)
		queue->tail->sched.next = thread;
	else
		queue->head = thread;
	queue->tail = thread;
	queue->count++;
	thread->sched.queue_kind = kind;
}

static void
queue_remove(struct sched_queue *queue, struct thread *thread)
{
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
	thread->sched.next = thread->sched.prev = NULL;
	thread->sched.queue_kind = SCHED_QUEUE_NONE;
}

static void
queue_remove_thread(struct sched_cpu *cpu, struct thread *thread)
{
	if (thread->sched.queue_kind == SCHED_QUEUE_RUN)
		queue_remove(&cpu->run[thread->sched.priority], thread);
	else if (thread->sched.queue_kind == SCHED_QUEUE_SLEEP)
		queue_remove(&cpu->sleep, thread);
}

static struct thread *
pick_next_locked(struct sched_cpu *cpu)
{
	int priority;

	for (priority = SCHED_PRIOR_HIGH; priority <= SCHED_PRIOR_LOW;
	    priority++) {
		struct thread *next = cpu->run[priority].head;
		if (next != NULL) {
			queue_remove(&cpu->run[priority], next);
			return next;
		}
	}
	return NULL;
}

static void
complete_retired(struct sched_cpu *cpu)
{
	struct thread *thread = cpu->retired;

	if (thread == NULL)
		return;
	cpu->retired = NULL;
	thread_sched_retired(thread);
}

static int
cpu_online(hal_cpu_id_t cpu)
{
	return cpu < scheduler_cpu_count && atomic_raw_load_acquire(
	    &scheduler_cpus[cpu].online) != 0;
}

static hal_cpu_id_t
choose_cpu(void)
{
	unsigned start, offset;

	start = atomic_raw_fetch_add_relaxed(&scheduler_round_robin, 1U);
	for (offset = 0; offset < scheduler_cpu_count; offset++) {
		hal_cpu_id_t cpu = (start + offset) % scheduler_cpu_count;
		if (cpu_online(cpu))
			return cpu;
	}
	return HAL_CPU_MAX;
}

static void
notify_cpu(hal_cpu_id_t cpu)
{
	int error;

	if (cpu == hal_cpu_current())
		return;
	error = hal_cpu_notify(cpu);
	if (error != HAL_OK)
		HAL_FATAL("scheduler CPU notification failed");
}

void
sched_init(void)
{
	unsigned cpu;

	if (curthread == NULL || curthread != &thread0)
		HAL_FATAL("scheduler before process0");
	scheduler_cpu_count = hal_cpu_count();
	if (scheduler_cpu_count == 0 || scheduler_cpu_count > HAL_CPU_MAX)
		HAL_FATAL("invalid scheduler CPU count");
	scheduler_cpus = kern_calloc(scheduler_cpu_count,
	    sizeof(*scheduler_cpus));
	if (scheduler_cpus == NULL)
		HAL_FATAL("scheduler allocation failed");
	for (cpu = 0; cpu < scheduler_cpu_count; cpu++)
		spin_init(&scheduler_cpus[cpu].lock, LOCK_RANK_SCHEDULER,
		    "scheduler CPU");
	hal_cpu_mask_zero(&scheduler_online_mask);
	scheduler_cpus[0].idle = &thread0;
	atomic_raw_store_release(&scheduler_cpus[0].online, 1U);
	hal_cpu_mask_set(&scheduler_online_mask, 0);
	thread0.state = THREAD_RUNNING;
	thread0.sched.cpu = 0;
	thread0.sched.last_cpu = 0;
	thread0.sched.quantum = SCHED_QUANTUM_TICKS;
	atomic_u64_store_release(&scheduler_ticks, 0);
}

int
sched_prepare_thread(struct thread *thread)
{
	hal_cpu_id_t cpu;
	int error;

	if (thread == NULL || thread->task == NULL ||
	    thread->state != THREAD_NEW)
		return EINVAL;
	cpu = choose_cpu();
	if (cpu == HAL_CPU_MAX)
		return EAGAIN;
	error = hal_task_transfer(thread->task, cpu);
	if (error != HAL_OK)
		return error == HAL_ERR_NOMEM ? ENOMEM : EBUSY;
	thread->sched.cpu = cpu;
	thread->sched.last_cpu = cpu;
	return 0;
}

void
sched_add(struct thread *thread)
{
	struct sched_cpu *cpu;
	unsigned long irq;

	if (thread == NULL || thread->sched.cpu >= scheduler_cpu_count ||
	    thread->sched.queue_kind != SCHED_QUEUE_NONE ||
	    thread->sched.priority < SCHED_PRIOR_HIGH ||
	    thread->sched.priority > SCHED_PRIOR_LOW ||
	    (thread->state != THREAD_NEW && thread->state != THREAD_SLEEPING))
		HAL_FATAL("invalid sched_add");
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

void
sched_unlink(struct thread *thread)
{
	struct sched_cpu *cpu;
	unsigned long irq;
	hal_cpu_id_t id;

	if (thread == NULL)
		return;
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

void
sched_wakeup(struct thread *thread)
{
	struct sched_cpu *cpu;
	unsigned long irq;
	hal_cpu_id_t id;

	if (thread == NULL)
		return;
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
	if ((thread->sched.need_migrate & SCHED_MIGRATING) != 0) {
		thread->sched.need_migrate |= SCHED_WAKE_PENDING;
		spin_unlock_irqrestore(&cpu->lock, irq);
		return;
	}
	if (thread->state != THREAD_SLEEPING) {
		spin_unlock_irqrestore(&cpu->lock, irq);
		return;
	}
	queue_remove_thread(cpu, thread);
	thread->state = THREAD_RUNNABLE;
	thread->sched.wakeup_tick = 0;
	thread->sched.quantum = SCHED_QUANTUM_TICKS;
	queue_append(&cpu->run[thread->sched.priority], thread,
	    SCHED_QUEUE_RUN);
	cpu->need_resched = 1;
	spin_unlock_irqrestore(&cpu->lock, irq);
	notify_cpu(id);
}

void sched_awake_from_sleep(struct thread *thread) { sched_wakeup(thread); }

static void
switch_without_enqueue(void)
{
	struct thread *current = curthread, *next;
	hal_cpu_id_t id = hal_cpu_current();
	struct sched_cpu *cpu = sched_cpu_state(id);
	unsigned long irq = spin_lock_irqsave(&cpu->lock);

	next = pick_next_locked(cpu);
	if (next == NULL) {
		if (current != NULL && current->state == THREAD_RUNNING) {
			spin_unlock_irqrestore(&cpu->lock, irq);
			return;
		}
		next = cpu->idle;
		if (next == NULL)
			HAL_FATAL("scheduler CPU has no idle thread");
	}
	next->state = THREAD_RUNNING;
	next->sched.last_cpu = id;
	next->sched.quantum = SCHED_QUANTUM_TICKS;
	cpu->need_resched = 0;
	spin_unlock_irqrestore(&cpu->lock, irq);
	if (next != current)
		hal_task_context_switch(next->task);
	complete_retired(cpu);
}

void
sched_switch(void)
{
	switch_without_enqueue();
}

void
sched_yield(void)
{
	bool enabled = hal_irq_disable();
	struct thread *current = curthread, *next;
	hal_cpu_id_t id = hal_cpu_current();
	struct sched_cpu *cpu = sched_cpu_state(id);
	unsigned long ignored = spin_lock_irqsave(&cpu->lock);

	(void)ignored;
	if (current != NULL && current->state == THREAD_RUNNING &&
	    (current->flags & THREAD_FLAG_IDLE) == 0) {
		if (current->sched.cpu != id)
			HAL_FATAL("running thread on wrong scheduler CPU");
		current->state = THREAD_RUNNABLE;
		current->sched.quantum = SCHED_QUANTUM_TICKS;
		queue_append(&cpu->run[current->sched.priority], current,
		    SCHED_QUEUE_RUN);
	}
	next = pick_next_locked(cpu);
	if (next == NULL)
		next = current != NULL && current->state == THREAD_RUNNING ?
		    current : cpu->idle;
	if (next == NULL)
		HAL_FATAL("yield without idle thread");
	next->state = THREAD_RUNNING;
	next->sched.last_cpu = id;
	next->sched.quantum = SCHED_QUANTUM_TICKS;
	cpu->need_resched = 0;
	spin_unlock(&cpu->lock);
	if (next != current)
		hal_task_context_switch(next->task);
	complete_retired(cpu);
	if (enabled)
		hal_irq_enable();
}

void
sched_exit_current(void)
{
	struct thread *current = curthread, *next;
	hal_cpu_id_t id = hal_cpu_current();
	struct sched_cpu *cpu = sched_cpu_state(id);
	unsigned long ignored;

	(void)hal_irq_disable();
	if (current == NULL || (current->flags & THREAD_FLAG_IDLE) != 0 ||
	    current->sched.cpu != id)
		HAL_FATAL("invalid scheduler exit");
	/* A newly started task does not return through the previous task's
	 * hal_task_context_switch() call.  Retire that predecessor before this
	 * task is allowed to become the next retirement candidate. */
	complete_retired(cpu);
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
	next->sched.last_cpu = id;
	next->sched.quantum = SCHED_QUANTUM_TICKS;
	cpu->need_resched = 0;
	cpu->retired = current;
	spin_unlock(&cpu->lock);
	hal_task_context_switch(next->task);
	complete_retired(cpu);
	HAL_FATAL("retired thread resumed");
	__builtin_unreachable();
}

void
sched_clock_cpu(hal_cpu_id_t id, uint64_t now)
{
	struct sched_cpu *cpu;
	struct thread *thread;
	unsigned long irq;
	int preempt = 0;

	if (id != hal_cpu_current() || !cpu_online(id))
		return;
	if (id == 0)
		atomic_u64_store_release(&scheduler_ticks, now);
	cpu = sched_cpu_state(id);
	irq = spin_lock_irqsave(&cpu->lock);
	thread = cpu->sleep.head;
	while (thread != NULL) {
		struct thread *next = thread->sched.next;
		if (thread->sched.wakeup_tick != 0 &&
		    thread->sched.wakeup_tick <= now) {
			queue_remove(&cpu->sleep, thread);
			thread->state = THREAD_RUNNABLE;
			thread->sched.wakeup_tick = 0;
			queue_append(&cpu->run[thread->sched.priority], thread,
			    SCHED_QUEUE_RUN);
			cpu->need_resched = 1;
		}
		thread = next;
	}
	thread = curthread;
	if (thread != NULL && thread->state == THREAD_RUNNING &&
	    (thread->flags & THREAD_FLAG_IDLE) == 0 &&
	    thread->sched.quantum != 0 && --thread->sched.quantum == 0)
		preempt = 1;
	spin_unlock_irqrestore(&cpu->lock, irq);
	if (preempt)
		sched_yield();
}

void kernel_yield(void) { sched_yield(); }

void
sched_sleep(uint64_t timeout_tick)
{
	bool enabled = hal_irq_disable();
	struct thread *thread = curthread;
	struct sched_cpu *cpu;
	unsigned long ignored;

	if (thread == NULL || thread->sched.cpu != hal_cpu_current())
		HAL_FATAL("invalid scheduler sleep");
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

void
sched_sleep_locked(uint64_t timeout_tick, struct spinlock *condition_lock)
{
	struct thread *thread = curthread;
	struct sched_cpu *cpu;

	(void)hal_irq_disable();
	if (thread == NULL || condition_lock == NULL ||
	    thread->sched.cpu != hal_cpu_current())
		HAL_FATAL("invalid locked sleep");
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
}

uint64_t
sched_ticks(void)
{
	return atomic_u64_load_acquire(&scheduler_ticks);
}

int
sched_has_runnable(void)
{
	struct sched_cpu *cpu = sched_cpu_state(hal_cpu_current());
	unsigned long irq = spin_lock_irqsave(&cpu->lock);
	int priority, found = 0;

	for (priority = SCHED_PRIOR_HIGH; priority <= SCHED_PRIOR_LOW;
	    priority++)
		if (cpu->run[priority].head != NULL) {
			found = 1;
			break;
		}
	spin_unlock_irqrestore(&cpu->lock, irq);
	return found;
}

void
sched_idle(void)
{
	struct thread *idle = curthread;
	hal_cpu_id_t cpu = hal_cpu_current();

	if (idle == NULL || (idle->flags & THREAD_FLAG_IDLE) == 0 ||
	    idle->sched.cpu != cpu)
		HAL_FATAL("invalid idle context");
	for (;;) {
		(void)hal_irq_disable();
		if (sched_has_runnable())
			sched_switch();
		if (curthread != idle)
			HAL_FATAL("idle resumed on foreign task");
		hal_cpu_idle();
		sched_switch();
	}
}

void
sched_secondary_init(hal_cpu_id_t id)
{
	struct sched_cpu *cpu;
	struct thread *idle = curthread;

	if (id == 0 || id != hal_cpu_current() || id >= scheduler_cpu_count ||
	    idle == NULL || (idle->flags & THREAD_FLAG_IDLE) == 0 ||
	    idle->sched.cpu != id)
		HAL_FATAL("invalid secondary scheduler entry");
	cpu = sched_cpu_state(id);
	spin_lock(&cpu->lock);
	if (cpu->idle != NULL || cpu->online)
		HAL_FATAL("secondary scheduler initialized twice");
	cpu->idle = idle;
	atomic_raw_store_release(&cpu->online, 1U);
	(void)atomic_u64_fetch_or_release(
	    &scheduler_online_mask.bits[id / 64U],
	    (uint64_t)1U << (id % 64U));
	spin_unlock(&cpu->lock);
	hal_irq_enable();
	sched_idle();
}

int
sched_wait_others_online(void)
{
	struct hal_cpu_mask ready;
	unsigned timeout;
	hal_cpu_id_t cpu;

	hal_cpu_ready_mask(&ready);
	for (timeout = 0; timeout < SCHED_ONLINE_TIMEOUT; timeout++) {
		for (cpu = 0; cpu < scheduler_cpu_count; cpu++)
			if (hal_cpu_mask_test(&ready, cpu) &&
			    !hal_cpu_mask_test(&scheduler_online_mask, cpu))
				break;
		if (cpu == scheduler_cpu_count)
			return 0;
		hal_compiler_barrier();
	}
	return ETIMEDOUT;
}

int
sched_set_cpu(struct thread *thread, hal_cpu_id_t target)
{
	struct sched_cpu *old_cpu, *new_cpu;
	hal_cpu_id_t old;
	unsigned old_kind, pending;
	unsigned long irq;
	int error;

	if (thread == NULL || thread->task == NULL || !cpu_online(target) ||
	    thread->state == THREAD_RUNNING || thread->state == THREAD_ZOMBIE ||
	    thread->state == THREAD_DEAD)
		return EINVAL;
	old = thread->sched.cpu;
	if (old == target)
		return 0;
	if (old >= scheduler_cpu_count)
		return EINVAL;
	old_cpu = sched_cpu_state(old);
	irq = spin_lock_irqsave(&old_cpu->lock);
	if (thread->sched.cpu != old || thread->sched.need_migrate != 0) {
		spin_unlock_irqrestore(&old_cpu->lock, irq);
		return EBUSY;
	}
	old_kind = thread->sched.queue_kind;
	queue_remove_thread(old_cpu, thread);
	thread->sched.need_migrate = SCHED_MIGRATING;
	spin_unlock_irqrestore(&old_cpu->lock, irq);

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
		return error == HAL_ERR_BUSY ? EBUSY : EIO;
	}

	new_cpu = sched_cpu_state(target);
	irq = spin_lock_irqsave(&new_cpu->lock);
	pending = thread->sched.need_migrate & SCHED_WAKE_PENDING;
	thread->sched.cpu = target;
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
	return 0;
}

void
sched_cpu_notify(hal_cpu_id_t id)
{
	struct sched_cpu *cpu;
	unsigned long irq;
	int runnable;

	if (id != hal_cpu_current() || !cpu_online(id))
		return;
	cpu = sched_cpu_state(id);
	irq = spin_lock_irqsave(&cpu->lock);
	runnable = cpu->need_resched != 0;
	spin_unlock_irqrestore(&cpu->lock, irq);
	if (runnable)
		sched_yield();
}

#ifdef ZEDBSD_SCHED_TEST
int
sched_test_cpu_online(hal_cpu_id_t id, struct thread *idle)
{
	struct sched_cpu *cpu;
	if (id == 0 || id >= scheduler_cpu_count || idle == NULL)
		return EINVAL;
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
	return 0;
}
#endif
