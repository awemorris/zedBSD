/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * Standard POSIX signals
 *
 * SPDX-License-Identifier: Zlib
 */

#include "kern/signal.h"
#include "kern/cred.h"
#include "kern/process.h"
#include "kern/process-timer.h"
#include "kern/sched.h"
#include "kern/thread.h"
#include "kern/uaccess.h"

#include <errno.h>
#include <hal/hal.h>
#include <stddef.h>
#include <string.h>

#define SIGNAL_BIT(n)		((sigset_t)1ULL << ((unsigned)(n) - 1U))
#define SIGNAL_VALID_MASK	((sigset_t)(UINT64_MAX >> 1U))

static int signal_valid(int signo)
{
	return signo > 0 && signo < NSIG;
}

static int signal_stop(int s)
{
	return s == SIGSTOP || s == SIGTSTP || s == SIGTTIN || s == SIGTTOU;
}

static int signal_ignored_default(int s)
{
	return s == SIGCHLD || s == SIGCONT || s == SIGURG || s == SIGWINCH;
}

static sigset_t signal_wait_claims_locked(const struct process *);

/* Caller holds process->lock. */
static int
signal_ignored_disposition_locked(const struct process *process, int signo)
{
	const struct signal_action *action = &process->signal_actions[signo];

	return signo != SIGKILL && signo != SIGSTOP &&
	    (action->handler == (uintptr_t)SIG_IGN ||
	    (action->handler == (uintptr_t)SIG_DFL &&
	    signal_ignored_default(signo)));
}

struct signal_timer_completion {
	unsigned slot;
	uint32_t generation;
};

#define SIGNAL_TIMER_COMPLETION_MAX (SIGNAL_QUEUE_MAX + NSIG)

static void
signal_timer_completion_add(struct signal_timer_completion *completions,
	unsigned *count, const struct signal_info *info)
{
	unsigned index;

	if (info == NULL || info->code != SI_TIMER ||
	    info->timer_generation == 0)
		return;
	for (index = 0; index < *count; index++)
		if (completions[index].slot == info->timer_slot &&
		    completions[index].generation == info->timer_generation)
			return;
	if (*count >= SIGNAL_TIMER_COMPLETION_MAX)
		HAL_FATAL("signal timer completion overflow");
	completions[*count].slot = info->timer_slot;
	completions[*count].generation = info->timer_generation;
	(*count)++;
}

static void
signal_timer_completions_run(struct process *process,
	const struct signal_timer_completion *completions, unsigned count)
{
	unsigned index;

	for (index = 0; index < count; index++)
		process_timer_notification_complete(process, completions[index].slot,
		    completions[index].generation);
}

static void
signal_timer_complete_one(struct process *process,
	const struct signal_info *info)
{
	if (info != NULL && info->code == SI_TIMER &&
	    info->timer_generation != 0)
		process_timer_notification_complete(process, info->timer_slot,
		    info->timer_generation);
}

/* Caller holds process->lock. */
int
signal_pending_unblocked_locked(const struct thread *thread)
{
	const struct process *process;
	sigset_t pending;
	int signo;

	if (thread == NULL || (process = thread->proc) == NULL)
		return 0;
	pending = (thread->signal_pending |
	    (process->signal_pending & ~signal_wait_claims_locked(process))) &
	    ~thread->signal_mask;
	pending |= (thread->signal_pending | process->signal_pending) &
	    (SIGNAL_BIT(SIGKILL) | SIGNAL_BIT(SIGSTOP));
	for (signo = 1; signo < NSIG; signo++) {
		const struct signal_action *action;

		if ((pending & SIGNAL_BIT(signo)) == 0)
			continue;
		action = &process->signal_actions[signo];
		if (action->handler == (uintptr_t)SIG_IGN ||
		    (action->handler == (uintptr_t)SIG_DFL &&
		     signal_ignored_default(signo)))
			continue;
		return 1;
	}
	return 0;
}

/* Caller holds process->lock.  A process-directed signal selected by
 * sigtimedwait() must not be consumed by another thread's ordinary return to
 * user mode before the waiter runs. */
static sigset_t
signal_wait_claims_locked(const struct process *process)
{
	const struct thread *thread;
	sigset_t claims = 0;

	for (thread = process->threads; thread != NULL;
	    thread = thread->proc_next)
		if (thread->signal_waiting)
			claims |= thread->signal_wait_set;
	return claims;
}

