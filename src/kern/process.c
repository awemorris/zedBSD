/*
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#include "kern/process.h"
#ifndef ZEDBSD_PROCESS_TEST
#include "kern/process-timer.h"
#else
/* Targeted host lifetime tests link only the tree/interval-timer sections.
 * Avoid importing the host's incompatible POSIX signal ABI through
 * <signal.h>; discarded cleanup callers still need a declaration. */
void process_timer_cleanup(struct process *);
#endif
#include "kern/thread.h"
#include "kern/tty.h"
#include "kern/vmspace.h"
#include "kern/filedesc.h"
#include "kern/kmem.h"
#include "kern/cred.h"
#include "kern/signal.h"
#include "kern/sched.h"
#include "kern/namei.h"
#include "kern/test-checkpoint.h"

#include <errno.h>
#include <hal/hal.h>
#include <string.h>
#include <sys/wait.h>

struct process process0;
struct process_retired_cred {
	struct ucred *cred;
	struct process_retired_cred *next;
};
struct process_cred_reservation {
	struct process_retired_cred retired;
};
static struct process *all_processes;
static struct process *creating_processes;
static pid_t next_pid = 1;
static struct thread *reaper_thread;
static struct spinlock process_tree_lock = {
	{ 0 }, LOCK_RANK_PROCESS_TREE, "process tree", 0, 0
};
#define PROCESS_EXT __attribute__((section(".hightext")))

static void process_exit_cleanup(int) __attribute__((noreturn));

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

int
process_pgrp_is_orphaned(const struct process *process)
{
	unsigned long irq;
	int orphaned;

	if (process == NULL || process == &process0)
		return 0;
	irq = spin_lock_irqsave(&process_tree_lock);
	orphaned = (process->flags & PROCESS_PGRP_ORPHANED) != 0;
	spin_unlock_irqrestore(&process_tree_lock, irq);
	return orphaned;
}

static PROCESS_EXT void
child_waiters_wake(struct process *parent)
{
	if (parent != NULL)
		waitq_wake_all(&parent->child_waitq);
}

/* The scheduler latch closes the reaper's scan-to-sleep interval without
 * acquiring the process-tree lock from VM reclaim or scheduler retirement. */
static void
process_vmspace_reaper_notify(void *argument)
{
	hal_task_t task = argument;

	/* sched_notify_task() is a retained notification: if the reaper has not
	 * entered kernel_wait_task() yet, notify_pending closes that handoff. */
	sched_notify_task(task);
}

static void
process_reaper_notify(void)
{
	if (reaper_thread != NULL)
		process_vmspace_reaper_notify(reaper_thread->task);
}

