/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * POSIX per-process interval timers.
 *
 * Timers live in a fixed table guarded by one lock; an identifier encodes
 * the slot and a generation so that a stale identifier is refused.  A
 * timer counts in clock ticks, or in nanoseconds of real time when armed
 * absolutely on CLOCK_REALTIME.  Expiry is detected on the clock tick,
 * which batches signal notifications and tracks overruns until the
 * signal is delivered.
 */

#include "kern/process-timer.h"

#include "kern/clock.h"
#include "kern/lock.h"
#include "kern/process.h"
#include "kern/signal.h"
#include "kern/test-checkpoint.h"

#include <errno.h>
#include <limits.h>
#include <string.h>

#define PROCESS_TIMER_MAX 128U
#define TIMER_SLOT_BITS 8U
#define TIMER_SLOT_MASK ((1U << TIMER_SLOT_BITS) - 1U)

struct process_timer {
	struct process *owner;
	uint32_t generation;
	clockid_t clock;
	struct sigevent event;
	uint64_t expiry;
	uint64_t interval;
	int overrun;
	int pending_overrun;
	unsigned armed;
	unsigned realtime_absolute;
	unsigned notification_pending;
	unsigned notification_retry;
};

struct timer_notification {
	struct process *owner;
	struct signal_info info;
	int signo;
	unsigned slot;
	uint32_t generation;
	int overrun;
};

static struct process_timer process_timers[PROCESS_TIMER_MAX];
static struct spinlock process_timer_lock;

static int overrun_add(int current, uint64_t additional);
static int timespec_to_units(const struct timespec *value, uint64_t units_per_second, uint64_t *result);
static void units_to_timespec(uint64_t value, uint64_t units_per_second, struct timespec *result);
static timer_t timer_id(unsigned slot, uint32_t generation);
static int timer_lookup_locked(struct process *owner, timer_t id, unsigned *slotp);
static int realtime_units(uint64_t *result);
static int timer_remaining_locked(const struct process_timer *timer, uint64_t now_ticks, uint64_t now_realtime, uint64_t *remaining);
static int timer_clock_snapshot_locked(const struct process_timer *timer, uint64_t *now_ticks, uint64_t *now_realtime);

/*
 * Initializes the empty timer table.
 */
void
process_timer_init(
	void)
{
	memset(process_timers, 0, sizeof(process_timers));
	spin_init(&process_timer_lock, LOCK_RANK_PROCESS_TREE, "process timers");
}

/*
 * Creates a disarmed timer for a process.
 *
 * Without a requested event the timer sends SIGALRM.  Only signal and no
 * notification are supported.
 */