/* Caller holds process->lock. */
static void
signal_take_process_locked(struct process *process, int signo,
	struct signal_info *info)
{
	unsigned i, selected = SIGNAL_QUEUE_MAX;
	for (i = 0; i < process->signal_queue_count; i++)
		if (process->signal_queue[i].signo == signo) {
			selected = i;
			break;
		}
	if (selected != SIGNAL_QUEUE_MAX) {
		*info = process->signal_queue[selected].info;
		for (i = selected + 1U; i < process->signal_queue_count; i++)
			process->signal_queue[i - 1U] = process->signal_queue[i];
		process->signal_queue_count--;
		for (i = 0; i < process->signal_queue_count; i++)
			if (process->signal_queue[i].signo == signo)
				return;
		if ((process->signal_unqueued_pending & SIGNAL_BIT(signo)) != 0)
			return;
	} else {
		*info = process->signal_info[signo];
		process->signal_unqueued_pending &= ~SIGNAL_BIT(signo);
	}
	process->signal_pending &= ~SIGNAL_BIT(signo);
	memset(&process->signal_info[signo], 0,
	    sizeof(process->signal_info[signo]));
}

int
signal_pending_unblocked(const struct thread *thread)
{
	struct process *process;
	unsigned long irq;
	int pending;

	if (thread == NULL || (process = thread->proc) == NULL)
		return 0;
	irq = spin_lock_irqsave(&process->lock);
	pending = signal_pending_unblocked_locked(thread);
	spin_unlock_irqrestore(&process->lock, irq);
	return pending;
}

enum signal_stop_return_result
signal_stop_before_return(struct thread *thread)
{
	struct process *process;
	struct signal_info ignored_info;
	sigset_t pending;
	unsigned long irq;
	int interrupt_pending = 0;
	int candidate, signo;

	if (thread == NULL || (process = thread->proc) == NULL ||
	    process == &process0)
		return 0;
	memset(&ignored_info, 0, sizeof(ignored_info));
	irq = spin_lock_irqsave(&process->lock);
	pending = (thread->signal_pending |
	    (process->signal_pending & ~signal_wait_claims_locked(process))) &
	    ~thread->signal_mask;
	pending |= (thread->signal_pending | process->signal_pending) &
	    (SIGNAL_BIT(SIGKILL) | SIGNAL_BIT(SIGSTOP));
	/*
	 * Record an independently effective signal before consuming the stop.
	 * A caught signal already made the syscall return EINTR; after SIGCONT it
	 * must reach user mode before a newly-ready I/O condition can win a
	 * transparent redispatch.  Fatal defaults follow the same ordering.
	 */
	for (candidate = 1; candidate < NSIG; candidate++) {
		const struct signal_action *action;

		if ((pending & SIGNAL_BIT(candidate)) == 0)
			continue;
		action = &process->signal_actions[candidate];
		if (action->handler == (uintptr_t)SIG_IGN ||
		    (action->handler == (uintptr_t)SIG_DFL &&
		    signal_ignored_default(candidate)))
			continue;
		if (signal_stop(candidate) &&
		    (candidate == SIGSTOP ||
		    action->handler == (uintptr_t)SIG_DFL))
			continue;
		interrupt_pending = 1;
		break;
	}
	for (signo = 1; signo < NSIG; signo++) {
		if ((pending & SIGNAL_BIT(signo)) == 0 || !signal_stop(signo) ||
		    (signo != SIGSTOP &&
		    process->signal_actions[signo].handler != (uintptr_t)SIG_DFL))
			continue;
		if ((thread->signal_pending & SIGNAL_BIT(signo)) != 0) {
			thread->signal_pending &= ~SIGNAL_BIT(signo);
			ignored_info = thread->signal_info[signo];
			memset(&thread->signal_info[signo], 0,
			    sizeof(thread->signal_info[signo]));
		} else {
			signal_take_process_locked(process, signo, &ignored_info);
		}
		break;
	}
	spin_unlock_irqrestore(&process->lock, irq);
	if (signo == NSIG)
		return SIGNAL_STOP_RETURN_NONE;
	signal_timer_complete_one(process, &ignored_info);
	process_stop_current(signo);
	return interrupt_pending ? SIGNAL_STOP_RETURN_INTERRUPT :
	    SIGNAL_STOP_RETURN_REDISPATCH;
}

