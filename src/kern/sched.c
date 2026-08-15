/*
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#include "kern/sched.h"
#include "kern/boot-device.h"
#include "kern/thread.h"
#include "kern/lock.h"

#include <hal/hal.h>
#include <string.h>

static struct sched_queue run_queues[SCHED_PRIOR_LEVELS];
static struct sched_queue sleep_queue;
static uint64_t scheduler_ticks;

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

void
sched_init(void)
{
	memset(run_queues, 0, sizeof(run_queues));
	memset(&sleep_queue, 0, sizeof(sleep_queue));
	scheduler_ticks = 0;
	if (curthread == NULL || curthread != &thread0)
		HAL_FATAL("scheduler before process0");
	thread0.state = THREAD_RUNNING;
	thread0.sched.quantum = SCHED_QUANTUM_TICKS;
}

void
sched_add(struct thread *thread)
{
	if (thread == NULL || thread->sched.queue_kind != SCHED_QUEUE_NONE ||
	    thread->sched.priority < SCHED_PRIOR_HIGH ||
	    thread->sched.priority > SCHED_PRIOR_LOW ||
	    (thread->state != THREAD_NEW && thread->state != THREAD_SLEEPING))
		HAL_FATAL("invalid sched_add");
	thread->state = THREAD_RUNNABLE;
	thread->sched.quantum = SCHED_QUANTUM_TICKS;
	queue_append(&run_queues[thread->sched.priority], thread,
		     SCHED_QUEUE_RUN);
}

void
sched_unlink(struct thread *thread)
{
	if (thread == NULL)
		return;
	if (thread->sched.queue_kind == SCHED_QUEUE_RUN)
		queue_remove(&run_queues[thread->sched.priority], thread);
	else if (thread->sched.queue_kind == SCHED_QUEUE_SLEEP)
		queue_remove(&sleep_queue, thread);
}

void
sched_wakeup(struct thread *thread)
{
	if (thread == NULL || thread->state != THREAD_SLEEPING)
		return;
	sched_unlink(thread);
	sched_add(thread);
}

void sched_awake_from_sleep(struct thread *thread) { sched_wakeup(thread); }

void
sched_switch(void)
{
	struct thread *next = NULL;
	int priority;

	for (priority = SCHED_PRIOR_HIGH; priority <= SCHED_PRIOR_LOW;
	     priority++)
		if (run_queues[priority].head != NULL) {
			next = run_queues[priority].head;
			queue_remove(&run_queues[priority], next);
			break;
		}
	if (next == NULL) {
		if (curthread != NULL && curthread->state == THREAD_RUNNING)
			return;
		next = &thread0;
		if (curthread == next)
			return;
	}
	next->state = THREAD_RUNNING;
	next->sched.quantum = SCHED_QUANTUM_TICKS;
	hal_task_context_switch(next->task);
}

void
sched_yield(void)
{
	bool enabled = hal_irq_disable();
	struct thread *current = curthread;

	if (current != NULL && current->state == THREAD_RUNNING &&
	    (current->flags & THREAD_FLAG_IDLE) == 0) {
		current->state = THREAD_RUNNABLE;
		current->sched.quantum = SCHED_QUANTUM_TICKS;
		queue_append(&run_queues[current->sched.priority], current,
			     SCHED_QUEUE_RUN);
	}
	sched_switch();
	if (enabled)
		hal_irq_enable();
}

void
sched_clock(void)
{
	struct thread *thread = sleep_queue.head;
	scheduler_ticks++;
	while (thread != NULL) {
		struct thread *next = thread->sched.next;
		if (thread->sched.wakeup_tick != 0 &&
		    thread->sched.wakeup_tick <= scheduler_ticks)
			sched_awake_from_sleep(thread);
		thread = next;
	}
	if (curthread != NULL && curthread->state == THREAD_RUNNING &&
	    curthread->sched.quantum != 0 && --curthread->sched.quantum == 0)
		hal_reschedule_on_interrupt_return();
}

void
sched_sleep(uint64_t timeout_tick)
{
	bool enabled = hal_irq_disable();
	struct thread *thread = curthread;

	thread->state = THREAD_SLEEPING;
	thread->sched.wakeup_tick = timeout_tick;
	if (timeout_tick != 0)
		queue_append(&sleep_queue, thread, SCHED_QUEUE_SLEEP);
	sched_yield();
	if (enabled)
		hal_irq_enable();
}

void
sched_sleep_locked(uint64_t timeout_tick, struct spinlock *condition_lock)
{
	struct thread *thread = curthread;
	(void)hal_irq_disable();
	if (thread == NULL || condition_lock == NULL)
		HAL_FATAL("invalid locked sleep");
	thread->state = THREAD_SLEEPING;
	thread->sched.wakeup_tick = timeout_tick;
	if (timeout_tick != 0)
		queue_append(&sleep_queue, thread, SCHED_QUEUE_SLEEP);
	spin_unlock(condition_lock);
	sched_switch();
	spin_lock(condition_lock);
}

uint64_t sched_ticks(void) { return scheduler_ticks; }

int
sched_has_runnable(void)
{
	int priority;
	for (priority = SCHED_PRIOR_HIGH; priority <= SCHED_PRIOR_LOW; priority++)
		if (run_queues[priority].head != NULL)
			return 1;
	return 0;
}

void
sched_idle(void)
{
	thread0.flags |= THREAD_FLAG_IDLE;
	for (;;) {
		(void)hal_irq_disable();
		if (kern_boot_pending())
			kern_boot_execute_pending();
		if (sched_has_runnable())
			sched_switch();
		if (curthread != &thread0)
			HAL_FATAL("idle resumed on foreign task");
		hal_cpu_idle();
		if (kern_boot_pending())
			kern_boot_execute_pending();
		sched_switch();
	}
}