static PROCESS_EXT void
reparent_children(struct process *process)
{
	struct process *adopter = &process0, *candidate, *child;
	int wake_reaper = 0;

	/* PID 1 is the userspace child reaper.  Falling back to process0 is only
	 * necessary while init itself is exiting or before it has been spawned. */
	for (candidate = all_processes; candidate != NULL;
	    candidate = candidate->all_next)
		if (candidate != process && candidate->pid == 1 &&
		    candidate->state != PROCESS_EXITING &&
		    candidate->state != PROCESS_ZOMBIE &&
		    candidate->state != PROCESS_DEAD) {
			adopter = candidate;
			break;
		}
	while ((child = process->children) != NULL) {
		pid_t child_session = child->session;
		pid_t child_pgrp = child->pgrp;
		process_group_recheck_locked(child_session, child_pgrp, 0);
		process->children = child->sibling;
		child->parent = adopter;
		child->sibling = adopter->children;
		adopter->children = child;
		if (adopter == &process0)
			child->flags |= PROCESS_AUTOREAP;
		if (adopter == &process0 && child->state == PROCESS_ZOMBIE)
			wake_reaper = 1;
		process_group_recheck_locked(child_session, child_pgrp, 1);
	}
	child_waiters_wake(adopter);
	if (wake_reaper)
		process_reaper_notify();
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
	(void)mutex_init(&process0.resource_lock, LOCK_RANK_PROCESS_RESOURCE,
	    "process0 resource limits");
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

/* Claim and commit use the same EXITED reservation as waitpid().  This makes
 * the registry/list unlink, rather than a racy zombie observation, the single
 * reap linearization point. */
static int
process_reap_threads(struct process *process)
{
	for (;;) {
		struct thread *thread;
		unsigned long irq;
		int error;

		/* Detached self-reap removes the list entry under process->lock.  Acquire
		 * the reference in that same domain so the scan cannot race the final
		 * unlink/free between loading process->threads and thread_ref(). */
		irq = spin_lock_irqsave(&process->lock);
		thread = process->threads;
		if (thread != NULL)
			thread_ref(thread);
		spin_unlock_irqrestore(&process->lock, irq);
		if (thread == NULL)
			return 0;
		error = thread_wait(thread, NULL);
		thread_release(thread);
		if (error != 0)
			return error;
	}
}

static int
process_autoreap_claim(struct process *process)
{
	unsigned long irq;
	int claimed;

	irq = spin_lock_irqsave(&process_tree_lock);
	claimed = process != NULL && process != &process0 &&
	    (process->flags & PROCESS_AUTOREAP) != 0 &&
	    process->state == PROCESS_ZOMBIE &&
	    process->wait_reserved == PROCESS_WAIT_NONE;
	if (claimed)
		process->wait_reserved = PROCESS_WAIT_EXITED;
	spin_unlock_irqrestore(&process_tree_lock, irq);
	return claimed;
}

static int
process_autoreap_commit(struct process *process)
{
	unsigned long irq;
	int error;

	error = process_reap_threads(process);
	if (error == 0) {
		process_free_mem(process);
		return 1;
	}
	irq = spin_lock_irqsave(&process_tree_lock);
	if (process->wait_reserved == PROCESS_WAIT_EXITED) {
		process->wait_reserved = PROCESS_WAIT_NONE;
		child_waiters_wake(process->parent);
	}
	spin_unlock_irqrestore(&process_tree_lock, irq);
	return 0;
}

#ifdef ZEDBSD_PROCESS_TEST
int
process_test_autoreap_once(struct process *process)
{
	return process_autoreap_claim(process) ?
	    process_autoreap_commit(process) : 0;
}
#endif

static void
process_reaper(void *argument)
{
	(void)argument;
	for (;;) {
		struct process *process = NULL;
		pid_t cursor = -1;
		int reaped;

		reaped = vmspace_reap_pending() != 0;
		while ((process = process_find_next_ref(cursor)) != NULL) {
			cursor = process->pid;
			if (process_autoreap_claim(process) &&
			    process_autoreap_commit(process))
				reaped = 1;
			process_release(process);
		}
		if (reaped) {
			sched_yield();
		} else {
			/* The scheduler retains notifications delivered after the empty
			 * scan but before this call, so no condition-lock inversion is
			 * needed to close the scan-to-sleep handoff. */
			kernel_wait_task();
		}
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
	if (error == 0) {
		thread_start(reaper_thread);
		vmspace_set_reaper_notify(process_vmspace_reaper_notify,
		    reaper_thread->task);
	}
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

pid_t
process_parent_pid(struct process *process)
{
	pid_t pid = 0;
	unsigned long irq;

	if (process == NULL)
		return 0;
	irq = spin_lock_irqsave(&process_tree_lock);
	if (process->parent != NULL)
		pid = process->parent->pid;
	spin_unlock_irqrestore(&process_tree_lock, irq);
	return pid;
}

struct process *
process_parent_ref(struct process *process)
{
	struct process *parent = NULL;
	unsigned long irq;

	if (process == NULL)
		return NULL;
	irq = spin_lock_irqsave(&process_tree_lock);
	parent = process->parent;
	if (parent != NULL) {
		/* The checkpoint deliberately runs while the tree lock still excludes
		 * reparent/reap, proving that the reference is acquired inside the
		 * lifetime barrier rather than after a bare pointer snapshot. */
		KERN_TEST_CHECKPOINT(KERN_TEST_PROCESS_PARENT_BEFORE_REF, parent);
		process_ref(parent);
	}
	spin_unlock_irqrestore(&process_tree_lock, irq);
	return parent;
}

#ifdef ZEDBSD_PROCESS_TEST
void
process_test_reparent(struct process *child, struct process *parent)
{
	unsigned long irq = spin_lock_irqsave(&process_tree_lock);

	child->parent = parent;
	spin_unlock_irqrestore(&process_tree_lock, irq);
}

void
process_test_set_registry(struct process *head)
{
	unsigned long irq = spin_lock_irqsave(&process_tree_lock);

	all_processes = head;
	creating_processes = NULL;
	spin_unlock_irqrestore(&process_tree_lock, irq);
}
#endif

int
process_controlling_tty_snapshot(struct process *process, struct tty **tty,
	uint64_t *generation)
{
	unsigned long irq;

	if (process == NULL || tty == NULL || generation == NULL)
		return EINVAL;
	irq = spin_lock_irqsave(&process_tree_lock);
	*tty = process->controlling_tty;
	*generation = process->controlling_tty_generation;
	spin_unlock_irqrestore(&process_tree_lock, irq);
	return 0;
}

int
process_controlling_tty_matches(struct process *process, struct tty *tty,
	uint64_t generation)
{
	int matches;
	unsigned long irq;

	if (process == NULL || tty == NULL || generation == 0)
		return 0;
	irq = spin_lock_irqsave(&process_tree_lock);
	matches = process->controlling_tty == tty &&
	    process->controlling_tty_generation == generation;
	spin_unlock_irqrestore(&process_tree_lock, irq);
	return matches;
}

int
process_controlling_tty_attach(struct process *process, struct tty *tty,
	uint64_t generation)
{
	unsigned long irq;
	int error = 0;

	if (process == NULL || tty == NULL || generation == 0)
		return EINVAL;
	irq = spin_lock_irqsave(&process_tree_lock);
	/* Exit publishes PROCESS_EXITING while holding this same tree serializer.
	 * Refuse a late TIOCSCTTY publication after the early exit detach. */
	if (process->state == PROCESS_EXITING ||
	    process->state == PROCESS_ZOMBIE || process->state == PROCESS_DEAD)
		error = ESRCH;
	else if (process->controlling_tty != NULL &&
	    (process->controlling_tty != tty ||
	    process->controlling_tty_generation != generation))
		error = EBUSY;
	else {
		process->controlling_tty = tty;
		process->controlling_tty_generation = generation;
	}
	spin_unlock_irqrestore(&process_tree_lock, irq);
	return error;
}

void
process_controlling_tty_detach_one(struct process *process, struct tty *tty,
	uint64_t generation)
{
	unsigned long irq;

	if (process == NULL || tty == NULL || generation == 0)
		return;
	irq = spin_lock_irqsave(&process_tree_lock);
	if (process->controlling_tty == tty &&
	    process->controlling_tty_generation == generation) {
		process->controlling_tty = NULL;
		process->controlling_tty_generation = 0;
	}
	spin_unlock_irqrestore(&process_tree_lock, irq);
}

static void
detach_tty_from_list_locked(struct process *list, pid_t session,
	struct tty *tty, uint64_t generation)
{
	struct process *member;

	for (member = list; member != NULL; member = member->all_next)
		if (member->session == session && member->controlling_tty == tty &&
		    member->controlling_tty_generation == generation) {
			member->controlling_tty = NULL;
			member->controlling_tty_generation = 0;
		}
}

void
process_controlling_tty_detach_session(pid_t session, struct tty *tty,
	uint64_t generation)
{
	unsigned long irq;

	if (session <= 0 || tty == NULL || generation == 0)
		return;
	irq = spin_lock_irqsave(&process_tree_lock);
	/* Include unpublished fork children: association inheritance and detach
	 * are one tree-lock domain, so a child can never escape with an old slot
	 * generation while its session is being detached. */
	detach_tty_from_list_locked(all_processes, session, tty, generation);
	detach_tty_from_list_locked(creating_processes, session, tty, generation);
	spin_unlock_irqrestore(&process_tree_lock, irq);
}

struct vmspace *
process_vmspace_ref(struct process *process)
{
	struct vmspace *vmspace = NULL;
	unsigned long irq;

	if (process == NULL)
		return NULL;
	irq = spin_lock_irqsave(&process->lock);
	vmspace = process->vmspace;
	/* The published pointer owns a strong reference.  Taking the additional
	 * reference while process->lock is held closes the exec/exit detach race. */
	if (vmspace != NULL)
		vmspace_ref(vmspace);
	spin_unlock_irqrestore(&process->lock, irq);
	return vmspace;
}

static void
release_retired_creds(struct process_retired_cred *retired)
{
	while (retired != NULL) {
		struct process_retired_cred *next = retired->next;
		cred_release(retired->cred);
		kern_free(retired);
		retired = next;
	}
}

void
process_cred_read_enter(struct process *process)
{
	unsigned long irq;

	if (process == NULL)
		return;
	irq = spin_lock_irqsave(&process->lock);
	process->cred_readers++;
	spin_unlock_irqrestore(&process->lock, irq);
}

void
process_cred_read_leave(struct process *process)
{
	struct process_retired_cred *retired = NULL;
	unsigned long irq;

	if (process == NULL)
		return;
	irq = spin_lock_irqsave(&process->lock);
	if (process->cred_readers == 0)
		HAL_FATAL("credential reader underflow");
	if (--process->cred_readers == 0) {
		retired = process->retired_creds;
		process->retired_creds = NULL;
	}
	spin_unlock_irqrestore(&process->lock, irq);
	release_retired_creds(retired);
}

int
process_cred_reserve(struct process *process,
	struct process_cred_reservation **result)
{
	struct process_cred_reservation *reservation;

	if (result == NULL)
		return EINVAL;
	*result = NULL;
	if (process == NULL)
		return EINVAL;
	if (process == &process0)
		return EPERM;
	reservation = kern_calloc(1, sizeof(*reservation));
	if (reservation == NULL)
		return ENOMEM;
	*result = reservation;
	return 0;
}

void
process_cred_reservation_abort(struct process_cred_reservation *reservation)
{
	kern_free(reservation);
}

void
process_cred_commit_reserved(struct process *process,
	struct ucred *replacement, struct process_cred_reservation *reservation)
{
	struct process_retired_cred *retired;
	struct ucred *old;
	unsigned long irq;

	if (process == NULL || replacement == NULL || reservation == NULL ||
	    process == &process0)
		HAL_FATAL("invalid reserved credential commit");
	retired = &reservation->retired;
	irq = spin_lock_irqsave(&process->lock);
	old = process->cred;
	process->cred = replacement;
	if (process->cred_readers != 0) {
		retired->cred = old;
		retired->next = process->retired_creds;
		process->retired_creds = retired;
		retired = NULL;
	}
	spin_unlock_irqrestore(&process->lock, irq);
	if (retired != NULL) {
		cred_release(old);
		kern_free(reservation);
	}
	/* When readers exist, reservation is now the first member of the retired
	 * list and is reclaimed by process_cred_read_leave(). */
}

int
process_cred_replace(struct process *process, struct ucred *replacement)
{
	struct process_cred_reservation *reservation;
	struct process_retired_cred *retired;
	struct ucred *old;
	unsigned long irq;
	int error;

	if (process == NULL || replacement == NULL || process == &process0)
		return EPERM;
	error = process_cred_reserve(process, &reservation);
	if (error != 0)
		return error;
	/* Once exec has taken ownership of the process image, sibling credential
	 * syscalls must not race the prospective exec credential. */
	irq = spin_lock_irqsave(&process->lock);
	if (process->execing) {
		spin_unlock_irqrestore(&process->lock, irq);
		process_cred_reservation_abort(reservation);
		return EBUSY;
	}
	retired = &reservation->retired;
	old = process->cred;
	process->cred = replacement;
	if (process->cred_readers != 0) {
		retired->cred = old;
		retired->next = process->retired_creds;
		process->retired_creds = retired;
		retired = NULL;
	}
	spin_unlock_irqrestore(&process->lock, irq);
	if (retired != NULL) {
		cred_release(old);
		kern_free(reservation);
	}
	return 0;
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
	return process_signal_pgrp_except(session, pgrp, signo, NULL);
}

int
process_signal_pgrp_except(pid_t session, pid_t pgrp, int signo,
	struct process *excluded)
{
	struct process *member;
	pid_t cursor = -1;
	int found = 0;

	if (session <= 0 || pgrp <= 0 || signo <= 0 || signo >= NSIG)
		return EINVAL;
	while ((member = process_find_next_ref(cursor)) != NULL) {
		cursor = member->pid;
		if (member != excluded && member->state != PROCESS_DEAD &&
		    member->session == session && member->pgrp == pgrp) {
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
	struct process *process, *candidate;
	unsigned long parent_irq, tree_irq;
	pid_t assigned;

	if (parent == NULL || result == NULL || requested_pid < 0)
		return EINVAL;
	process = kern_calloc(1, sizeof(*process));
	if (process == NULL)
		return ENOMEM;
	refcount_init(&process->refs, 1);
	spin_init(&process->lock, LOCK_RANK_PROCESS, "process");
	(void)mutex_init(&process->resource_lock, LOCK_RANK_PROCESS_RESOURCE,
	    "process resource limits");
	waitq_init(&process->child_waitq, "process children");
	resource_limits_default(&process->limits);
	process->fd = filedesc_create(process);
	if (process->fd == NULL) {
		kern_free(process);
		return ENOMEM;
	}
	process->state = PROCESS_NEW;
	process->parent = parent;
	parent_irq = spin_lock_irqsave(&parent->lock);
	process->umask = parent->umask;
	process->nice_value = parent->nice_value;
	process->limits = parent->limits;
	process->cred = parent->cred;
	cred_ref(process->cred);
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
	/* Reserve the PID before returning an unpublished process.  The
	 * creating list closes both requested-PID races and allocator wrap
	 * collisions without exposing half-initialized processes to lookup. */
	tree_irq = spin_lock_irqsave(&process_tree_lock);
	assigned = requested_pid != 0 ? requested_pid : next_pid;
	for (;;) {
		int collision = assigned <= 0;

		for (candidate = all_processes; !collision && candidate != NULL;
		    candidate = candidate->all_next)
			collision = candidate->pid == assigned;
		for (candidate = creating_processes;
		    !collision && candidate != NULL; candidate = candidate->all_next)
			collision = candidate->pid == assigned;
		if (!collision)
			break;
		if (requested_pid != 0) {
			spin_unlock_irqrestore(&process_tree_lock, tree_irq);
			cred_release(process->cred);
			cwdinfo_release(process->cwdi);
			filedesc_destroy(process->fd);
			kern_free(process);
			return EBUSY;
		}
		assigned = assigned == INT32_MAX ? 1 : assigned + 1;
	}
	process->pid = assigned;
	next_pid = assigned == INT32_MAX ? 1 : assigned + 1;
	if (parent == &process0) {
		process->pgrp = process->pid;
		process->session = process->pid;
	} else {
		process->pgrp = parent->pgrp;
		process->session = parent->session;
		process->controlling_tty = parent->controlling_tty;
		process->controlling_tty_generation =
		    parent->controlling_tty_generation;
	}
	process->all_next = creating_processes;
	creating_processes = process;
	spin_unlock_irqrestore(&process_tree_lock, tree_irq);
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
	struct process **link;
	unsigned long irq;
	if (process == NULL || process == &process0 || process->state != PROCESS_NEW)
		return;
	irq = spin_lock_irqsave(&process_tree_lock);
	link = &creating_processes;
	while (*link != NULL && *link != process)
		link = &(*link)->all_next;
	if (*link != process) {
		spin_unlock_irqrestore(&process_tree_lock, irq);
		return;
	}
	*link = process->all_next;
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
	struct process_retired_cred *retired;
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
	retired = process->retired_creds;
	cwdi = process->cwdi;
	process->vmspace = NULL;
	process->fd = NULL;
	process->cred = NULL;
	process->retired_creds = NULL;
	process->cred_readers = 0;
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
	else {
		link = &creating_processes;
		while (*link != NULL && *link != process)
			link = &(*link)->all_next;
		if (*link == process)
			*link = process->all_next;
	}
	if (process->parent != NULL) {
		child_link = &process->parent->children;
		while (*child_link != NULL && *child_link != process)
			child_link = &(*child_link)->sibling;
		if (*child_link == process) {
			*child_link = process->sibling;
			/* A waiter which skipped this reserved zombie must rescan now that
			 * removal, rather than reservation release, is the reap commit. */
			child_waiters_wake(process->parent);
		}
	}
	process->state = PROCESS_DEAD;
	process_group_recheck_locked(old_session, old_pgrp, 1);
	spin_unlock_irqrestore(&process_tree_lock, tree_irq);
	process_group_deliver_notifications();
	if (vmspace != NULL)
		vmspace_put(vmspace);
	filedesc_destroy(fd);
	cred_release(cred);
	release_retired_creds(retired);
	cwdinfo_release(cwdi);
	process_release(process);
}

int
process_wait(struct process *process, int *status)
{
	struct thread *thread;
	unsigned long irq;
	int error, thread_status;

	if (process == NULL || process == &process0 || curthread == NULL)
		return ECHILD;
	irq = spin_lock_irqsave(&process_tree_lock);
	if (process->parent != curthread->proc) {
		spin_unlock_irqrestore(&process_tree_lock, irq);
		return ECHILD;
	}
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
			    child->state == PROCESS_ZOMBIE &&
			    (child->flags & PROCESS_AUTOREAP) == 0) {
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
		/* Detached threads may self-reap immediately after scheduler retirement,
		 * leaving a fully valid zombie with an empty thread list. */
		if (child->state != PROCESS_ZOMBIE) {
			error = ECHILD;
			goto out;
		}
		reap = 1;
		goto out;
	}
	child->wait_reserved = PROCESS_WAIT_NONE;
	child_waiters_wake(event->parent);
out:
	spin_unlock_irqrestore(&process_tree_lock, irq);
	if (error == 0 && reap) {
		/* Keep PROCESS_WAIT_EXITED reserved until process_free_mem() removes the
		 * child from both the registry and the parent's list.  A second waiter
		 * therefore sleeps instead of committing the same zombie. */
		KERN_TEST_CHECKPOINT(KERN_TEST_PROCESS_WAIT_REAP_RESERVED, child);
		error = process_reap_threads(child);
		if (error == 0) {
			uint64_t child_ticks =
			    atomic_u64_load_acquire(&child->cpu_ticks) +
			    atomic_u64_load_acquire(&child->child_cpu_ticks);
			uint64_t child_user_ticks =
			    atomic_u64_load_acquire(&child->user_ticks) +
			    atomic_u64_load_acquire(&child->child_user_ticks);
			uint64_t child_system_ticks =
			    atomic_u64_load_acquire(&child->system_ticks) +
			    atomic_u64_load_acquire(&child->child_system_ticks);
			(void)atomic_u64_fetch_add_relaxed(
			    &event->parent->child_cpu_ticks, child_ticks);
			(void)atomic_u64_fetch_add_relaxed(
			    &event->parent->child_user_ticks, child_user_ticks);
			(void)atomic_u64_fetch_add_relaxed(
			    &event->parent->child_system_ticks, child_system_ticks);
			process_free_mem(child);
		}
	}
	if (error == 0) {
		process_release(child);
		memset(event, 0, sizeof(*event));
	}
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

/* Caller holds process_tree_lock.  Keeping the tree serializer through signal
 * generation makes job-control transition publication and parent notification
 * one ordered stream: process_continue() cannot overtake a validated STOP, and
 * a new STOP callback cannot overtake CONTINUED. */
static void
notify_parent_job_event_tree_locked(struct process *process, int code,
	int status)
{
	struct signal_info info;
	struct process *parent;
	unsigned long irq;
	unsigned action_flags;

	parent = process != NULL ? process->parent : NULL;
	if (parent == NULL || parent == &process0)
		return;
	memset(&info, 0, sizeof(info));
	info.code = code;
	info.pid = process->pid;
	info.uid = process->cred != NULL ? process->cred->ruid : 0;
	info.status = status;
	irq = spin_lock_irqsave(&parent->lock);
	action_flags = parent->signal_actions[SIGCHLD].flags;
	spin_unlock_irqrestore(&parent->lock, irq);
	if ((action_flags & SA_NOCLDSTOP) == 0)
		(void)signal_send_process_info(parent, SIGCHLD, &info);
	child_waiters_wake(parent);
}

struct process_stop_notification {
	struct process *process;
	unsigned generation;
	int signo;
};

static void
process_stop_notify(void *argument)
{
	struct process_stop_notification *notification = argument;
	struct process *process = notification->process;
	unsigned long process_irq, tree_irq;
	int current;

	KERN_TEST_CHECKPOINT(KERN_TEST_PROCESS_STOP_CALLBACK_BEFORE_NOTIFY,
	    process);
	tree_irq = spin_lock_irqsave(&process_tree_lock);
	process_irq = spin_lock_irqsave(&process->lock);
	current = process->stop_requested && process->state == PROCESS_STOPPED &&
	    process->stop_generation == notification->generation &&
	    process->wait_stopped;
	spin_unlock_irqrestore(&process->lock, process_irq);
	if (current)
		notify_parent_job_event_tree_locked(process, CLD_STOPPED,
		    notification->signo);
	spin_unlock_irqrestore(&process_tree_lock, tree_irq);
}

void
process_stop_current(int signo)
{
	struct process *process = curthread != NULL ? curthread->proc : NULL;
	struct process_stop_notification notification;
	struct thread *member;
	unsigned long process_irq;
	int notify = 0;

	if (process == NULL || process == &process0)
		return;
	process_irq = spin_lock_irqsave(&process->lock);
	if (!process->stop_requested) {
		if (signo == 0 || process->state != PROCESS_RUNNING) {
			spin_unlock_irqrestore(&process->lock, process_irq);
			return;
		}
		process->stop_generation++;
		if (process->stop_generation == 0)
			process->stop_generation++;
		process->stop_requested = 1;
		process->stop_signo = signo;
		process->stop_target_count = 0;
		process->stop_ack_count = 0;
		for (member = process->threads; member != NULL;
		    member = member->proc_next)
			if (!member->exit_committed &&
			    member->state != THREAD_EXITING &&
			    member->state != THREAD_ZOMBIE &&
			    member->state != THREAD_REAPING &&
			    member->state != THREAD_DEAD)
				process->stop_target_count++;
		for (member = process->threads; member != NULL;
		    member = member->proc_next)
			if (member != curthread && !member->exit_committed &&
			    member->state != THREAD_EXITING &&
			    member->state != THREAD_ZOMBIE &&
			    member->state != THREAD_REAPING &&
			    member->state != THREAD_DEAD)
				sched_interrupt(member);
	}
	if (curthread->stop_generation != process->stop_generation) {
		curthread->stop_generation = process->stop_generation;
		process->stop_ack_count++;
	}
	while (process->stop_requested) {
		/* Re-evaluate after every wake.  A target may retire before reaching
		 * its safe point; thread_sched_retired() then shrinks the generation
		 * target and wakes an acknowledged waiter to finish the transition. */
		if (process->state == PROCESS_RUNNING &&
		    process->stop_ack_count == process->stop_target_count) {
			process->state = PROCESS_STOPPED;
			process->wait_status =
			    ((process->stop_signo & 0xff) << 8) | 0x7f;
			process->wait_stopped = 1;
			process->wait_continued = 0;
			notify = 1;
		}
		if (notify) {
			notify = 0;
			notification.process = process;
			notification.generation = process->stop_generation;
			notification.signo = process->stop_signo;
			sched_sleep_locked_notify(0, &process->lock,
			    process_stop_notify, &notification);
		} else {
			sched_sleep_locked(0, &process->lock);
		}
	}
	spin_unlock_irqrestore(&process->lock, process_irq);
}

int
process_stop_requested(const struct thread *thread)
{
	return thread != NULL && thread->proc != NULL &&
	    atomic_raw_load_acquire(&thread->proc->stop_requested) != 0;
}

int
process_continue(struct process *process, int report_continued)
{
	struct thread *thread;
	unsigned long process_irq, tree_irq;
	int continued = 0;

	if (process == NULL || process == &process0)
		return 0;
	tree_irq = spin_lock_irqsave(&process_tree_lock);
	process_irq = spin_lock_irqsave(&process->lock);
	if (process->stop_requested || process->state == PROCESS_STOPPED) {
		continued = process->state == PROCESS_STOPPED;
		process->stop_requested = 0;
		process->state = PROCESS_RUNNING;
		process->wait_stopped = 0;
		process->stop_target_count = 0;
		process->stop_ack_count = 0;
		if (report_continued && continued)
			process->wait_continued = 1;
		for (thread = process->threads; thread != NULL;
		    thread = thread->proc_next)
			if (thread->state == THREAD_SLEEPING)
				sched_wakeup(thread);
	}
	spin_unlock_irqrestore(&process->lock, process_irq);
	if (continued && report_continued)
		notify_parent_job_event_tree_locked(process, CLD_CONTINUED, SIGCONT);
	spin_unlock_irqrestore(&process_tree_lock, tree_irq);
	return continued;
}

int
process_itimer_get(struct process *process, int which, uint64_t *remaining,
	uint64_t *interval)
{
	unsigned before, after;

	if (process == NULL || which < 0 || which >= 3 || remaining == NULL ||
	    interval == NULL)
		return EINVAL;
	do {
		before = atomic_raw_load_acquire(
		    &process->itimer_sequence[which]);
		if ((before & 1U) != 0)
			continue;
		*remaining = atomic_u64_load_acquire(
		    &process->itimer_remaining[which]);
		*interval = atomic_u64_load_acquire(
		    &process->itimer_interval[which]);
		after = atomic_raw_load_acquire(
		    &process->itimer_sequence[which]);
	} while (before != after || (after & 1U) != 0);
	return 0;
}

int
process_itimer_set(struct process *process, int which, uint64_t remaining,
	uint64_t interval, uint64_t *old_remaining, uint64_t *old_interval)
{
	unsigned sequence;
	bool enabled;

	if (process == NULL || which < 0 || which >= 3)
		return EINVAL;
	enabled = hal_irq_disable();
	for (;;) {
		sequence = atomic_raw_load_acquire(
		    &process->itimer_sequence[which]);
		if ((sequence & 1U) != 0)
			continue;
		if (atomic_raw_compare_exchange(
		    &process->itimer_sequence[which], &sequence, sequence + 1U))
			break;
	}
	if (old_remaining != NULL)
		*old_remaining = atomic_u64_load_acquire(
		    &process->itimer_remaining[which]);
	if (old_interval != NULL)
		*old_interval = atomic_u64_load_acquire(
		    &process->itimer_interval[which]);
	atomic_u64_store_release(&process->itimer_interval[which], interval);
	atomic_u64_store_release(&process->itimer_remaining[which], remaining);
	atomic_raw_store_release(&process->itimer_sequence[which], sequence + 2U);
	if (enabled)
		hal_irq_enable();
	return 0;
}

int
process_itimer_tick(struct process *process, int which)
{
	uint64_t remaining, interval;
	unsigned sequence;

	if (process == NULL || which < 0 || which >= 3)
		return 0;
	/* A timer tick is accounting, not a best-effort notification.  Serialize
	 * with setitimer() and ticks arriving on other CPUs until this exact tick
	 * owns the sequence; returning on an odd/CAS conflict loses CPU time. */
	for (;;) {
		sequence = atomic_raw_load_acquire(
		    &process->itimer_sequence[which]);
		if ((sequence & 1U) != 0) {
			KERN_TEST_CHECKPOINT(KERN_TEST_ITIMER_TICK_RETRY, process);
			continue;
		}
		if (atomic_raw_compare_exchange(&process->itimer_sequence[which],
		    &sequence, sequence + 1U))
			break;
		KERN_TEST_CHECKPOINT(KERN_TEST_ITIMER_TICK_RETRY, process);
	}
	KERN_TEST_CHECKPOINT(KERN_TEST_ITIMER_TICK_LOCKED, process);
	remaining = atomic_u64_load_acquire(&process->itimer_remaining[which]);
	interval = atomic_u64_load_acquire(&process->itimer_interval[which]);
	if (remaining != 0)
		atomic_u64_store_release(&process->itimer_remaining[which],
		    remaining == 1U ? interval : remaining - 1U);
	atomic_raw_store_release(&process->itimer_sequence[which], sequence + 2U);
	return remaining == 1U;
}

void
process_itimer_real_tick_all(void)
{
	struct process *process;
	struct signal_info info;
	pid_t cursor = -1;

	memset(&info, 0, sizeof(info));
	info.code = SI_TIMER;
	/* Hold a process reference, not the global tree lock, while queuing the
	 * signal.  Delivery can wake a remote CPU and must not extend the process
	 * registry critical section on every timer tick. */
	while ((process = process_find_next_ref(cursor)) != NULL) {
		cursor = process->pid;
		if (process != &process0 && process->state != PROCESS_DEAD &&
		    process_itimer_tick(process, 0))
			(void)signal_send_process_info(process, SIGALRM, &info);
		process_release(process);
	}
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
	struct vmspace *dead_vmspace = NULL;
	unsigned long irq, process_irq;
	int notify = 0, autoreap = 0, final_cleanup = 0;
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
	if (last && process != &process0 && process->state == PROCESS_EXITING) {
		/* No task can enter this address space again.  Keep only wait-visible
		 * metadata in the zombie instead of pinning all user mappings until
		 * the parent reaps it. */
		dead_vmspace = process->vmspace;
		process->vmspace = NULL;
		process->state = PROCESS_ZOMBIE;
		final_cleanup = 1;
		parent = process->parent;
		if (parent != NULL)
			process_ref(parent);
		if (parent != NULL)
			child_waiters_wake(parent);
		notify = parent != NULL && parent != &process0;
		autoreap = (process->flags & PROCESS_AUTOREAP) != 0;
	}
	spin_unlock_irqrestore(&process->lock, process_irq);
	spin_unlock_irqrestore(&process_tree_lock, irq);
	/* Exit performs an early teardown before terminating sibling threads.  This
	 * final idempotent pass closes any device/timer admission which was already
	 * in flight at that point; the retired hook's process reference protects the
	 * object across both calls. */
	if (final_cleanup) {
		process_timer_cleanup(process);
		tty_detach_process(process);
	}
	if (dead_vmspace != NULL && dead_vmspace != &kernel_vmspace) {
		vmspace_put_deferred(dead_vmspace);
		/* vmspace destruction may enter VFS and is performed by the reaper. */
	}
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
	if (autoreap)
		process_reaper_notify();
}

static void
process_exit_cleanup(int thread_status)
{
	struct process *process = curthread->proc;
	struct process *parent;
	struct thread *other;
	struct filedesc *fd;
	struct cwdinfo *cwdi;
	unsigned long parent_irq, process_irq, tree_irq;

	process_timer_cleanup(process);
	tty_detach_process(process);

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
		struct signal_action action;

		/* Disposition changes are serialized by parent->lock.  The tree lock
		 * keeps the parent relationship stable while the snapshot decides whether
		 * this child will be visible as a zombie. */
		parent_irq = spin_lock_irqsave(&parent->lock);
		action = parent->signal_actions[SIGCHLD];
		spin_unlock_irqrestore(&parent->lock, parent_irq);
		if ((action.flags & SA_NOCLDWAIT) != 0 ||
		    action.handler == (uintptr_t)SIG_IGN)
			process->flags |= PROCESS_AUTOREAP;
	}
	spin_unlock_irqrestore(&process_tree_lock, tree_irq);
	process_group_deliver_notifications();
	if (parent != NULL)
		process_release(parent);
	curthread->exit_status = thread_status;
	sched_exit_current();
}

static int process_commit_thread_exit(struct thread *, int);

void
process_exit_if_last_thread(int status)
{
	struct thread *thread = curthread;
	int owner;

	if (thread == NULL || thread->proc == NULL || thread->proc == &process0)
		return;
	owner = process_commit_thread_exit(thread, status);
	if (owner)
		process_exit_cleanup(status);
}

static void
process_thread_exit_publish_locked(struct process *process,
	struct thread *thread)
{
	struct thread *member;

	if (thread->exit_committed)
		HAL_FATAL("thread exit committed twice");
	thread->exit_committed = 1;
	/* A stop generation counts only threads which can still return to a
	 * scheduler/user safe point.  Retire this member at the same process-lock
	 * linearization point as exit_committed, whether or not it acknowledged. */
	if (!process->stop_requested)
		return;
	if (process->stop_target_count == 0)
		HAL_FATAL("process stop target underflow at thread exit");
	process->stop_target_count--;
	if (thread->stop_generation == process->stop_generation) {
		if (process->stop_ack_count == 0)
			HAL_FATAL("process stop ack underflow at thread exit");
		process->stop_ack_count--;
	}
	for (member = process->threads; member != NULL;
	    member = member->proc_next)
		if (member != thread && member->state == THREAD_SLEEPING &&
		    member->stop_generation == process->stop_generation)
			sched_wakeup(member);
}

static int
process_commit_thread_exit(struct thread *thread, int status)
{
	struct process *process = thread != NULL ? thread->proc : NULL;
	struct thread *member;
	unsigned long process_irq, tree_irq;
	int owner = 0;

	if (process == NULL || process == &process0)
		return 0;
	tree_irq = spin_lock_irqsave(&process_tree_lock);
	process_irq = spin_lock_irqsave(&process->lock);
	process_thread_exit_publish_locked(process, thread);
	if (process->state == PROCESS_RUNNING ||
	    process->state == PROCESS_STOPPED) {
		owner = 1;
		for (member = process->threads; member != NULL;
		    member = member->proc_next)
			if (!member->exit_committed &&
			    member->state != THREAD_ZOMBIE &&
			    member->state != THREAD_REAPING &&
			    member->state != THREAD_DEAD) {
				owner = 0;
				break;
			}
	}
	if (owner) {
		process->stop_requested = 0;
		process->stop_target_count = 0;
		process->stop_ack_count = 0;
		process->state = PROCESS_EXITING;
		process->exit_status = (status & 0xff) << 8;
	}
	spin_unlock_irqrestore(&process->lock, process_irq);
	spin_unlock_irqrestore(&process_tree_lock, tree_irq);
	KERN_TEST_CHECKPOINT(KERN_TEST_THREAD_EXIT_COMMITTED, thread);
	return owner;
}

#ifdef ZEDBSD_PROCESS_TEST
int
process_test_commit_thread_exit(struct thread *thread, int status)
{
	return process_commit_thread_exit(thread, status);
}
#endif

static void __attribute__((noreturn))
process_exit_final(int thread_status, int wait_status)
{
	struct process *process = curthread->proc;
	unsigned long process_irq, tree_irq;
	int owner = 0;

	if (process == NULL || process == &process0)
		HAL_FATAL("process0 exit");
	tree_irq = spin_lock_irqsave(&process_tree_lock);
	process_irq = spin_lock_irqsave(&process->lock);
	process_thread_exit_publish_locked(process, curthread);
	if (process->state != PROCESS_EXITING) {
		process->stop_requested = 0;
		process->stop_target_count = 0;
		process->stop_ack_count = 0;
		process->state = PROCESS_EXITING;
		process->exit_status = wait_status;
		owner = 1;
	}
	spin_unlock_irqrestore(&process->lock, process_irq);
	spin_unlock_irqrestore(&process_tree_lock, tree_irq);
	if (owner)
		process_exit_cleanup(thread_status);
	curthread->exit_status = thread_status;
	sched_exit_current();
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
