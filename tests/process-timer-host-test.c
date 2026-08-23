/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include <kern/clock.h>
#include <kern/process.h>
#include <kern/process-timer.h>
#include <kern/process-timer-notify.h>
#include <kern/signal.h>
#include <kern/test-checkpoint.h>

#include "process-timer-thread-host-stubs.h"

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_TIMER_COUNT 20U
#define CAPTURE_MAX 64U

struct process process0;

static uint64_t fake_ticks;
static struct signal_info captures[CAPTURE_MAX];
static unsigned capture_count;
static unsigned fail_count;
static unsigned process_refs;
static struct process *settime_owner;
static unsigned settime_snapshot_count;
static unsigned first_snapshot_reached;
static unsigned release_first_snapshot;
static struct process *create_owner;
static unsigned create_admitted;
static unsigned release_create;

struct settime_worker {
	struct process *owner;
	timer_t timer;
	struct itimerspec requested;
	struct itimerspec previous;
	int flags;
	int result;
	unsigned started;
	unsigned completed;
};

struct create_worker {
	struct process *owner;
	struct sigevent event;
	timer_t timer;
	int result;
	unsigned completed;
};

struct exit_worker {
	struct process *owner;
	unsigned started;
	unsigned completed;
};

struct gettime_worker {
	struct process *owner;
	timer_t timer;
	struct itimerspec current;
	int result;
	unsigned started;
};

void
__libc_assert_fail(const char *expression, const char *file, int line)
{
	fprintf(stderr, "assertion failed: %s (%s:%d)\n", expression, file, line);
	abort();
}

void
spin_init(struct spinlock *lock, enum lock_rank rank, const char *name)
{
	memset(lock, 0, sizeof(*lock));
	lock->rank = rank;
	lock->name = name;
}

unsigned long
spin_lock_irqsave(struct spinlock *lock)
{
	(void)lock;
	host_timer_lock();
	return 0;
}

void
spin_unlock_irqrestore(struct spinlock *lock, unsigned long irq)
{
	(void)lock;
	(void)irq;
	host_timer_unlock();
}

static void
checkpoint(enum kern_test_checkpoint_id id, void *object, void *argument)
{
	(void)argument;
	if (id == KERN_TEST_PROCESS_TIMER_CREATE_ADMITTED &&
	    object == create_owner) {
		host_checkpoint_lock();
		create_admitted = 1;
		host_checkpoint_broadcast();
		while (!release_create)
			host_checkpoint_wait();
		host_checkpoint_unlock();
		return;
	}
	if (id != KERN_TEST_PROCESS_TIMER_SETTIME_SNAPSHOT ||
	    object != settime_owner)
		return;
	host_checkpoint_lock();
	settime_snapshot_count++;
	if (settime_snapshot_count == 1) {
		first_snapshot_reached = 1;
		host_checkpoint_broadcast();
		while (!release_first_snapshot)
			host_checkpoint_wait();
	}
	host_checkpoint_unlock();
}

void
process_ref(struct process *process)
{
	assert(process != NULL && process != &process0);
	process_refs++;
}

void
process_release(struct process *process)
{
	assert(process != NULL);
	assert(process_refs != 0);
	process_refs--;
}

uint64_t
clock_ticks(void)
{
	return fake_ticks;
}

int
kern_clock_gettime(clockid_t clock, struct timespec *result)
{
	assert(clock == CLOCK_REALTIME);
	result->tv_sec = 0;
	result->tv_nsec = (long)(fake_ticks *
	    (KERN_NSEC_PER_SEC / KERN_CLOCK_HZ));
	return 0;
}

int
kern_timespec_validate(const struct timespec *value)
{
	return value == NULL || value->tv_sec < 0 || value->tv_nsec < 0 ||
	    value->tv_nsec >= (long)KERN_NSEC_PER_SEC ? EINVAL : 0;
}

int
kern_deadline_after(uint64_t now, uint64_t delta, uint64_t *deadline)
{
	if (deadline == NULL || delta > UINT64_MAX - now)
		return EOVERFLOW;
	*deadline = now + delta;
	return 0;
}

int
signal_send_process_info(struct process *owner, int signo,
	const struct signal_info *info)
{
	assert(owner != NULL && owner != &process0);
	assert(signo == SIGRTMIN);
	assert(info != NULL && info->code == SI_TIMER);
	assert(info->timer_generation != 0);
	if (fail_count != 0) {
		fail_count--;
		return EAGAIN;
	}
	assert(capture_count < CAPTURE_MAX);
	captures[capture_count++] = *info;
	return 0;
}

