/*
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#include "kern/thread.h"
#include "kern/process.h"
#include "kern/vmspace.h"
#include "kern/kmem.h"

#include <errno.h>
#include <hal/hal.h>
#include <string.h>

struct thread thread0;
static tid_t next_tid = 1;

struct thread *
thread_current(void)
{
	hal_task_t task = hal_task_get_current();
	return task != NULL ? hal_task_get_private(task) : NULL;
}

static void
attach_thread(struct process *process, struct thread *thread)
{
	thread->proc_next = process->threads;
	process->threads = thread;
	process->thread_count++;
}

int
thread_create(struct process *process, uintptr_t entry, uintptr_t user_sp,
	      struct thread **result)
{
	struct thread *thread;

	if (process == NULL || process->vmspace == NULL ||
	    process->vmspace == &kernel_vmspace ||
	    !vmspace_user_range_valid(entry, 1) ||
	    !vmspace_user_range_valid(user_sp, 1) || result == NULL)
		return EINVAL;
	thread = kern_calloc(1, sizeof(*thread));
	if (thread == NULL)
		return ENOMEM;
	thread->tid = next_tid++;
	thread->proc = process;
	thread->state = THREAD_NEW;
	thread->sched.priority = SCHED_PRIORITY_DEFAULT;
	thread->sched.quantum = SCHED_QUANTUM_TICKS;
	thread->task = hal_task_create(process->vmspace->space,
		(void (*)(void *))entry, NULL, (void *)user_sp);
	if (thread->task == NULL) {
		kern_free(thread);
		return ENOMEM;
	}
	hal_task_set_private(thread->task, thread);
	attach_thread(process, thread);
	*result = thread;
	return 0;
}

int
thread_fork(struct process *process, hal_task_t task, struct thread **result)
{
	struct thread *thread;
	if (process == NULL || process->vmspace == NULL || task == NULL ||
	    result == NULL || hal_task_get_space(task) != process->vmspace->space)
		return EINVAL;
	thread = kern_calloc(1, sizeof(*thread));
	if (thread == NULL)
		return ENOMEM;
	thread->tid = next_tid++;
	thread->proc = process;
	thread->task = task;
	thread->state = THREAD_NEW;
	thread->sched.priority = SCHED_PRIORITY_DEFAULT;
	thread->sched.quantum = SCHED_QUANTUM_TICKS;
	hal_task_set_private(task, thread);
	attach_thread(process, thread);
	*result = thread;
	return 0;
}

static void
kernel_thread_trampoline(void *argument)
{
	struct thread *thread = argument;
	thread->kernel_entry(thread->kernel_arg);
	thread_exit(0);
}

int
kthread_create(void (*entry)(void *), void *arg, int priority,
	       struct thread **result)
{
	struct thread *thread;

	if (entry == NULL || result == NULL || priority < SCHED_PRIOR_HIGH ||
	    priority > SCHED_PRIOR_LOW)
		return EINVAL;
	thread = kern_calloc(1, sizeof(*thread));
	if (thread == NULL)
		return ENOMEM;
	thread->tid = next_tid++;
	thread->proc = &process0;
	thread->state = THREAD_NEW;
	thread->sched.priority = priority;
	thread->sched.quantum = SCHED_QUANTUM_TICKS;
	thread->kernel_entry = entry;
	thread->kernel_arg = arg;
	thread->task = hal_task_create(HAL_SPACE_SYS, kernel_thread_trampoline,
				       thread, NULL);
	if (thread->task == NULL) {
		kern_free(thread);
		return ENOMEM;
	}
	hal_task_set_private(thread->task, thread);
	attach_thread(&process0, thread);
	*result = thread;
	return 0;
}

void
thread_start(struct thread *thread)
{
	bool enabled;
	if (thread == NULL || thread->state != THREAD_NEW)
		return;
	enabled = hal_irq_disable();
	sched_add(thread);
	if (enabled)
		hal_irq_enable();
}

void
thread_exit(int status)
{
	struct thread *thread = curthread;
	bool enabled = hal_irq_disable();

	(void)enabled;
	if (thread == NULL)
		HAL_FATAL("thread_exit without current thread");
	thread->exit_status = status;
	thread->state = THREAD_ZOMBIE;
	sched_unlink(thread);
	sched_yield();
	HAL_FATAL("zombie thread resumed");
	__builtin_unreachable();
}

int
thread_wait(struct thread *thread, int *status)
{
	struct thread **link;
	if (thread == NULL || thread == curthread)
		return EINVAL;
	if (thread->state != THREAD_ZOMBIE)
		return EBUSY;
	if (status != NULL)
		*status = thread->exit_status;
	hal_task_set_private(thread->task, NULL);
	hal_task_destroy(thread->task);
	link = &thread->proc->threads;
	while (*link != NULL && *link != thread)
		link = &(*link)->proc_next;
	if (*link == thread)
		*link = thread->proc_next;
	thread->proc->thread_count--;
	thread->state = THREAD_DEAD;
	kern_free(thread);
	return 0;
}