int
process_timer_create(
	struct process *owner,
	clockid_t clock,
	const struct sigevent *requested,
	timer_t *result)
{
	struct sigevent event;
	unsigned long irq;
	unsigned long process_irq;
	unsigned slot;
	uint32_t generation;

	/* Rejects the kernel process, a missing result, or an unknown clock. */
	if (owner == NULL ||
	    owner == &process0 ||
	    result == NULL ||
	    (clock != CLOCK_MONOTONIC && clock != CLOCK_REALTIME))
		return EINVAL;

	/* Takes the requested event, defaulting to SIGALRM. */
	memset(&event, 0, sizeof(event));
	if (requested != NULL) {
		event = *requested;
	} else {
		event.sigev_notify = SIGEV_SIGNAL;
		event.sigev_signo = SIGALRM;
	}
	if (event.sigev_notify != SIGEV_NONE &&
	    event.sigev_notify != SIGEV_SIGNAL)
		return EOPNOTSUPP;
	if (event.sigev_notify == SIGEV_SIGNAL &&
	    (event.sigev_signo <= 0 || event.sigev_signo >= NSIG))
		return EINVAL;

	/*
	 * The timer registry precedes process->lock in the global lock
	 * order.  Hold both through admission and publication so
	 * PROCESS_EXITING publication is ordered either before this create
	 * (which is rejected) or before cleanup (which then observes and
	 * removes the newly published timer).
	 */
	irq = spin_lock_irqsave(&process_timer_lock);
	process_irq = spin_lock_irqsave(&owner->lock);
	if (owner->state != PROCESS_RUNNING && owner->state != PROCESS_STOPPED) {
		spin_unlock_irqrestore(&owner->lock, process_irq);
		spin_unlock_irqrestore(&process_timer_lock, irq);
		return ESRCH;
	}

	/* Takes the first free slot. */
	for (slot = 0; slot < PROCESS_TIMER_MAX; slot++) {
		if (process_timers[slot].owner == NULL)
			break;
	}
	if (slot == PROCESS_TIMER_MAX) {
		spin_unlock_irqrestore(&owner->lock, process_irq);
		spin_unlock_irqrestore(&process_timer_lock, irq);
		return EAGAIN;
	}
	KERN_TEST_CHECKPOINT(KERN_TEST_PROCESS_TIMER_CREATE_ADMITTED, owner);

	/* Clears the slot under a new non-zero generation. */
	generation = process_timers[slot].generation + 1U;
	if (generation == 0)
		generation = 1;
	memset(&process_timers[slot], 0, sizeof(process_timers[slot]));
	process_timers[slot].generation = generation;

	/* Publishes the timer, which holds a reference on its owner. */
	process_timers[slot].owner = owner;
	process_timers[slot].clock = clock;
	process_timers[slot].event = event;
	process_ref(owner);
	*result = timer_id(slot, process_timers[slot].generation);
	spin_unlock_irqrestore(&owner->lock, process_irq);
	spin_unlock_irqrestore(&process_timer_lock, irq);

	/* Reports the created timer. */
	return 0;
}

/*
 * Deletes a timer of a process.
 */
int
process_timer_delete(
	struct process *owner,
	timer_t id)
{
	struct process *release;
	unsigned long irq;
	unsigned slot;
	int error;

	/* Frees the slot under the lock, keeping the owner to release after. */
	irq = spin_lock_irqsave(&process_timer_lock);
	error = timer_lookup_locked(owner, id, &slot);
	if (error == 0) {
		release = process_timers[slot].owner;
		process_timers[slot].owner = NULL;
		process_timers[slot].armed = 0;
		process_timers[slot].notification_pending = 0;
		process_timers[slot].notification_retry = 0;
	} else {
		release = NULL;
	}
	spin_unlock_irqrestore(&process_timer_lock, irq);
	process_release(release);

	/* Reports the lookup result. */
	return error;
}

/*
 * Reports the time left on a timer and its interval.
 */
int
process_timer_gettime(
	struct process *owner,
	timer_t id,
	struct itimerspec *result)
{
	struct process_timer *timer;
	uint64_t now_ticks;
	uint64_t now_realtime;
	uint64_t remaining;
	uint64_t units;
	unsigned long irq;
	unsigned slot;
	int error;

	/* Rejects a missing result. */
	if (result == NULL)
		return EINVAL;

	/* Samples the timer's clock and converts under the lock. */
	irq = spin_lock_irqsave(&process_timer_lock);
	error = timer_lookup_locked(owner, id, &slot);
	if (error == 0) {
		timer = &process_timers[slot];
		error = timer_clock_snapshot_locked(timer, &now_ticks,
		    &now_realtime);
		if (error == 0) {
			(void)timer_remaining_locked(timer, now_ticks,
			    now_realtime, &remaining);
			if (timer->realtime_absolute)
				units = KERN_NSEC_PER_SEC;
			else
				units = KERN_CLOCK_HZ;
			units_to_timespec(remaining, units, &result->it_value);
			units_to_timespec(timer->interval, units, &result->it_interval);
		}
	}
	spin_unlock_irqrestore(&process_timer_lock, irq);

	/* Reports the lookup or snapshot result. */
	return error;
}

/*
 * Arms or disarms a timer, optionally reporting its previous setting.
 *
 * A relative or monotonic timer counts ticks; an absolute CLOCK_REALTIME
 * timer counts nanoseconds of real time.  A zero value disarms.
 */