int
signal_job_control_decision(const struct thread *thread, int signo)
{
	struct process *process;
	struct signal_action action;
	unsigned long irq;
	int blocked;

	if (thread == NULL || (process = thread->proc) == NULL ||
	    !signal_stop(signo))
		return EINVAL;
	irq = spin_lock_irqsave(&process->lock);
	action = process->signal_actions[signo];
	blocked = (thread->signal_mask & SIGNAL_BIT(signo)) != 0;
	spin_unlock_irqrestore(&process->lock, irq);
	if (blocked || action.handler == (uintptr_t)SIG_IGN)
		return EIO;
	return action.handler == (uintptr_t)SIG_DFL ? 0 : EINTR;
}

/* Remove every queued and non-queued instance in set.  Caller holds the
 * process lock; stop/continue generation uses this to implement POSIX's
 * mutual-discard rule atomically with enqueuing the new signal. */
static void
signal_discard_locked(struct process *process, sigset_t set,
	struct signal_timer_completion *completions, unsigned *completion_count)
{
	struct thread *thread;
	unsigned read_index, write_index = 0;
	int signo;

	process->signal_pending &= ~set;
	process->signal_unqueued_pending &= ~set;
	for (signo = 1; signo < NSIG; signo++)
		if ((set & SIGNAL_BIT(signo)) != 0) {
			signal_timer_completion_add(completions, completion_count,
			    &process->signal_info[signo]);
			memset(&process->signal_info[signo], 0,
			    sizeof(process->signal_info[signo]));
		}
	for (read_index = 0; read_index < process->signal_queue_count;
	    read_index++) {
		if ((set & SIGNAL_BIT(
		    process->signal_queue[read_index].signo)) != 0) {
			signal_timer_completion_add(completions, completion_count,
			    &process->signal_queue[read_index].info);
			continue;
		}
		if (write_index != read_index)
			process->signal_queue[write_index] =
			    process->signal_queue[read_index];
		write_index++;
	}
	process->signal_queue_count = write_index;
	for (thread = process->threads; thread != NULL;
	    thread = thread->proc_next) {
		thread->signal_pending &= ~set;
		for (int signo = 1; signo < NSIG; signo++)
			if ((set & SIGNAL_BIT(signo)) != 0) {
				memset(&thread->signal_info[signo], 0,
				    sizeof(thread->signal_info[signo]));
			}
	}
}

int
signal_action_set(struct process *process, int signo,
	const struct signal_action *requested, struct signal_action *previous)
{
	struct signal_action replacement;
	struct signal_timer_completion completions[SIGNAL_TIMER_COMPLETION_MAX];
	unsigned long irq;
	unsigned completion_count = 0;

	if (process == NULL || !signal_valid(signo) ||
	    (requested != NULL && (signo == SIGKILL || signo == SIGSTOP)))
		return EINVAL;
	if (requested != NULL) {
		replacement = *requested;
		replacement.mask &= SIGNAL_VALID_MASK &
		    ~(SIGNAL_BIT(SIGKILL) | SIGNAL_BIT(SIGSTOP));
	}
	irq = spin_lock_irqsave(&process->lock);
	if (previous != NULL)
		*previous = process->signal_actions[signo];
	if (requested != NULL) {
		process->signal_actions[signo] = replacement;
		if (signal_ignored_disposition_locked(process, signo))
			signal_discard_locked(process, SIGNAL_BIT(signo), completions,
			    &completion_count);
	}
	spin_unlock_irqrestore(&process->lock, irq);
	signal_timer_completions_run(process, completions, completion_count);
	return 0;
}

void signal_init(void) { }

void
kernel_user_return_handler(void)
{
	/*
	 * HAL has finished acknowledging any asynchronous source and keeps the
	 * active user frame attached.  Run sleepable return policy with IRQs
	 * enabled, then close the window before HAL detaches or commits that frame.
	 */
	if (hal_irq_disable())
		HAL_FATAL("user-return callback entered with IRQs enabled");
	sched_accounting_kernel_enter();
	hal_irq_enable();
	if (curthread != NULL && curthread->terminate_requested)
		thread_exit(0);
	if (process_stop_requested(curthread))
		process_stop_current(0);
	signal_deliver_on_user_return();
	if (!hal_irq_disable())
		HAL_FATAL("user-return callback returned with IRQs disabled");
	sched_accounting_kernel_leave();
}