static void
arm_periodic(struct process *owner, timer_t *id, int value)
{
	struct sigevent event;
	struct itimerspec setting;

	memset(&event, 0, sizeof(event));
	memset(&setting, 0, sizeof(setting));
	event.sigev_notify = SIGEV_SIGNAL;
	event.sigev_signo = SIGRTMIN;
	event.sigev_value.sival_int = value;
	setting.it_value.tv_nsec = KERN_NSEC_PER_SEC / KERN_CLOCK_HZ;
	setting.it_interval = setting.it_value;
	assert(process_timer_create(owner, CLOCK_MONOTONIC, &event, id) == 0);
	assert(process_timer_settime(owner, *id, 0, &setting, NULL) == 0);
}

static struct itimerspec
tick_setting(unsigned value, unsigned interval)
{
	struct itimerspec setting;

	memset(&setting, 0, sizeof(setting));
	setting.it_value.tv_nsec =
	    (long)(value * (KERN_NSEC_PER_SEC / KERN_CLOCK_HZ));
	setting.it_interval.tv_nsec =
	    (long)(interval * (KERN_NSEC_PER_SEC / KERN_CLOCK_HZ));
	return setting;
}

static void
complete_captures(struct process *owner, unsigned first, unsigned last)
{
	unsigned index;

	for (index = first; index < last; index++)
		process_timer_notification_complete(owner,
		    captures[index].timer_slot, captures[index].timer_generation);
}

static void
test_fair_scan_and_single_pending_notification(void)
{
	struct process owner;
	timer_t timers[TEST_TIMER_COUNT];
	unsigned index;

	memset(&owner, 0, sizeof(owner));
	owner.state = PROCESS_RUNNING;
	fake_ticks = 0;
	capture_count = 0;
	for (index = 0; index < TEST_TIMER_COUNT; index++)
		arm_periodic(&owner, &timers[index], (int)index + 1);
	assert(process_refs == TEST_TIMER_COUNT);

	fake_ticks = 1;
	process_timer_tick(fake_ticks);
	/* More than the 16-entry stack batch must still be evaluated this tick. */
	assert(capture_count == TEST_TIMER_COUNT);
	for (index = 0; index < TEST_TIMER_COUNT; index++)
		assert(captures[index].value >= 1 &&
		    captures[index].value <= TEST_TIMER_COUNT);

	/* Four later expirations occur while each timer's first notification is
	 * blocked/unconsumed.  They accumulate as overruns without adding RT queue
	 * entries for the same timer. */
	fake_ticks = 5;
	process_timer_tick(fake_ticks);
	assert(capture_count == TEST_TIMER_COUNT);
	complete_captures(&owner, 0, TEST_TIMER_COUNT);
	for (index = 0; index < TEST_TIMER_COUNT; index++) {
		int overrun = -1;
		assert(process_timer_getoverrun(&owner, timers[index], &overrun) == 0);
		assert(overrun == 4);
	}

	fake_ticks = 6;
	process_timer_tick(fake_ticks);
	assert(capture_count == TEST_TIMER_COUNT * 2U);
	complete_captures(&owner, TEST_TIMER_COUNT, TEST_TIMER_COUNT * 2U);
	for (index = 0; index < TEST_TIMER_COUNT; index++)
		assert(process_timer_delete(&owner, timers[index]) == 0);
	assert(process_refs == 0);
}

static void
test_queue_failure_retries_without_losing_expiration(void)
{
	struct process owner;
	timer_t timer;
	int overrun = -1;

	memset(&owner, 0, sizeof(owner));
	owner.state = PROCESS_RUNNING;
	fake_ticks = 0;
	capture_count = 0;
	fail_count = 1;
	arm_periodic(&owner, &timer, 77);
	fake_ticks = 1;
	process_timer_tick(fake_ticks);
	assert(capture_count == 0);
	fake_ticks = 2;
	process_timer_tick(fake_ticks);
	assert(capture_count == 1 && captures[0].value == 77);
	process_timer_notification_complete(&owner, captures[0].timer_slot,
	    captures[0].timer_generation);
	assert(process_timer_getoverrun(&owner, timer, &overrun) == 0);
	assert(overrun == 1);
	assert(process_timer_delete(&owner, timer) == 0);
	assert(process_refs == 0);
}