int
process_timer_settime(
	struct process *owner,
	timer_t id,
	int flags,
	const struct itimerspec *requested,
	struct itimerspec *previous)
{
	struct itimerspec old_snapshot;
	struct process_timer *timer;
	uint64_t value;
	uint64_t interval;
	uint64_t tick_value;
	uint64_t tick_interval;
	uint64_t realtime_value;
	uint64_t realtime_interval;
	uint64_t now_ticks;
	uint64_t now_realtime;
	uint64_t deadline;
	uint64_t remaining;
	uint64_t units;
	unsigned long irq;
	unsigned slot;
	unsigned realtime_absolute;
	int realtime_error;
	int tick_error;
	int error;

	/* Starts with an unarmed timer and no sampled clock. */
	value = 0;
	interval = 0;
	tick_value = 0;
	tick_interval = 0;
	realtime_value = 0;
	realtime_interval = 0;
	now_ticks = 0;
	now_realtime = 0;
	deadline = 0;
	realtime_error = 0;

	/* Rejects a missing request, an unknown flag, or a bad time. */
	if (requested == NULL || (flags & ~TIMER_ABSTIME) != 0)
		return EINVAL;
	error = kern_timespec_validate(&requested->it_value);
	if (error == 0)
		error = kern_timespec_validate(&requested->it_interval);
	if (error != 0)
		return error;

	/*
	 * Converts outside the registry lock.  Absolute timers need both
	 * candidate unit domains because the timer's clock is discovered by
	 * the locked lookup; only the selected domain's conversion result is
	 * authoritative.
	 */
	tick_error = timespec_to_units(&requested->it_value, KERN_CLOCK_HZ,
	    &tick_value);
	if (tick_error == 0)
		tick_error = timespec_to_units(&requested->it_interval,
		    KERN_CLOCK_HZ, &tick_interval);
	if ((flags & TIMER_ABSTIME) != 0) {
		realtime_error = timespec_to_units(&requested->it_value,
		    KERN_NSEC_PER_SEC, &realtime_value);
		if (realtime_error == 0)
			realtime_error = timespec_to_units(&requested->it_interval,
			    KERN_NSEC_PER_SEC, &realtime_interval);
	}

	/*
	 * Clock sampling, old-value snapshot, relative deadline construction,
	 * and commit share one critical section, making concurrent settime
	 * calls fully ordered.  No timer field is changed before every
	 * selected check succeeds.
	 */
	irq = spin_lock_irqsave(&process_timer_lock);
	error = timer_lookup_locked(owner, id, &slot);
	if (error != 0) {
		spin_unlock_irqrestore(&process_timer_lock, irq);
		return error;
	}

	/* Selects the unit domain the timer's clock and the flags imply. */
	realtime_absolute = 0;
	if ((flags & TIMER_ABSTIME) != 0 &&
	    process_timers[slot].clock == CLOCK_REALTIME)
		realtime_absolute = 1;
	if (realtime_absolute) {
		error = realtime_error;
		value = realtime_value;
		interval = realtime_interval;
	} else {
		error = tick_error;
		value = tick_value;
		interval = tick_interval;
	}

	/* Samples the clocks and builds the deadline. */
	if (error == 0) {
		if (previous != NULL)
			error = timer_clock_snapshot_locked(&process_timers[slot],
			    &now_ticks, &now_realtime);
		else
			now_ticks = clock_ticks();
	}
	if (error == 0 && value != 0) {
		if ((flags & TIMER_ABSTIME) != 0)
			deadline = value;
		else
			error = kern_deadline_after(now_ticks, value, &deadline);
	}
	if (error != 0) {
		spin_unlock_irqrestore(&process_timer_lock, irq);
		return error;
	}

	/* Snapshots the old setting before changing anything. */
	if (previous != NULL) {
		timer = &process_timers[slot];
		(void)timer_remaining_locked(timer, now_ticks, now_realtime,
		    &remaining);
		if (timer->realtime_absolute)
			units = KERN_NSEC_PER_SEC;
		else
			units = KERN_CLOCK_HZ;
		units_to_timespec(remaining, units, &old_snapshot.it_value);
		units_to_timespec(timer->interval, units, &old_snapshot.it_interval);
	}
	KERN_TEST_CHECKPOINT(KERN_TEST_PROCESS_TIMER_SETTIME_SNAPSHOT, owner);