int
signal_send_process(struct process *process, int signo)
{
	struct signal_info info;
	memset(&info, 0, sizeof(info));
	info.code = SI_KERNEL;
	return signal_send_process_info(process, signo, &info);
}

int
signal_send_process_info(struct process *process, int signo,
			 const struct signal_info *info)
{
	struct signal_timer_completion completions[SIGNAL_TIMER_COMPLETION_MAX];
	struct thread *thread;
	unsigned long irq;
	unsigned completion_count = 0;
	int queued_notification;
	if (process == NULL || process == &process0 || !signal_valid(signo))
		return EINVAL;
	irq = spin_lock_irqsave(&process->lock);
	if (signo == SIGCONT)
		signal_discard_locked(process, SIGNAL_BIT(SIGSTOP) |
		    SIGNAL_BIT(SIGTSTP) | SIGNAL_BIT(SIGTTIN) |
		    SIGNAL_BIT(SIGTTOU), completions, &completion_count);
	else if (signal_stop(signo))
		signal_discard_locked(process, SIGNAL_BIT(SIGCONT), completions,
		    &completion_count);
	if (signal_ignored_disposition_locked(process, signo)) {
		spin_unlock_irqrestore(&process->lock, irq);
		signal_timer_completions_run(process, completions, completion_count);
		signal_timer_complete_one(process, info);
		if (signo == SIGCONT)
			(void)process_continue(process, 1);
		return 0;
	}
	queued_notification = info != NULL && (info->code == SI_QUEUE ||
	    info->code == SI_TIMER || signo >= SIGRTMIN);
	if (queued_notification) {
		struct queued_signal *queued;
		if (process->signal_queue_count == SIGNAL_QUEUE_MAX) {
			spin_unlock_irqrestore(&process->lock, irq);
			signal_timer_completions_run(process, completions,
			    completion_count);
			return EAGAIN;
		}
		queued = &process->signal_queue[process->signal_queue_count++];
		queued->signo = signo;
		queued->info = *info;
		queued->sequence = ++process->signal_queue_sequence;
	}
	if (!queued_notification && (process->signal_unqueued_pending &
	    SIGNAL_BIT(signo)) == 0) {
		memset(&process->signal_info[signo], 0,
		    sizeof(process->signal_info[signo]));
		if (info != NULL)
			process->signal_info[signo] = *info;
		process->signal_unqueued_pending |= SIGNAL_BIT(signo);
	}
	process->signal_pending |= SIGNAL_BIT(signo);
	for (thread = process->threads; thread != NULL;
	     thread = thread->proc_next)
		if (signal_pending_unblocked_locked(thread) ||
		    (thread->signal_waiting &&
		    (thread->signal_wait_set & SIGNAL_BIT(signo)) != 0))
			sched_interrupt(thread);
	spin_unlock_irqrestore(&process->lock, irq);
	signal_timer_completions_run(process, completions, completion_count);
	if (signo == SIGCONT || signo == SIGKILL)
		(void)process_continue(process, signo == SIGCONT);
	return 0;
}

static int
signal_permitted(const struct process *sender, const struct process *target,
		 int signo)
{
	struct ucred *sender_cred, *target_cred;
	int permitted;

	if (sender == NULL || target == NULL || target == &process0)
		return 0;
	sender_cred = cred_process_ref((struct process *)sender);
	target_cred = cred_process_ref((struct process *)target);
	permitted = sender_cred != NULL && target_cred != NULL &&
	    (cred_is_superuser(sender_cred) ||
	     sender_cred->ruid == target_cred->ruid ||
	     sender_cred->euid == target_cred->ruid ||
	     sender_cred->ruid == target_cred->suid ||
	     sender_cred->euid == target_cred->suid ||
	     (signo == SIGCONT && sender->session == target->session));
	cred_release(target_cred);
	cred_release(sender_cred);
	return permitted;
}

