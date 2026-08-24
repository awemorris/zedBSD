/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

/*
 * Exercise signal_deliver_on_user_return() itself.  In particular, model a
 * signal which interrupted a restartable syscall and whose disposition was
 * changed to SIG_IGN before the kernel reached the user-return boundary.
 */
#include <kern/process.h>
#include <kern/cred.h>
#include <kern/thread.h>

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SIGNAL_BIT(signo) \
	((sigset_t)1ULL << ((unsigned)(signo) - 1U))

struct process process0;

static struct process test_process;
static struct thread test_thread;
static struct ucred test_cred;
static unsigned char user_stack[4096];
static unsigned signal_enters;
static uintptr_t entered_handler;
static int entered_signo;
static int irq_enabled;
static unsigned accounting_depth;
static unsigned accounting_enters;
static uint64_t fake_ticks;
static uint64_t sleep_deadlines[4];
static unsigned sleep_calls;
static unsigned stop_calls;
static unsigned continue_calls;
static unsigned timer_completion_calls;

enum timedwait_test_mode {
	TIMEDWAIT_NONE,
	TIMEDWAIT_TERMINATE,
	TIMEDWAIT_STOP,
	TIMEDWAIT_STOP_BEFORE_RETURN,
};

static enum timedwait_test_mode timedwait_mode;

struct thread *
thread_current(void)
{
	return &test_thread;
}

bool
hal_irq_disable(void)
{
	bool was_enabled = irq_enabled != 0;

	irq_enabled = 0;
	return was_enabled;
}

void
hal_irq_enable(void)
{
	irq_enabled = 1;
}

void
hal_fatal(const char *file, int line, const char *message)
{
	(void)file;
	(void)line;
	(void)message;
	abort();
}

void
sched_accounting_kernel_enter(void)
{
	assert(irq_enabled == 0);
	accounting_depth++;
	accounting_enters++;
}

void
sched_accounting_kernel_leave(void)
{
	assert(irq_enabled == 0);
	assert(accounting_depth == 1U);
	accounting_depth--;
}

void
thread_exit(int status)
{
	(void)status;
	abort();
}

int
process_stop_requested(const struct thread *thread)
{
	return thread != NULL && thread->proc != NULL &&
	    thread->proc->stop_requested;
}

unsigned long
spin_lock_irqsave(struct spinlock *lock)
{
	(void)lock;
	return 0;
}

void
spin_unlock_irqrestore(struct spinlock *lock, unsigned long irq)
{
	(void)lock;
	(void)irq;
}

void
process_stop_current(int signo)
{
	if (timedwait_mode == TIMEDWAIT_STOP) {
		assert(signo == 0);
		assert(test_thread.signal_waiting == 0);
		assert(test_thread.signal_wait_set == 0);
		assert(test_process.stop_requested);
		test_process.stop_requested = 0;
		test_process.state = PROCESS_RUNNING;
		fake_ticks = 150;
	} else {
		assert(timedwait_mode == TIMEDWAIT_STOP_BEFORE_RETURN);
		assert(signo == SIGTSTP);
	}
	stop_calls++;
}

int
process_continue(struct process *process, int explicit_continue)
{
	(void)process;
	assert(explicit_continue == 0 || explicit_continue == 1);
	continue_calls++;
	if (explicit_continue) {
		process->stop_requested = 0;
		process->state = PROCESS_RUNNING;
	}
	return 0;
}

void
process_timer_notification_complete(struct process *process, unsigned slot,
	uint32_t generation)
{
	(void)process;
	(void)slot;
	assert(generation != 0);
	timer_completion_calls++;
}

struct process *
process_find_next_ref(pid_t cursor)
{
	return cursor < test_process.pid ? &test_process : NULL;
}

void
process_release(struct process *process)
{
	(void)process;
}

struct ucred *
cred_process_ref(struct process *process)
{
	return process != NULL ? process->cred : NULL;
}

void
cred_release(struct ucred *cred)
{
	(void)cred;
}

int
cred_is_superuser(const struct ucred *cred)
{
	return cred != NULL && cred->euid == 0;
}

