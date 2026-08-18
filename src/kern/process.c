/*
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#include "kern/process.h"
#include "kern/process-timer.h"
#include "kern/thread.h"
#include "kern/tty.h"
#include "kern/vmspace.h"
#include "kern/filedesc.h"
#include "kern/kmem.h"
#include "kern/cred.h"
#include "kern/signal.h"
#include "kern/sched.h"
#include "kern/namei.h"

#include <errno.h>
#include <hal/hal.h>
#include <string.h>
#include <sys/wait.h>

struct process process0;
static struct process *all_processes;
static pid_t next_pid = 1;
static struct thread *reaper_thread;
static struct spinlock process_tree_lock = {
	{ 0 }, LOCK_RANK_PROCESS_TREE, "process tree", 0, 0
};
#define PROCESS_EXT __attribute__((section(".hightext")))

/* Caller holds process_tree_lock. */
static void
process_group_recheck_locked(pid_t session, pid_t pgrp, int notify)
{
	struct process *member;
	int have_member = 0, was_orphaned = 1, orphaned = 1, stopped = 0;

	if (pgrp <= 0)
		return;
	for (member = all_processes; member != NULL; member = member->all_next) {
		struct process *parent;
		if (member->state == PROCESS_DEAD || member->session != session ||
		    member->pgrp != pgrp)
			continue;
		have_member = 1;
		if ((member->flags & PROCESS_PGRP_ORPHANED) == 0)
			was_orphaned = 0;
		if (member->state == PROCESS_STOPPED)
			stopped = 1;
		parent = member->parent;
		if (parent != NULL && parent != &process0 &&
		    parent->session == session && parent->pgrp != pgrp)
			orphaned = 0;
	}
	if (!have_member)
		return;
	for (member = all_processes; member != NULL; member = member->all_next) {
		if (member->state == PROCESS_DEAD || member->session != session ||
		    member->pgrp != pgrp)
			continue;
		if (orphaned)
			member->flags |= PROCESS_PGRP_ORPHANED;
		else
			member->flags &= ~PROCESS_PGRP_ORPHANED;
		if (notify && !was_orphaned && orphaned && stopped)
			member->flags |= PROCESS_PGRP_NOTIFY;
	}
}

static void
process_group_deliver_notifications(void)
{
	struct process *process;
	pid_t cursor = -1;

	while ((process = process_find_next_ref(cursor)) != NULL) {
		unsigned long irq;
		int notify;
		cursor = process->pid;
		irq = spin_lock_irqsave(&process_tree_lock);
		notify = (process->flags & PROCESS_PGRP_NOTIFY) != 0;
		process->flags &= ~PROCESS_PGRP_NOTIFY;
		spin_unlock_irqrestore(&process_tree_lock, irq);
		if (notify) {
			(void)signal_send_process(process, SIGHUP);
			(void)signal_send_process(process, SIGCONT);
		}
		process_release(process);
	}
}

static PROCESS_EXT void
child_waiters_wake(struct process *parent)
{
	if (parent != NULL)
		waitq_wake_all(&parent->child_waitq);
}

static PROCESS_EXT void
reparent_children(struct process *process)
{
	struct process *child;
	int wake_reaper = 0;

	while ((child = process->children) != NULL) {
		pid_t child_session = child->session;
		pid_t child_pgrp = child->pgrp;
		process_group_recheck_locked(child_session, child_pgrp, 0);
		process->children = child->sibling;
		child->parent = &process0;
		child->flags |= PROCESS_AUTOREAP;
		child->sibling = process0.children;
		process0.children = child;
		if (child->state == PROCESS_ZOMBIE)
			wake_reaper = 1;
		process_group_recheck_locked(child_session, child_pgrp, 1);
	}
	if (wake_reaper && reaper_thread != NULL)
		sched_wakeup(reaper_thread);
}

