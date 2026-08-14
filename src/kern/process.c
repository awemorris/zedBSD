/*
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#include "kern/process.h"
#include "kern/thread.h"
#include "kern/vmspace.h"
#include "kern/filedesc.h"
#include "kern/kmem.h"
#include "kern/cred.h"
#include "kern/signal.h"
#include "kern/namei.h"

#include <errno.h>
#include <hal/hal.h>
#include <string.h>
#include <sys/wait.h>

struct process process0;
static struct process *all_processes;
static pid_t next_pid = 1;
static struct thread *reaper_thread;

void
process_init(void)
{
	if (all_processes != NULL)
		return;
	memset(&process0, 0, sizeof(process0));
	memset(&thread0, 0, sizeof(thread0));
	process0.pid = 0;
	process0.pgrp = 0;
	process0.session = 0;
	process0.umask = 0022U;
	process0.cred = cred_alloc_root();
	if (process0.cred == NULL)
		HAL_FATAL("process0 credentials");
	process0.state = PROCESS_RUNNING;
	process0.vmspace = &kernel_vmspace;
	process0.threads = &thread0;
	process0.thread_count = 1;
	thread0.tid = 0;
	thread0.proc = &process0;
	thread0.task = hal_task_get_current();
	thread0.state = THREAD_RUNNING;
	thread0.flags = THREAD_FLAG_IDLE;
	thread0.sched.priority = SCHED_PRIORITY_DEFAULT;
	thread0.sched.quantum = SCHED_QUANTUM_TICKS;
	hal_task_set_private(thread0.task, &thread0);
	all_processes = &process0;
}

static void
process_reaper(void *argument)
{
	(void)argument;
	for (;;) {
		struct process *process = all_processes;
		int reaped = 0;
		while (process != NULL) {
			struct process *next = process->all_next;
			if (process != &process0 &&
			    (process->flags & PROCESS_AUTOREAP) != 0 &&
			    process->state == PROCESS_ZOMBIE) {
				while (process->threads != NULL)
					if (thread_wait(process->threads, NULL) != 0)
						break;
				if (process->threads == NULL) {
					process_free_mem(process);
					reaped = 1;
				}
			}
			process = next;
		}
		if (!reaped)
			sched_sleep(0);
		else
			sched_yield();
	}
}

int
process_reaper_start(void)
{
	int error;
	if (reaper_thread != NULL)
		return 0;
	/* The current scheduler uses strict priority queues.  A low-priority
	 * reaper would starve forever while process0 remains runnable. */
	error = kthread_create(process_reaper, NULL, SCHED_PRIORITY_DEFAULT,
			       &reaper_thread);
	if (error == 0)
		thread_start(reaper_thread);
	return error;
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
struct process *process_first(void) { return all_processes; }
struct process *process_next(struct process *p) { return p != NULL ? p->all_next : NULL; }

int
process_setpgid(struct process *caller, pid_t pid, pid_t pgid)
{
	struct process *target, *member;
	if (caller == NULL || caller == &process0 || pid < 0 || pgid < 0)
		return EINVAL;
	target = pid == 0 ? caller : process_find(pid);
	if (target == NULL)
		return ESRCH;
	if (target != caller && target->parent != caller)
		return ESRCH;
	if (target->session != caller->session || target->session == target->pid)
		return EPERM;
	if (pgid == 0)
		pgid = target->pid;
	if (pgid != target->pid) {
		for (member = all_processes; member != NULL;
		     member = member->all_next)
			if (member->session == caller->session &&
			    member->pgrp == pgid)
				break;
		if (member == NULL)
			return EPERM;
	}
	target->pgrp = pgid;
	return 0;
}

pid_t
process_setsid(struct process *process)
{
	struct process *member;
	if (process == NULL || process == &process0)
		return -EPERM;
	for (member = all_processes; member != NULL; member = member->all_next)
		if (member->pgrp == process->pid)
			return -EPERM;
	process->session = process->pid;
	process->pgrp = process->pid;
	return process->pid;
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
	process->umask = parent->umask;
	process->cred = parent->cred;
	cred_ref(process->cred);
	if (parent == &process0) {
		process->pgrp = process->pid;
		process->session = process->pid;
	} else {
		process->pgrp = parent->pgrp;
		process->session = parent->session;
	}
	if (parent->cwdi != NULL) {
		int error = cwdinfo_clone(parent->cwdi, &process->cwdi);
		if (error != 0) {
			cred_release(process->cred);
			filedesc_destroy(process->fd);
			kern_free(process);
			return error;
		}
	}
	*result = process;
	return 0;
}