static void
test_settime_failure_is_transactional(void)
{
	struct process owner;
	struct sigevent event;
	struct itimerspec initial, before, after, bad, previous, sentinel;
	timer_t timer;

	memset(&owner, 0, sizeof(owner));
	owner.state = PROCESS_RUNNING;
	memset(&event, 0, sizeof(event));
	event.sigev_notify = SIGEV_NONE;
	fake_ticks = 0;
	assert(process_timer_create(&owner, CLOCK_REALTIME, &event, &timer) == 0);
	initial = tick_setting(5, 2);
	assert(process_timer_settime(&owner, timer, 0, &initial, NULL) == 0);
	assert(process_timer_gettime(&owner, timer, &before) == 0);

	memset(&bad, 0, sizeof(bad));
	bad.it_value.tv_sec = 1;
	bad.it_interval.tv_sec =
	    (time_t)(UINT64_MAX / KERN_NSEC_PER_SEC + 1U);
	memset(&previous, 0x5a, sizeof(previous));
	sentinel = previous;
	assert(process_timer_settime(&owner, timer, TIMER_ABSTIME, &bad,
	    &previous) == EOVERFLOW);
	assert(memcmp(&previous, &sentinel, sizeof(previous)) == 0);
	assert(process_timer_gettime(&owner, timer, &after) == 0);
	assert(before.it_value.tv_sec == after.it_value.tv_sec &&
	    before.it_value.tv_nsec == after.it_value.tv_nsec);
	assert(before.it_interval.tv_sec == after.it_interval.tv_sec &&
	    before.it_interval.tv_nsec == after.it_interval.tv_nsec);
	assert(process_timer_delete(&owner, timer) == 0);
	assert(process_refs == 0);
}

static int
settime_worker(void *argument)
{
	struct settime_worker *worker = argument;

	host_checkpoint_lock();
	worker->started = 1;
	host_checkpoint_broadcast();
	host_checkpoint_unlock();
	worker->result = process_timer_settime(worker->owner, worker->timer,
	    worker->flags, &worker->requested, &worker->previous);
	host_checkpoint_lock();
	worker->completed = 1;
	host_checkpoint_broadcast();
	host_checkpoint_unlock();
	return 0;
}

static void
test_concurrent_settime_old_value_is_linearized(void)
{
	struct process owner;
	struct sigevent event;
	struct itimerspec initial, after;
	struct settime_worker first, second;
	timer_t timer;
	struct host_thread_handle first_thread, second_thread;

	memset(&owner, 0, sizeof(owner));
	owner.state = PROCESS_RUNNING;
	memset(&event, 0, sizeof(event));
	memset(&first, 0, sizeof(first));
	memset(&second, 0, sizeof(second));
	event.sigev_notify = SIGEV_NONE;
	fake_ticks = 0;
	assert(process_timer_create(&owner, CLOCK_MONOTONIC, &event, &timer) == 0);
	initial = tick_setting(2, 1);
	assert(process_timer_settime(&owner, timer, 0, &initial, NULL) == 0);
	first.owner = second.owner = &owner;
	first.timer = second.timer = timer;
	first.requested = tick_setting(3, 1);
	second.requested = tick_setting(4, 1);
	settime_owner = &owner;
	settime_snapshot_count = 0;
	first_snapshot_reached = 0;
	release_first_snapshot = 0;
	kern_test_checkpoint_set(checkpoint, NULL);

	assert(host_thread_create(&first_thread, settime_worker, &first) == 0);
	host_checkpoint_lock();
	while (!first_snapshot_reached)
		host_checkpoint_wait();
	assert(host_thread_create(&second_thread, settime_worker, &second) == 0);
	while (!second.started)
		host_checkpoint_wait();
	/* The first worker is paused after its old-value snapshot while holding the
	 * production timer lock; the second cannot sample its clock, snapshot, or
	 * commit yet.  Advance time while it is blocked to make stale pre-lock clock
	 * sampling observable. */
	assert(!first.completed && !second.completed && settime_snapshot_count == 1);
	fake_ticks = 1;
	release_first_snapshot = 1;
	host_checkpoint_broadcast();
	host_checkpoint_unlock();
	assert(host_thread_join(&first_thread) == 0);
	assert(host_thread_join(&second_thread) == 0);
	kern_test_checkpoint_set(NULL, NULL);
	assert(first.result == 0 && second.result == 0);
	assert(first.previous.it_value.tv_sec == 0 &&
	    first.previous.it_value.tv_nsec ==
	    (long)(2U * (KERN_NSEC_PER_SEC / KERN_CLOCK_HZ)));
	assert(second.previous.it_value.tv_sec == 0 &&
	    second.previous.it_value.tv_nsec ==
	    (long)(2U * (KERN_NSEC_PER_SEC / KERN_CLOCK_HZ)));
	assert(process_timer_gettime(&owner, timer, &after) == 0);
	assert(after.it_value.tv_sec == 0 && after.it_value.tv_nsec ==
	    (long)(4U * (KERN_NSEC_PER_SEC / KERN_CLOCK_HZ)));
	assert(settime_snapshot_count == 2);
	assert(process_timer_delete(&owner, timer) == 0);
	assert(process_refs == 0);
}

