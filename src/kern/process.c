/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Processes.
 *
 * The process tree, process groups and sessions, credentials with a
 * reader-retired replacement scheme, controlling terminals, interval
 * timers, and the exit, stop, continue, and wait machinery.  The tree
 * lock serializes every relationship change; a zombie is reaped at the
 * single point where it leaves the registry, whether by a waiter or by
 * the kernel reaper thread.
 */

#include "kern/process.h"
#ifndef ZEDBSD_PROCESS_TEST
#include "kern/process-timer.h"
#else
/*
 * Targeted host lifetime tests link only the tree/interval-timer sections.
 * Avoid importing the host's incompatible POSIX signal ABI through
 * <signal.h>; discarded cleanup callers still need a declaration.
 */
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

struct process_stop_notification {
	struct process *process;
	unsigned generation;
	int signo;
};

static void process_group_recheck_locked(pid_t session, pid_t pgrp, int notify);
static void process_group_deliver_notifications(void);
static PROCESS_EXT void child_waiters_wake(struct process *parent);
static void process_vmspace_reaper_notify(void *argument);
static void process_reaper_notify(void);
static PROCESS_EXT void reparent_children(struct process *process);
static int process_reap_threads(struct process *process);
static int process_autoreap_claim(struct process *process);
static int process_autoreap_commit(struct process *process);
static void process_reaper(void *argument);
static void detach_tty_from_list_locked(struct process *list, pid_t session, struct tty *tty, uint64_t generation);
static void release_retired_creds(struct process_retired_cred *retired);
static int wait_selector_matches(const struct process *child, pid_t selector, pid_t caller_pgrp);
static void notify_parent_job_event_tree_locked(struct process *process, int code, int status);
static void process_stop_notify(void *argument);
static void process_exit_cleanup(int thread_status) __attribute__((noreturn));
static void process_thread_exit_publish_locked(struct process *process, struct thread *thread);
static int process_commit_thread_exit(struct thread *thread, int status);
static void __attribute__((noreturn)) process_exit_final(int thread_status, int wait_status);

/*
 * Tests whether a process belongs to an orphaned process group.
 */
int
process_pgrp_is_orphaned(
	const struct process *process)
{
	unsigned long irq;
	int orphaned;

	/* The kernel process has no group. */
	if (process == NULL || process == &process0)
		return 0;

	/* Samples the flag the group recheck maintains. */
	irq = spin_lock_irqsave(&process_tree_lock);

	orphaned = (process->flags & PROCESS_PGRP_ORPHANED) != 0;

	spin_unlock_irqrestore(&process_tree_lock, irq);

	/* Reports the sampled flag. */
	return orphaned;
}

/*
 * Initializes the kernel process and its idle thread once.
 */
