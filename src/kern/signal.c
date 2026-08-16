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
#include <stddef.h>
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

void signal_init(void) { }

void
kernel_user_return_handler(void)
{
	signal_deliver_on_user_return();
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
	struct thread *thread;
	unsigned long irq;
	int continued = 0;
	if (process == NULL || process == &process0 || !signal_valid(signo))
		return EINVAL;
	irq = spin_lock_irqsave(&process->lock);
	if ((process->signal_pending & SIGNAL_BIT(signo)) == 0) {
		memset(&process->signal_info[signo], 0,
		    sizeof(process->signal_info[signo]));
		if (info != NULL)
			process->signal_info[signo] = *info;
	}
	process->signal_pending |= SIGNAL_BIT(signo);
	for (thread = process->threads; thread != NULL;
	     thread = thread->proc_next)
		if (thread->state == THREAD_SLEEPING &&
		    signal_pending_unblocked(thread))
			sched_wakeup(thread);
	if ((signo == SIGCONT || signo == SIGKILL) &&
	    process->state == PROCESS_STOPPED) {
		process->state = PROCESS_RUNNING;
		continued = signo == SIGCONT;
		for (thread = process->threads; thread != NULL;
		     thread = thread->proc_next)
			if (thread->state == THREAD_SLEEPING)
				sched_wakeup(thread);
	}
	spin_unlock_irqrestore(&process->lock, irq);
	if (continued)
		process_note_continued(process);
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
	struct signal_info info;
	pid_t cursor = -1;
	int found = 0, permitted = 0;
	if (sender == NULL || signo < 0 || signo >= NSIG)
		return EINVAL;
	memset(&info, 0, sizeof(info));
	info.code = SI_USER;
	info.pid = sender->pid;
	info.uid = sender->cred != NULL ? sender->cred->euid : 0;
	while ((p = process_find_next_ref(cursor)) != NULL) {
		int match = selector > 0 ? p->pid == selector :
		    selector == 0 ? p->pgrp == sender->pgrp :
		    selector == -1 ? p != &process0 : p->pgrp == -selector;
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
	memset(child->signal_info, 0, sizeof(child->signal_info));
	child_thread->signal_mask = parent_thread->signal_mask;
	child_thread->signal_pending = 0;
	child_thread->signal_token = 0;
	child_thread->signal_token_counter = 0;
	child_thread->signal_depth = 0;
	memset(child_thread->signal_levels, 0,
	    sizeof(child_thread->signal_levels));
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
	memset(process->signal_info, 0, sizeof(process->signal_info));
	if (curthread != NULL) {
		curthread->signal_pending = 0;
		curthread->signal_token = 0;
		curthread->signal_token_counter = 0;
		curthread->signal_depth = 0;
		memset(curthread->signal_levels, 0,
		    sizeof(curthread->signal_levels));
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
	struct signal_action action;
	struct signal_info selected_info;
	struct thread_signal_level *level;
	struct hal_user_context interrupted;
	siginfo_t user_info;
	ucontext_t user_context;
	sigset_t pending;
	uintptr_t sp, restorer, info_pointer, context_pointer;
	uint32_t token;
	unsigned long irq;
	int signo;

	if (thread == NULL || (process = thread->proc) == NULL ||
	    process == &process0)
		return;
retry:
	irq = spin_lock_irqsave(&process->lock);
	pending = (thread->signal_pending | process->signal_pending) &
	    ~thread->signal_mask;
	pending |= (thread->signal_pending | process->signal_pending) &
	    (SIGNAL_BIT(SIGKILL) | SIGNAL_BIT(SIGSTOP));
	if (pending == 0) {
		spin_unlock_irqrestore(&process->lock, irq);
		return;
	}
	for (signo = 1; signo < NSIG && (pending & SIGNAL_BIT(signo)) == 0;
	     signo++)
		;
	thread->signal_pending &= ~SIGNAL_BIT(signo);
	process->signal_pending &= ~SIGNAL_BIT(signo);
	action = process->signal_actions[signo];
	selected_info = process->signal_info[signo];
	memset(&process->signal_info[signo], 0,
	    sizeof(process->signal_info[signo]));
	spin_unlock_irqrestore(&process->lock, irq);
	if (action.handler == (uintptr_t)SIG_IGN && signo != SIGKILL &&
	    signo != SIGSTOP)
		goto retry;
	if (action.handler == (uintptr_t)SIG_DFL) {
		if (signal_ignored_default(signo))
			goto retry;
		if (signal_stop(signo)) {
			process->state = PROCESS_STOPPED;
			process_note_stopped(process, signo);
			thread->state = THREAD_SLEEPING;
			sched_yield();
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
	restorer = action.restorer;
	token = ++thread->signal_token_counter;
	if (token == 0)
		token = ++thread->signal_token_counter;
	level = &thread->signal_levels[thread->signal_depth];
	memset(level, 0, sizeof(*level));
	level->token = token;
	level->saved_mask = thread->signal_suspended ?
	    thread->signal_suspend_mask : thread->signal_mask;
	level->restart_number = thread->syscall_restart_number;
	memcpy(level->restart_args, thread->syscall_restart_args,
	    sizeof(level->restart_args));
	level->restart_on_return = thread->syscall_restart_valid &&
	    (action.flags & SA_RESTART) != 0;
	memset(&user_info, 0, sizeof(user_info));
	user_info.si_signo = signo;
	user_info.si_errno = selected_info.error;
	user_info.si_code = selected_info.code;
	user_info.si_pid = selected_info.pid;
	user_info.si_uid = selected_info.uid;
	user_info.si_status = selected_info.status;
	user_info.si_addr = (uint64_t)selected_info.address;
	memset(&user_context, 0, sizeof(user_context));
	user_context.uc_sigmask = level->saved_mask;
	user_context.uc_mcontext.mc_pc = (uint64_t)interrupted.pc;
	user_context.uc_mcontext.mc_sp = (uint64_t)interrupted.stack_pointer;
	user_context.uc_mcontext.mc_retval = (int64_t)interrupted.return_value;
	level->saved_ucontext = user_context;
	thread->signal_depth++;
	thread->signal_token = token;
#if defined(HAL_ARCH_ARM64)
	{
		struct {
			uint32_t token, reserved;
			uint64_t context_pointer;
			siginfo_t info;
			ucontext_t context;
		} frame;
		memset(&frame, 0, sizeof(frame));
		sp = (sp - sizeof(frame)) & ~(uintptr_t)15U;
		info_pointer = sp + offsetof(typeof(frame), info);
		context_pointer = sp + offsetof(typeof(frame), context);
		frame.token = token;
		frame.context_pointer = context_pointer;
		frame.info = user_info;
		frame.context = user_context;
		if (copyout(&frame, sp, sizeof(frame)) != 0)
			exit1_signal(SIGSEGV);
	}
#elif defined(HAL_ARCH_AMD64)
	{
		struct {
			uint64_t restorer, token, context_pointer;
			siginfo_t info;
			ucontext_t context;
		} frame;
		memset(&frame, 0, sizeof(frame));
		sp = (sp - sizeof(frame)) & ~(uintptr_t)15U;
		info_pointer = sp + offsetof(typeof(frame), info);
		context_pointer = sp + offsetof(typeof(frame), context);
		frame.restorer = (uint64_t)restorer;
		frame.token = token;
		frame.context_pointer = context_pointer;
		frame.info = user_info;
		frame.context = user_context;
		if (copyout(&frame, sp, sizeof(frame)) != 0)
			exit1_signal(SIGSEGV);
	}
#elif defined(HAL_ARCH_SPARCV9)
	{
		/*
		 * A SPARC V9 caller frame is 176 bytes.  Offsets 0..127 are the
		 * register-window spill area, so keep the private token at 128.
		 */
		struct {
			uint64_t prefix[22];
			siginfo_t info;
			ucontext_t context;
		} frame;
		memset(&frame, 0, sizeof(frame));
		sp = (sp - sizeof(frame)) & ~(uintptr_t)15U;
		info_pointer = sp + offsetof(typeof(frame), info);
		context_pointer = sp + offsetof(typeof(frame), context);
		frame.prefix[16] = token;
		frame.prefix[17] = context_pointer;
		frame.info = user_info;
		frame.context = user_context;
		if (copyout(&frame, sp, sizeof(frame)) != 0)
			exit1_signal(SIGSEGV);
	}
#else
	{
		struct {
			uint32_t restorer, signo, info_pointer;
			uint32_t context_pointer, token;
			siginfo_t info;
			ucontext_t context;
		} frame;
		memset(&frame, 0, sizeof(frame));
		sp = (sp - sizeof(frame)) & ~(uintptr_t)3U;
		info_pointer = sp + offsetof(typeof(frame), info);
		context_pointer = sp + offsetof(typeof(frame), context);
		frame.restorer = (uint32_t)restorer;
		frame.signo = (uint32_t)signo;
		frame.info_pointer = (uint32_t)info_pointer;
		frame.context_pointer = (uint32_t)context_pointer;
		frame.token = token;
		frame.info = user_info;
		frame.context = user_context;
		if (copyout(&frame, sp, sizeof(frame)) != 0)
			exit1_signal(SIGSEGV);
	}
#endif
	level->user_ucontext = context_pointer;
	if (thread->signal_suspended) {
		thread->signal_mask = thread->signal_suspend_mask;
		thread->signal_suspended = 0;
	}
	thread->syscall_restart_on_return = level->restart_on_return;
	thread->signal_mask |= action.mask;
	if ((action.flags & SA_NODEFER) == 0)
		thread->signal_mask |= SIGNAL_BIT(signo);
	if ((action.flags & SA_RESETHAND) != 0) {
		irq = spin_lock_irqsave(&process->lock);
		if (process->signal_actions[signo].handler == action.handler) {
			process->signal_actions[signo].handler =
			    (uintptr_t)SIG_DFL;
			process->signal_actions[signo].mask = 0;
			process->signal_actions[signo].flags = 0;
			process->signal_actions[signo].restorer = 0;
		}
		spin_unlock_irqrestore(&process->lock, irq);
	}
	if (hal_task_signal_enter(action.handler, sp, signo, info_pointer,
	    context_pointer, restorer, token) != 0)
		exit1_signal(SIGSEGV);
}
