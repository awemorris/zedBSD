/*
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#include "kern/process.h"
#include "kern/thread.h"
#include "kern/vmspace.h"
#include "kern/filedesc.h"
#include "kern/kmem.h"
#include "kern/namei.h"

#include <errno.h>
#include <hal/hal.h>
#include <string.h>

struct process process0;
static struct process *all_processes;
static pid_t next_pid = 1;

void
process_init(void)
{
	if (all_processes != NULL)
		return;
	memset(&process0, 0, sizeof(process0));
	memset(&thread0, 0, sizeof(thread0));
	process0.pid = 0;
	process0.state = PROCESS_RUNNING;
	process0.vmspace = &kernel_vmspace;
	process0.threads = &thread0;
	process0.thread_count = 1;
	thread0.tid = 0;
	thread0.proc = &process0;
	thread0.task = hal_task_get_current();
	thread0.state = THREAD_RUNNING;
	thread0.sched.priority = SCHED_PRIORITY_DEFAULT;
	thread0.sched.quantum = SCHED_QUANTUM_TICKS;
	hal_task_set_private(thread0.task, &thread0);
	all_processes = &process0;
}

struct process *
process_find(pid_t pid)
{
	struct process *process;
	for (process = all_processes; process != NULL; process = process->all_next)
		if (process->pid == pid && process->state != PROCESS_DEAD)
			return process;
	return NULL;
}

struct thread *
process_find_by_tid(tid_t tid)
{
	struct process *process;
	for (process = all_processes; process != NULL; process = process->all_next) {
		struct thread *thread;
		for (thread = process->threads; thread != NULL;
		     thread = thread->proc_next)
			if (thread->tid == tid && thread->state != THREAD_DEAD)
				return thread;
	}
	return NULL;
}

int
process_create(struct process *parent, pid_t requested_pid,
	       struct process **result)
{
	struct process *process;

	if (parent == NULL || result == NULL || requested_pid < 0)
		return EINVAL;
	if (requested_pid != 0 && process_find(requested_pid) != NULL)
		return EBUSY;
	process = kern_calloc(1, sizeof(*process));
	if (process == NULL)
		return ENOMEM;
	process->fd = filedesc_create();
	if (process->fd == NULL) {
		kern_free(process);
		return ENOMEM;
	}
	process->pid = requested_pid != 0 ? requested_pid : next_pid++;
	if (process->pid >= next_pid)
		next_pid = process->pid + 1;
	process->state = PROCESS_NEW;
	process->parent = parent;
	if (parent->cwdi != NULL) {
		int error = cwdinfo_clone(parent->cwdi, &process->cwdi);
		if (error != 0) {
			filedesc_destroy(process->fd);
			kern_free(process);
			return error;
		}
	}
	*result = process;
	return 0;
}

void
process_publish(struct process *process)
{
	if (process == NULL || process == &process0 || process->state != PROCESS_NEW)
		return;
	process->all_next = all_processes;
	all_processes = process;
	process->sibling = process->parent->children;
	process->parent->children = process;
	process->state = PROCESS_RUNNING;
}

void
process_attach_boot_cwd(struct cwdinfo *cwd)
{
	process0.cwdi = cwd;
}

void
process_free_mem(struct process *process)
{
	struct process **link;
	struct process **child_link;

	if (process == NULL || process == &process0 || process->thread_count != 0 ||
	    process == curthread->proc)
		return;
	if (process->vmspace != NULL)
		vmspace_free(process->vmspace);
	filedesc_destroy(process->fd);
	cwdinfo_release(process->cwdi);
	link = &all_processes;
	while (*link != NULL && *link != process)
		link = &(*link)->all_next;
	if (*link == process)
		*link = process->all_next;
	if (process->parent != NULL) {
		child_link = &process->parent->children;
		while (*child_link != NULL && *child_link != process)
			child_link = &(*child_link)->sibling;
		if (*child_link == process)
			*child_link = process->sibling;
	}
	process->state = PROCESS_DEAD;
	kern_free(process);
}

int
process_wait(struct process *process, int *status, char *result,
	     size_t result_capacity)
{
	struct thread *thread;
	bool enabled;
	int error;

	if (process == NULL || process == &process0 || curthread == NULL ||
	    process->parent != curthread->proc)
		return ECHILD;
	enabled = hal_irq_disable();
	while (process->state != PROCESS_ZOMBIE) {
		if (process->waiter != NULL && process->waiter != curthread) {
			if (enabled) hal_irq_enable();
			return EBUSY;
		}
		process->waiter = curthread;
		sched_sleep(0);
	}
	process->waiter = NULL;
	if (enabled)
		hal_irq_enable();
	thread = process->threads;
	if (thread == NULL)
		return ECHILD;
	error = thread_wait(thread, status);
	if (error != 0)
		return error;
	if (result != NULL && result_capacity != 0) {
		size_t length = process->result_length;
		if (length >= result_capacity)
			length = result_capacity - 1U;
		memcpy(result, process->result, length);
		result[length] = '\0';
	}
	process_free_mem(process);
	return 0;
}

int
process_quiesce_users(void)
{
	struct process *process;
	bool enabled;

	if (curthread == NULL || curthread->proc != &process0)
		return EBUSY;
	enabled = hal_irq_disable();
	for (process = process0.children; process != NULL;
	     process = process->sibling) {
		if (process->state != PROCESS_ZOMBIE) {
			if (enabled) hal_irq_enable();
			return EBUSY;
		}
	}
	while (process0.children != NULL) {
		struct process *child = process0.children;
		while (child->threads != NULL) {
			int error = thread_wait(child->threads, NULL);
			if (error != 0) {
				if (enabled) hal_irq_enable();
				return error;
			}
		}
		process_free_mem(child);
	}
	if (enabled)
		hal_irq_enable();
	return 0;
}

void
exit1(int status)
{
	struct process *process = curthread->proc;
	struct thread *waiter;
	bool enabled;

	if (process == NULL || process == &process0)
		HAL_FATAL("process0 exit");
	filedesc_destroy(process->fd);
	process->fd = NULL;
	cwdinfo_release(process->cwdi);
	process->cwdi = NULL;
	enabled = hal_irq_disable();
	process->exit_status = status;
	process->state = PROCESS_ZOMBIE;
	waiter = process->waiter;
	if (waiter != NULL)
		sched_wakeup(waiter);
	(void)enabled;
	thread_exit(status);
}