void
process_init(void)
{
	if (all_processes != NULL)
		return;
	memset(&process0, 0, sizeof(process0));
	memset(&thread0, 0, sizeof(thread0));
	refcount_init(&process0.refs, 1);
	spin_init(&process0.lock, LOCK_RANK_PROCESS, "process0");
	waitq_init(&process0.child_waitq, "process0 children");
	waitq_init(&thread0.join_waitq, "thread0 join");
	process0.pid = 0;
	process0.pgrp = 0;
	process0.session = 0;
	process0.umask = 0022U;
	resource_limits_default(&process0.limits);
	process0.cred = cred_alloc_root();
	if (process0.cred == NULL)
		HAL_FATAL("process0 credentials");
	process0.state = PROCESS_RUNNING;
	process0.vmspace = &kernel_vmspace;
	process0.threads = &thread0;
	process0.thread_count = 1;
	thread0.tid = 0;
	refcount_init(&thread0.refs, 1);
	thread0.proc = &process0;
	thread0.task = hal_task_get_current();
	thread0.state = THREAD_RUNNING;
	thread0.flags = THREAD_FLAG_IDLE;
	thread0.sched.priority = SCHED_PRIORITY_DEFAULT;
	thread0.sched.quantum = SCHED_QUANTUM_TICKS;
	thread0.sched.cpu = 0;
	thread0.sched.last_cpu = 0;
	hal_task_set_private(thread0.task, &thread0);
	all_processes = &process0;
}