int
signal_kill(struct process *sender, pid_t selector, int signo)
{
	struct process *p;
	struct ucred *sender_cred;
	struct signal_info info;
	pid_t cursor = -1;
	int found = 0, permitted = 0;
	if (sender == NULL || signo < 0 || signo >= NSIG)
		return EINVAL;
	/* A negative selector names a process group, but INT32_MIN has no
	 * representable positive counterpart in pid_t. */
	if (selector == (pid_t)INT32_MIN)
		return ESRCH;
	memset(&info, 0, sizeof(info));
	info.code = SI_USER;
	info.pid = sender->pid;
	sender_cred = cred_process_ref(sender);
	info.uid = sender_cred != NULL ? sender_cred->euid : 0;
	cred_release(sender_cred);
	while ((p = process_find_next_ref(cursor)) != NULL) {
		int match = selector > 0 ? p->pid == selector :
		    selector == 0 ? p->pgrp == sender->pgrp :
		    selector == -1 ? p != &process0 :
		    p->pgrp == -selector;
		cursor = p->pid;
		if (!match || p == &process0 || p->state == PROCESS_DEAD) {
			process_release(p);
			continue;
		}
		found = 1;
		if (!signal_permitted(sender, p, signo)) {
			process_release(p);
			continue;
		}
		permitted = 1;
		if (signo != 0)
			(void)signal_send_process_info(p, signo, &info);
		process_release(p);
	}
	return !found ? ESRCH : (!permitted ? EPERM : 0);
}

void
signal_fork(struct process *child, const struct process *parent,
	    struct thread *child_thread, const struct thread *parent_thread)
{
	if (child == NULL || parent == NULL || child_thread == NULL ||
	    parent_thread == NULL)
		return;
	memcpy(child->signal_actions, parent->signal_actions,
	    sizeof(child->signal_actions));
	child->signal_pending = 0;
	child->signal_unqueued_pending = 0;
	memset(child->signal_info, 0, sizeof(child->signal_info));
	child_thread->signal_mask = parent_thread->signal_mask;
	child_thread->signal_pending = 0;
	memset(child_thread->signal_info, 0, sizeof(child_thread->signal_info));
	child_thread->signal_token = 0;
	child_thread->signal_token_counter = 0;
	child_thread->signal_depth = 0;
	child_thread->signal_altstack_base = parent_thread->signal_altstack_base;
	child_thread->signal_altstack_size = parent_thread->signal_altstack_size;
	child_thread->signal_altstack_flags = parent_thread->signal_altstack_flags;
	child_thread->signal_on_altstack_depth = 0;
	child_thread->signal_wait_set = 0;
	child_thread->signal_waiting = 0;
	memset(child_thread->signal_levels, 0,
	    sizeof(child_thread->signal_levels));
	child_thread->syscall_restart_valid = 0;
	child_thread->syscall_redispatch_valid = 0;
}

void
signal_exec(struct process *process)
{
	struct signal_timer_completion completions[SIGNAL_TIMER_COMPLETION_MAX];
	unsigned long irq;
	unsigned completion_count = 0;
	int i;
	if (process == NULL)
		return;
	irq = spin_lock_irqsave(&process->lock);
	/* libc's SIGEV_THREAD worker does not survive exec.  Its implementation
	 * signal is outside the public namespace, so discard queued notifications
	 * while preserving every application-visible pending signal. */
	signal_discard_locked(process,
	    SIGNAL_BIT(__ZEDBSD_SIGEV_THREAD_SIGNAL), completions,
	    &completion_count);
	for (i = 1; i < NSIG; i++)
		if (process->signal_actions[i].handler != (uintptr_t)SIG_IGN)
			memset(&process->signal_actions[i], 0,
			    sizeof(process->signal_actions[i]));
	if (curthread != NULL) {
		curthread->signal_token = 0;
		curthread->signal_token_counter = 0;
		curthread->signal_depth = 0;
		memset(curthread->signal_levels, 0,
		    sizeof(curthread->signal_levels));
		curthread->signal_suspended = 0;
		curthread->signal_altstack_base = 0;
		curthread->signal_altstack_size = 0;
		curthread->signal_altstack_flags = SS_DISABLE;
		curthread->signal_on_altstack_depth = 0;
		curthread->signal_wait_set = 0;
		curthread->signal_waiting = 0;
		curthread->syscall_restart_valid = 0;
		curthread->syscall_redispatch_valid = 0;
	}
	spin_unlock_irqrestore(&process->lock, irq);
	signal_timer_completions_run(process, completions, completion_count);
}

