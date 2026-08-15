/*
 * Standard POSIX signals
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */
#include "kern/signal.h"
#include "kern/cred.h"
#include "kern/process.h"
#include "kern/sched.h"
#include "kern/thread.h"
#include "kern/uaccess.h"

#include <errno.h>
#include <hal/hal.h>
#include <string.h>

#define SIGNAL_BIT(n) (1U << ((unsigned)(n) - 1U))

static int signal_valid(int signo) { return signo > 0 && signo < NSIG; }
static int signal_stop(int s)
{ return s==SIGSTOP||s==SIGTSTP||s==SIGTTIN||s==SIGTTOU; }
static int signal_ignored_default(int s) { return s==SIGCHLD||s==SIGCONT; }

int
signal_pending_unblocked(const struct thread *thread)
{
	const struct process *process;
	sigset_t pending;
	int signo;
	if (thread == NULL || (process = thread->proc) == NULL)
		return 0;
	pending = (thread->signal_pending | process->signal_pending) &
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

void signal_init(void) { hal_user_return_set_handler(signal_deliver_on_user_return); }

int
signal_send_process(struct process *process, int signo)
{
	struct thread *thread;
	if (process == NULL || process == &process0 || !signal_valid(signo))
		return EINVAL;
	process->signal_pending |= SIGNAL_BIT(signo);
	for (thread = process->threads; thread != NULL;
	     thread = thread->proc_next)
		if (thread->state == THREAD_SLEEPING &&
		    signal_pending_unblocked(thread))
			sched_wakeup(thread);
	if ((signo == SIGCONT || signo == SIGKILL) &&
	    process->state == PROCESS_STOPPED) {
		process->state = PROCESS_RUNNING;
		if (signo == SIGCONT)
			process_note_continued(process);
		for (thread = process->threads; thread != NULL;
		     thread = thread->proc_next)
			if (thread->state == THREAD_SLEEPING)
				sched_wakeup(thread);
	}
	return 0;
}

static int
signal_permitted(const struct process *sender, const struct process *target,
		 int signo)
{
	if (sender == NULL || target == NULL || sender->cred == NULL ||
	    target->cred == NULL || target == &process0)
		return 0;
	return cred_is_superuser(sender->cred) ||
	    sender->cred->ruid == target->cred->ruid ||
	    sender->cred->euid == target->cred->ruid ||
	    sender->cred->ruid == target->cred->suid ||
	    sender->cred->euid == target->cred->suid ||
	    (signo == SIGCONT && sender->session == target->session);
}

int
signal_kill(struct process *sender, pid_t selector, int signo)
{
	struct process *p;
	int found = 0, permitted = 0;
	if (sender == NULL || signo < 0 || signo >= NSIG)
		return EINVAL;
	for (p = process_first(); p != NULL; p = process_next(p)) {
		int match = selector > 0 ? p->pid == selector :
		    selector == 0 ? p->pgrp == sender->pgrp :
		    selector == -1 ? p != &process0 : p->pgrp == -selector;
		if (!match || p == &process0 || p->state == PROCESS_DEAD)
			continue;
		found = 1;
		if (!signal_permitted(sender, p, signo))
			continue;
		permitted = 1;
		if (signo != 0)
			(void)signal_send_process(p, signo);
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
	child_thread->signal_mask = parent_thread->signal_mask;
	child_thread->signal_pending = 0;
	child_thread->syscall_restart_valid = 0;
	child_thread->syscall_restart_on_return = 0;
}

void
signal_exec(struct process *process)
{
	int i;
	if (process == NULL)
		return;
	for (i = 1; i < NSIG; i++)
		if (process->signal_actions[i].handler != (uintptr_t)SIG_IGN)
			memset(&process->signal_actions[i], 0,
			    sizeof(process->signal_actions[i]));
	process->signal_pending = 0;
	if (curthread != NULL) {
		curthread->signal_pending = 0;
		curthread->signal_token = 0;
		curthread->signal_suspended = 0;
		curthread->syscall_restart_valid = 0;
		curthread->syscall_restart_on_return = 0;
	}
}

void
signal_deliver_on_user_return(void)
{
	struct thread *thread = curthread;
	struct process *process;
	struct signal_action *action;
	sigset_t pending;
	uintptr_t sp, restorer;
	uint32_t token;
	int signo;

	if (thread == NULL || (process = thread->proc) == NULL ||
	    process == &process0 || thread->signal_token != 0)
		return;
	pending = (thread->signal_pending | process->signal_pending) &
	    ~thread->signal_mask;
	pending |= (thread->signal_pending | process->signal_pending) &
	    (SIGNAL_BIT(SIGKILL) | SIGNAL_BIT(SIGSTOP));
	if (pending == 0)
		return;
	for (signo = 1; signo < NSIG && (pending & SIGNAL_BIT(signo)) == 0;
	     signo++)
		;
	thread->signal_pending &= ~SIGNAL_BIT(signo);
	process->signal_pending &= ~SIGNAL_BIT(signo);
	action = &process->signal_actions[signo];
	if (action->handler == (uintptr_t)SIG_IGN && signo != SIGKILL &&
	    signo != SIGSTOP)
		return;
	if (action->handler == (uintptr_t)SIG_DFL) {
		if (signal_ignored_default(signo))
			return;
		if (signal_stop(signo)) {
			process->state = PROCESS_STOPPED;
			process_note_stopped(process, signo);
			thread->state = THREAD_SLEEPING;
			sched_yield();
			return;
		}
		exit1_signal(signo);
	}
	sp = hal_task_user_stack();
	restorer = action->restorer;
	token = ++thread->signal_token;
	if (token == 0)
		token = ++thread->signal_token;
#if defined(HAL_ARCH_ARM64)
	sp = (sp - 16U) & ~(uintptr_t)15U;
	if (copyout(&token, sp, sizeof(token)) != 0)
		exit1_signal(SIGSEGV);
#elif defined(HAL_ARCH_AMD64)
	{
		uint64_t frame[2];
		sp = (sp - sizeof(frame)) & ~(uintptr_t)15U;
		frame[0] = (uint64_t)restorer;
		frame[1] = token;
		if (copyout(frame, sp, sizeof(frame)) != 0)
			exit1_signal(SIGSEGV);
	}
#elif defined(HAL_ARCH_SPARCV9)
	{
		uint64_t frame[2];
		sp = (sp - sizeof(frame)) & ~(uintptr_t)15U;
		frame[0] = token;
		frame[1] = 0;
		if (copyout(frame, sp, sizeof(frame)) != 0)
			exit1_signal(SIGSEGV);
	}
#else
	{
		uint32_t frame[3];
		sp = (sp - sizeof(frame)) & ~(uintptr_t)3U;
		frame[0] = (uint32_t)restorer;
		frame[1] = (uint32_t)signo;
		frame[2] = token;
		if (copyout(frame, sp, sizeof(frame)) != 0)
			exit1_signal(SIGSEGV);
	}
#endif
	thread->signal_saved_mask = thread->signal_mask;
	if (thread->signal_suspended) {
		thread->signal_saved_mask = thread->signal_suspend_mask;
		thread->signal_mask = thread->signal_suspend_mask;
		thread->signal_suspended = 0;
	}
	thread->syscall_restart_on_return =
	    thread->syscall_restart_valid &&
	    (action->flags & SA_RESTART) != 0;
	thread->signal_mask |= action->mask;
	if ((action->flags & SA_NODEFER) == 0)
		thread->signal_mask |= SIGNAL_BIT(signo);
	if ((action->flags & SA_RESETHAND) != 0) {
		action->handler = (uintptr_t)SIG_DFL;
		action->mask = 0;
		action->flags = 0;
		action->restorer = 0;
	}
	if (hal_task_signal_enter(action->handler, sp, signo, restorer,
	    token) != 0)
		exit1_signal(SIGSEGV);
}
