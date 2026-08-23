/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

/*
 * Deterministically exercise the kernel-owned state retained while a stopped
 * syscall is transparently redispatched.  No HAL restart policy is involved.
 */
#include <kern/process.h>
#include <kern/syscall.h>
#include <kern/thread.h>
#include <kern/uaccess.h>
#include <zedbsd/thread.h>

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define SIGNAL_BIT(signo) \
	((sigset_t)1ULL << ((unsigned)(signo) - 1U))

static struct process test_process;
static struct thread test_thread;
static struct thread test_second_thread;
static struct thread test_target_thread;
static struct thread *test_current_thread = &test_thread;
static uint64_t test_ticks;
static int test_pin_error;
static int test_copyout_error;
static int test_waitq_error;
static int test_waitq_is_stop;
static int test_waitq_is_cancel;
static int test_copyout_succeeded;
static int test_thread_wait_calls;
static int test_interrupt_calls;

int syscall_test_poll_mask_cycle(uint64_t, int, unsigned);
int syscall_test_thread_join_claim(struct thread *, tid_t, unsigned);
void syscall_test_thread_join_release(struct thread *, tid_t);
intptr_t syscall_test_thread_join_call(const uintptr_t [6]);
intptr_t syscall_test_thread_cancel_call(const uintptr_t [6]);

void
__libc_assert_fail(const char *expression, const char *file, int line)
{
	(void)expression;
	(void)file;
	(void)line;
	__builtin_trap();
}

struct thread *
thread_current(void)
{
	return test_current_thread;
}

struct thread *
thread_find_ref(tid_t tid)
{
	return tid == test_target_thread.tid ? &test_target_thread : NULL;
}

void
thread_release(struct thread *thread)
{
	assert(thread == &test_target_thread);
}

void
sched_interrupt(struct thread *thread)
{
	assert(thread == &test_target_thread);
	test_interrupt_calls++;
}

int
uaccess_pin(uintptr_t address, size_t length, uint32_t prot,
	struct uaccess_pin *pin)
{
	(void)address;
	(void)length;
	assert(prot == HAL_SPACE_WRITE && pin != NULL);
	return test_pin_error;
}

void
uaccess_unpin(struct uaccess_pin *pin)
{
	assert(pin != NULL);
}

int
copyout_pinned(const struct uaccess_pin *pin, size_t offset,
	const void *source, size_t length)
{
	(void)pin;
	assert(offset == 0 && source != NULL && length == sizeof(uintptr_t));
	if (test_copyout_error != 0)
		return test_copyout_error;
	test_copyout_succeeded = 1;
	return 0;
}

uint64_t
waitq_sequence(const struct wait_queue *queue)
{
	(void)queue;
	return 1;
}

int
waitq_sleep(struct wait_queue *queue, struct spinlock *lock,
	uint64_t sequence, uint64_t deadline, unsigned flags)
{
	(void)queue;
	(void)lock;
	assert(sequence == 1 && deadline == 0 &&
	    (flags == WAITQ_INTERRUPTIBLE ||
	    flags == (WAITQ_INTERRUPTIBLE | WAITQ_CANCELABLE)));
	if (test_waitq_is_stop)
		test_current_thread->stop_interrupted = 1;
	if (test_waitq_is_cancel)
		test_current_thread->cancel_pending = 1;
	return test_waitq_error;
}

int
thread_wait(struct thread *thread, int *status)
{
	(void)status;
	assert(thread == &test_target_thread);
	assert(test_copyout_succeeded);
	test_thread_wait_calls++;
	thread->state = THREAD_DEAD;
	return 0;
}

uint64_t
sched_ticks(void)
{
	return test_ticks;
}