	/* Commits the new setting. */
	process_timers[slot].expiry = deadline;
	process_timers[slot].interval = interval;
	process_timers[slot].overrun = 0;
	process_timers[slot].realtime_absolute = realtime_absolute;
	process_timers[slot].armed = value != 0;
	if (previous != NULL)
		*previous = old_snapshot;
	spin_unlock_irqrestore(&process_timer_lock, irq);

	/* Reports the armed or disarmed timer. */
	return 0;
}

/*
 * Reports the overrun count of the last delivered expiry.
 */
int
process_timer_getoverrun(
	struct process *owner,
	timer_t id,
	int *result)
{
	unsigned long irq;
	unsigned slot;
	int error;

	/* Rejects a missing result. */
	if (result == NULL)
		return EINVAL;

	/* Reads the count under the lock. */
	irq = spin_lock_irqsave(&process_timer_lock);
	error = timer_lookup_locked(owner, id, &slot);
	if (error == 0)
		*result = process_timers[slot].overrun;
	spin_unlock_irqrestore(&process_timer_lock, irq);

	/* Reports the lookup result. */
	return error;
}

/*
 * Expires timers on the clock tick and sends their signals.
 *
 * Expiries are accumulated as overruns while a notification is
 * outstanding, and a notification that could not be sent is retried on a
 * later tick.
 */
void
process_timer_tick(
	uint64_t now_ticks)
{
	struct timer_notification notifications[16];
	struct process_timer *timer;
	uint64_t now_realtime;
	uint64_t now;
	uint64_t missed;
	unsigned cursor;
	unsigned count;
	unsigned index;
	unsigned long irq;
	int already_outstanding;
	int error;

	now_realtime = 0;
	cursor = 0;

	(void)realtime_units(&now_realtime);

	/*
	 * A fixed-size stack batch bounds IRQ-off time and stack use, while
	 * cursor advancement guarantees that every slot is evaluated exactly
	 * once for this tick.  The timer lock is dropped only after all state
	 * for each captured notification has been published.
	 */
	while (cursor < PROCESS_TIMER_MAX) {
		count = 0;

		/* Collects a batch of notifications under the lock. */
		irq = spin_lock_irqsave(&process_timer_lock);
		for (; cursor < PROCESS_TIMER_MAX && count < 16U; cursor++) {
			timer = &process_timers[cursor];
			missed = 0;
			if (timer->owner == NULL)
				continue;
			already_outstanding = timer->notification_pending ||
			    timer->notification_retry;

			/* Counts the expiries since the last tick and re-arms. */
			if (timer->armed) {
				if (timer->realtime_absolute)
					now = now_realtime;
				else
					now = now_ticks;
				if (now >= timer->expiry) {
					missed = 1;
					if (timer->interval != 0)
						missed += (now - timer->expiry) /
						    timer->interval;
					if (timer->interval == 0 || missed >
					    (UINT64_MAX - timer->expiry) / timer->interval)
						timer->armed = 0;
					else
						timer->expiry += missed * timer->interval;
				}
			}

			/* A timer without notification only re-arms. */
			if (timer->event.sigev_notify == SIGEV_NONE)
				continue;

			/* Accumulates overruns behind an outstanding notification. */
			if (missed != 0) {
				if (already_outstanding)
					timer->pending_overrun = overrun_add(
					    timer->pending_overrun, missed);
				else if (missed - 1U > INT_MAX)
					timer->pending_overrun = INT_MAX;
				else
					timer->pending_overrun = (int)missed - 1;
			}

			/* Notifies on a new expiry or a retry, once at a time. */
			if (timer->notification_pending ||
			    (!timer->notification_retry && missed == 0))
				continue;
			timer->notification_pending = 1;
			timer->notification_retry = 0;
			notifications[count].owner = timer->owner;
			process_ref(timer->owner);
			notifications[count].signo = timer->event.sigev_signo;
			memset(&notifications[count].info, 0,
			    sizeof(notifications[count].info));
			notifications[count].info.code = SI_TIMER;
			notifications[count].info.value =
			    timer->event.sigev_value.__sival_pad;
			notifications[count].info.timer_slot = cursor;
			notifications[count].info.timer_generation = timer->generation;
			notifications[count].slot = cursor;
			notifications[count].generation = timer->generation;
			notifications[count].overrun = timer->pending_overrun;
			count++;
		}
		spin_unlock_irqrestore(&process_timer_lock, irq);

		/* Sends the batch outside the lock, marking failures for retry. */
		for (index = 0; index < count; index++) {
			error = signal_send_process_info(notifications[index].owner,
			    notifications[index].signo, &notifications[index].info);
			if (error != 0)
				process_timer_notification_failed(
				    notifications[index].owner,
				    notifications[index].slot,
				    notifications[index].generation);
			process_release(notifications[index].owner);
		}
	}
}