static int
timer_gettime_worker(void *argument)
{
	struct gettime_worker *worker = argument;

	host_checkpoint_lock();
	worker->started = 1;
	host_checkpoint_broadcast();
	host_checkpoint_unlock();
	worker->result = process_timer_gettime(worker->owner, worker->timer,
	    &worker->current);
	return 0;
}

static void
test_gettime_clock_snapshot_is_linearized(void)
{
	struct process owner;
	struct sigevent event;
	struct itimerspec initial;
	struct settime_worker setter;
	struct gettime_worker getter;
	struct host_thread_handle setter_thread, getter_thread;
	timer_t timer;

	memset(&owner, 0, sizeof(owner));
	memset(&event, 0, sizeof(event));
	memset(&setter, 0, sizeof(setter));
	memset(&getter, 0, sizeof(getter));
	owner.state = PROCESS_RUNNING;
	event.sigev_notify = SIGEV_NONE;
	fake_ticks = 0;
	assert(process_timer_create(&owner, CLOCK_REALTIME, &event, &timer) == 0);
	initial = tick_setting(5, 0);
	assert(process_timer_settime(&owner, timer, TIMER_ABSTIME, &initial,
	    NULL) == 0);
	setter.owner = &owner;
	setter.timer = timer;
	setter.requested = tick_setting(3, 0);
	setter.flags = TIMER_ABSTIME;
	getter.owner = &owner;
	getter.timer = timer;
	settime_owner = &owner;
	settime_snapshot_count = 0;
	first_snapshot_reached = 0;
	release_first_snapshot = 0;
	kern_test_checkpoint_set(checkpoint, NULL);

	assert(host_thread_create(&setter_thread, settime_worker, &setter) == 0);
	host_checkpoint_lock();
	while (!first_snapshot_reached)
		host_checkpoint_wait();
	assert(host_thread_create(&getter_thread, timer_gettime_worker,
	    &getter) == 0);
	while (!getter.started)
		host_checkpoint_wait();
	/* gettime is blocked behind the setter.  It must sample after acquiring the
	 * registry lock, not combine tick zero with the setting committed at tick
	 * one. */
	fake_ticks = 1;
	release_first_snapshot = 1;
	host_checkpoint_broadcast();
	host_checkpoint_unlock();
	assert(host_thread_join(&setter_thread) == 0);
	assert(host_thread_join(&getter_thread) == 0);
	kern_test_checkpoint_set(NULL, NULL);
	assert(setter.result == 0 && getter.result == 0);
	assert(getter.current.it_value.tv_sec == 0 &&
	    getter.current.it_value.tv_nsec ==
	    (long)(2U * (KERN_NSEC_PER_SEC / KERN_CLOCK_HZ)));
	assert(process_timer_delete(&owner, timer) == 0);
	assert(process_refs == 0);
}

static int
timer_create_worker(void *argument)
{
	struct create_worker *worker = argument;

	worker->result = process_timer_create(worker->owner, CLOCK_MONOTONIC,
	    &worker->event, &worker->timer);
	host_checkpoint_lock();
	worker->completed = 1;
	host_checkpoint_broadcast();
	host_checkpoint_unlock();
	return 0;
}