void
sched_interrupt(struct thread *thread)
{
	(void)thread;
}

uint64_t
sched_ticks(void)
{
	return fake_ticks;
}

void
sched_sleep_locked(uint64_t deadline, struct spinlock *lock)
{
	assert(lock == &test_process.lock);
	assert(sleep_calls < sizeof(sleep_deadlines) / sizeof(sleep_deadlines[0]));
	sleep_deadlines[sleep_calls++] = deadline;
	if (timedwait_mode == TIMEDWAIT_TERMINATE && sleep_calls == 1) {
		test_thread.terminate_requested = 1;
		return;
	}
	if (timedwait_mode == TIMEDWAIT_STOP && sleep_calls == 1) {
		test_process.stop_requested = 1;
		test_process.state = PROCESS_STOPPED;
		return;
	}
	if (timedwait_mode == TIMEDWAIT_STOP && sleep_calls == 2) {
		test_thread.signal_pending |= SIGNAL_BIT(SIGUSR1);
		test_thread.signal_info[SIGUSR1].code = SI_QUEUE;
		test_thread.signal_info[SIGUSR1].value = 0x51U;
		return;
	}
	abort();
}

void
exit1_signal(int signo)
{
	(void)signo;
	abort();
}

int
hal_task_user_context(struct hal_user_context *context)
{
	context->pc = 0x1000U;
	context->stack_pointer = (uintptr_t)(user_stack + sizeof(user_stack));
	context->return_value = -EINTR;
	return 0;
}

int
copyout(const void *source, uintptr_t destination, size_t size)
{
	uintptr_t first = (uintptr_t)user_stack;
	uintptr_t last = first + sizeof(user_stack);

	assert(irq_enabled != 0);
	assert(destination >= first);
	assert(destination <= last);
	assert(size <= last - destination);
	memcpy((void *)destination, source, size);
	return 0;
}

int
hal_task_signal_enter(uintptr_t handler, uintptr_t stack, int signo,
	uintptr_t siginfo, uintptr_t ucontext, uintptr_t restorer, uint32_t token)
{
	(void)stack;
	(void)siginfo;
	(void)ucontext;
	(void)restorer;
	assert(irq_enabled != 0);
	assert(token != 0);
	signal_enters++;
	entered_handler = handler;
	entered_signo = signo;
	return 0;
}

static void
reset_fixture(void)
{
	memset(&test_process, 0, sizeof(test_process));
	memset(&test_thread, 0, sizeof(test_thread));
	memset(&test_cred, 0, sizeof(test_cred));
	memset(user_stack, 0, sizeof(user_stack));
	test_process.pid = 2;
	test_process.state = PROCESS_RUNNING;
	test_process.threads = &test_thread;
	test_process.thread_count = 1;
	test_process.cred = &test_cred;
	test_cred.ruid = test_cred.euid = test_cred.suid = 1000;
	test_thread.proc = &test_process;
	test_thread.state = THREAD_RUNNING;
	signal_enters = 0;
	entered_handler = 0;
	entered_signo = 0;
	irq_enabled = 0;
	accounting_depth = 0;
	accounting_enters = 0;
	fake_ticks = 100;
	memset(sleep_deadlines, 0, sizeof(sleep_deadlines));
	sleep_calls = 0;
	stop_calls = 0;
	continue_calls = 0;
	timer_completion_calls = 0;
	timedwait_mode = TIMEDWAIT_NONE;
}

static void
deliver_on_user_return(void)
{
	unsigned before = accounting_enters;

	assert(irq_enabled == 0);
	assert(accounting_depth == 0);
	kernel_user_return_handler();
	assert(irq_enabled == 0);
	assert(accounting_depth == 0);
	assert(accounting_enters == before + 1U);
}

static void
test_kill_all_delivers_blocked_signal_to_self(void)
{
	struct signal_info info;
	int signo = 0;

	reset_fixture();
	test_thread.signal_mask = SIGNAL_BIT(SIGUSR1);
	assert(signal_kill(&test_process, -1, SIGUSR1) == 0);
	assert((test_process.signal_pending & SIGNAL_BIT(SIGUSR1)) != 0);
	assert(signal_timedwait(&test_thread, SIGNAL_BIT(SIGUSR1), 0, 0,
	    &info, &signo) == 0);
	assert(signo == SIGUSR1);
	assert(info.code == SI_USER);
	assert(info.pid == test_process.pid);
	assert(info.uid == test_cred.euid);
	assert((test_process.signal_pending & SIGNAL_BIT(SIGUSR1)) == 0);
}