/*
 * Records that a timer's signal was delivered.
 *
 * The overruns accumulated meanwhile become the reported overrun count.
 */
void
process_timer_notification_complete(
	struct process *owner,
	unsigned slot,
	uint32_t generation)
{
	struct process_timer *timer;
	unsigned long irq;

	/* Ignores an impossible timer reference. */
	if (owner == NULL || slot >= PROCESS_TIMER_MAX || generation == 0)
		return;

	/* Completes only the notification of the same timer generation. */
	irq = spin_lock_irqsave(&process_timer_lock);
	timer = &process_timers[slot];
	if (timer->owner == owner &&
	    timer->generation == generation &&
	    timer->notification_pending) {
		timer->overrun = timer->pending_overrun;
		timer->pending_overrun = 0;
		timer->notification_pending = 0;
		timer->notification_retry = 0;
	}
	spin_unlock_irqrestore(&process_timer_lock, irq);
}

/*
 * Records that a timer's signal could not be sent, for a retry.
 */
void
process_timer_notification_failed(
	struct process *owner,
	unsigned slot,
	uint32_t generation)
{
	struct process_timer *timer;
	unsigned long irq;

	/* Ignores an impossible timer reference. */
	if (owner == NULL || slot >= PROCESS_TIMER_MAX || generation == 0)
		return;

	/* Marks only the notification of the same timer generation. */
	irq = spin_lock_irqsave(&process_timer_lock);
	timer = &process_timers[slot];
	if (timer->owner == owner &&
	    timer->generation == generation &&
	    timer->notification_pending) {
		timer->notification_pending = 0;
		timer->notification_retry = 1;
	}
	spin_unlock_irqrestore(&process_timer_lock, irq);
}

/*
 * Deletes every timer of an exiting process.
 */
void
process_timer_cleanup(
	struct process *owner)
{
	unsigned slot;
	unsigned releases;
	unsigned long irq;

	releases = 0;

	/* Frees the process's slots, counting the references to drop. */
	irq = spin_lock_irqsave(&process_timer_lock);
	for (slot = 0; slot < PROCESS_TIMER_MAX; slot++) {
		if (process_timers[slot].owner == owner) {
			process_timers[slot].owner = NULL;
			process_timers[slot].armed = 0;
			process_timers[slot].notification_pending = 0;
			process_timers[slot].notification_retry = 0;
			releases++;
		}
	}
	spin_unlock_irqrestore(&process_timer_lock, irq);

	/* Drops the references outside the lock. */
	while (releases-- != 0)
		process_release(owner);
}

/* Adds to an overrun count, saturating at INT_MAX. */
static int
overrun_add(
	int current,
	uint64_t additional)
{
	/* Saturates when the sum would not fit. */
	if (additional >= (uint64_t)INT_MAX ||
	    current >= INT_MAX - (int)additional)
		return INT_MAX;

	/* Reports the sum. */
	return current + (int)additional;
}

