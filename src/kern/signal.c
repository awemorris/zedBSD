/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Signal generation, disposition, and delivery.
 *
 * Signals are recorded per process or per thread as pending bits with
 * one information record each, plus a queue for real-time and timer
 * notifications.  Delivery happens on the return to user mode: the
 * selected signal either stops or kills the process, or gets a frame
 * pushed on the user stack for its handler.  Timer notifications report
 * their completion back to the process timers.
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
#define SIGNAL_TIMER_COMPLETION_MAX (SIGNAL_QUEUE_MAX + NSIG)

struct signal_timer_completion {
	unsigned slot;
	uint32_t generation;
};

struct signal_frame {
	struct hal_task_signal_frame_head head;
	siginfo_t info;
	ucontext_t context;
};

static int signal_valid(int signo);
static int signal_stop(int s);
static int signal_ignored_default(int s);
static int signal_ignored_disposition_locked(const struct process *process, int signo);
static void signal_timer_completion_add(struct signal_timer_completion *completions, unsigned *count, const struct signal_info *info);
static void signal_timer_completions_run(struct process *process, const struct signal_timer_completion *completions, unsigned count);
static void signal_timer_complete_one(struct process *process, const struct signal_info *info);
static sigset_t signal_wait_claims_locked(const struct process *);
static void signal_take_process_locked(struct process *process, int signo, struct signal_info *info);
static void signal_discard_locked(struct process *process, sigset_t set, struct signal_timer_completion *completions, unsigned *completion_count);
static int signal_permitted(const struct process *sender, const struct process *target, int signo);

/*
 * Tests whether a thread has a deliverable signal; the caller holds the
 * process lock.
 *
 * Signals claimed by a sigtimedwait() of another thread and ignored
 * dispositions do not count; SIGKILL and SIGSTOP always do.
 */
int
signal_pending_unblocked_locked(
	const struct thread *thread)
{
	const struct process *process;
	const struct signal_action *action;
	sigset_t pending;
	int signo;

	/* A thread without a process has no signals. */
	if (thread == NULL)
		return 0;
	process = thread->proc;
	if (process == NULL)
		return 0;

	/* Gathers the unblocked pending signals, always including the unmaskable. */
	pending = (thread->signal_pending |
	    (process->signal_pending & ~signal_wait_claims_locked(process))) &
	    ~thread->signal_mask;
	pending |= (thread->signal_pending | process->signal_pending) &
	    (SIGNAL_BIT(SIGKILL) | SIGNAL_BIT(SIGSTOP));

	/* The first pending signal that is not ignored is deliverable. */
	for (signo = 1; signo < NSIG; signo++) {
		if ((pending & SIGNAL_BIT(signo)) == 0)
			continue;
		action = &process->signal_actions[signo];
		if (action->handler == (uintptr_t)SIG_IGN ||
		    (action->handler == (uintptr_t)SIG_DFL &&
		     signal_ignored_default(signo)))
			continue;
		return 1;
	}

	/* Reports nothing deliverable. */
	return 0;
}

/*
 * Tests whether a thread has a deliverable signal.
 */
int
signal_pending_unblocked(
	const struct thread *thread)
{
	struct process *process;
	unsigned long irq;
	int pending;

	/* A thread without a process has no signals. */
	if (thread == NULL)
		return 0;
	process = thread->proc;
	if (process == NULL)
		return 0;

	/* Checks under the process lock. */
	irq = spin_lock_irqsave(&process->lock);

	pending = signal_pending_unblocked_locked(thread);

	spin_unlock_irqrestore(&process->lock, irq);

	/* Reports the check result. */
	return pending;
}

/*
 * Consumes a pending default-action stop signal before a syscall returns.
 *
 * The process stops here.  The result tells the syscall layer whether
 * another effective signal was also pending, which forces an EINTR
 * return, or whether the syscall may be redispatched transparently.
 */
enum signal_stop_return_result
signal_stop_before_return(
	struct thread *thread)
{
	struct process *process;
	struct signal_info ignored_info;
	const struct signal_action *action;
	sigset_t pending;
	unsigned long irq;
	int interrupt_pending;
	int candidate;
	int signo;

	interrupt_pending = 0;

	/* Kernel threads and threads without a process never stop here. */
	if (thread == NULL)
		return 0;
	process = thread->proc;
	if (process == NULL || process == &process0)
		return 0;
	memset(&ignored_info, 0, sizeof(ignored_info));