static void
test_kill_minimum_selector_is_not_negated(void)
{
	reset_fixture();
	assert(signal_kill(&test_process, (pid_t)INT32_MIN, 0) == ESRCH);
	assert(test_process.signal_pending == 0);
}

static void
test_stop_selection_preserves_independent_interrupt(void)
{
	reset_fixture();
	timedwait_mode = TIMEDWAIT_STOP_BEFORE_RETURN;
	test_process.signal_actions[SIGUSR1].handler = 0x12345000U;
	test_thread.signal_pending = SIGNAL_BIT(SIGUSR1) |
	    SIGNAL_BIT(SIGTSTP);

	assert(signal_stop_before_return(&test_thread) ==
	    SIGNAL_STOP_RETURN_INTERRUPT);
	assert(stop_calls == 1);
	assert((test_thread.signal_pending & SIGNAL_BIT(SIGTSTP)) == 0);
	assert((test_thread.signal_pending & SIGNAL_BIT(SIGUSR1)) != 0);
}

static void
seed_restart_candidate(uint32_t number, uintptr_t base)
{
	unsigned index;

	test_thread.syscall_restart_number = number;
	for (index = 0; index < HAL_SYSCALL_ARGS; index++)
		test_thread.syscall_restart_args[index] = base + index;
	test_thread.syscall_restart_valid = 1;
}

static void
test_ignored_candidate_does_not_cross_user_return(void)
{
	const uint32_t old_number = 0x1234U;

	reset_fixture();
	seed_restart_candidate(old_number, 0x2000U);

	/* The syscall wait observed SIGUSR1, but another thread changed its
	 * disposition before this user-return boundary selected the signal. */
	test_process.signal_actions[SIGUSR1].handler = (uintptr_t)SIG_IGN;
	test_thread.signal_pending = SIGNAL_BIT(SIGUSR1);
	deliver_on_user_return();

	assert(test_thread.syscall_restart_valid == 0);
	assert(test_thread.signal_pending == 0);
	assert(test_thread.signal_depth == 0);
	assert(signal_enters == 0);

	/* A later, unrelated caught signal with SA_RESTART must not inherit the
	 * old candidate and arrange a redispatch when its handler returns. */
	test_process.signal_actions[SIGUSR2].handler = 0x34567000U;
	test_process.signal_actions[SIGUSR2].restorer = 0x45678000U;
	test_process.signal_actions[SIGUSR2].flags = SA_RESTART;
	test_thread.signal_pending = SIGNAL_BIT(SIGUSR2);
	deliver_on_user_return();

	assert(signal_enters == 1);
	assert(entered_handler == 0x34567000U);
	assert(entered_signo == SIGUSR2);
	assert(test_thread.signal_depth == 1);
	assert(test_thread.signal_levels[0].restart_on_return == 0);
	assert(test_thread.syscall_redispatch_valid == 0);
}

static void
test_same_boundary_restart_is_preserved(void)
{
	const uint32_t number = 0x5678U;
	unsigned index;

	reset_fixture();
	seed_restart_candidate(number, 0x6000U);
	test_process.signal_actions[SIGUSR2].handler = 0x789ab000U;
	test_process.signal_actions[SIGUSR2].restorer = 0x89abc000U;
	test_process.signal_actions[SIGUSR2].flags = SA_RESTART;
	test_thread.signal_pending = SIGNAL_BIT(SIGUSR2);
	deliver_on_user_return();

	assert(test_thread.syscall_restart_valid == 0);
	assert(test_thread.signal_depth == 1);
	assert(test_thread.signal_levels[0].restart_on_return == 1);
	assert(test_thread.signal_levels[0].restart_number == number);
	for (index = 0; index < HAL_SYSCALL_ARGS; index++)
		assert(test_thread.signal_levels[0].restart_args[index] ==
		    0x6000U + index);
}