void
signal_deliver_on_user_return(void)
{
	struct thread *thread = curthread;
	struct process *process;
	struct signal_action action;
	struct signal_info selected_info;
	struct thread_signal_level *level;
	struct hal_user_context interrupted;
	siginfo_t user_info;
	ucontext_t user_context;
	sigset_t pending;
	uintptr_t sp, restorer, info_pointer, context_pointer;
	uintptr_t restart_args[HAL_SYSCALL_ARGS];
	uint32_t restart_number;
	unsigned restart_valid;
	uint32_t token;
	unsigned long irq;
	int signo;

	if (thread == NULL || (process = thread->proc) == NULL ||
	    process == &process0)
		return;
	/*
	 * A restart candidate belongs exclusively to this return-to-user
	 * boundary.  Consume it before inspecting dispositions: a signal may be
	 * changed to SIG_IGN after it interrupted the wait, and retaining the
	 * candidate would let an unrelated, later SA_RESTART handler redispatch
	 * a syscall whose EINTR was already observed by user space.
	 */
	restart_number = thread->syscall_restart_number;
	memcpy(restart_args, thread->syscall_restart_args,
	    sizeof(restart_args));
	restart_valid = thread->syscall_restart_valid;
	thread->syscall_restart_valid = 0;
retry:
	irq = spin_lock_irqsave(&process->lock);
	pending = (thread->signal_pending |
	    (process->signal_pending & ~signal_wait_claims_locked(process))) &
	    ~thread->signal_mask;
	pending |= (thread->signal_pending | process->signal_pending) &
	    (SIGNAL_BIT(SIGKILL) | SIGNAL_BIT(SIGSTOP));
	if (pending == 0) {
		if (thread->signal_suspended) {
			thread->signal_mask = thread->signal_suspend_mask;
			thread->signal_suspended = 0;
		}
		spin_unlock_irqrestore(&process->lock, irq);
		return;
	}
	for (signo = 1; signo < NSIG && (pending & SIGNAL_BIT(signo)) == 0;
	     signo++)
		;
	action = process->signal_actions[signo];
	if ((thread->signal_pending & SIGNAL_BIT(signo)) != 0) {
		thread->signal_pending &= ~SIGNAL_BIT(signo);
		selected_info = thread->signal_info[signo];
		memset(&thread->signal_info[signo], 0,
		    sizeof(thread->signal_info[signo]));
	} else {
		signal_take_process_locked(process, signo, &selected_info);
	}
	spin_unlock_irqrestore(&process->lock, irq);
	signal_timer_complete_one(process, &selected_info);
	if (action.handler == (uintptr_t)SIG_IGN && signo != SIGKILL &&
	    signo != SIGSTOP)
		goto retry;
	if (action.handler == (uintptr_t)SIG_DFL) {
		if (signal_ignored_default(signo))
			goto retry;
		if (signal_stop(signo)) {
			irq = spin_lock_irqsave(&process->lock);
			if (thread->signal_suspended) {
				thread->signal_mask = thread->signal_suspend_mask;
				thread->signal_suspended = 0;
			}
			spin_unlock_irqrestore(&process->lock, irq);
			process_stop_current(signo);
			/* SIGCONT may have made SIGHUP or another signal deliverable. */
			goto retry;
		}
		exit1_signal(signo);
	}
	if (thread->signal_depth >= SIGNAL_NEST_MAX)
		exit1_signal(SIGSEGV);
	if (hal_task_user_context(&interrupted) != 0)
		exit1_signal(SIGSEGV);
	sp = interrupted.stack_pointer;
	if ((action.flags & SA_ONSTACK) != 0 &&
	    (thread->signal_altstack_flags & SS_DISABLE) == 0 &&
	    thread->signal_on_altstack_depth == 0) {
		sp = thread->signal_altstack_base + thread->signal_altstack_size;
		thread->signal_on_altstack_depth++;
		level = &thread->signal_levels[thread->signal_depth];
		level->used_altstack = 1;
	}
	restorer = action.restorer;
	token = ++thread->signal_token_counter;
	if (token == 0)
		token = ++thread->signal_token_counter;
	level = &thread->signal_levels[thread->signal_depth];
	{
		unsigned used_altstack = level->used_altstack;
		memset(level, 0, sizeof(*level));
		level->used_altstack = used_altstack;
	}
	level->token = token;
	level->saved_mask = thread->signal_suspended ?
	    thread->signal_suspend_mask : thread->signal_mask;
	level->restart_number = restart_number;
	memcpy(level->restart_args, restart_args,
	    sizeof(level->restart_args));
	level->restart_on_return = restart_valid &&
	    (action.flags & SA_RESTART) != 0;
	memset(&user_info, 0, sizeof(user_info));
	user_info.si_signo = signo;
	user_info.si_errno = selected_info.error;
	user_info.si_code = selected_info.code;
	user_info.si_pid = selected_info.pid;
	user_info.si_uid = selected_info.uid;
	user_info.si_status = selected_info.status;
	user_info.si_addr = (uint64_t)selected_info.address;
	memcpy(&user_info.si_value, &selected_info.value,
	    sizeof(selected_info.value));
	memset(&user_context, 0, sizeof(user_context));
	user_context.uc_sigmask = level->saved_mask;
	user_context.uc_mcontext.mc_pc = (uint64_t)interrupted.pc;
	user_context.uc_mcontext.mc_sp = (uint64_t)interrupted.stack_pointer;
	user_context.uc_mcontext.mc_retval = (int64_t)interrupted.return_value;
	level->saved_ucontext = user_context;
	thread->signal_depth++;
	thread->signal_token = token;
	{
		struct {
			struct hal_task_signal_frame_head head;
			siginfo_t info;
			ucontext_t context;
		} frame;

		memset(&frame, 0, sizeof(frame));
		sp = (sp - sizeof(frame)) &
		    ~((uintptr_t)HAL_TASK_SIGNAL_FRAME_ALIGNMENT - 1U);
		info_pointer = sp + offsetof(__typeof__(frame), info);
		context_pointer = sp + offsetof(__typeof__(frame), context);
#if HAL_TASK_SIGNAL_FRAME_HAS_RESTORER
		frame.head.restorer = restorer;
#endif
#if HAL_TASK_SIGNAL_FRAME_HAS_SIGNO
		frame.head.signo = (uint32_t)signo;
#endif
#if HAL_TASK_SIGNAL_FRAME_HAS_INFO_POINTER
		frame.head.info_pointer = info_pointer;
#endif
		frame.head.token = token;
		frame.head.context_pointer = context_pointer;
		frame.info = user_info;
		frame.context = user_context;
		if (copyout(&frame, sp, sizeof(frame)) != 0)
			exit1_signal(SIGSEGV);
	}
	level->user_ucontext = context_pointer;
	irq = spin_lock_irqsave(&process->lock);
	if (thread->signal_suspended) {
		thread->signal_mask = thread->signal_suspend_mask;
		thread->signal_suspended = 0;
	}
	thread->signal_mask |= action.mask;
	if ((action.flags & SA_NODEFER) == 0)
		thread->signal_mask |= SIGNAL_BIT(signo);
	if ((action.flags & SA_RESETHAND) != 0) {
		if (process->signal_actions[signo].handler == action.handler) {
			process->signal_actions[signo].handler =
			    (uintptr_t)SIG_DFL;
			process->signal_actions[signo].mask = 0;
			process->signal_actions[signo].flags = 0;
			process->signal_actions[signo].restorer = 0;
		}
	}
	spin_unlock_irqrestore(&process->lock, irq);
	if (hal_task_signal_enter(action.handler, sp, signo, info_pointer,
	    context_pointer, restorer, token) != 0)
		exit1_signal(SIGSEGV);
}