int
kern_deadline_after(uint64_t now, uint64_t delta, uint64_t *deadline)
{
	if (deadline == NULL)
		return EINVAL;
	if (UINT64_MAX - now < delta)
		return EOVERFLOW;
	*deadline = now + delta;
	return 0;
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

int
copyin(uintptr_t source, void *destination, size_t size)
{
	memcpy(destination, (const void *)source, size);
	return 0;
}

static void
reset_fixture(void)
{
	memset(&test_process, 0, sizeof(test_process));
	memset(&test_thread, 0, sizeof(test_thread));
	memset(&test_second_thread, 0, sizeof(test_second_thread));
	memset(&test_target_thread, 0, sizeof(test_target_thread));
	test_thread.proc = &test_process;
	test_thread.tid = 101;
	test_second_thread.proc = &test_process;
	test_second_thread.tid = 202;
	test_target_thread.proc = &test_process;
	test_target_thread.tid = 303;
	test_target_thread.state = THREAD_RUNNABLE;
	test_current_thread = &test_thread;
	test_ticks = 100;
	test_pin_error = 0;
	test_copyout_error = 0;
	test_waitq_error = 0;
	test_waitq_is_stop = 0;
	test_copyout_succeeded = 0;
	test_thread_wait_calls = 0;
	test_interrupt_calls = 0;
	syscall_restart_state_begin(&test_thread);
}

static void
test_deadline_and_mask_survive_multiple_stops(void)
{
	const sigset_t original = SIGNAL_BIT(SIGUSR2);
	const sigset_t temporary = SIGNAL_BIT(SIGUSR1);
	uint64_t deadline;

	reset_fixture();
	assert(syscall_restart_deadline_after(20, &deadline) == 0);
	assert(deadline == 120);

	test_thread.signal_mask = original;
	test_ticks = 107;
	assert(syscall_test_poll_mask_cycle(temporary, EINTR, 1) == 0);
	assert(test_thread.signal_mask == original);
	assert(test_thread.signal_suspended == 0);
	syscall_restart_prepare_stop(&test_thread);
	assert(test_thread.signal_mask == original);
	assert(test_thread.signal_suspended == 0);
	assert(test_thread.syscall_stop_redispatch != 0);
	assert(syscall_restart_deadline_after(20, &deadline) == 0);
	assert(deadline == 120);

	/* Model a second ppoll()/pselect() wait unwinding for STOP after the
	 * redispatched body installed the same temporary mask again. */
	test_ticks = 114;
	assert(syscall_test_poll_mask_cycle(temporary, EINTR, 1) == 0);
	assert(test_thread.signal_mask == original);
	assert(test_thread.signal_suspended == 0);
	syscall_restart_prepare_stop(&test_thread);
	assert(test_thread.signal_mask == original);
	assert(test_thread.signal_suspended == 0);
	assert(syscall_restart_deadline_after(20, &deadline) == 0);
	assert(deadline == 120);

	/* Time elapsed while stopped counts against the original finite wait. */
	test_ticks = 130;
	assert(syscall_restart_deadline_after(20, &deadline) == 0);
	assert(deadline == 120);
	syscall_restart_state_finish(&test_thread);
	assert(test_thread.syscall_wait_deadline_valid == 0);
}

static void
test_caught_signal_keeps_deferred_restore(void)
{
	const sigset_t original = SIGNAL_BIT(SIGUSR2);
	const sigset_t temporary = SIGNAL_BIT(SIGUSR1);

	reset_fixture();
	test_thread.signal_mask = original;
	assert(syscall_test_poll_mask_cycle(temporary, EINTR, 0) == 0);
	/* Normal EINTR crosses the user-return hook, so finishing the syscall must
	 * not preempt the existing deferred-mask restoration protocol. */
	syscall_restart_state_finish(&test_thread);
	assert(test_thread.signal_mask == temporary);
	assert(test_thread.signal_suspend_mask == original);
	assert(test_thread.signal_suspended != 0);
}

static void
test_stop_selected_at_return_cancels_deferred_restore(void)
{
	const sigset_t original = SIGNAL_BIT(SIGUSR2);
	const sigset_t temporary = SIGNAL_BIT(SIGUSR1);

	reset_fixture();
	test_thread.signal_mask = original;
	/* The wait first observed an ordinary caught signal, so ppoll/pselect
	 * deferred restoration.  A default stop can still be selected by the
	 * dispatcher before that signal reaches user mode. */
	assert(syscall_test_poll_mask_cycle(temporary, EINTR, 0) == 0);
	assert(test_thread.signal_suspended != 0);
	syscall_restart_prepare_stop(&test_thread);
	assert(test_thread.signal_mask == original);
	assert(test_thread.signal_suspended == 0);
}

static void
test_join_claim_survives_stop_for_only_its_owner(void)
{
	struct thread target;

	memset(&target, 0, sizeof(target));
	assert(syscall_test_thread_join_claim(&target, 101, 0) == 0);
	assert(target.join_claimed && target.join_owner_tid == 101);
	/* Neither another joiner nor a fresh syscall by the same thread may steal
	 * an in-flight claim. */
	assert(syscall_test_thread_join_claim(&target, 202, 0) == EINVAL);
	assert(syscall_test_thread_join_claim(&target, 101, 0) == EINVAL);
	/* Transparent STOP redispatch is the sole same-owner re-entry. */
	assert(syscall_test_thread_join_claim(&target, 202, 1) == EINVAL);
	assert(syscall_test_thread_join_claim(&target, 101, 1) == 0);
	syscall_test_thread_join_release(&target, 101);
	assert(!target.join_claimed && target.join_owner_tid == 0);
	assert(syscall_test_thread_join_claim(&target, 202, 0) == 0);
	syscall_test_thread_join_release(&target, 202);
}

static void
test_join_stop_redispatch_and_copyout_commit_order(void)
{
	uintptr_t arguments[6] = { 303, 1, 0, 0, 0, 0 };

	reset_fixture();
	test_waitq_error = EINTR;
	test_waitq_is_stop = 1;
	assert(syscall_test_thread_join_call(arguments) == -EINTR);
	assert(test_target_thread.join_claimed);
	assert(test_target_thread.join_owner_tid == test_thread.tid);
	assert(test_thread_wait_calls == 0);

	/* A second joiner cannot consume the target while the stopped syscall's
	 * owner is waiting for transparent CONT redispatch. */
	test_current_thread = &test_second_thread;
	assert(syscall_test_thread_join_call(arguments) == -EINVAL);
	assert(test_target_thread.join_owner_tid == test_thread.tid);

	test_current_thread = &test_thread;
	test_thread.syscall_stop_redispatch = 1;
	test_thread.stop_interrupted = 0;
	test_target_thread.state = THREAD_ZOMBIE;
	test_target_thread.user_exit_value = 0x1234;
	test_waitq_error = 0;
	test_waitq_is_stop = 0;
	assert(syscall_test_thread_join_call(arguments) == 0);
	assert(test_copyout_succeeded);
	assert(test_thread_wait_calls == 1);
	assert(test_target_thread.state == THREAD_DEAD);
}

static void
test_join_pin_and_copyout_failure_do_not_consume(void)
{
	uintptr_t arguments[6] = { 303, 1, 0, 0, 0, 0 };

	reset_fixture();
	test_target_thread.state = THREAD_ZOMBIE;
	test_pin_error = EFAULT;
	assert(syscall_test_thread_join_call(arguments) == -EFAULT);
	assert(!test_target_thread.join_claimed);
	assert(test_target_thread.state == THREAD_ZOMBIE);
	assert(test_thread_wait_calls == 0);

	test_pin_error = 0;
	test_copyout_error = EFAULT;
	assert(syscall_test_thread_join_call(arguments) == -EFAULT);
	assert(!test_target_thread.join_claimed);
	assert(test_target_thread.state == THREAD_ZOMBIE);
	assert(test_thread_wait_calls == 0);
}

static void
test_join_cancellation_releases_claim_before_commit(void)
{
	uintptr_t arguments[6] = {
		303, 1, ZEDBSD_THREAD_JOIN_CANCELABLE, 0, 0, 0
	};

	reset_fixture();
	test_thread.cancel_pending = 1;
	assert(syscall_test_thread_join_call(arguments) == -EINTR);
	assert(!test_target_thread.join_claimed);
	assert(test_thread_wait_calls == 0 && !test_copyout_succeeded);

	/* Model a request published after wait registration.  The cancel-aware
	 * wait returns EINTR and the join target remains available to another
	 * joiner. */
	reset_fixture();
	test_waitq_error = EINTR;
	test_waitq_is_cancel = 1;
	assert(syscall_test_thread_join_call(arguments) == -EINTR);
	assert(!test_target_thread.join_claimed);
	assert(test_thread_wait_calls == 0 && !test_copyout_succeeded);
}

static void
test_cancel_request_publishes_before_retained_interrupt(void)
{
	uintptr_t arguments[6] = {
		303, ZEDBSD_THREAD_CANCEL_REQUEST, 0, 0, 0, 0
	};

	reset_fixture();
	assert(syscall_test_thread_cancel_call(arguments) == 0);
	assert(test_target_thread.cancel_pending == 1);
	assert(test_interrupt_calls == 1);
}

int
main(void)
{
	test_deadline_and_mask_survive_multiple_stops();
	test_caught_signal_keeps_deferred_restore();
	test_stop_selected_at_return_cancels_deferred_restore();
	test_join_claim_survives_stop_for_only_its_owner();
	test_join_stop_redispatch_and_copyout_commit_order();
	test_join_pin_and_copyout_failure_do_not_consume();
	test_join_cancellation_releases_claim_before_commit();
	test_cancel_request_publishes_before_retained_interrupt();
	puts("zedBSD syscall STOP redispatch host tests: PASS");
	return 0;
}