static void
test_timedwait_termination_interrupts(void)
{
	struct signal_info info;
	int signo = 0;

	reset_fixture();
	timedwait_mode = TIMEDWAIT_TERMINATE;
	assert(signal_timedwait(&test_thread, SIGNAL_BIT(SIGUSR1), 200, 1,
	    &info, &signo) == EINTR);
	assert(sleep_calls == 1 && sleep_deadlines[0] == 200);
	assert(test_thread.signal_waiting == 0);
	assert(test_thread.signal_wait_set == 0);
}

static void
test_timedwait_stop_preserves_absolute_deadline(void)
{
	struct signal_info info;
	int signo = 0;

	reset_fixture();
	timedwait_mode = TIMEDWAIT_STOP;
	assert(signal_timedwait(&test_thread, SIGNAL_BIT(SIGUSR1), 200, 1,
	    &info, &signo) == 0);
	assert(signo == SIGUSR1 && info.value == 0x51U);
	assert(stop_calls == 1);
	assert(sleep_calls == 2);
	assert(sleep_deadlines[0] == 200 && sleep_deadlines[1] == 200);
	assert(test_thread.signal_waiting == 0);
	assert(test_thread.signal_wait_set == 0);
}

static void
test_ignored_generation_discards_all_instances(void)
{
	struct signal_action ignored, caught, previous;
	struct signal_info queued, delivered;
	int signo = 0;
	unsigned index;

	reset_fixture();
	memset(&ignored, 0, sizeof(ignored));
	memset(&caught, 0, sizeof(caught));
	memset(&queued, 0, sizeof(queued));
	ignored.handler = (uintptr_t)SIG_IGN;
	caught.handler = 0x12345000U;
	queued.code = SI_QUEUE;
	assert(signal_action_set(&test_process, SIGRTMIN, &ignored,
	    &previous) == 0);
	for (index = 0; index < SIGNAL_QUEUE_MAX + 8U; index++) {
		queued.value = index;
		assert(signal_send_process_info(&test_process, SIGRTMIN,
		    &queued) == 0);
	}
	assert(test_process.signal_queue_count == 0);
	assert((test_process.signal_pending & SIGNAL_BIT(SIGRTMIN)) == 0);
	assert(signal_send_thread_info(&test_thread, SIGRTMIN, &queued) == 0);
	assert((test_thread.signal_pending & SIGNAL_BIT(SIGRTMIN)) == 0);

	assert(signal_action_set(&test_process, SIGRTMIN, &caught,
	    &previous) == 0);
	assert(previous.handler == (uintptr_t)SIG_IGN);
	assert(test_process.signal_queue_count == 0);
	assert(test_process.signal_pending == 0 && test_thread.signal_pending == 0);
	queued.value = 0x99U;
	assert(signal_send_process_info(&test_process, SIGRTMIN, &queued) == 0);
	assert(test_process.signal_queue_count == 1);
	assert(signal_timedwait(&test_thread, SIGNAL_BIT(SIGRTMIN), 0, 0,
	    &delivered, &signo) == 0);
	assert(signo == SIGRTMIN && delivered.value == 0x99U);

	/* Installing SIG_IGN is itself the discard linearization point for both
	 * process- and thread-directed instances and the RT queue. */
	queued.value = 0xa1U;
	assert(signal_send_process_info(&test_process, SIGRTMIN, &queued) == 0);
	test_thread.signal_pending |= SIGNAL_BIT(SIGRTMIN);
	test_thread.signal_info[SIGRTMIN] = queued;
	assert(signal_action_set(&test_process, SIGRTMIN, &ignored, NULL) == 0);
	assert(test_process.signal_queue_count == 0);
	assert((test_process.signal_pending & SIGNAL_BIT(SIGRTMIN)) == 0);
	assert((test_thread.signal_pending & SIGNAL_BIT(SIGRTMIN)) == 0);
	assert(signal_action_set(&test_process, SIGRTMIN, &caught, NULL) == 0);
	assert(test_process.signal_pending == 0 && test_thread.signal_pending == 0);

	/* Default-ignore applies at generation time too. */
	assert(signal_send_process_info(&test_process, SIGWINCH, &queued) == 0);
	assert(signal_send_thread_info(&test_thread, SIGWINCH, &queued) == 0);
	assert((test_process.signal_pending & SIGNAL_BIT(SIGWINCH)) == 0);
	assert((test_thread.signal_pending & SIGNAL_BIT(SIGWINCH)) == 0);

	/* SIGCONT still performs its resume side effect even when ignored, after
	 * atomically discarding all pending stop-family signals. */
	test_process.stop_requested = 1;
	test_process.state = PROCESS_STOPPED;
	test_process.signal_pending = SIGNAL_BIT(SIGTSTP);
	test_thread.signal_pending = SIGNAL_BIT(SIGTTIN);
	assert(signal_send_thread_info(&test_thread, SIGCONT, &queued) == 0);
	assert(continue_calls == 1);
	assert(!test_process.stop_requested &&
	    test_process.state == PROCESS_RUNNING);
	assert((test_process.signal_pending & SIGNAL_BIT(SIGTSTP)) == 0);
	assert((test_thread.signal_pending & SIGNAL_BIT(SIGTTIN)) == 0);
}