int
signal_send_thread(struct thread *thread, int signo)
{
	struct signal_info info;

	memset(&info, 0, sizeof(info));
	info.code = SI_KERNEL;
	return signal_send_thread_info(thread, signo, &info);
}

int
signal_send_thread_info(struct thread *thread, int signo,
	const struct signal_info *info)
{
	struct signal_timer_completion completions[SIGNAL_TIMER_COMPLETION_MAX];
	struct process *process;
	unsigned long irq;
	unsigned completion_count = 0;
	if (thread == NULL || (process = thread->proc) == NULL ||
	    !signal_valid(signo)) return EINVAL;
	irq = spin_lock_irqsave(&process->lock);
	if (signo == SIGCONT)
		signal_discard_locked(process, SIGNAL_BIT(SIGSTOP) |
		    SIGNAL_BIT(SIGTSTP) | SIGNAL_BIT(SIGTTIN) |
		    SIGNAL_BIT(SIGTTOU), completions, &completion_count);
	else if (signal_stop(signo))
		signal_discard_locked(process, SIGNAL_BIT(SIGCONT), completions,
		    &completion_count);
	if (signal_ignored_disposition_locked(process, signo)) {
		spin_unlock_irqrestore(&process->lock, irq);
		signal_timer_completions_run(process, completions, completion_count);
		signal_timer_complete_one(process, info);
		if (signo == SIGCONT)
			(void)process_continue(process, 1);
		return 0;
	}
	if ((thread->signal_pending & SIGNAL_BIT(signo)) == 0) {
		memset(&thread->signal_info[signo], 0,
		    sizeof(thread->signal_info[signo]));
		if (info != NULL)
			thread->signal_info[signo] = *info;
	}
	thread->signal_pending |= SIGNAL_BIT(signo);
	if (signal_pending_unblocked_locked(thread) ||
	    (thread->signal_waiting &&
	    (thread->signal_wait_set & SIGNAL_BIT(signo)) != 0))
		sched_interrupt(thread);
	spin_unlock_irqrestore(&process->lock, irq);
	signal_timer_completions_run(process, completions, completion_count);
	if (signo == SIGCONT || signo == SIGKILL)
		(void)process_continue(process, signo == SIGCONT);
	return 0;
}