int
process_fork(struct process *parent, struct process **result)
{
	struct process *child = NULL;
	struct filedesc *files = NULL;
	struct thread *thread;
	hal_task_t task = NULL;
	int error;

	if (parent == NULL || parent == &process0 || result == NULL ||
	    parent != curthread->proc || parent->thread_count != 1 ||
	    parent->vmspace == NULL || parent->fd == NULL)
		return EINVAL;
	error = process_create(parent, 0, &child);
	if (error != 0)
		return error;
	error = filedesc_clone(parent->fd, &files);
	if (error != 0)
		goto fail;
	filedesc_destroy(child->fd);
	child->fd = files;
	files = NULL;
	error = vmspace_fork(parent->vmspace, &child->vmspace);
	if (error != 0)
		goto fail;
	task = hal_task_fork_current(child->vmspace->space, 0);
	if (task == NULL) {
		error = EAGAIN;
		goto fail;
	}
	error = thread_fork(child, task, &thread);
	if (error != 0)
		goto fail;
	task = NULL;
	signal_fork(child, parent, thread, curthread);
	process_publish(child);
	thread_start(thread);
	*result = child;
	return 0;

fail:
	if (task != NULL)
		hal_task_destroy(task);
	filedesc_destroy(files);
	process_free_mem(child);
	return error;
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
	cred_release(process->cred);
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

static int
wait_selector_matches(const struct process *child, pid_t selector,
		      pid_t caller_pgrp)
{
	if (selector > 0)
		return child->pid == selector;
	if (selector == -1)
		return 1;
	if (selector == 0)
		return child->pgrp == caller_pgrp;
	return child->pgrp == -selector;
}

pid_t
process_waitpid(struct process *parent, pid_t selector, int *status,
		int options)
{
	if (parent == NULL || parent != curthread->proc || selector == INT32_MIN ||
	    (options & ~(WNOHANG | WUNTRACED | WCONTINUED)) != 0)
		return -EINVAL;
	for (;;) {
		struct process *child, *zombie = NULL;
		int matched = 0;
		for (child = parent->children; child != NULL; child = child->sibling) {
			if (!wait_selector_matches(child, selector, parent->pgrp))
				continue;
			matched = 1;
			if ((options & WUNTRACED) != 0 && child->wait_stopped) {
				pid_t pid = child->pid;
				if (status != NULL)
					*status = child->wait_status;
				child->wait_stopped = 0;
				return pid;
			}
			if ((options & WCONTINUED) != 0 && child->wait_continued) {
				pid_t pid = child->pid;
				if (status != NULL)
					*status = 0xffff;
				child->wait_continued = 0;
				return pid;
			}
			if (child->state == PROCESS_ZOMBIE) {
				zombie = child;
				break;
			}
		}
		if (!matched)
			return -ECHILD;
		if (zombie != NULL) {
			pid_t pid = zombie->pid;
			struct thread *thread = zombie->threads;
			int child_status = zombie->exit_status;
			int error;
			if (thread == NULL)
				return -ECHILD;
			error = thread_wait(thread, NULL);
			if (error != 0)
				return -error;
			if (status != NULL)
				*status = child_status;
			process_free_mem(zombie);
			return pid;
		}
		if ((options & WNOHANG) != 0)
			return 0;
		if (parent->child_waiter != NULL &&
		    parent->child_waiter != curthread)
			return -EBUSY;
		parent->child_waiter = curthread;
		sched_sleep(0);
		parent->child_waiter = NULL;
		if (signal_pending_unblocked(curthread))
			return -EINTR;
	}
}

static void
notify_parent_event(struct process *process)
{
	if (process == NULL || process->parent == NULL ||
	    process->parent == &process0)
		return;
	(void)signal_send_process(process->parent, SIGCHLD);
	if (process->parent->child_waiter != NULL)
		sched_wakeup(process->parent->child_waiter);
}

void
process_note_stopped(struct process *process, int signo)
{
	if (process == NULL || process == &process0)
		return;
	process->wait_status = ((signo & 0xff) << 8) | 0x7f;
	process->wait_stopped = 1;
	process->wait_continued = 0;
	notify_parent_event(process);
}

void
process_note_continued(struct process *process)
{
	if (process == NULL || process == &process0)
		return;
	process->wait_continued = 1;
	process->wait_stopped = 0;
	notify_parent_event(process);
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
process_force_quiesce_users(void)
{
	struct process *process;
	bool enabled;

	if (curthread != &thread0)
		HAL_FATAL("user quiescence outside thread0");
	enabled = hal_irq_disable();
	(void)enabled;
	process0.children = NULL;
	for (process = all_processes; process != NULL;
	     process = process->all_next) {
		if (process == &process0)
			continue;
		process->parent = &process0;
		process->sibling = process0.children;
		process0.children = process;
	}
	while (process0.children != NULL) {
		struct process *victim = process0.children;
		struct thread *thread = victim->threads;
		while (thread != NULL) {
			struct thread *next = thread->proc_next;
			sched_unlink(thread);
			hal_task_set_private(thread->task, NULL);
			hal_task_destroy(thread->task);
			thread->state = THREAD_DEAD;
			kern_free(thread);
			thread = next;
		}
		victim->threads = NULL;
		victim->thread_count = 0;
		process_free_mem(victim);
	}
}

static void __attribute__((noreturn))
process_exit_final(int thread_status, int wait_status)
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
	process->exit_status = wait_status;
	process->state = PROCESS_ZOMBIE;
	if (process->parent != NULL && process->parent != &process0)
		(void)signal_send_process(process->parent, SIGCHLD);
	if ((process->flags & PROCESS_AUTOREAP) != 0 && reaper_thread != NULL)
		sched_wakeup(reaper_thread);
	waiter = process->waiter;
	if (waiter != NULL)
		sched_wakeup(waiter);
	if (process->parent != NULL && process->parent->child_waiter != NULL)
		sched_wakeup(process->parent->child_waiter);
	(void)enabled;
	thread_exit(thread_status);
}

void
exit1(int status)
{
	process_exit_final(status, (status & 0xff) << 8);
}

void
exit1_signal(int signo)
{
	process_exit_final(128 + signo, signo & 0x7f);
}