static void
process_reaper(void *argument)
{
	(void)argument;
	for (;;) {
		struct process *process = NULL;
		pid_t cursor = -1;
		int reaped = 0;
		while ((process = process_find_next_ref(cursor)) != NULL) {
			int reap;
			unsigned long irq;
			cursor = process->pid;
			irq = spin_lock_irqsave(&process->lock);
			reap = process != &process0 &&
			    (process->flags & PROCESS_AUTOREAP) != 0 &&
			    process->state == PROCESS_ZOMBIE;
			spin_unlock_irqrestore(&process->lock, irq);
			if (reap) {
				while (process->threads != NULL)
					if (thread_wait(process->threads, NULL) != 0)
						break;
				if (process->threads == NULL) {
					process_free_mem(process);
					reaped = 1;
				}
			}
			process_release(process);
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

void
process_ref(struct process *process)
{
	if (process != NULL)
		refcount_get(&process->refs);
}

void
process_release(struct process *process)
{
	if (process == NULL || !refcount_put(&process->refs))
		return;
	if (process == &process0 || process->state != PROCESS_DEAD)
		HAL_FATAL("releasing live process");
	kern_free(process);
}

void
process_resource_count(uint64_t *processes, uint64_t *threads)
{
	struct process *process;
	uint64_t pc = 0, tc = 0;
	unsigned long irq = spin_lock_irqsave(&process_tree_lock);
	for (process = all_processes; process != NULL; process = process->all_next) {
		pc++;
		tc += process->thread_count;
	}
	spin_unlock_irqrestore(&process_tree_lock, irq);
	if (processes != NULL)
		*processes = pc;
	if (threads != NULL)
		*threads = tc;
}

struct process *
process_find_ref(pid_t pid)
{
	struct process *process, *result = NULL;
	unsigned long irq = spin_lock_irqsave(&process_tree_lock);
	for (process = all_processes; process != NULL; process = process->all_next)
		if (process->pid == pid && process->state != PROCESS_DEAD) {
			process_ref(process);
			result = process;
			break;
		}
	spin_unlock_irqrestore(&process_tree_lock, irq);
	return result;
}

struct process *
process_find_next_ref(pid_t after)
{
	struct process *process, *result = NULL;
	unsigned long irq = spin_lock_irqsave(&process_tree_lock);
	for (process = all_processes; process != NULL; process = process->all_next)
		if (process->state != PROCESS_DEAD && process->pid > after &&
		    (result == NULL || process->pid < result->pid))
			result = process;
	if (result != NULL)
		process_ref(result);
	spin_unlock_irqrestore(&process_tree_lock, irq);
	return result;
}

struct thread *
thread_find_ref(tid_t tid)
{
	struct process *process;
	struct thread *result = NULL;
	unsigned long tree_irq = spin_lock_irqsave(&process_tree_lock);
	for (process = all_processes; process != NULL && result == NULL;
	    process = process->all_next) {
		struct thread *thread;
		unsigned long process_irq = spin_lock_irqsave(&process->lock);
		for (thread = process->threads; thread != NULL;
		    thread = thread->proc_next)
			if (thread->tid == tid && thread->state != THREAD_DEAD) {
				thread_ref(thread);
				result = thread;
				break;
			}
		spin_unlock_irqrestore(&process->lock, process_irq);
	}
	spin_unlock_irqrestore(&process_tree_lock, tree_irq);
	return result;
}

int
process_setpgid(struct process *caller, pid_t pid, pid_t pgid)
{
	struct process *target, *member;
	unsigned long irq;
	int error = 0;
	if (caller == NULL || caller == &process0 || pid < 0 || pgid < 0)
		return EINVAL;
	target = pid == 0 ? caller : process_find_ref(pid);
	if (target == NULL)
		return ESRCH;
	irq = spin_lock_irqsave(&process_tree_lock);
	if (target != caller && target->parent != caller)
		error = ESRCH;
	else if (target != caller && target->did_exec)
		error = EACCES;
	else if (target->session != caller->session ||
	    target->session == target->pid)
		error = EPERM;
	if (error != 0)
		goto out;
	if (pgid == 0)
		pgid = target->pid;
	if (pgid != target->pid) {
		for (member = all_processes; member != NULL;
		     member = member->all_next)
			if (member->session == caller->session &&
			    member->pgrp == pgid)
				break;
		if (member == NULL)
			error = EPERM;
	}
	if (error == 0) {
		pid_t old_pgrp = target->pgrp;
		process_group_recheck_locked(target->session, old_pgrp, 0);
		process_group_recheck_locked(target->session, pgid, 0);
		target->pgrp = pgid;
		process_group_recheck_locked(target->session, old_pgrp, 1);
		process_group_recheck_locked(target->session, pgid, 1);
	}
out:
	spin_unlock_irqrestore(&process_tree_lock, irq);
	process_group_deliver_notifications();
	if (target != caller)
		process_release(target);
	return error;
}

pid_t
process_setsid(struct process *process)
{
	struct process *member;
	unsigned long irq;
	if (process == NULL || process == &process0)
		return -EPERM;
	irq = spin_lock_irqsave(&process_tree_lock);
	for (member = all_processes; member != NULL; member = member->all_next)
		if (member->pgrp == process->pid) {
			spin_unlock_irqrestore(&process_tree_lock, irq);
			return -EPERM;
		}
	process_group_recheck_locked(process->session, process->pgrp, 0);
	{
		pid_t old_session = process->session;
		pid_t old_pgrp = process->pgrp;
		process->session = process->pid;
		process->pgrp = process->pid;
		process_group_recheck_locked(old_session, old_pgrp, 1);
	}
	process_group_recheck_locked(process->session, process->pgrp, 0);
	spin_unlock_irqrestore(&process_tree_lock, irq);
	tty_detach_process(process);
	process_group_deliver_notifications();
	return process->pid;
}

int
process_pgrp_in_session(pid_t session, pid_t pgrp)
{
	struct process *member;
	unsigned long irq;
	int found = 0;

	if (session <= 0 || pgrp <= 0)
		return 0;
	irq = spin_lock_irqsave(&process_tree_lock);
	for (member = all_processes; member != NULL; member = member->all_next)
		if (member->state != PROCESS_DEAD && member->session == session &&
		    member->pgrp == pgrp) {
			found = 1;
			break;
		}
	spin_unlock_irqrestore(&process_tree_lock, irq);
	return found;
}

int
process_signal_pgrp(pid_t session, pid_t pgrp, int signo)
{
	struct process *member;
	pid_t cursor = -1;
	int found = 0;

	if (session <= 0 || pgrp <= 0 || signo <= 0 || signo >= NSIG)
		return EINVAL;
	while ((member = process_find_next_ref(cursor)) != NULL) {
		cursor = member->pid;
		if (member->state != PROCESS_DEAD && member->session == session &&
		    member->pgrp == pgrp) {
			(void)signal_send_process(member, signo);
			found = 1;
		}
		process_release(member);
	}
	return found ? 0 : ESRCH;
}

int
process_create(struct process *parent, pid_t requested_pid,
	       struct process **result)
{
	struct process *process;
	struct process *duplicate;
	unsigned long parent_irq;

	if (parent == NULL || result == NULL || requested_pid < 0)
		return EINVAL;
	duplicate = requested_pid != 0 ? process_find_ref(requested_pid) : NULL;
	if (duplicate != NULL) {
		process_release(duplicate);
		return EBUSY;
	}
	process = kern_calloc(1, sizeof(*process));
	if (process == NULL)
		return ENOMEM;
	refcount_init(&process->refs, 1);
	spin_init(&process->lock, LOCK_RANK_PROCESS, "process");
	waitq_init(&process->child_waitq, "process children");
	resource_limits_default(&process->limits);
	process->fd = filedesc_create(process);
	if (process->fd == NULL) {
		kern_free(process);
		return ENOMEM;
	}
	process->pid = requested_pid != 0 ? requested_pid :
	    (pid_t)atomic_raw_fetch_add_relaxed(
	    (volatile unsigned *)&next_pid, 1U);
	if (requested_pid != 0) {
		unsigned observed = atomic_raw_load_acquire(
		    (volatile unsigned *)&next_pid);
		while ((unsigned)process->pid >= observed &&
		    !atomic_raw_compare_exchange(
		    (volatile unsigned *)&next_pid, &observed,
		    (unsigned)process->pid + 1U))
			;
	}
	process->state = PROCESS_NEW;
	process->parent = parent;
	parent_irq = spin_lock_irqsave(&parent->lock);
	process->umask = parent->umask;
	process->limits = parent->limits;
	process->cred = parent->cred;
	process->controlling_tty = parent->controlling_tty;
	cred_ref(process->cred);
	if (parent == &process0) {
		process->pgrp = process->pid;
		process->session = process->pid;
	} else {
		process->pgrp = parent->pgrp;
		process->session = parent->session;
	}
	spin_unlock_irqrestore(&parent->lock, parent_irq);
	(void)filedesc_set_limit(process->fd,
	    (unsigned)process->limits.values[RLIMIT_NOFILE].current);
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
	    parent != curthread->proc ||
	    parent->vmspace == NULL || parent->fd == NULL)
		return EINVAL;
	error = process_create(parent, 0, &child);
	if (error != 0)
		return error;
	error = filedesc_clone(parent->fd, child, &files);
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
	unsigned long irq;
	if (process == NULL || process == &process0 || process->state != PROCESS_NEW)
		return;
	irq = spin_lock_irqsave(&process_tree_lock);
	process->all_next = all_processes;
	all_processes = process;
	process->sibling = process->parent->children;
	process->parent->children = process;
	process->state = PROCESS_RUNNING;
	process_group_recheck_locked(process->session, process->pgrp, 0);
	waitq_wake_all(&process->parent->child_waitq);
	spin_unlock_irqrestore(&process_tree_lock, irq);
	process_group_deliver_notifications();
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
	unsigned long tree_irq, process_irq;
	struct vmspace *vmspace;
	struct filedesc *fd;
	struct ucred *cred;
	struct cwdinfo *cwdi;
	pid_t old_session, old_pgrp;

	if (process == NULL || process == &process0 ||
	    (curthread != NULL && process == curthread->proc))
		return;
	tty_detach_process(process);
	process_irq = spin_lock_irqsave(&process->lock);
	if (process->thread_count != 0) {
		spin_unlock_irqrestore(&process->lock, process_irq);
		return;
	}
	vmspace = process->vmspace;
	fd = process->fd;
	cred = process->cred;
	cwdi = process->cwdi;
	process->vmspace = NULL;
	process->fd = NULL;
	process->cred = NULL;
	process->cwdi = NULL;
	spin_unlock_irqrestore(&process->lock, process_irq);
	process_timer_cleanup(process);

	tree_irq = spin_lock_irqsave(&process_tree_lock);
	old_session = process->session;
	old_pgrp = process->pgrp;
	process_group_recheck_locked(old_session, old_pgrp, 0);
	if (process->children != NULL)
		HAL_FATAL("freeing process with children");
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
	process_group_recheck_locked(old_session, old_pgrp, 1);
	spin_unlock_irqrestore(&process_tree_lock, tree_irq);
	process_group_deliver_notifications();
	if (vmspace != NULL)
		vmspace_free(vmspace);
	filedesc_destroy(fd);
	cred_release(cred);
	cwdinfo_release(cwdi);
	process_release(process);
}

int
process_wait(struct process *process, int *status, char *result,
	     size_t result_capacity)
{
	struct thread *thread;
	unsigned long irq;
	int error, thread_status;

	if (process == NULL || process == &process0 || curthread == NULL ||
	    process->parent != curthread->proc)
		return ECHILD;
	irq = spin_lock_irqsave(&process_tree_lock);
	while (process->state != PROCESS_ZOMBIE) {
		uint64_t sequence = waitq_sequence(&process->parent->child_waitq);
		error = waitq_sleep(&process->parent->child_waitq,
		    &process_tree_lock, sequence, 0, WAITQ_INTERRUPTIBLE);
		if (error == EINTR) {
			spin_unlock_irqrestore(&process_tree_lock, irq);
			return EINTR;
		}
	}
	thread = process->threads;
	if (thread != NULL)
		thread_ref(thread);
	spin_unlock_irqrestore(&process_tree_lock, irq);
	if (thread == NULL)
		return ECHILD;
	error = thread_wait(thread, &thread_status);
	thread_release(thread);
	if (error != 0)
		return error;
	(void)thread_status;
	if (status != NULL)
		*status = process->exit_status;
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

PROCESS_EXT pid_t
process_wait_select_mask(struct process *parent, pid_t selector, int options,
		    unsigned event_mask, struct process_wait_event *event)
{
	unsigned long irq;

	if (parent == NULL || parent != curthread->proc || selector == INT32_MIN ||
	    event == NULL ||
	    (options & ~WNOHANG) != 0 || event_mask == 0 ||
	    (event_mask & ~(PROCESS_WAIT_EVENT_EXITED |
	    PROCESS_WAIT_EVENT_STOPPED | PROCESS_WAIT_EVENT_CONTINUED)) != 0)
		return -EINVAL;
	memset(event, 0, sizeof(*event));
	irq = spin_lock_irqsave(&process_tree_lock);
	for (;;) {
		struct process *child;
		int matched = 0;
		for (child = parent->children; child != NULL; child = child->sibling) {
			if (!wait_selector_matches(child, selector, parent->pgrp))
				continue;
			matched = 1;
			if (child->wait_reserved != PROCESS_WAIT_NONE)
				continue;
			if ((event_mask & PROCESS_WAIT_EVENT_STOPPED) != 0 &&
			    child->wait_stopped) {
				event->kind = PROCESS_WAIT_STOPPED;
				event->status = child->wait_status;
				goto reserve;
			}
			if ((event_mask & PROCESS_WAIT_EVENT_CONTINUED) != 0 &&
			    child->wait_continued) {
				event->kind = PROCESS_WAIT_CONTINUED;
				event->status = 0xffff;
				goto reserve;
			}
			if ((event_mask & PROCESS_WAIT_EVENT_EXITED) != 0 &&
			    child->state == PROCESS_ZOMBIE) {
				event->kind = PROCESS_WAIT_EXITED;
				event->status = child->exit_status;
				goto reserve;
			}
			continue;
reserve:
			child->wait_reserved = event->kind;
			process_ref(child);
			event->parent = parent;
			event->child = child;
			event->pid = child->pid;
			event->uid = child->cred != NULL ? child->cred->ruid : 0;
			spin_unlock_irqrestore(&process_tree_lock, irq);
			return event->pid;
		}
		if (!matched) {
			spin_unlock_irqrestore(&process_tree_lock, irq);
			return -ECHILD;
		}
		if ((options & WNOHANG) != 0) {
			spin_unlock_irqrestore(&process_tree_lock, irq);
			return 0;
		}
		{
			uint64_t sequence = waitq_sequence(&parent->child_waitq);
			int error = waitq_sleep(&parent->child_waitq,
			    &process_tree_lock, sequence, 0, WAITQ_INTERRUPTIBLE);
			if (error != EINTR)
				continue;
			spin_unlock_irqrestore(&process_tree_lock, irq);
			return -EINTR;
		}
	}
}

PROCESS_EXT int
process_wait_commit(struct process_wait_event *event)
{
	struct process *child;
	unsigned long irq;
	int error = 0;
	int reap = 0;

	if (event == NULL || event->parent == NULL || event->child == NULL ||
	    event->kind == PROCESS_WAIT_NONE)
		return EINVAL;
	irq = spin_lock_irqsave(&process_tree_lock);
	child = event->child;
	if (child->parent != event->parent || child->pid != event->pid ||
	    child->wait_reserved != event->kind) {
		error = ECHILD;
		goto out;
	}
	if (event->kind == PROCESS_WAIT_STOPPED)
		child->wait_stopped = 0;
	else if (event->kind == PROCESS_WAIT_CONTINUED)
		child->wait_continued = 0;
	else {
		if (child->state != PROCESS_ZOMBIE || child->threads == NULL) {
			error = ECHILD;
			goto out;
		}
		child->wait_reserved = PROCESS_WAIT_NONE;
		reap = 1;
		goto out;
	}
	child->wait_reserved = PROCESS_WAIT_NONE;
	child_waiters_wake(event->parent);
out:
	if (error != 0 && child->wait_reserved == event->kind)
		child->wait_reserved = PROCESS_WAIT_NONE;
	spin_unlock_irqrestore(&process_tree_lock, irq);
	if (error == 0 && reap) {
		struct thread *thread;
		while ((thread = child->threads) != NULL) {
			thread_ref(thread);
			error = thread_wait(thread, NULL);
			thread_release(thread);
			if (error != 0)
				break;
		}
		if (error == 0)
			process_free_mem(child);
	}
	process_release(child);
	if (error == 0)
		memset(event, 0, sizeof(*event));
	return error;
}

PROCESS_EXT void
process_wait_abort(struct process_wait_event *event)
{
	unsigned long irq;
	struct process *child;
	if (event == NULL || event->child == NULL || event->parent == NULL)
		return;
	child = event->child;
	irq = spin_lock_irqsave(&process_tree_lock);
	if (child->parent == event->parent &&
	    child->wait_reserved == event->kind) {
		child->wait_reserved = PROCESS_WAIT_NONE;
		child_waiters_wake(event->parent);
	}
	spin_unlock_irqrestore(&process_tree_lock, irq);
	process_release(child);
	memset(event, 0, sizeof(*event));
}

PROCESS_EXT pid_t
process_waitpid(struct process *parent, pid_t selector, int *status,
		int options)
{
	struct process_wait_event event;
	pid_t pid = process_wait_select(parent, selector, options, &event);
	int error;
	if (pid <= 0)
		return pid;
	if (status != NULL)
		*status = event.status;
	error = process_wait_commit(&event);
	if (error != 0) {
		process_wait_abort(&event);
		return -error;
	}
	return pid;
}

static void
notify_parent_event(struct process *process, int code, int status)
{
	const struct signal_action *action;
	struct signal_info info;
	struct process *parent;
	unsigned long irq;

	if (process == NULL || process->parent == NULL ||
	    process->parent == &process0)
		return;
	parent = process->parent;
	process_ref(parent);
	memset(&info, 0, sizeof(info));
	info.code = code;
	info.pid = process->pid;
	info.uid = process->cred != NULL ? process->cred->ruid : 0;
	info.status = status;
	action = &parent->signal_actions[SIGCHLD];
	if ((code != CLD_STOPPED && code != CLD_CONTINUED) ||
	    (action->flags & SA_NOCLDSTOP) == 0)
		(void)signal_send_process_info(parent, SIGCHLD, &info);
	irq = spin_lock_irqsave(&process_tree_lock);
	child_waiters_wake(parent);
	spin_unlock_irqrestore(&process_tree_lock, irq);
	process_release(parent);
}

void
process_note_stopped(struct process *process, int signo)
{
	unsigned long irq;
	if (process == NULL || process == &process0)
		return;
	irq = spin_lock_irqsave(&process->lock);
	process->wait_status = ((signo & 0xff) << 8) | 0x7f;
	process->wait_stopped = 1;
	process->wait_continued = 0;
	spin_unlock_irqrestore(&process->lock, irq);
	notify_parent_event(process, CLD_STOPPED, signo);
}

void
process_note_continued(struct process *process)
{
	unsigned long irq;
	if (process == NULL || process == &process0)
		return;
	irq = spin_lock_irqsave(&process->lock);
	process->wait_continued = 1;
	process->wait_stopped = 0;
	spin_unlock_irqrestore(&process->lock, irq);
	notify_parent_event(process, CLD_CONTINUED, SIGCONT);
}

PROCESS_EXT pid_t
process_wait_select(struct process *parent, pid_t selector, int options,
		    struct process_wait_event *event)
{
	unsigned mask = PROCESS_WAIT_EVENT_EXITED;
	if ((options & WUNTRACED) != 0)
		mask |= PROCESS_WAIT_EVENT_STOPPED;
	if ((options & WCONTINUED) != 0)
		mask |= PROCESS_WAIT_EVENT_CONTINUED;
	return process_wait_select_mask(parent, selector, options & WNOHANG,
	    mask, event);
}

void
process_thread_retired(struct thread *thread)
{
	struct process *process, *parent = NULL;
	struct thread *member;
	unsigned long irq, process_irq;
	int notify = 0, autoreap = 0;
	int last = 1;

	if (thread == NULL || (process = thread->proc) == NULL)
		return;
	irq = spin_lock_irqsave(&process_tree_lock);
	process_irq = spin_lock_irqsave(&process->lock);
	for (member = process->threads; member != NULL; member = member->proc_next) {
		if (member->state != THREAD_ZOMBIE &&
		    member->state != THREAD_REAPING && member->state != THREAD_DEAD) {
			last = 0;
			break;
		}
	}
	spin_unlock_irqrestore(&process->lock, process_irq);
	if (last && process != &process0 && process->state == PROCESS_EXITING) {
		process->state = PROCESS_ZOMBIE;
		parent = process->parent;
		if (parent != NULL)
			process_ref(parent);
		if (parent != NULL)
			child_waiters_wake(parent);
		notify = parent != NULL && parent != &process0;
		autoreap = (process->flags & PROCESS_AUTOREAP) != 0;
	}
	spin_unlock_irqrestore(&process_tree_lock, irq);
	if (notify) {
		struct signal_info info;
		memset(&info, 0, sizeof(info));
		info.code = (process->exit_status & 0x7f) == 0 ?
		    CLD_EXITED : CLD_KILLED;
		info.pid = process->pid;
		info.uid = process->cred != NULL ? process->cred->ruid : 0;
		info.status = info.code == CLD_EXITED ?
		    (process->exit_status >> 8) & 0xff :
		    process->exit_status & 0x7f;
		(void)signal_send_process_info(parent, SIGCHLD, &info);
	}
	if (parent != NULL)
		process_release(parent);
	if (autoreap && reaper_thread != NULL)
		sched_wakeup(reaper_thread);
}

static void __attribute__((noreturn))
process_exit_final(int thread_status, int wait_status)
{
	struct process *process = curthread->proc;
	struct process *parent;
	struct thread *other;
	struct filedesc *fd;
	struct cwdinfo *cwdi;
	unsigned long process_irq, tree_irq;

	if (process == NULL || process == &process0)
		HAL_FATAL("process0 exit");
	tree_irq = spin_lock_irqsave(&process_tree_lock);
	if (process->state == PROCESS_EXITING) {
		spin_unlock_irqrestore(&process_tree_lock, tree_irq);
		thread_exit(thread_status);
	}
	process->state = PROCESS_EXITING;
	process->exit_status = wait_status;
	spin_unlock_irqrestore(&process_tree_lock, tree_irq);
	process_timer_cleanup(process);

	/* _exit terminates the process, not only the calling POSIX thread. */
	for (;;) {
		other = NULL;
		process_irq = spin_lock_irqsave(&process->lock);
		for (other = process->threads; other != NULL;
		     other = other->proc_next) {
			if (other != curthread && other->state != THREAD_ZOMBIE &&
			    other->state != THREAD_REAPING &&
			    other->state != THREAD_DEAD) {
				thread_ref(other);
				break;
			}
		}
		spin_unlock_irqrestore(&process->lock, process_irq);
		if (other == NULL)
			break;
		(void)signal_send_thread(other, SIGKILL);
		thread_release(other);
		sched_sleep(sched_ticks() + 1U);
	}
	process_irq = spin_lock_irqsave(&process->lock);
	fd = process->fd;
	cwdi = process->cwdi;
	process->fd = NULL;
	process->cwdi = NULL;
	spin_unlock_irqrestore(&process->lock, process_irq);
	filedesc_destroy(fd);
	cwdinfo_release(cwdi);
	tree_irq = spin_lock_irqsave(&process_tree_lock);
	reparent_children(process);
	parent = process->parent;
	if (parent != NULL)
		process_ref(parent);
	if (parent != NULL && parent != &process0) {
		const struct signal_action *action =
		    &parent->signal_actions[SIGCHLD];
		if ((action->flags & SA_NOCLDWAIT) != 0 ||
		    action->handler == (uintptr_t)SIG_IGN)
			process->flags |= PROCESS_AUTOREAP;
	}
	spin_unlock_irqrestore(&process_tree_lock, tree_irq);
	process_group_deliver_notifications();
	if (parent != NULL)
		process_release(parent);
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