int
signal_timedwait(struct thread *thread, sigset_t set, uint64_t deadline,
	int timed, struct signal_info *info, int *signo_out)
{
	struct process *process;
	unsigned long irq;
	int signo;

	if (thread == NULL || (process = thread->proc) == NULL || info == NULL ||
	    signo_out == NULL || set == 0)
		return EINVAL;
	set &= SIGNAL_VALID_MASK &
	    ~(SIGNAL_BIT(SIGKILL) | SIGNAL_BIT(SIGSTOP));
	if (set == 0) return EINVAL;
	irq = spin_lock_irqsave(&process->lock);
	for (;;) {
		sigset_t pending = (thread->signal_pending |
		    process->signal_pending) & set;
		if (thread->terminate_requested) {
			thread->signal_waiting = 0;
			thread->signal_wait_set = 0;
			spin_unlock_irqrestore(&process->lock, irq);
			return EINTR;
		}
		if (process->stop_requested) {
			thread->signal_waiting = 0;
			thread->signal_wait_set = 0;
			spin_unlock_irqrestore(&process->lock, irq);
			process_stop_current(0);
			irq = spin_lock_irqsave(&process->lock);
			continue;
		}
		if (pending != 0) {
			for (signo = 1; signo < NSIG &&
			    (pending & SIGNAL_BIT(signo)) == 0; signo++) ;
			if ((thread->signal_pending & SIGNAL_BIT(signo)) != 0) {
				thread->signal_pending &= ~SIGNAL_BIT(signo);
				*info = thread->signal_info[signo];
				memset(&thread->signal_info[signo], 0,
				    sizeof(thread->signal_info[signo]));
			} else {
				signal_take_process_locked(process, signo, info);
			}
			thread->signal_waiting = 0;
			thread->signal_wait_set = 0;
			spin_unlock_irqrestore(&process->lock, irq);
			signal_timer_complete_one(process, info);
			*signo_out = signo;
			return 0;
		}
		if (timed && sched_ticks() >= deadline) {
			thread->signal_waiting = 0;
			thread->signal_wait_set = 0;
			spin_unlock_irqrestore(&process->lock, irq);
			return EAGAIN;
		}
		if (signal_pending_unblocked_locked(thread)) {
			thread->signal_waiting = 0;
			thread->signal_wait_set = 0;
			spin_unlock_irqrestore(&process->lock, irq);
			return EINTR;
		}
		thread->signal_wait_set = set;
		thread->signal_waiting = 1;
		sched_sleep_locked(deadline, &process->lock);
	}
}