static int
timer_exit_worker(void *argument)
{
	struct exit_worker *worker = argument;
	unsigned long irq;

	host_checkpoint_lock();
	worker->started = 1;
	host_checkpoint_broadcast();
	host_checkpoint_unlock();
	irq = spin_lock_irqsave(&worker->owner->lock);
	worker->owner->state = PROCESS_EXITING;
	spin_unlock_irqrestore(&worker->owner->lock, irq);
	process_timer_cleanup(worker->owner);
	irq = spin_lock_irqsave(&worker->owner->lock);
	worker->owner->state = PROCESS_ZOMBIE;
	spin_unlock_irqrestore(&worker->owner->lock, irq);
	host_checkpoint_lock();
	worker->completed = 1;
	host_checkpoint_broadcast();
	host_checkpoint_unlock();
	return 0;
}

static void
test_timer_create_is_ordered_with_process_exit(void)
{
	struct process owner;
	struct create_worker creator;
	struct exit_worker exiter;
	struct host_thread_handle create_thread, exit_thread;
	struct itimerspec value;
	struct sigevent event;
	timer_t rejected;

	memset(&owner, 0, sizeof(owner));
	memset(&creator, 0, sizeof(creator));
	memset(&exiter, 0, sizeof(exiter));
	owner.state = PROCESS_RUNNING;
	creator.owner = exiter.owner = &owner;
	creator.event.sigev_notify = SIGEV_NONE;
	create_owner = &owner;
	create_admitted = 0;
	release_create = 0;
	kern_test_checkpoint_set(checkpoint, NULL);

	assert(host_thread_create(&create_thread, timer_create_worker,
	    &creator) == 0);
	host_checkpoint_lock();
	while (!create_admitted)
		host_checkpoint_wait();
	assert(host_thread_create(&exit_thread, timer_exit_worker, &exiter) == 0);
	while (!exiter.started)
		host_checkpoint_wait();
	/* The exit transition is blocked behind the admitted create's process lock.
	 * Thus cleanup cannot miss a timer which later becomes visible. */
	assert(!creator.completed && !exiter.completed);
	release_create = 1;
	host_checkpoint_broadcast();
	host_checkpoint_unlock();
	assert(host_thread_join(&create_thread) == 0);
	assert(host_thread_join(&exit_thread) == 0);
	kern_test_checkpoint_set(NULL, NULL);
	assert(creator.result == 0 && owner.state == PROCESS_ZOMBIE);
	assert(process_refs == 0);
	assert(process_timer_gettime(&owner, creator.timer, &value) == EINVAL);

	/* The opposite linearization order rejects admission after EXITING and a
	 * completed cleanup, rather than leaving a new slot owned by the zombie. */
	memset(&event, 0, sizeof(event));
	event.sigev_notify = SIGEV_NONE;
	assert(process_timer_create(&owner, CLOCK_MONOTONIC, &event,
	    &rejected) == ESRCH);
	assert(process_refs == 0);
}

static void
test_exit_cleanup_discards_pending_notification(void)
{
	struct process owner;
	timer_t timer;
	unsigned long irq;

	memset(&owner, 0, sizeof(owner));
	owner.state = PROCESS_RUNNING;
	fake_ticks = 0;
	capture_count = 0;
	fail_count = 0;
	arm_periodic(&owner, &timer, 91);
	fake_ticks = 1;
	process_timer_tick(fake_ticks);
	assert(capture_count == 1 && process_refs == 1);
	irq = spin_lock_irqsave(&owner.lock);
	owner.state = PROCESS_EXITING;
	spin_unlock_irqrestore(&owner.lock, irq);
	process_timer_cleanup(&owner);
	irq = spin_lock_irqsave(&owner.lock);
	owner.state = PROCESS_ZOMBIE;
	spin_unlock_irqrestore(&owner.lock, irq);
	assert(process_refs == 0);
	assert(process_timer_getoverrun(&owner, timer, &(int){ 0 }) == EINVAL);
	/* Completion of a signal queued before exit cannot revive or mutate the
	 * generation-cleared zombie slot. */
	process_timer_notification_complete(&owner, captures[0].timer_slot,
	    captures[0].timer_generation);
	assert(process_refs == 0);
}

int
main(void)
{
	assert(host_sync_init() == 0);
	process_timer_init();
	test_fair_scan_and_single_pending_notification();
	test_queue_failure_retries_without_losing_expiration();
	test_settime_failure_is_transactional();
	test_concurrent_settime_old_value_is_linearized();
	test_gettime_clock_snapshot_is_linearized();
	test_timer_create_is_ordered_with_process_exit();
	test_exit_cleanup_discards_pending_notification();
	puts("zedBSD process timer fairness/pending host tests: PASS");
	return 0;
}