	/* Gathers the unblocked pending signals, always including the unmaskable. */
	irq = spin_lock_irqsave(&process->lock);

	pending = (thread->signal_pending |
	    (process->signal_pending & ~signal_wait_claims_locked(process))) &
	    ~thread->signal_mask;
	pending |= (thread->signal_pending | process->signal_pending) &
	    (SIGNAL_BIT(SIGKILL) | SIGNAL_BIT(SIGSTOP));

	/*
	 * Records an independently effective signal before consuming the
	 * stop.  A caught signal already made the syscall return EINTR;
	 * after SIGCONT it must reach user mode before a newly-ready I/O
	 * condition can win a transparent redispatch.  Fatal defaults follow
	 * the same ordering.
	 */
	for (candidate = 1; candidate < NSIG; candidate++) {
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

	/* Takes the first default-action stop signal from the thread or process. */
	for (signo = 1; signo < NSIG; signo++) {
		if ((pending & SIGNAL_BIT(signo)) == 0 ||
		    !signal_stop(signo) ||
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

	/* Without a stop signal the syscall returns normally. */
	if (signo == NSIG)
		return SIGNAL_STOP_RETURN_NONE;

	/* Completes a timer notification and stops the process. */
	signal_timer_complete_one(process, &ignored_info);
	process_stop_current(signo);

	/* Reports whether another signal interrupts the syscall. */
	if (interrupt_pending)
		return SIGNAL_STOP_RETURN_INTERRUPT;
	return SIGNAL_STOP_RETURN_REDISPATCH;
}

/*
 * Decides how a terminal delivers a job control stop signal to a thread.
 *
 * EIO reports a blocked or ignored signal, EINTR a caught one, and zero
 * the default stop.
 */
int
signal_job_control_decision(
	const struct thread *thread,
	int signo)
{
	struct process *process;
	struct signal_action action;
	unsigned long irq;
	int blocked;

	/* Rejects a thread without a process or a signal that does not stop. */
	if (thread == NULL)
		return EINVAL;
	process = thread->proc;
	if (process == NULL || !signal_stop(signo))
		return EINVAL;

	/* Samples the disposition and the mask under the process lock. */
	irq = spin_lock_irqsave(&process->lock);

	action = process->signal_actions[signo];
	blocked = (thread->signal_mask & SIGNAL_BIT(signo)) != 0;

	spin_unlock_irqrestore(&process->lock, irq);

	/* Reports the decision. */
	if (blocked || action.handler == (uintptr_t)SIG_IGN)
		return EIO;
	if (action.handler == (uintptr_t)SIG_DFL)
		return 0;
	return EINTR;
}

/*
 * Sets or reads the action of a signal.
 *
 * A signal whose new disposition ignores it is discarded from the
 * pending sets, completing any timer notifications it carried.
 */
int
signal_action_set(
	struct process *process,
	int signo,
	const struct signal_action *requested,
	struct signal_action *previous)
{
	struct signal_action replacement;
	struct signal_timer_completion completions[SIGNAL_TIMER_COMPLETION_MAX];
	unsigned long irq;
	unsigned completion_count;

	completion_count = 0;

	/* Rejects a bad signal, or a change to SIGKILL or SIGSTOP. */
	if (process == NULL ||
	    !signal_valid(signo) ||
	    (requested != NULL && (signo == SIGKILL || signo == SIGSTOP)))
		return EINVAL;

	/* The unmaskable signals cannot be added to a handler's mask. */
	if (requested != NULL) {
		replacement = *requested;
		replacement.mask &= SIGNAL_VALID_MASK &
		    ~(SIGNAL_BIT(SIGKILL) | SIGNAL_BIT(SIGSTOP));
	}

	/* Reads the old action and installs the new one under the lock. */
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

	/* Reports the changed action. */
	return 0;
}

/*
 * Initializes the signal subsystem, which needs no state yet.
 */
void
signal_init(
	void)
{
}

/*
 * Runs the return-to-user policy: termination, stops, and signal delivery.
 */
void
kernel_user_return_handler(
	void)
{
	/*
	 * HAL has finished acknowledging any asynchronous source and keeps
	 * the active user frame attached.  Run sleepable return policy with
	 * IRQs enabled, then close the window before HAL detaches or commits
	 * that frame.
	 */
	if (hal_irq_disable())
		HAL_FATAL("user-return callback entered with IRQs enabled");
	sched_accounting_kernel_enter();
	hal_irq_enable();

	/* A retired thread exits; a stopped process waits; signals deliver. */
	if (curthread != NULL && curthread->terminate_requested)
		thread_exit(0);
	if (process_stop_requested(curthread))
		process_stop_current(0);
	signal_deliver_on_user_return();

	/* Closes the window again. */
	if (!hal_irq_disable())
		HAL_FATAL("user-return callback returned with IRQs disabled");
	sched_accounting_kernel_leave();
}

/*
 * Sends a kernel-originated signal to a process.
 */
int
signal_send_process(
	struct process *process,
	int signo)
{
	struct signal_info info;
	int error;

	/* Describes the signal as coming from the kernel. */
	memset(&info, 0, sizeof(info));
	info.code = SI_KERNEL;

	/* Reports why the send failed. */
	error = signal_send_process_info(process, signo, &info);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Sends a signal with its information to a process.
 *
 * SIGCONT and the stop signals discard each other.  An ignored signal is
 * dropped at once.  Queued notifications and real-time signals go on the
 * queue; anything else keeps one information record.  A thread that can
 * take the signal is interrupted, and SIGCONT and SIGKILL continue a
 * stopped process.
 */
int
signal_send_process_info(
	struct process *process,
	int signo,
	const struct signal_info *info)
{
	struct signal_timer_completion completions[SIGNAL_TIMER_COMPLETION_MAX];
	struct queued_signal *queued;
	struct thread *thread;
	unsigned long irq;
	unsigned completion_count;
	int queued_notification;

	completion_count = 0;

	/* Rejects the kernel process or a bad signal. */
	if (process == NULL || process == &process0 || !signal_valid(signo))
		return EINVAL;

	/* Applies the mutual discard of SIGCONT and the stop signals. */
	irq = spin_lock_irqsave(&process->lock);

	if (signo == SIGCONT)
		signal_discard_locked(process, SIGNAL_BIT(SIGSTOP) |
		    SIGNAL_BIT(SIGTSTP) | SIGNAL_BIT(SIGTTIN) |
		    SIGNAL_BIT(SIGTTOU), completions, &completion_count);
	else if (signal_stop(signo))
		signal_discard_locked(process, SIGNAL_BIT(SIGCONT), completions,
		    &completion_count);

	/* An ignored signal is dropped, but SIGCONT still continues. */
	if (signal_ignored_disposition_locked(process, signo)) {
		spin_unlock_irqrestore(&process->lock, irq);
		signal_timer_completions_run(process, completions, completion_count);
		signal_timer_complete_one(process, info);
		if (signo == SIGCONT)
			(void)process_continue(process, 1);
		return 0;
	}

	/* Queued and real-time notifications each take a queue entry. */
	queued_notification = 0;
	if (info != NULL &&
	    (info->code == SI_QUEUE ||
	     info->code == SI_TIMER ||
	     signo >= SIGRTMIN))
		queued_notification = 1;
	if (queued_notification) {
		if (process->signal_queue_count == SIGNAL_QUEUE_MAX) {
			spin_unlock_irqrestore(&process->lock, irq);
			signal_timer_completions_run(process, completions,
			    completion_count);
			return EAGAIN;
		}

		queued = &process->signal_queue[process->signal_queue_count++];
		queued->signo = signo;
		queued->info = *info;
		process->signal_queue_sequence++;
		queued->sequence = process->signal_queue_sequence;
	}

	/* Anything else keeps the first information record until taken. */
	if (!queued_notification &&
	    (process->signal_unqueued_pending & SIGNAL_BIT(signo)) == 0) {
		memset(&process->signal_info[signo], 0,
		    sizeof(process->signal_info[signo]));
		if (info != NULL)
			process->signal_info[signo] = *info;
		process->signal_unqueued_pending |= SIGNAL_BIT(signo);
	}

	process->signal_pending |= SIGNAL_BIT(signo);

	/* Interrupts every thread that can take or is waiting for the signal. */
	for (thread = process->threads; thread != NULL;
	     thread = thread->proc_next) {
		if (signal_pending_unblocked_locked(thread) ||
		    (thread->signal_waiting &&
		    (thread->signal_wait_set & SIGNAL_BIT(signo)) != 0))
			sched_interrupt(thread);
	}

	spin_unlock_irqrestore(&process->lock, irq);

	signal_timer_completions_run(process, completions, completion_count);

	/* Continues a stopped process for SIGCONT and SIGKILL. */
	if (signo == SIGCONT || signo == SIGKILL)
		(void)process_continue(process, signo == SIGCONT);

	/* Reports the sent signal. */
	return 0;
}

/*
 * Implements kill(): sends a signal to the processes a selector names.
 *
 * A positive selector names a process, zero the sender's process group,
 * -1 every process, and another negative value a process group.  ESRCH
 * reports no matching process and EPERM no permitted one.
 */
int
signal_kill(
	struct process *sender,
	pid_t selector,
	int signo)
{
	struct process *p;
	struct ucred *sender_cred;
	struct signal_info info;
	pid_t cursor;
	int found;
	int permitted;
	int match;

	cursor = -1;
	found = 0;
	permitted = 0;

	/* Rejects a missing sender or a signal out of range. */
	if (sender == NULL || signo < 0 || signo >= NSIG)
		return EINVAL;

	/*
	 * A negative selector names a process group, but INT32_MIN has no
	 * representable positive counterpart in pid_t.
	 */
	if (selector == (pid_t)INT32_MIN)
		return ESRCH;

	/* Describes the signal as sent by the sender's user. */
	memset(&info, 0, sizeof(info));
	info.code = SI_USER;
	info.pid = sender->pid;
	sender_cred = cred_process_ref(sender);
	if (sender_cred != NULL)
		info.uid = sender_cred->euid;
	else
		info.uid = 0;
	cred_release(sender_cred);

	/* Walks the process table, sending to every permitted match. */
	for (;;) {
		p = process_find_next_ref(cursor);
		if (p == NULL)
			break;
		if (selector > 0)
			match = p->pid == selector;
		else if (selector == 0)
			match = p->pgrp == sender->pgrp;
		else if (selector == -1)
			match = p != &process0;
		else
			match = p->pgrp == -selector;
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

	/* Reports missing or forbidden targets. */
	if (!found)
		return ESRCH;
	if (!permitted)
		return EPERM;
	return 0;
}

/*
 * Copies the signal state of a parent into a forked child.
 *
 * The actions, mask, and alternate stack are inherited; nothing pending
 * is.
 */
void
signal_fork(
	struct process *child,
	const struct process *parent,
	struct thread *child_thread,
	const struct thread *parent_thread)
{
	/* Ignores a missing operand. */
	if (child == NULL ||
	    parent == NULL ||
	    child_thread == NULL ||
	    parent_thread == NULL)
		return;

	/* The process inherits the actions with nothing pending. */
	memcpy(child->signal_actions, parent->signal_actions,
	    sizeof(child->signal_actions));
	child->signal_pending = 0;
	child->signal_unqueued_pending = 0;
	memset(child->signal_info, 0, sizeof(child->signal_info));

	/* The thread inherits the mask and alternate stack with nothing pending. */
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

/*
 * Resets the signal state of a process across exec.
 *
 * Caught signals revert to their defaults, ignored ones stay ignored,
 * and the current thread loses its handler nesting and alternate stack.
 */
void
signal_exec(
	struct process *process)
{
	struct signal_timer_completion completions[SIGNAL_TIMER_COMPLETION_MAX];
	unsigned long irq;
	unsigned completion_count;
	int i;

	completion_count = 0;

	/* Ignores a missing process. */
	if (process == NULL)
		return;

	irq = spin_lock_irqsave(&process->lock);

	/*
	 * libc's SIGEV_THREAD worker does not survive exec.  Its
	 * implementation signal is outside the public namespace, so discard
	 * queued notifications while preserving every application-visible
	 * pending signal.
	 */
	signal_discard_locked(process,
	    SIGNAL_BIT(__ZEDBSD_SIGEV_THREAD_SIGNAL), completions,
	    &completion_count);

	/* Caught signals revert to the default action. */
	for (i = 1; i < NSIG; i++) {
		if (process->signal_actions[i].handler != (uintptr_t)SIG_IGN)
			memset(&process->signal_actions[i], 0,
			    sizeof(process->signal_actions[i]));
	}

	/* The current thread starts the new image with a clean handler state. */
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

/*
 * Delivers one pending signal to the current thread on its way to user mode.
 *
 * The lowest deliverable signal is taken.  An ignored one is dropped, a
 * default stop stops the process, a default fatal one exits it, and a
 * handled one gets a signal frame pushed and the thread redirected to its
 * handler with the handler's mask installed.
 */
void
signal_deliver_on_user_return(
	void)
{
	struct thread *thread;
	struct process *process;
	struct signal_action action;
	struct signal_info selected_info;
	struct thread_signal_level *level;
	struct hal_user_context interrupted;
	struct signal_frame frame;
	siginfo_t user_info;
	ucontext_t user_context;
	sigset_t pending;
	uintptr_t sp;
	uintptr_t restorer;
	uintptr_t info_pointer;
	uintptr_t context_pointer;
	uintptr_t restart_args[HAL_SYSCALL_ARGS];
	uint32_t restart_number;
	unsigned restart_valid;
	uint32_t token;
	unsigned long irq;
	int signo;
	unsigned used_altstack;

	/* Kernel threads and threads without a process deliver nothing. */
	thread = curthread;
	if (thread == NULL)
		return;
	process = thread->proc;
	if (process == NULL || process == &process0)
		return;

	/*
	 * A restart candidate belongs exclusively to this return-to-user
	 * boundary.  Consume it before inspecting dispositions: a signal may
	 * be changed to SIG_IGN after it interrupted the wait, and retaining
	 * the candidate would let an unrelated, later SA_RESTART handler
	 * redispatch a syscall whose EINTR was already observed by user
	 * space.
	 */
	restart_number = thread->syscall_restart_number;
	memcpy(restart_args, thread->syscall_restart_args,
	    sizeof(restart_args));
	restart_valid = thread->syscall_restart_valid;
	thread->syscall_restart_valid = 0;
retry:
	/* Gathers the unblocked pending signals, always including the unmaskable. */
	irq = spin_lock_irqsave(&process->lock);

	pending = (thread->signal_pending |
	    (process->signal_pending & ~signal_wait_claims_locked(process))) &
	    ~thread->signal_mask;
	pending |= (thread->signal_pending | process->signal_pending) &
	    (SIGNAL_BIT(SIGKILL) | SIGNAL_BIT(SIGSTOP));

	/* With nothing pending, a sigsuspend() restores its mask and returns. */
	if (pending == 0) {
		if (thread->signal_suspended) {
			thread->signal_mask = thread->signal_suspend_mask;
			thread->signal_suspended = 0;
		}

		spin_unlock_irqrestore(&process->lock, irq);
		return;
	}

	/* Takes the lowest pending signal from the thread or the process. */
	signo = 1;
	while (signo < NSIG && (pending & SIGNAL_BIT(signo)) == 0)
		signo++;
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

	/* An ignored signal is dropped and the next one considered. */
	if (action.handler == (uintptr_t)SIG_IGN &&
	    signo != SIGKILL &&
	    signo != SIGSTOP)
		goto retry;

	/* A default action ignores, stops, or terminates. */
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

	/* Too deep a handler nesting or an unreadable user frame is fatal. */
	if (thread->signal_depth >= SIGNAL_NEST_MAX)
		exit1_signal(SIGSEGV);
	if (hal_task_user_context(&interrupted) != 0)
		exit1_signal(SIGSEGV);

	/* Switches to the alternate stack when the handler asked for it. */
	sp = interrupted.stack_pointer;
	if ((action.flags & SA_ONSTACK) != 0 &&
	    (thread->signal_altstack_flags & SS_DISABLE) == 0 &&
	    thread->signal_on_altstack_depth == 0) {
		sp = thread->signal_altstack_base + thread->signal_altstack_size;
		thread->signal_on_altstack_depth++;
		level = &thread->signal_levels[thread->signal_depth];
		level->used_altstack = 1;
	}

	/* Records the handler level with a fresh non-zero token. */
	restorer = action.restorer;
	thread->signal_token_counter++;
	token = thread->signal_token_counter;
	if (token == 0) {
		thread->signal_token_counter++;
		token = thread->signal_token_counter;
	}

	level = &thread->signal_levels[thread->signal_depth];
	used_altstack = level->used_altstack;
	memset(level, 0, sizeof(*level));
	level->used_altstack = used_altstack;
	level->token = token;
	if (thread->signal_suspended)
		level->saved_mask = thread->signal_suspend_mask;
	else
		level->saved_mask = thread->signal_mask;
	level->restart_number = restart_number;
	memcpy(level->restart_args, restart_args,
	    sizeof(level->restart_args));
	level->restart_on_return = restart_valid &&
	    (action.flags & SA_RESTART) != 0;

	/* Builds the user-visible signal information and context. */
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

	/* Pushes the signal frame on the chosen stack. */
	memset(&frame, 0, sizeof(frame));
	sp = (sp - sizeof(frame)) &
	    ~((uintptr_t)HAL_TASK_SIGNAL_FRAME_ALIGNMENT - 1U);
	info_pointer = sp + offsetof(struct signal_frame, info);
	context_pointer = sp + offsetof(struct signal_frame, context);
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
	level->user_ucontext = context_pointer;

	/* Installs the handler's mask, and resets a one-shot handler. */
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

	/* Redirects the thread into the handler. */
	if (hal_task_signal_enter(action.handler, sp, signo, info_pointer,
	    context_pointer, restorer, token) != 0)
		exit1_signal(SIGSEGV);
}

/*
 * Sends a kernel-originated signal to a thread.
 */
int
signal_send_thread(
	struct thread *thread,
	int signo)
{
	struct signal_info info;
	int error;

	/* Describes the signal as coming from the kernel. */
	memset(&info, 0, sizeof(info));
	info.code = SI_KERNEL;

	/* Reports why the send failed. */
	error = signal_send_thread_info(thread, signo, &info);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Sends a signal with its information to one thread.
 *
 * The discard and ignore rules are those of a process-directed signal;
 * the signal itself is recorded on the thread alone.
 */
int
signal_send_thread_info(
	struct thread *thread,
	int signo,
	const struct signal_info *info)
{
	struct signal_timer_completion completions[SIGNAL_TIMER_COMPLETION_MAX];
	struct process *process;
	unsigned long irq;
	unsigned completion_count;

	completion_count = 0;

	/* Rejects a thread without a process or a bad signal. */
	if (thread == NULL)
		return EINVAL;
	process = thread->proc;
	if (process == NULL || !signal_valid(signo))
		return EINVAL;

	/* Applies the mutual discard of SIGCONT and the stop signals. */
	irq = spin_lock_irqsave(&process->lock);

	if (signo == SIGCONT)
		signal_discard_locked(process, SIGNAL_BIT(SIGSTOP) |
		    SIGNAL_BIT(SIGTSTP) | SIGNAL_BIT(SIGTTIN) |
		    SIGNAL_BIT(SIGTTOU), completions, &completion_count);
	else if (signal_stop(signo))
		signal_discard_locked(process, SIGNAL_BIT(SIGCONT), completions,
		    &completion_count);

	/* An ignored signal is dropped, but SIGCONT still continues. */
	if (signal_ignored_disposition_locked(process, signo)) {
		spin_unlock_irqrestore(&process->lock, irq);
		signal_timer_completions_run(process, completions, completion_count);
		signal_timer_complete_one(process, info);
		if (signo == SIGCONT)
			(void)process_continue(process, 1);
		return 0;
	}

	/* Keeps the first information record until the signal is taken. */
	if ((thread->signal_pending & SIGNAL_BIT(signo)) == 0) {
		memset(&thread->signal_info[signo], 0,
		    sizeof(thread->signal_info[signo]));
		if (info != NULL)
			thread->signal_info[signo] = *info;
	}

	thread->signal_pending |= SIGNAL_BIT(signo);

	/* Interrupts the thread when it can take or is waiting for the signal. */
	if (signal_pending_unblocked_locked(thread) ||
	    (thread->signal_waiting &&
	    (thread->signal_wait_set & SIGNAL_BIT(signo)) != 0))
		sched_interrupt(thread);

	spin_unlock_irqrestore(&process->lock, irq);

	signal_timer_completions_run(process, completions, completion_count);

	/* Continues a stopped process for SIGCONT and SIGKILL. */
	if (signo == SIGCONT || signo == SIGKILL)
		(void)process_continue(process, signo == SIGCONT);

	/* Reports the sent signal. */
	return 0;
}

/*
 * Implements sigtimedwait(): takes one signal of a set, waiting for it.
 *
 * The wait ends with EINTR for a termination request or another
 * deliverable signal, and with EAGAIN when the deadline passes.  A stop
 * request stops the thread and resumes the wait.
 */
int
signal_timedwait(
	struct thread *thread,
	sigset_t set,
	uint64_t deadline,
	int timed,
	struct signal_info *info,
	int *signo_out)
{
	struct process *process;
	sigset_t pending;
	unsigned long irq;
	int signo;

	/* Rejects a thread without a process, a missing result, or an empty set. */
	if (thread == NULL)
		return EINVAL;
	process = thread->proc;
	if (process == NULL || info == NULL || signo_out == NULL || set == 0)
		return EINVAL;

	/* The unmaskable signals cannot be waited for. */
	set &= SIGNAL_VALID_MASK &
	    ~(SIGNAL_BIT(SIGKILL) | SIGNAL_BIT(SIGSTOP));
	if (set == 0)
		return EINVAL;

	/* Waits, claiming the set so that nobody else takes its signals. */
	irq = spin_lock_irqsave(&process->lock);

	for (;;) {
		pending = (thread->signal_pending |
		    process->signal_pending) & set;

		/* A termination request ends the wait. */
		if (thread->terminate_requested) {
			thread->signal_waiting = 0;
			thread->signal_wait_set = 0;
			spin_unlock_irqrestore(&process->lock, irq);
			return EINTR;
		}

		/* A stop request stops the thread, then the wait resumes. */
		if (process->stop_requested) {
			thread->signal_waiting = 0;
			thread->signal_wait_set = 0;
			spin_unlock_irqrestore(&process->lock, irq);
			process_stop_current(0);
			irq = spin_lock_irqsave(&process->lock);
			continue;
		}

		/* Takes the lowest pending signal of the set. */
		if (pending != 0) {
			signo = 1;
			while (signo < NSIG && (pending & SIGNAL_BIT(signo)) == 0)
				signo++;
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

		/* The deadline or another deliverable signal ends the wait. */
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

		/* Sleeps with the claim published. */
		thread->signal_wait_set = set;
		thread->signal_waiting = 1;
		sched_sleep_locked(deadline, &process->lock);
	}
}

/* Tests whether a signal number is in range. */
static int
signal_valid(
	int signo)
{
	/* Signals are numbered from one below NSIG. */
	if (signo <= 0)
		return 0;
	if (signo >= NSIG)
		return 0;
	return 1;
}

/* Tests whether a signal is a job control stop signal. */
static int
signal_stop(
	int s)
{
	/* The four stop signals. */
	if (s == SIGSTOP)
		return 1;
	if (s == SIGTSTP)
		return 1;
	if (s == SIGTTIN)
		return 1;
	if (s == SIGTTOU)
		return 1;
	return 0;
}

/* Tests whether a signal's default action is to ignore it. */
static int
signal_ignored_default(
	int s)
{
	/* The four signals ignored by default. */
	if (s == SIGCHLD)
		return 1;
	if (s == SIGCONT)
		return 1;
	if (s == SIGURG)
		return 1;
	if (s == SIGWINCH)
		return 1;
	return 0;
}

/* Tests whether a process's disposition ignores a signal; the caller holds the lock. */
static int
signal_ignored_disposition_locked(
	const struct process *process,
	int signo)
{
	const struct signal_action *action;

	action = &process->signal_actions[signo];

	/* SIGKILL and SIGSTOP can never be ignored. */
	if (signo == SIGKILL || signo == SIGSTOP)
		return 0;

	/* An explicit ignore, or a default that ignores, ignores. */
	if (action->handler == (uintptr_t)SIG_IGN)
		return 1;
	if (action->handler == (uintptr_t)SIG_DFL && signal_ignored_default(signo))
		return 1;
	return 0;
}

/* Remembers a discarded timer notification once for completion. */
static void
signal_timer_completion_add(
	struct signal_timer_completion *completions,
	unsigned *count,
	const struct signal_info *info)
{
	unsigned index;

	/* Only timer notifications need completing. */
	if (info == NULL ||
	    info->code != SI_TIMER ||
	    info->timer_generation == 0)
		return;

	/* Each timer generation is completed once. */
	for (index = 0; index < *count; index++) {
		if (completions[index].slot == info->timer_slot &&
		    completions[index].generation == info->timer_generation)
			return;
	}

	if (*count >= SIGNAL_TIMER_COMPLETION_MAX)
		HAL_FATAL("signal timer completion overflow");
	completions[*count].slot = info->timer_slot;
	completions[*count].generation = info->timer_generation;
	(*count)++;
}

/* Completes the remembered timer notifications outside the process lock. */
static void
signal_timer_completions_run(
	struct process *process,
	const struct signal_timer_completion *completions,
	unsigned count)
{
	unsigned index;

	/* Reports each completion to the process timers. */
	for (index = 0; index < count; index++)
		process_timer_notification_complete(process, completions[index].slot,
		    completions[index].generation);
}

/* Completes the timer notification a taken signal carried, if any. */
static void
signal_timer_complete_one(
	struct process *process,
	const struct signal_info *info)
{
	/* Only a timer notification needs completing. */
	if (info != NULL &&
	    info->code == SI_TIMER &&
	    info->timer_generation != 0)
		process_timer_notification_complete(process, info->timer_slot,
		    info->timer_generation);
}

/* Unions the signal sets the waiting threads claimed; the caller holds the lock. */
static sigset_t
signal_wait_claims_locked(
	const struct process *process)
{
	const struct thread *thread;
	sigset_t claims;

	/*
	 * A process-directed signal selected by sigtimedwait() must not be
	 * consumed by another thread's ordinary return to user mode before
	 * the waiter runs.
	 */
	claims = 0;
	for (thread = process->threads; thread != NULL;
	     thread = thread->proc_next) {
		if (thread->signal_waiting)
			claims |= thread->signal_wait_set;
	}

	/* Reports the claimed signals. */
	return claims;
}

/* Takes a process-directed signal and its information; the caller holds the lock. */
static void
signal_take_process_locked(
	struct process *process,
	int signo,
	struct signal_info *info)
{
	unsigned i;
	unsigned selected;

	selected = SIGNAL_QUEUE_MAX;

	/* A queued instance is taken before the unqueued record. */
	for (i = 0; i < process->signal_queue_count; i++) {
		if (process->signal_queue[i].signo == signo) {
			selected = i;
			break;
		}
	}

	if (selected != SIGNAL_QUEUE_MAX) {
		*info = process->signal_queue[selected].info;
		for (i = selected + 1U; i < process->signal_queue_count; i++)
			process->signal_queue[i - 1U] = process->signal_queue[i];
		process->signal_queue_count--;

		/* The signal stays pending while another instance remains. */
		for (i = 0; i < process->signal_queue_count; i++) {
			if (process->signal_queue[i].signo == signo)
				return;
		}

		if ((process->signal_unqueued_pending & SIGNAL_BIT(signo)) != 0)
			return;
	} else {
		*info = process->signal_info[signo];
		process->signal_unqueued_pending &= ~SIGNAL_BIT(signo);
	}

	/* Clears the last instance. */
	process->signal_pending &= ~SIGNAL_BIT(signo);
	memset(&process->signal_info[signo], 0,
	    sizeof(process->signal_info[signo]));
}

/* Removes every instance of a signal set from the process and its threads. */
static void
signal_discard_locked(
	struct process *process,
	sigset_t set,
	struct signal_timer_completion *completions,
	unsigned *completion_count)
{
	struct thread *thread;
	unsigned read_index;
	unsigned write_index;
	int signo;

	write_index = 0;

	/*
	 * The caller holds the process lock; stop/continue generation uses
	 * this to implement POSIX's mutual-discard rule atomically with
	 * enqueuing the new signal.
	 */
	process->signal_pending &= ~set;
	process->signal_unqueued_pending &= ~set;
	for (signo = 1; signo < NSIG; signo++) {
		if ((set & SIGNAL_BIT(signo)) != 0) {
			signal_timer_completion_add(completions, completion_count,
			    &process->signal_info[signo]);
			memset(&process->signal_info[signo], 0,
			    sizeof(process->signal_info[signo]));
		}
	}

	/* Compacts the queue, remembering the discarded timer notifications. */
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

	/* Clears the thread-directed instances too. */
	for (thread = process->threads; thread != NULL;
	     thread = thread->proc_next) {
		thread->signal_pending &= ~set;
		for (signo = 1; signo < NSIG; signo++) {
			if ((set & SIGNAL_BIT(signo)) != 0) {
				memset(&thread->signal_info[signo], 0,
				    sizeof(thread->signal_info[signo]));
			}
		}
	}
}

/* Tests whether a process may send a signal to another. */
static int
signal_permitted(
	const struct process *sender,
	const struct process *target,
	int signo)
{
	struct ucred *sender_cred;
	struct ucred *target_cred;
	int permitted;

	/* The kernel process is never a target. */
	if (sender == NULL || target == NULL || target == &process0)
		return 0;

	/* The superuser, a matching user, or SIGCONT within a session may send. */
	sender_cred = cred_process_ref((struct process *)sender);
	target_cred = cred_process_ref((struct process *)target);
	permitted = 0;
	if (sender_cred != NULL &&
	    target_cred != NULL &&
	    (cred_is_superuser(sender_cred) ||
	     sender_cred->ruid == target_cred->ruid ||
	     sender_cred->euid == target_cred->ruid ||
	     sender_cred->ruid == target_cred->suid ||
	     sender_cred->euid == target_cred->suid ||
	     (signo == SIGCONT && sender->session == target->session)))
		permitted = 1;
	cred_release(target_cred);
	cred_release(sender_cred);

	/* Reports the permission. */
	return permitted;
}