static void
test_timer_notification_completion_handoff(void)
{
	struct signal_action caught, ignored;
	struct signal_info timer_info, delivered;
	int signo = 0;

	reset_fixture();
	memset(&caught, 0, sizeof(caught));
	memset(&ignored, 0, sizeof(ignored));
	memset(&timer_info, 0, sizeof(timer_info));
	caught.handler = 0x12345000U;
	ignored.handler = (uintptr_t)SIG_IGN;
	timer_info.code = SI_TIMER;
	timer_info.value = 0x77U;
	timer_info.timer_slot = 3;
	timer_info.timer_generation = 9;
	assert(signal_action_set(&test_process, SIGUSR1, &caught, NULL) == 0);
	assert(signal_send_process(&test_process, SIGUSR1) == 0);
	assert(signal_send_process_info(&test_process, SIGUSR1, &timer_info) == 0);
	/* SI_TIMER is queued even for a classic signal so its per-timer pending
	 * identity cannot be coalesced away. */
	assert(test_process.signal_queue_count == 1);
	assert(signal_timedwait(&test_thread, SIGNAL_BIT(SIGUSR1), 0, 0,
	    &delivered, &signo) == 0);
	assert(signo == SIGUSR1 && delivered.value == 0x77U);
	assert(timer_completion_calls == 1);
	assert((test_process.signal_pending & SIGNAL_BIT(SIGUSR1)) != 0);
	assert(signal_timedwait(&test_thread, SIGNAL_BIT(SIGUSR1), 0, 0,
	    &delivered, &signo) == 0);
	assert(signo == SIGUSR1 && delivered.code == SI_KERNEL);
	assert(timer_completion_calls == 1);

	assert(signal_send_process_info(&test_process, SIGUSR1, &timer_info) == 0);
	assert(signal_action_set(&test_process, SIGUSR1, &ignored, NULL) == 0);
	assert(test_process.signal_queue_count == 0);
	assert(timer_completion_calls == 2);

	/* A timer firing after SIG_IGN receives the same completion handoff without
	 * ever consuming an RT queue slot. */
	assert(signal_send_process_info(&test_process, SIGUSR1, &timer_info) == 0);
	assert(test_process.signal_queue_count == 0);
	assert(timer_completion_calls == 3);
}

int
main(void)
{
	test_ignored_candidate_does_not_cross_user_return();
	test_same_boundary_restart_is_preserved();
	test_kill_all_delivers_blocked_signal_to_self();
	test_kill_minimum_selector_is_not_negated();
	test_stop_selection_preserves_independent_interrupt();
	test_timedwait_termination_interrupts();
	test_timedwait_stop_preserves_absolute_deadline();
	test_ignored_generation_discards_all_instances();
	test_timer_notification_completion_handoff();
	puts("zedBSD signal restart/selector host tests: PASS");
	return 0;
}