/* Converts a timespec to whole units, rounding the fraction up. */
static int
timespec_to_units(
	const struct timespec *value,
	uint64_t units_per_second,
	uint64_t *result)
{
	uint64_t seconds;
	uint64_t fraction;
	int error;

	/* Rejects a bad time or a missing result. */
	error = kern_timespec_validate(value);
	if (error != 0)
		return error;
	if (result == NULL)
		return EINVAL;

	/* Converts the seconds and the rounded-up nanoseconds, checking overflow. */
	seconds = (uint64_t)value->tv_sec;
	if (seconds > UINT64_MAX / units_per_second)
		return EOVERFLOW;
	fraction = ((uint64_t)value->tv_nsec * units_per_second +
	    KERN_NSEC_PER_SEC - 1U) / KERN_NSEC_PER_SEC;
	if (seconds * units_per_second > UINT64_MAX - fraction)
		return EOVERFLOW;
	*result = seconds * units_per_second + fraction;

	/* Reports the converted value. */
	return 0;
}

/* Converts whole units back to a timespec. */
static void
units_to_timespec(
	uint64_t value,
	uint64_t units_per_second,
	struct timespec *result)
{
	result->tv_sec = (time_t)(value / units_per_second);
	result->tv_nsec = (long)((value % units_per_second) *
	    (KERN_NSEC_PER_SEC / units_per_second));
}

/* Encodes a slot and generation as a timer identifier. */
static timer_t
timer_id(
	unsigned slot,
	uint32_t generation)
{
	return (timer_t)((generation << TIMER_SLOT_BITS) | (slot + 1U));
}

/* Decodes a timer identifier of an owner; the caller holds the lock. */
static int
timer_lookup_locked(
	struct process *owner,
	timer_t id,
	unsigned *slotp)
{
	uint32_t raw;
	unsigned encoded;
	unsigned slot;

	raw = (uint32_t)id;
	encoded = raw & TIMER_SLOT_MASK;

	/* Slots are encoded from one so that a zero identifier is invalid. */
	if (encoded == 0)
		return EINVAL;
	slot = encoded - 1U;

	/* The slot must hold the owner's timer of the same generation. */
	if (slot >= PROCESS_TIMER_MAX ||
	    process_timers[slot].owner != owner ||
	    process_timers[slot].generation != raw >> TIMER_SLOT_BITS)
		return EINVAL;

	*slotp = slot;

	/* Reports the decoded slot. */
	return 0;
}

/* Reads the real-time clock in nanoseconds. */
static int
realtime_units(
	uint64_t *result)
{
	struct timespec now;
	int error;

	/* Reads the clock. */
	error = kern_clock_gettime(CLOCK_REALTIME, &now);
	if (error != 0)
		return error;

	/* Converts it to nanoseconds. */
	error = timespec_to_units(&now, KERN_NSEC_PER_SEC, result);

	/* Reports the conversion result. */
	return error;
}

/* Computes the units left until a timer expires; zero when disarmed. */
static int
timer_remaining_locked(
	const struct process_timer *timer,
	uint64_t now_ticks,
	uint64_t now_realtime,
	uint64_t *remaining)
{
	uint64_t now;

	/* Reads the clock the timer counts in. */
	if (timer->realtime_absolute)
		now = now_realtime;
	else
		now = now_ticks;

	/* A disarmed or expired timer has nothing left. */
	if (!timer->armed || now >= timer->expiry)
		*remaining = 0;
	else
		*remaining = timer->expiry - now;

	/* Reports the computed remainder. */
	return 0;
}

/* Samples the clocks a timer needs; real time only for an absolute timer. */
static int
timer_clock_snapshot_locked(
	const struct process_timer *timer,
	uint64_t *now_ticks,
	uint64_t *now_realtime)
{
	int error;

	*now_ticks = clock_ticks();
	*now_realtime = 0;

	/* Only an absolute real-time timer needs the real-time clock. */
	if (!timer->realtime_absolute)
		return 0;

	/* Reads the real-time clock. */
	error = realtime_units(now_realtime);

	/* Reports the clock read result. */
	return error;
}