void
process_init(
	void)
{
	/* Initializes only once. */
	if (all_processes != NULL)
		return;

	/* Sets up process0 as a running root process in the kernel space. */
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

	/* thread0 is the current task, marked idle. */
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

#ifdef ZEDBSD_PROCESS_TEST
/*
 * Runs one autoreap claim and commit for a host test.
 */
int
process_test_autoreap_once(
	struct process *process)
{
	int reaped;

	if (!process_autoreap_claim(process))
		return 0;
	reaped = process_autoreap_commit(process);
	return reaped;
}
#endif

/*
 * Starts the kernel reaper thread once.
 */
int
process_reaper_start(
	void)
{
	int error;

	/* Starts only once. */
	if (reaper_thread != NULL)
		return 0;

	/*
	 * The current scheduler uses strict priority queues.  A low-priority
	 * reaper would starve forever while process0 remains runnable.
	 */
	error = kthread_create(process_reaper, NULL, SCHED_PRIORITY_DEFAULT,
			       &reaper_thread);
	if (error == 0) {
		thread_start(reaper_thread);
		vmspace_set_reaper_notify(process_vmspace_reaper_notify,
		    reaper_thread->task);
	}

	/* Reports why the start failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Takes a reference to a process.
 */
void
process_ref(
	struct process *process)
{
	if (process != NULL)
		refcount_get(&process->refs);
}

/*
 * Drops a reference to a process, freeing a dead one with the last.
 */
void
process_release(
	struct process *process)
{
	/* Only the last reference frees. */
	if (process == NULL || !refcount_put(&process->refs))
		return;

	/* A live process must never lose its last reference. */
	if (process == &process0 || process->state != PROCESS_DEAD)
		HAL_FATAL("releasing live process");
	kern_free(process);
}

/*
 * Reports the parent's PID, or zero without a parent.
 */
pid_t
process_parent_pid(
	struct process *process)
{
	pid_t pid;
	unsigned long irq;

	pid = 0;

	/* Ignores a missing process. */
	if (process == NULL)
		return 0;

	/* Samples the parent under the tree lock. */
	irq = spin_lock_irqsave(&process_tree_lock);

	if (process->parent != NULL)
		pid = process->parent->pid;

	spin_unlock_irqrestore(&process_tree_lock, irq);

	/* Reports the sampled PID. */
	return pid;
}

/*
 * Takes a reference to a process's parent.
 */
struct process *
process_parent_ref(
	struct process *process)
{
	struct process *parent;
	unsigned long irq;

	/* Ignores a missing process. */
	parent = NULL;
	if (process == NULL)
		return NULL;

	/* References the parent under the tree lock. */
	irq = spin_lock_irqsave(&process_tree_lock);

	parent = process->parent;
	if (parent != NULL) {
		/*
		 * The checkpoint deliberately runs while the tree lock still
		 * excludes reparent/reap, proving that the reference is
		 * acquired inside the lifetime barrier rather than after a
		 * bare pointer snapshot.
		 */
		KERN_TEST_CHECKPOINT(KERN_TEST_PROCESS_PARENT_BEFORE_REF, parent);
		process_ref(parent);
	}

	spin_unlock_irqrestore(&process_tree_lock, irq);

	/* Reports the referenced parent, or NULL. */
	return parent;
}

#ifdef ZEDBSD_PROCESS_TEST
/*
 * Changes a process's parent for a host test.
 */
void
process_test_reparent(
	struct process *child,
	struct process *parent)
{
	unsigned long irq;

	irq = spin_lock_irqsave(&process_tree_lock);

	child->parent = parent;

	spin_unlock_irqrestore(&process_tree_lock, irq);
}

/*
 * Replaces the process registry for a host test.
 */
void
process_test_set_registry(
	struct process *head)
{
	unsigned long irq;

	irq = spin_lock_irqsave(&process_tree_lock);

	all_processes = head;
	creating_processes = NULL;

	spin_unlock_irqrestore(&process_tree_lock, irq);
}
#endif

/*
 * Samples a process's controlling terminal and its generation.
 */
int
process_controlling_tty_snapshot(
	struct process *process,
	struct tty **tty,
	uint64_t *generation)
{
	unsigned long irq;

	/* Rejects a missing process or result. */
	if (process == NULL || tty == NULL || generation == NULL)
		return EINVAL;

	/* Samples both under the tree lock. */
	irq = spin_lock_irqsave(&process_tree_lock);

	*tty = process->controlling_tty;
	*generation = process->controlling_tty_generation;

	spin_unlock_irqrestore(&process_tree_lock, irq);

	/* Reports the snapshot. */
	return 0;
}

/*
 * Tests whether a process's controlling terminal is a given generation
 * of a terminal.
 */
int
process_controlling_tty_matches(
	struct process *process,
	struct tty *tty,
	uint64_t generation)
{
	int matches;
	unsigned long irq;

	/* Rejects a missing process or terminal, or the null generation. */
	if (process == NULL || tty == NULL || generation == 0)
		return 0;

	/* Compares under the tree lock. */
	irq = spin_lock_irqsave(&process_tree_lock);

	matches = process->controlling_tty == tty &&
	    process->controlling_tty_generation == generation;

	spin_unlock_irqrestore(&process_tree_lock, irq);

	/* Reports the comparison. */
	return matches;
}

/*
 * Makes a terminal generation the controlling terminal of a process.
 */
int
process_controlling_tty_attach(
	struct process *process,
	struct tty *tty,
	uint64_t generation)
{
	unsigned long irq;
	int error;

	error = 0;

	/* Rejects a missing process or terminal, or the null generation. */
	if (process == NULL || tty == NULL || generation == 0)
		return EINVAL;

	/*
	 * Exit publishes PROCESS_EXITING while holding this same tree
	 * serializer.  Refuse a late TIOCSCTTY publication after the early
	 * exit detach.
	 */
	irq = spin_lock_irqsave(&process_tree_lock);

	if (process->state == PROCESS_EXITING ||
	    process->state == PROCESS_ZOMBIE ||
	    process->state == PROCESS_DEAD) {
		error = ESRCH;
	} else if (process->controlling_tty != NULL &&
	    (process->controlling_tty != tty ||
	    process->controlling_tty_generation != generation)) {
		error = EBUSY;
	} else {
		process->controlling_tty = tty;
		process->controlling_tty_generation = generation;
	}

	spin_unlock_irqrestore(&process_tree_lock, irq);

	/* Reports why the attach failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Detaches a terminal generation from one process.
 */
void
process_controlling_tty_detach_one(
	struct process *process,
	struct tty *tty,
	uint64_t generation)
{
	unsigned long irq;

	/* Ignores a missing process or terminal, or the null generation. */
	if (process == NULL || tty == NULL || generation == 0)
		return;

	/* Clears the association only when it is still this generation. */
	irq = spin_lock_irqsave(&process_tree_lock);

	if (process->controlling_tty == tty &&
	    process->controlling_tty_generation == generation) {
		process->controlling_tty = NULL;
		process->controlling_tty_generation = 0;
	}

	spin_unlock_irqrestore(&process_tree_lock, irq);
}

/*
 * Detaches a terminal generation from every process of a session.
 */
void
process_controlling_tty_detach_session(
	pid_t session,
	struct tty *tty,
	uint64_t generation)
{
	unsigned long irq;

	/* Ignores a bad session or terminal, or the null generation. */
	if (session <= 0 || tty == NULL || generation == 0)
		return;

	/*
	 * Include unpublished fork children: association inheritance and
	 * detach are one tree-lock domain, so a child can never escape with
	 * an old slot generation while its session is being detached.
	 */
	irq = spin_lock_irqsave(&process_tree_lock);

	detach_tty_from_list_locked(all_processes, session, tty, generation);
	detach_tty_from_list_locked(creating_processes, session, tty, generation);

	spin_unlock_irqrestore(&process_tree_lock, irq);
}

/*
 * Takes a reference to a process's address space.
 */
struct vmspace *
process_vmspace_ref(
	struct process *process)
{
	struct vmspace *vmspace;
	unsigned long irq;

	vmspace = NULL;

	/* Ignores a missing process. */
	if (process == NULL)
		return NULL;

	/*
	 * The published pointer owns a strong reference.  Taking the
	 * additional reference while process->lock is held closes the
	 * exec/exit detach race.
	 */
	irq = spin_lock_irqsave(&process->lock);

	vmspace = process->vmspace;
	if (vmspace != NULL)
		vmspace_ref(vmspace);

	spin_unlock_irqrestore(&process->lock, irq);

	/* Reports the referenced address space, or NULL. */
	return vmspace;
}

/*
 * Enters a credential read section, keeping a replaced credential alive.
 */
void
process_cred_read_enter(
	struct process *process)
{
	unsigned long irq;

	/* Ignores a missing process. */
	if (process == NULL)
		return;

	/* Counts the reader under the process lock. */
	irq = spin_lock_irqsave(&process->lock);

	process->cred_readers++;

	spin_unlock_irqrestore(&process->lock, irq);
}

/*
 * Leaves a credential read section, releasing credentials retired
 * meanwhile once the last reader is gone.
 */
void
process_cred_read_leave(
	struct process *process)
{
	struct process_retired_cred *retired;
	unsigned long irq;

	/* Ignores a missing process. */
	retired = NULL;
	if (process == NULL)
		return;

	/* The last reader takes the retired list with it. */
	irq = spin_lock_irqsave(&process->lock);

	if (process->cred_readers == 0)
		HAL_FATAL("credential reader underflow");
	process->cred_readers--;
	if (process->cred_readers == 0) {
		retired = process->retired_creds;
		process->retired_creds = NULL;
	}

	spin_unlock_irqrestore(&process->lock, irq);

	release_retired_creds(retired);
}

/*
 * Reserves the memory a later credential commit needs.
 */
int
process_cred_reserve(
	struct process *process,
	struct process_cred_reservation **result)
{
	struct process_cred_reservation *reservation;

	/* Rejects a missing result or process, or the kernel process. */
	if (result == NULL)
		return EINVAL;
	*result = NULL;
	if (process == NULL)
		return EINVAL;
	if (process == &process0)
		return EPERM;

	/* Allocates the retirement record. */
	reservation = kern_calloc(1, sizeof(*reservation));
	if (reservation == NULL)
		return ENOMEM;
	*result = reservation;

	/* Reports the reservation. */
	return 0;
}

/*
 * Drops an unused credential reservation.
 */
void
process_cred_reservation_abort(
	struct process_cred_reservation *reservation)
{
	kern_free(reservation);
}

/*
 * Replaces a process's credential using a reservation, which cannot fail.
 */
void
process_cred_commit_reserved(
	struct process *process,
	struct ucred *replacement,
	struct process_cred_reservation *reservation)
{
	struct process_retired_cred *retired;
	struct ucred *old;
	unsigned long irq;

	/* Rejects a missing operand or the kernel process. */
	if (process == NULL ||
	    replacement == NULL ||
	    reservation == NULL ||
	    process == &process0)
		HAL_FATAL("invalid reserved credential commit");

	/* Retires the old credential while readers still use it. */
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

	/*
	 * Without readers the old credential goes at once.  When readers
	 * exist, reservation is now the first member of the retired list and
	 * is reclaimed by process_cred_read_leave().
	 */
	if (retired != NULL) {
		cred_release(old);
		kern_free(reservation);
	}
}

/*
 * Replaces a process's credential.
 *
 * An exec in progress owns the credential and refuses the replacement.
 */
int
process_cred_replace(
	struct process *process,
	struct ucred *replacement)
{
	struct process_cred_reservation *reservation;
	struct process_retired_cred *retired;
	struct ucred *old;
	unsigned long irq;
	int error;

	/* Rejects a missing operand or the kernel process. */
	if (process == NULL || replacement == NULL || process == &process0)
		return EPERM;
	error = process_cred_reserve(process, &reservation);
	if (error != 0)
		return error;

	/*
	 * Once exec has taken ownership of the process image, sibling
	 * credential syscalls must not race the prospective exec credential.
	 */
	irq = spin_lock_irqsave(&process->lock);

	if (process->execing) {
		spin_unlock_irqrestore(&process->lock, irq);
		process_cred_reservation_abort(reservation);
		return EBUSY;
	}

	/* Retires the old credential while readers still use it. */
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

	/* Reports the replaced credential. */
	return 0;
}

/*
 * Counts the processes and threads in the registry.
 */
void
process_resource_count(
	uint64_t *processes,
	uint64_t *threads)
{
	struct process *process;
	uint64_t pc;
	uint64_t tc;
	unsigned long irq;

	pc = 0;
	tc = 0;
	irq = spin_lock_irqsave(&process_tree_lock);

	/* Sums over the published processes. */
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

/*
 * Finds a live process by PID and takes a reference.
 */
struct process *
process_find_ref(
	pid_t pid)
{
	struct process *process;
	struct process *result;
	unsigned long irq;

	result = NULL;
	irq = spin_lock_irqsave(&process_tree_lock);

	/* Searches the registry for a live match. */
	for (process = all_processes; process != NULL; process = process->all_next) {
		if (process->pid == pid && process->state != PROCESS_DEAD) {
			process_ref(process);
			result = process;
			break;
		}
	}

	spin_unlock_irqrestore(&process_tree_lock, irq);

	/* Reports the referenced process, or NULL. */
	return result;
}

/*
 * Finds the live process with the smallest PID above a cursor.
 */
struct process *
process_find_next_ref(
	pid_t after)
{
	struct process *process;
	struct process *result;
	unsigned long irq;

	result = NULL;
	irq = spin_lock_irqsave(&process_tree_lock);

	/* Takes the minimum PID above the cursor. */
	for (process = all_processes; process != NULL; process = process->all_next) {
		if (process->state != PROCESS_DEAD &&
		    process->pid > after &&
		    (result == NULL || process->pid < result->pid))
			result = process;
	}

	if (result != NULL)
		process_ref(result);

	spin_unlock_irqrestore(&process_tree_lock, irq);

	/* Reports the referenced process, or NULL past the end. */
	return result;
}

/*
 * Finds a live thread by TID and takes a reference.
 */
struct thread *
thread_find_ref(
	tid_t tid)
{
	struct process *process;
	struct thread *result;
	unsigned long tree_irq;
	struct thread *thread;
	unsigned long process_irq;

	result = NULL;
	tree_irq = spin_lock_irqsave(&process_tree_lock);

	/* Searches every process's thread list under its lock. */
	for (process = all_processes; process != NULL && result == NULL;
	     process = process->all_next) {
		process_irq = spin_lock_irqsave(&process->lock);
		for (thread = process->threads; thread != NULL;
		     thread = thread->proc_next) {
			if (thread->tid == tid && thread->state != THREAD_DEAD) {
				thread_ref(thread);
				result = thread;
				break;
			}
		}

		spin_unlock_irqrestore(&process->lock, process_irq);
	}

	spin_unlock_irqrestore(&process_tree_lock, tree_irq);

	/* Reports the referenced thread, or NULL. */
	return result;
}

/*
 * Moves a process into a process group of its session.
 *
 * The caller may move itself or a child that has not exec'd, and only
 * into its own group or an existing group of the same session.
 */
int
process_setpgid(
	struct process *caller,
	pid_t pid,
	pid_t pgid)
{
	struct process *target;
	struct process *member;
	unsigned long irq;
	int error;
	pid_t old_pgrp;

	error = 0;

	/* Rejects a missing caller, the kernel process, or negative ids. */
	if (caller == NULL || caller == &process0 || pid < 0 || pgid < 0)
		return EINVAL;
	if (pid == 0)
		target = caller;
	else
		target = process_find_ref(pid);
	if (target == NULL)
		return ESRCH;

	/* The target must be the caller or a child, in the same session. */
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

	/* A group other than the target's own must already exist in the session. */
	if (pgid == 0)
		pgid = target->pid;
	if (pgid != target->pid) {
		for (member = all_processes; member != NULL;
		     member = member->all_next) {
			if (member->session == caller->session &&
			    member->pgrp == pgid)
				break;
		}

		if (member == NULL)
			error = EPERM;
	}

	/* Moves the target, rechecking both groups for orphaning. */
	if (error == 0) {
		old_pgrp = target->pgrp;
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

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Makes a process the leader of a new session and process group.
 */
pid_t
process_setsid(
	struct process *process)
{
	struct process *member;
	unsigned long irq;
	pid_t old_session;
	pid_t old_pgrp;

	/* Rejects a missing process or the kernel process. */
	if (process == NULL || process == &process0)
		return -EPERM;

	/* A process group leader cannot start a session. */
	irq = spin_lock_irqsave(&process_tree_lock);

	for (member = all_processes; member != NULL; member = member->all_next) {
		if (member->pgrp == process->pid) {
			spin_unlock_irqrestore(&process_tree_lock, irq);
			return -EPERM;
		}
	}

	/* Moves the process, rechecking the old group for orphaning. */
	process_group_recheck_locked(process->session, process->pgrp, 0);
	old_session = process->session;
	old_pgrp = process->pgrp;
	process->session = process->pid;
	process->pgrp = process->pid;
	process_group_recheck_locked(old_session, old_pgrp, 1);
	process_group_recheck_locked(process->session, process->pgrp, 0);

	spin_unlock_irqrestore(&process_tree_lock, irq);

	/* A new session has no controlling terminal. */
	tty_detach_process(process);
	process_group_deliver_notifications();

	/* Reports the new session id. */
	return process->pid;
}

/*
 * Tests whether a session contains a process group.
 */
int
process_pgrp_in_session(
	pid_t session,
	pid_t pgrp)
{
	struct process *member;
	unsigned long irq;
	int found;

	found = 0;

	/* Rejects bad ids. */
	if (session <= 0 || pgrp <= 0)
		return 0;

	/* Searches for a live member of the group in the session. */
	irq = spin_lock_irqsave(&process_tree_lock);

	for (member = all_processes; member != NULL; member = member->all_next) {
		if (member->state != PROCESS_DEAD &&
		    member->session == session &&
		    member->pgrp == pgrp) {
			found = 1;
			break;
		}
	}

	spin_unlock_irqrestore(&process_tree_lock, irq);

	/* Reports the search result. */
	return found;
}

/*
 * Sends a signal to every process of a group.
 */
int
process_signal_pgrp(
	pid_t session,
	pid_t pgrp,
	int signo)
{
	int error;

	/* Reports why the delivery failed. */
	error = process_signal_pgrp_except(session, pgrp, signo, NULL);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Sends a signal to every process of a group except one.
 */
int
process_signal_pgrp_except(
	pid_t session,
	pid_t pgrp,
	int signo,
	struct process *excluded)
{
	struct process *member;
	pid_t cursor;
	int found;

	cursor = -1;
	found = 0;

	/* Rejects bad ids or an invalid signal. */
	if (session <= 0 || pgrp <= 0 || signo <= 0 || signo >= NSIG)
		return EINVAL;

	/* Walks the registry by PID, holding each member while signaling. */
	member = process_find_next_ref(cursor);
	while (member != NULL) {
		cursor = member->pid;
		if (member != excluded &&
		    member->state != PROCESS_DEAD &&
		    member->session == session &&
		    member->pgrp == pgrp) {
			(void)signal_send_process(member, signo);
			found = 1;
		}

		process_release(member);
		member = process_find_next_ref(cursor);
	}

	/* Reports an empty group as ESRCH. */
	if (!found)
		return ESRCH;
	return 0;
}

/*
 * Creates an unpublished child of a parent with a reserved PID.
 *
 * The child inherits the parent's umask, nice value, limits, credential,
 * working directory, and job-control identity.  A requested PID must be
 * free; zero takes the next one from the allocator.
 */
int
process_create(
	struct process *parent,
	pid_t requested_pid,
	struct process **result)
{
	struct process *process;
	struct process *candidate;
	unsigned long parent_irq;
	unsigned long tree_irq;
	pid_t assigned;
	int error;
	int collision;

	/* Rejects a missing parent or result, or a negative PID. */
	if (parent == NULL || result == NULL || requested_pid < 0)
		return EINVAL;

	/* Allocates the process and its descriptor table. */
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

	/* Inherits the parent's attributes. */
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
		error = cwdinfo_clone(parent->cwdi, &process->cwdi);
		if (error != 0) {
			cred_release(process->cred);
			filedesc_destroy(process->fd);
			kern_free(process);
			return error;
		}
	}

	/*
	 * Reserve the PID before returning an unpublished process.  The
	 * creating list closes both requested-PID races and allocator wrap
	 * collisions without exposing half-initialized processes to lookup.
	 */
	tree_irq = spin_lock_irqsave(&process_tree_lock);

	if (requested_pid != 0)
		assigned = requested_pid;
	else
		assigned = next_pid;
	for (;;) {
		collision = assigned <= 0;
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

		if (assigned == INT32_MAX)
			assigned = 1;
		else
			assigned = assigned + 1;
	}

	process->pid = assigned;
	if (assigned == INT32_MAX)
		next_pid = 1;
	else
		next_pid = assigned + 1;

	/* A child of the kernel process leads its own session and group. */
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

	/* Reports the unpublished child. */
	return 0;
}

/*
 * Forks the calling process.
 *
 * The child gets copies of the descriptor table and address space, a
 * forked task and thread, and the parent's signal state, and is
 * published running.
 */
int
process_fork(
	struct process *parent,
	struct process **result)
{
	struct process *child;
	struct filedesc *files;
	struct thread *thread;
	hal_task_t task;
	int error;

	child = NULL;
	files = NULL;
	task = NULL;

	/* Only the calling user process with an address space can fork. */
	if (parent == NULL ||
	    parent == &process0 ||
	    result == NULL ||
	    parent != curthread->proc ||
	    parent->vmspace == NULL ||
	    parent->fd == NULL)
		return EINVAL;

	/* Creates the child and copies the descriptors and address space. */
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

	/* Forks the task and thread, then publishes and starts the child. */
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

	/* Reports the running child. */
	return 0;

fail:
	if (task != NULL)
		hal_task_destroy(task);
	filedesc_destroy(files);
	process_free_mem(child);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Publishes a created process in the registry and its parent's children.
 */
void
process_publish(
	struct process *process)
{
	struct process **link;
	unsigned long irq;

	/* Ignores anything but a new process. */
	if (process == NULL ||
	    process == &process0 ||
	    process->state != PROCESS_NEW)
		return;

	/* Moves the process from the creating list to the registry. */
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

/*
 * Gives the kernel process the boot working directory.
 */
void
process_attach_boot_cwd(
	struct cwdinfo *cwd)
{
	process0.cwdi = cwd;
}

/*
 * Frees a process that has no threads left, removing it from the tree.
 *
 * The removal from the registry and the parent's children is the reap
 * commit that a waiter sleeping on a reserved zombie waits for.
 */
void
process_free_mem(
	struct process *process)
{
	struct process **link;
	struct process **child_link;
	unsigned long tree_irq;
	unsigned long process_irq;
	struct vmspace *vmspace;
	struct filedesc *fd;
	struct ucred *cred;
	struct process_retired_cred *retired;
	struct cwdinfo *cwdi;
	pid_t old_session;
	pid_t old_pgrp;

	/* Ignores a missing process, the kernel process, or the caller's own. */
	if (process == NULL ||
	    process == &process0 ||
	    (curthread != NULL && process == curthread->proc))
		return;

	/* Takes the resources out of a process without threads. */
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

	/* Unlinks the process from the registry, or the creating list. */
	tree_irq = spin_lock_irqsave(&process_tree_lock);

	old_session = process->session;
	old_pgrp = process->pgrp;
	process_group_recheck_locked(old_session, old_pgrp, 0);
	if (process->children != NULL)
		HAL_FATAL("freeing process with children");
	link = &all_processes;
	while (*link != NULL && *link != process)
		link = &(*link)->all_next;
	if (*link == process) {
		*link = process->all_next;
	} else {
		link = &creating_processes;
		while (*link != NULL && *link != process)
			link = &(*link)->all_next;
		if (*link == process)
			*link = process->all_next;
	}

	/* Unlinks it from the parent's children and wakes the waiters. */
	if (process->parent != NULL) {
		child_link = &process->parent->children;
		while (*child_link != NULL && *child_link != process)
			child_link = &(*child_link)->sibling;
		if (*child_link == process) {
			*child_link = process->sibling;

			/*
			 * A waiter which skipped this reserved zombie must rescan
			 * now that removal, rather than reservation release, is
			 * the reap commit.
			 */
			child_waiters_wake(process->parent);
		}
	}

	process->state = PROCESS_DEAD;
	process_group_recheck_locked(old_session, old_pgrp, 1);

	spin_unlock_irqrestore(&process_tree_lock, tree_irq);

	process_group_deliver_notifications();

	/* Releases the resources and the registry's reference. */
	if (vmspace != NULL)
		vmspace_put(vmspace);
	filedesc_destroy(fd);
	cred_release(cred);
	release_retired_creds(retired);
	cwdinfo_release(cwdi);
	process_release(process);
}

/*
 * Waits for one child process to exit and reaps it.
 */
int
process_wait(
	struct process *process,
	int *status)
{
	struct thread *thread;
	unsigned long irq;
	int error;
	int thread_status;
	uint64_t sequence;

	/* Only the parent may wait. */
	if (process == NULL || process == &process0 || curthread == NULL)
		return ECHILD;
	irq = spin_lock_irqsave(&process_tree_lock);

	if (process->parent != curthread->proc) {
		spin_unlock_irqrestore(&process_tree_lock, irq);
		return ECHILD;
	}

	/* Sleeps until the child is a zombie. */
	while (process->state != PROCESS_ZOMBIE) {
		sequence = waitq_sequence(&process->parent->child_waitq);
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

	/* Joins the last thread and frees the process. */
	error = thread_wait(thread, &thread_status);
	thread_release(thread);
	if (error != 0)
		return error;
	(void)thread_status;
	if (status != NULL)
		*status = process->exit_status;
	process_free_mem(process);

	/* Reports the reaped child. */
	return 0;
}

/*
 * Selects a child with a pending wait event and reserves it.
 *
 * The selector is a PID, -1 for any child, 0 for the caller's group, or
 * the negated group id.  The reserved event must be committed or
 * aborted.  Reports the child's PID, zero with WNOHANG, or a negated
 * error.
 */
PROCESS_EXT pid_t
process_wait_select_mask(
	struct process *parent,
	pid_t selector,
	int options,
	unsigned event_mask,
	struct process_wait_event *event)
{
	unsigned long irq;
	struct process *child;
	int matched;
	uint64_t sequence;
	int error;

	/* Rejects anything but the calling process with a valid request. */
	if (parent == NULL ||
	    parent != curthread->proc ||
	    selector == INT32_MIN ||
	    event == NULL ||
	    (options & ~WNOHANG) != 0 ||
	    event_mask == 0 ||
	    (event_mask & ~(PROCESS_WAIT_EVENT_EXITED |
	    PROCESS_WAIT_EVENT_STOPPED | PROCESS_WAIT_EVENT_CONTINUED)) != 0)
		return -EINVAL;
	memset(event, 0, sizeof(*event));

	/* Scans the children, sleeping between scans unless WNOHANG. */
	irq = spin_lock_irqsave(&process_tree_lock);

	for (;;) {
		matched = 0;
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
			/* Reserves the event so another waiter cannot take it. */
			child->wait_reserved = event->kind;
			process_ref(child);
			event->parent = parent;
			event->child = child;
			event->pid = child->pid;
			if (child->cred != NULL)
				event->uid = child->cred->ruid;
			else
				event->uid = 0;
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

		sequence = waitq_sequence(&parent->child_waitq);
		error = waitq_sleep(&parent->child_waitq,
		    &process_tree_lock, sequence, 0, WAITQ_INTERRUPTIBLE);
		if (error != EINTR)
			continue;
		spin_unlock_irqrestore(&process_tree_lock, irq);
		return -EINTR;
	}
}

/*
 * Commits a reserved wait event, reaping an exited child.
 *
 * The exited child's CPU time is charged to the parent's children
 * counters before it is freed.
 */
PROCESS_EXT int
process_wait_commit(
	struct process_wait_event *event)
{
	struct process *child;
	unsigned long irq;
	int error;
	int reap;
	uint64_t child_ticks;
	uint64_t child_user_ticks;
	uint64_t child_system_ticks;

	error = 0;
	reap = 0;

	/* Rejects an empty event. */
	if (event == NULL ||
	    event->parent == NULL ||
	    event->child == NULL ||
	    event->kind == PROCESS_WAIT_NONE)
		return EINVAL;

	/* The reservation must still be this event's. */
	irq = spin_lock_irqsave(&process_tree_lock);

	child = event->child;
	if (child->parent != event->parent ||
	    child->pid != event->pid ||
	    child->wait_reserved != event->kind) {
		error = ECHILD;
		goto out;
	}

	/* A stop or continue is consumed; an exit is reaped below. */
	if (event->kind == PROCESS_WAIT_STOPPED) {
		child->wait_stopped = 0;
	} else if (event->kind == PROCESS_WAIT_CONTINUED) {
		child->wait_continued = 0;
	} else {
		/*
		 * Detached threads may self-reap immediately after scheduler
		 * retirement, leaving a fully valid zombie with an empty thread
		 * list.
		 */
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

	/*
	 * Keep PROCESS_WAIT_EXITED reserved until process_free_mem() removes
	 * the child from both the registry and the parent's list.  A second
	 * waiter therefore sleeps instead of committing the same zombie.
	 */
	if (error == 0 && reap) {
		KERN_TEST_CHECKPOINT(KERN_TEST_PROCESS_WAIT_REAP_RESERVED, child);
		error = process_reap_threads(child);
		if (error == 0) {
			child_ticks =
			    atomic_u64_load_acquire(&child->cpu_ticks) +
			    atomic_u64_load_acquire(&child->child_cpu_ticks);
			child_user_ticks =
			    atomic_u64_load_acquire(&child->user_ticks) +
			    atomic_u64_load_acquire(&child->child_user_ticks);
			child_system_ticks =
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

	/* Drops the event's reference on success. */
	if (error == 0) {
		process_release(child);
		memset(event, 0, sizeof(*event));
	}

	/* Reports why the commit failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Abandons a reserved wait event, making it available again.
 */
PROCESS_EXT void
process_wait_abort(
	struct process_wait_event *event)
{
	unsigned long irq;
	struct process *child;

	/* Ignores an empty event. */
	if (event == NULL || event->child == NULL || event->parent == NULL)
		return;

	/* Releases the reservation when it is still this event's. */
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

/*
 * Waits for a child in one step, as waitpid() does.
 */
PROCESS_EXT pid_t
process_waitpid(
	struct process *parent,
	pid_t selector,
	int *status,
	int options)
{
	struct process_wait_event event;
	pid_t pid;
	int error;

	/* Selects and reserves an event. */
	pid = process_wait_select(parent, selector, options, &event);
	if (pid <= 0)
		return pid;
	if (status != NULL)
		*status = event.status;

	/* Commits it, abandoning it on failure. */
	error = process_wait_commit(&event);
	if (error != 0) {
		process_wait_abort(&event);
		return -error;
	}

	/* Reports the child's PID. */
	return pid;
}

/*
 * Stops the calling process at a safe point for a stop signal.
 *
 * The first thread to arrive starts a stop generation and interrupts
 * its siblings; the thread that completes the generation publishes the
 * stopped state and notifies the parent.  Every thread sleeps until
 * process_continue().
 */
void
process_stop_current(
	int signo)
{
	struct process *process;
	struct process_stop_notification notification;
	struct thread *member;
	unsigned long process_irq;
	int notify;

	if (curthread != NULL)
		process = curthread->proc;
	else
		process = NULL;
	notify = 0;

	/* Ignores a missing process or the kernel process. */
	if (process == NULL || process == &process0)
		return;

	/* The first arrival starts the generation and counts its targets. */
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
		     member = member->proc_next) {
			if (!member->exit_committed &&
			    member->state != THREAD_EXITING &&
			    member->state != THREAD_ZOMBIE &&
			    member->state != THREAD_REAPING &&
			    member->state != THREAD_DEAD)
				process->stop_target_count++;
		}

		for (member = process->threads; member != NULL;
		     member = member->proc_next) {
			if (member != curthread &&
			    !member->exit_committed &&
			    member->state != THREAD_EXITING &&
			    member->state != THREAD_ZOMBIE &&
			    member->state != THREAD_REAPING &&
			    member->state != THREAD_DEAD)
				sched_interrupt(member);
		}
	}

	/* Acknowledges the generation once per thread. */
	if (curthread->stop_generation != process->stop_generation) {
		curthread->stop_generation = process->stop_generation;
		process->stop_ack_count++;
	}

	/*
	 * Re-evaluate after every wake.  A target may retire before reaching
	 * its safe point; thread_sched_retired() then shrinks the generation
	 * target and wakes an acknowledged waiter to finish the transition.
	 */
	while (process->stop_requested) {
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

			/*
			 * sched_sleep_locked_notify() calls process_stop_notify()
			 * synchronously, so this stack-backed argument remains
			 * valid for the complete callback and is never retained.
			 */
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

/*
 * Tests whether a thread's process has a stop pending.
 */
int
process_stop_requested(
	const struct thread *thread)
{
	if (thread == NULL)
		return 0;
	if (thread->proc == NULL)
		return 0;
	if (atomic_raw_load_acquire(&thread->proc->stop_requested) == 0)
		return 0;
	return 1;
}

/*
 * Resumes a stopped or stopping process.
 *
 * Reports whether the process was actually stopped; a resumed stop is
 * reported to the parent when asked.
 */
int
process_continue(
	struct process *process,
	int report_continued)
{
	struct thread *thread;
	unsigned long process_irq;
	unsigned long tree_irq;
	int continued;

	continued = 0;

	/* Ignores a missing process or the kernel process. */
	if (process == NULL || process == &process0)
		return 0;

	/* Cancels the stop and wakes the sleeping threads. */
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
		     thread = thread->proc_next) {
			if (thread->state == THREAD_SLEEPING)
				sched_wakeup(thread);
		}
	}

	spin_unlock_irqrestore(&process->lock, process_irq);

	/* Notifies the parent under the tree lock, ordered after the stop. */
	if (continued && report_continued)
		notify_parent_job_event_tree_locked(process, CLD_CONTINUED, SIGCONT);

	spin_unlock_irqrestore(&process_tree_lock, tree_irq);

	/* Reports whether the process had been stopped. */
	return continued;
}

/*
 * Reads an interval timer consistently.
 */
int
process_itimer_get(
	struct process *process,
	int which,
	uint64_t *remaining,
	uint64_t *interval)
{
	unsigned before;
	unsigned after;

	/* Rejects a missing process or result, or an unknown timer. */
	if (process == NULL ||
	    which < 0 ||
	    which >= 3 ||
	    remaining == NULL ||
	    interval == NULL)
		return EINVAL;

	/* Retries until the sequence is even and unchanged around the read. */
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

	/* Reports the consistent values. */
	return 0;
}

/*
 * Sets an interval timer, reporting the old values.
 */
int
process_itimer_set(
	struct process *process,
	int which,
	uint64_t remaining,
	uint64_t interval,
	uint64_t *old_remaining,
	uint64_t *old_interval)
{
	unsigned sequence;
	bool enabled;

	/* Rejects a missing process or an unknown timer. */
	if (process == NULL || which < 0 || which >= 3)
		return EINVAL;

	/* Takes the sequence lock by making it odd. */
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

	/* Swaps the values and releases the sequence. */
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

	/* Reports the set timer. */
	return 0;
}

/*
 * Counts one tick against an interval timer, reporting its expiry.
 */
int
process_itimer_tick(
	struct process *process,
	int which)
{
	uint64_t remaining;
	uint64_t interval;
	unsigned sequence;

	/* Ignores a missing process or an unknown timer. */
	if (process == NULL || which < 0 || which >= 3)
		return 0;

	/*
	 * A timer tick is accounting, not a best-effort notification.
	 * Serialize with setitimer() and ticks arriving on other CPUs until
	 * this exact tick owns the sequence; returning on an odd/CAS conflict
	 * loses CPU time.
	 */
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

	/* Counts down, reloading the interval on expiry. */
	remaining = atomic_u64_load_acquire(&process->itimer_remaining[which]);
	interval = atomic_u64_load_acquire(&process->itimer_interval[which]);
	if (remaining != 0) {
		if (remaining == 1U)
			atomic_u64_store_release(&process->itimer_remaining[which],
			    interval);
		else
			atomic_u64_store_release(&process->itimer_remaining[which],
			    remaining - 1U);
	}

	atomic_raw_store_release(&process->itimer_sequence[which], sequence + 2U);

	/* Reports whether this tick expired the timer. */
	if (remaining != 1U)
		return 0;
	return 1;
}

/*
 * Ticks the real-time interval timer of every process, raising SIGALRM
 * on expiry.
 */
void
process_itimer_real_tick_all(
	void)
{
	struct process *process;
	struct signal_info info;
	pid_t cursor;

	cursor = -1;
	memset(&info, 0, sizeof(info));
	info.code = SI_TIMER;

	/*
	 * Hold a process reference, not the global tree lock, while queuing
	 * the signal.  Delivery can wake a remote CPU and must not extend the
	 * process registry critical section on every timer tick.
	 */
	process = process_find_next_ref(cursor);
	while (process != NULL) {
		cursor = process->pid;
		if (process != &process0 &&
		    process->state != PROCESS_DEAD &&
		    process_itimer_tick(process, 0))
			(void)signal_send_process_info(process, SIGALRM, &info);
		process_release(process);
		process = process_find_next_ref(cursor);
	}
}

/*
 * Selects a child wait event for the events waitpid() options request.
 */
PROCESS_EXT pid_t
process_wait_select(
	struct process *parent,
	pid_t selector,
	int options,
	struct process_wait_event *event)
{
	unsigned mask;
	pid_t pid;

	/* Maps the options to the event mask. */
	mask = PROCESS_WAIT_EVENT_EXITED;
	if ((options & WUNTRACED) != 0)
		mask |= PROCESS_WAIT_EVENT_STOPPED;
	if ((options & WCONTINUED) != 0)
		mask |= PROCESS_WAIT_EVENT_CONTINUED;
	pid = process_wait_select_mask(parent, selector, options & WNOHANG,
	    mask, event);

	/* Reports the selection. */
	return pid;
}

/*
 * Finishes a process whose last thread the scheduler retired.
 *
 * The exiting process becomes a zombie, drops its address space, and
 * notifies its parent with SIGCHLD or the reaper when it is autoreaped.
 */
void
process_thread_retired(
	struct thread *thread)
{
	struct process *process;
	struct process *parent;
	struct thread *member;
	struct vmspace *dead_vmspace;
	struct signal_info info;
	unsigned long irq;
	unsigned long process_irq;
	int notify;
	int autoreap;
	int final_cleanup;
	int last;

	/* Starts assuming this is the last thread and nothing to notify. */
	parent = NULL;
	dead_vmspace = NULL;
	notify = 0;
	autoreap = 0;
	final_cleanup = 0;
	last = 1;

	/* Ignores a thread without a process. */
	if (thread == NULL)
		return;
	process = thread->proc;
	if (process == NULL)
		return;

	/* The last retiring thread of an exiting process makes it a zombie. */
	irq = spin_lock_irqsave(&process_tree_lock);
	process_irq = spin_lock_irqsave(&process->lock);

	for (member = process->threads; member != NULL; member = member->proc_next) {
		if (member->state != THREAD_ZOMBIE &&
		    member->state != THREAD_REAPING &&
		    member->state != THREAD_DEAD) {
			last = 0;
			break;
		}
	}

	if (last && process != &process0 && process->state == PROCESS_EXITING) {
		/*
		 * No task can enter this address space again.  Keep only
		 * wait-visible metadata in the zombie instead of pinning all
		 * user mappings until the parent reaps it.
		 */
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

	/*
	 * Exit performs an early teardown before terminating sibling threads.
	 * This final idempotent pass closes any device/timer admission which
	 * was already in flight at that point; the retired hook's process
	 * reference protects the object across both calls.
	 */
	if (final_cleanup) {
		process_timer_cleanup(process);
		tty_detach_process(process);
	}

	/* vmspace destruction may enter VFS and is performed by the reaper. */
	if (dead_vmspace != NULL && dead_vmspace != &kernel_vmspace)
		vmspace_put_deferred(dead_vmspace);

	/* Tells the parent how the child ended. */
	if (notify) {
		memset(&info, 0, sizeof(info));
		if ((process->exit_status & 0x7f) == 0)
			info.code = CLD_EXITED;
		else
			info.code = CLD_KILLED;
		info.pid = process->pid;
		if (process->cred != NULL)
			info.uid = process->cred->ruid;
		else
			info.uid = 0;
		if (info.code == CLD_EXITED)
			info.status = (process->exit_status >> 8) & 0xff;
		else
			info.status = process->exit_status & 0x7f;
		(void)signal_send_process_info(parent, SIGCHLD, &info);
	}

	if (parent != NULL)
		process_release(parent);
	if (autoreap)
		process_reaper_notify();
}

/*
 * Exits the calling thread, and the process when it was the last one.
 */
void
process_exit_if_last_thread(
	int status)
{
	struct thread *thread;
	int owner;

	/* Ignores a thread without a user process. */
	thread = curthread;
	if (thread == NULL || thread->proc == NULL || thread->proc == &process0)
		return;

	/* The thread that commits the process exit performs the cleanup. */
	owner = process_commit_thread_exit(thread, status);
	if (owner)
		process_exit_cleanup(status);
}

#ifdef ZEDBSD_PROCESS_TEST
/*
 * Commits a thread exit for a host test.
 */
int
process_test_commit_thread_exit(
	struct thread *thread,
	int status)
{
	int owner;

	owner = process_commit_thread_exit(thread, status);
	return owner;
}
#endif

/*
 * Exits the calling process with an exit status.
 */
void
exit1(
	int status)
{
	process_exit_final(status, (status & 0xff) << 8);
}

/*
 * Exits the calling process as killed by a signal.
 */
void
exit1_signal(
	int signo)
{
	process_exit_final(128 + signo, signo & 0x7f);
}

/* Recomputes a process group's orphaned flag; the caller holds the tree lock. */
static void
process_group_recheck_locked(
	pid_t session,
	pid_t pgrp,
	int notify)
{
	struct process *member;
	int have_member;
	int was_orphaned;
	int orphaned;
	int stopped;
	struct process *parent;

	have_member = 0;
	was_orphaned = 1;
	orphaned = 1;
	stopped = 0;

	/* Ignores a bad group. */
	if (pgrp <= 0)
		return;

	/* A group is orphaned when no member's parent is in another group of the session. */
	for (member = all_processes; member != NULL; member = member->all_next) {
		if (member->state == PROCESS_DEAD ||
		    member->session != session ||
		    member->pgrp != pgrp)
			continue;
		have_member = 1;
		if ((member->flags & PROCESS_PGRP_ORPHANED) == 0)
			was_orphaned = 0;
		if (member->state == PROCESS_STOPPED)
			stopped = 1;
		parent = member->parent;
		if (parent != NULL &&
		    parent != &process0 &&
		    parent->session == session &&
		    parent->pgrp != pgrp)
			orphaned = 0;
	}

	if (!have_member)
		return;

	/* Publishes the flag, marking a newly orphaned stopped group for SIGHUP. */
	for (member = all_processes; member != NULL; member = member->all_next) {
		if (member->state == PROCESS_DEAD ||
		    member->session != session ||
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

/* Sends SIGHUP and SIGCONT to every process marked by an orphaning recheck. */
static void
process_group_deliver_notifications(
	void)
{
	struct process *process;
	pid_t cursor;
	unsigned long irq;
	int notify;

	cursor = -1;

	/* Takes each mark under the tree lock and signals outside it. */
	process = process_find_next_ref(cursor);
	while (process != NULL) {
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
		process = process_find_next_ref(cursor);
	}
}

/* Wakes the waiters on a parent's children. */
static PROCESS_EXT void
child_waiters_wake(
	struct process *parent)
{
	if (parent != NULL)
		waitq_wake_all(&parent->child_waitq);
}

/* Wakes the reaper task; the scheduler latch closes its scan-to-sleep window. */
static void
process_vmspace_reaper_notify(
	void *argument)
{
	hal_task_t task;

	task = argument;

	/*
	 * sched_notify_task() is a retained notification: if the reaper has
	 * not entered kernel_wait_task() yet, notify_pending closes that
	 * handoff.
	 */
	sched_notify_task(task);
}

/* Wakes the reaper thread when it exists. */
static void
process_reaper_notify(
	void)
{
	if (reaper_thread != NULL)
		process_vmspace_reaper_notify(reaper_thread->task);
}

/* Gives an exiting process's children to init, or to process0 without one. */
static PROCESS_EXT void
reparent_children(
	struct process *process)
{
	struct process *adopter;
	struct process *candidate;
	struct process *child;
	int wake_reaper;
	pid_t child_session;
	pid_t child_pgrp;

	adopter = &process0;
	wake_reaper = 0;

	/*
	 * PID 1 is the userspace child reaper.  Falling back to process0 is
	 * only necessary while init itself is exiting or before it has been
	 * spawned.
	 */
	for (candidate = all_processes; candidate != NULL;
	     candidate = candidate->all_next) {
		if (candidate != process &&
		    candidate->pid == 1 &&
		    candidate->state != PROCESS_EXITING &&
		    candidate->state != PROCESS_ZOMBIE &&
		    candidate->state != PROCESS_DEAD) {
			adopter = candidate;
			break;
		}
	}

	/* Moves each child, rechecking its group around the move. */
	child = process->children;
	while (child != NULL) {
		child_session = child->session;
		child_pgrp = child->pgrp;
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
		child = process->children;
	}

	child_waiters_wake(adopter);
	if (wake_reaper)
		process_reaper_notify();
}

/* Joins every thread of a zombie; claim and commit share waitpid()'s EXITED reservation. */
static int
process_reap_threads(
	struct process *process)
{
	struct thread *thread;
	unsigned long irq;
	int error;

	/*
	 * Detached self-reap removes the list entry under process->lock.
	 * Acquire the reference in that same domain so the scan cannot race
	 * the final unlink/free between loading process->threads and
	 * thread_ref().
	 */
	for (;;) {
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

/* Reserves an autoreaped zombie for the reaper, as waitpid() reserves for a waiter. */
static int
process_autoreap_claim(
	struct process *process)
{
	unsigned long irq;
	int claimed;

	/* Reserves an unclaimed autoreaping zombie for this caller. */
	irq = spin_lock_irqsave(&process_tree_lock);

	claimed = 0;
	if (process != NULL &&
	    process != &process0 &&
	    (process->flags & PROCESS_AUTOREAP) != 0 &&
	    process->state == PROCESS_ZOMBIE &&
	    process->wait_reserved == PROCESS_WAIT_NONE)
		claimed = 1;
	if (claimed)
		process->wait_reserved = PROCESS_WAIT_EXITED;

	spin_unlock_irqrestore(&process_tree_lock, irq);

	/* Reports whether this caller owns the reap. */
	return claimed;
}

/* Reaps a claimed zombie, releasing the claim when its threads cannot be joined. */
static int
process_autoreap_commit(
	struct process *process)
{
	unsigned long irq;
	int error;

	/* Joins the threads and frees the process. */
	error = process_reap_threads(process);
	if (error == 0) {
		process_free_mem(process);
		return 1;
	}

	/* Releases the reservation for a later attempt. */
	irq = spin_lock_irqsave(&process_tree_lock);

	if (process->wait_reserved == PROCESS_WAIT_EXITED) {
		process->wait_reserved = PROCESS_WAIT_NONE;
		child_waiters_wake(process->parent);
	}

	spin_unlock_irqrestore(&process_tree_lock, irq);

	return 0;
}

/* Runs the kernel reaper: frees dead address spaces and autoreaped zombies. */
static void
process_reaper(
	void *argument)
{
	struct process *process;
	pid_t cursor;
	int reaped;

	(void)argument;

	/* Scans forever, sleeping when a scan found nothing. */
	for (;;) {
		process = NULL;
		cursor = -1;
		reaped = vmspace_reap_pending() != 0;
		process = process_find_next_ref(cursor);
		while (process != NULL) {
			cursor = process->pid;
			if (process_autoreap_claim(process) &&
			    process_autoreap_commit(process))
				reaped = 1;
			process_release(process);
			process = process_find_next_ref(cursor);
		}

		if (reaped) {
			sched_yield();
		} else {
			/*
			 * The scheduler retains notifications delivered after
			 * the empty scan but before this call, so no
			 * condition-lock inversion is needed to close the
			 * scan-to-sleep handoff.
			 */
			kernel_wait_task();
		}
	}
}

/* Clears a terminal generation from every session member of a list. */
static void
detach_tty_from_list_locked(
	struct process *list,
	pid_t session,
	struct tty *tty,
	uint64_t generation)
{
	struct process *member;

	/* Clears the terminal from every process of the session that held it. */
	for (member = list; member != NULL; member = member->all_next) {
		if (member->session == session &&
		    member->controlling_tty == tty &&
		    member->controlling_tty_generation == generation) {
			member->controlling_tty = NULL;
			member->controlling_tty_generation = 0;
		}
	}
}

/* Releases a list of retired credentials. */
static void
release_retired_creds(
	struct process_retired_cred *retired)
{
	struct process_retired_cred *next;

	/* Releases every credential the exit deferred. */
	while (retired != NULL) {
		next = retired->next;
		cred_release(retired->cred);
		kern_free(retired);
		retired = next;
	}
}

/* Tests whether a child matches a waitpid() selector. */
static int
wait_selector_matches(
	const struct process *child,
	pid_t selector,
	pid_t caller_pgrp)
{
	/* A positive selector is a PID. */
	if (selector > 0) {
		if (child->pid != selector)
			return 0;
		return 1;
	}

	/* -1 is any child, 0 the caller's group, and a negative value a group. */
	if (selector == -1)
		return 1;
	if (selector == 0) {
		if (child->pgrp != caller_pgrp)
			return 0;
		return 1;
	}

	if (child->pgrp != -selector)
		return 0;
	return 1;
}

/* Sends SIGCHLD for a stop or continue and wakes the parent's waiters; the caller holds the tree lock. */
static void
notify_parent_job_event_tree_locked(
	struct process *process,
	int code,
	int status)
{
	struct signal_info info;
	struct process *parent;
	unsigned long irq;
	unsigned action_flags;

	/*
	 * Keeping the tree serializer through signal generation makes
	 * job-control transition publication and parent notification one
	 * ordered stream: process_continue() cannot overtake a validated
	 * STOP, and a new STOP callback cannot overtake CONTINUED.
	 */
	if (process != NULL)
		parent = process->parent;
	else
		parent = NULL;
	if (parent == NULL || parent == &process0)
		return;

	/* Describes the event. */
	memset(&info, 0, sizeof(info));
	info.code = code;
	info.pid = process->pid;
	if (process->cred != NULL)
		info.uid = process->cred->ruid;
	else
		info.uid = 0;
	info.status = status;

	/* SA_NOCLDSTOP suppresses the signal but not the wakeup. */
	irq = spin_lock_irqsave(&parent->lock);

	action_flags = parent->signal_actions[SIGCHLD].flags;

	spin_unlock_irqrestore(&parent->lock, irq);

	if ((action_flags & SA_NOCLDSTOP) == 0)
		(void)signal_send_process_info(parent, SIGCHLD, &info);
	child_waiters_wake(parent);
}

/* Notifies the parent of a completed stop, from the stopping thread's sleep. */
static void
process_stop_notify(
	void *argument)
{
	struct process_stop_notification *notification;
	struct process *process;
	unsigned long process_irq;
	unsigned long tree_irq;
	int current;

	notification = argument;
	process = notification->process;

	/* Notifies only while the stop of this generation is still current. */
	KERN_TEST_CHECKPOINT(KERN_TEST_PROCESS_STOP_CALLBACK_BEFORE_NOTIFY,
	    process);
	tree_irq = spin_lock_irqsave(&process_tree_lock);
	process_irq = spin_lock_irqsave(&process->lock);

	current = 0;
	if (process->stop_requested &&
	    process->state == PROCESS_STOPPED &&
	    process->stop_generation == notification->generation &&
	    process->wait_stopped)
		current = 1;

	spin_unlock_irqrestore(&process->lock, process_irq);

	if (current)
		notify_parent_job_event_tree_locked(process, CLD_STOPPED,
		    notification->signo);

	spin_unlock_irqrestore(&process_tree_lock, tree_irq);
}

/* Tears the calling process down after its exit was committed, then exits the thread. */
static void
process_exit_cleanup(
	int thread_status)
{
	struct process *process;
	struct process *parent;
	struct thread *other;
	struct filedesc *fd;
	struct cwdinfo *cwdi;
	unsigned long parent_irq;
	unsigned long process_irq;
	unsigned long tree_irq;
	struct signal_action action;

	process = curthread->proc;

	/* Stops the timers and detaches the terminal early. */
	process_timer_cleanup(process);
	tty_detach_process(process);

	/* _exit terminates the process, not only the calling POSIX thread. */
	for (;;) {
		other = NULL;
		process_irq = spin_lock_irqsave(&process->lock);
		for (other = process->threads; other != NULL;
		     other = other->proc_next) {
			if (other != curthread &&
			    other->state != THREAD_ZOMBIE &&
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

	/* Closes the descriptors and the working directory. */
	process_irq = spin_lock_irqsave(&process->lock);

	fd = process->fd;
	cwdi = process->cwdi;
	process->fd = NULL;
	process->cwdi = NULL;

	spin_unlock_irqrestore(&process->lock, process_irq);

	filedesc_destroy(fd);
	cwdinfo_release(cwdi);

	/* Hands the children over and decides whether a zombie is wanted. */
	tree_irq = spin_lock_irqsave(&process_tree_lock);

	reparent_children(process);
	parent = process->parent;
	if (parent != NULL)
		process_ref(parent);
	if (parent != NULL && parent != &process0) {
		/*
		 * Disposition changes are serialized by parent->lock.  The
		 * tree lock keeps the parent relationship stable while the
		 * snapshot decides whether this child will be visible as a
		 * zombie.
		 */
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

	/* Exits the calling thread. */
	curthread->exit_status = thread_status;
	sched_exit_current();
}

/* Publishes a thread's exit and retires it from a pending stop; the caller holds the process lock. */
static void
process_thread_exit_publish_locked(
	struct process *process,
	struct thread *thread)
{
	struct thread *member;

	/* A thread exits once. */
	if (thread->exit_committed)
		HAL_FATAL("thread exit committed twice");
	thread->exit_committed = 1;

	/*
	 * A stop generation counts only threads which can still return to a
	 * scheduler/user safe point.  Retire this member at the same
	 * process-lock linearization point as exit_committed, whether or not
	 * it acknowledged.
	 */
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

	/* An acknowledged waiter may now complete the generation. */
	for (member = process->threads; member != NULL;
	     member = member->proc_next) {
		if (member != thread &&
		    member->state == THREAD_SLEEPING &&
		    member->stop_generation == process->stop_generation)
			sched_wakeup(member);
	}
}

/* Commits a thread's exit, reporting whether it owns the process exit. */
static int
process_commit_thread_exit(
	struct thread *thread,
	int status)
{
	struct process *process;
	struct thread *member;
	unsigned long process_irq;
	unsigned long tree_irq;
	int owner;

	if (thread != NULL)
		process = thread->proc;
	else
		process = NULL;
	owner = 0;

	/* Ignores a thread without a user process. */
	if (process == NULL || process == &process0)
		return 0;

	/* The last uncommitted thread of a live process owns the exit. */
	tree_irq = spin_lock_irqsave(&process_tree_lock);
	process_irq = spin_lock_irqsave(&process->lock);

	process_thread_exit_publish_locked(process, thread);
	if (process->state == PROCESS_RUNNING ||
	    process->state == PROCESS_STOPPED) {
		owner = 1;
		for (member = process->threads; member != NULL;
		     member = member->proc_next) {
			if (!member->exit_committed &&
			    member->state != THREAD_ZOMBIE &&
			    member->state != THREAD_REAPING &&
			    member->state != THREAD_DEAD) {
				owner = 0;
				break;
			}
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

/* Exits the calling process with a thread status and a wait status. */
static void __attribute__((noreturn))
process_exit_final(
	int thread_status,
	int wait_status)
{
	struct process *process;
	unsigned long process_irq;
	unsigned long tree_irq;
	int owner;

	process = curthread->proc;
	owner = 0;

	/* The kernel process never exits. */
	if (process == NULL || process == &process0)
		HAL_FATAL("process0 exit");

	/* The first thread to exit the process owns the cleanup. */
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

	/* A later thread just exits itself. */
	curthread->exit_status = thread_status;
	sched_exit_current();
}
