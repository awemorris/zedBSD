/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/process-timer.h"

#include "kern/clock.h"
#include "kern/lock.h"
#include "kern/process.h"
#include "kern/signal.h"

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
	unsigned armed;
	unsigned realtime_absolute;
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

static int
timespec_to_units(const struct timespec *value, uint64_t units_per_second,
	uint64_t *result)
{
	uint64_t seconds, fraction;
	int error = kern_timespec_validate(value);
	if (error != 0 || result == NULL)
		return error != 0 ? error : EINVAL;
	seconds = (uint64_t)value->tv_sec;
	if (seconds > UINT64_MAX / units_per_second)
		return EOVERFLOW;
	fraction = ((uint64_t)value->tv_nsec * units_per_second +
	    KERN_NSEC_PER_SEC - 1U) / KERN_NSEC_PER_SEC;
	if (seconds * units_per_second > UINT64_MAX - fraction)
		return EOVERFLOW;
	*result = seconds * units_per_second + fraction;
	return 0;
}

static void
units_to_timespec(uint64_t value, uint64_t units_per_second,
	struct timespec *result)
{
	result->tv_sec = (time_t)(value / units_per_second);
	result->tv_nsec = (long)((value % units_per_second) *
	    (KERN_NSEC_PER_SEC / units_per_second));
}

static timer_t
timer_id(unsigned slot, uint32_t generation)
{
	return (timer_t)((generation << TIMER_SLOT_BITS) | (slot + 1U));
}

static int
timer_lookup_locked(struct process *owner, timer_t id, unsigned *slotp)
{
	uint32_t raw = (uint32_t)id;
	unsigned encoded = raw & TIMER_SLOT_MASK;
	unsigned slot;
	if (encoded == 0)
		return EINVAL;
	slot = encoded - 1U;
	if (slot >= PROCESS_TIMER_MAX || process_timers[slot].owner != owner ||
	    process_timers[slot].generation != raw >> TIMER_SLOT_BITS)
		return EINVAL;
	*slotp = slot;
	return 0;
}

static int
realtime_units(uint64_t *result)
{
	struct timespec now;
	int error = kern_clock_gettime(CLOCK_REALTIME, &now);
	return error != 0 ? error : timespec_to_units(&now,
	    KERN_NSEC_PER_SEC, result);
}

void
process_timer_init(void)
{
	memset(process_timers, 0, sizeof(process_timers));
	spin_init(&process_timer_lock, LOCK_RANK_PROCESS_TREE, "process timers");
}

int
process_timer_create(struct process *owner, clockid_t clock,
	const struct sigevent *requested, timer_t *result)
{
	struct sigevent event;
	unsigned long irq;
	unsigned slot;
	if (owner == NULL || owner == &process0 || result == NULL ||
	    (clock != CLOCK_MONOTONIC && clock != CLOCK_REALTIME))
		return EINVAL;
	memset(&event, 0, sizeof(event));
	if (requested != NULL)
		event = *requested;
	else {
		event.sigev_notify = SIGEV_SIGNAL;
		event.sigev_signo = SIGALRM;
	}
	if (event.sigev_notify != SIGEV_NONE &&
	    event.sigev_notify != SIGEV_SIGNAL)
		return EOPNOTSUPP;
	if (event.sigev_notify == SIGEV_SIGNAL &&
	    (event.sigev_signo <= 0 || event.sigev_signo >= NSIG))
		return EINVAL;
	irq = spin_lock_irqsave(&process_timer_lock);
	for (slot = 0; slot < PROCESS_TIMER_MAX; slot++)
		if (process_timers[slot].owner == NULL)
			break;
	if (slot == PROCESS_TIMER_MAX) {
		spin_unlock_irqrestore(&process_timer_lock, irq);
		return EAGAIN;
	}
	process_timers[slot].generation++;
	if (process_timers[slot].generation == 0)
		process_timers[slot].generation = 1;
	process_timers[slot].owner = owner;
	process_timers[slot].clock = clock;
	process_timers[slot].event = event;
	process_ref(owner);
	*result = timer_id(slot, process_timers[slot].generation);
	spin_unlock_irqrestore(&process_timer_lock, irq);
	return 0;
}

int
process_timer_delete(struct process *owner, timer_t id)
{
	struct process *release;
	unsigned long irq;
	unsigned slot;
	int error;
	irq = spin_lock_irqsave(&process_timer_lock);
	error = timer_lookup_locked(owner, id, &slot);
	if (error == 0) {
		release = process_timers[slot].owner;
		process_timers[slot].owner = NULL;
		process_timers[slot].armed = 0;
	} else release = NULL;
	spin_unlock_irqrestore(&process_timer_lock, irq);
	process_release(release);
	return error;
}

static int
timer_remaining_locked(const struct process_timer *timer, uint64_t now_ticks,
	uint64_t now_realtime, uint64_t *remaining)
{
	uint64_t now = timer->realtime_absolute ? now_realtime : now_ticks;
	*remaining = !timer->armed || now >= timer->expiry ? 0 :
	    timer->expiry - now;
	return 0;
}

int
process_timer_gettime(struct process *owner, timer_t id,
	struct itimerspec *result)
{
	uint64_t now_realtime = 0, remaining;
	unsigned long irq;
	unsigned slot;
	int error;
	if (result == NULL)
		return EINVAL;
	error = realtime_units(&now_realtime);
	if (error != 0)
		return error;
	irq = spin_lock_irqsave(&process_timer_lock);
	error = timer_lookup_locked(owner, id, &slot);
	if (error == 0) {
		struct process_timer *timer = &process_timers[slot];
		(void)timer_remaining_locked(timer, zedbsd_kernel_ticks(),
		    now_realtime, &remaining);
		units_to_timespec(remaining, timer->realtime_absolute ?
		    KERN_NSEC_PER_SEC : KERN_CLOCK_HZ, &result->it_value);
		units_to_timespec(timer->interval, timer->realtime_absolute ?
		    KERN_NSEC_PER_SEC : KERN_CLOCK_HZ, &result->it_interval);
	}
	spin_unlock_irqrestore(&process_timer_lock, irq);
	return error;
}

int
process_timer_settime(struct process *owner, timer_t id, int flags,
	const struct itimerspec *requested, struct itimerspec *previous)
{
	uint64_t value, interval, now, deadline;
	unsigned long irq;
	unsigned slot;
	int error;
	if (requested == NULL || (flags & ~TIMER_ABSTIME) != 0)
		return EINVAL;
	error = kern_timespec_validate(&requested->it_value);
	if (error == 0)
		error = kern_timespec_validate(&requested->it_interval);
	if (error != 0)
		return error;
	if (previous != NULL) {
		error = process_timer_gettime(owner, id, previous);
		if (error != 0)
			return error;
	}
	irq = spin_lock_irqsave(&process_timer_lock);
	error = timer_lookup_locked(owner, id, &slot);
	if (error != 0) {
		spin_unlock_irqrestore(&process_timer_lock, irq);
		return error;
	}
	if (requested->it_value.tv_sec == 0 && requested->it_value.tv_nsec == 0) {
		process_timers[slot].armed = 0;
		process_timers[slot].interval = 0;
		spin_unlock_irqrestore(&process_timer_lock, irq);
		return 0;
	}
	if ((flags & TIMER_ABSTIME) != 0 &&
	    process_timers[slot].clock == CLOCK_REALTIME) {
		error = timespec_to_units(&requested->it_value,
		    KERN_NSEC_PER_SEC, &value);
		if (error == 0)
			error = timespec_to_units(&requested->it_interval,
			    KERN_NSEC_PER_SEC, &interval);
		process_timers[slot].realtime_absolute = 1;
		deadline = value;
	} else {
		error = timespec_to_units(&requested->it_value, KERN_CLOCK_HZ,
		    &value);
		if (error == 0)
			error = timespec_to_units(&requested->it_interval,
			    KERN_CLOCK_HZ, &interval);
		process_timers[slot].realtime_absolute = 0;
		now = zedbsd_kernel_ticks();
		if (error == 0 && (flags & TIMER_ABSTIME) != 0)
			deadline = value;
		else if (error == 0)
			error = kern_deadline_after(now, value, &deadline);
	}
	if (error == 0) {
		process_timers[slot].expiry = deadline;
		process_timers[slot].interval = interval;
		process_timers[slot].overrun = 0;
		process_timers[slot].armed = 1;
	}
	spin_unlock_irqrestore(&process_timer_lock, irq);
	return error;
}

int
process_timer_getoverrun(struct process *owner, timer_t id, int *result)
{
	unsigned long irq;
	unsigned slot;
	int error;
	if (result == NULL)
		return EINVAL;
	irq = spin_lock_irqsave(&process_timer_lock);
	error = timer_lookup_locked(owner, id, &slot);
	if (error == 0)
		*result = process_timers[slot].overrun;
	spin_unlock_irqrestore(&process_timer_lock, irq);
	return error;
}

void
process_timer_tick(uint64_t now_ticks)
{
	struct timer_notification notifications[16];
	uint64_t now_realtime = 0;
	unsigned count = 0, slot;
	unsigned long irq;
	(void)realtime_units(&now_realtime);
	irq = spin_lock_irqsave(&process_timer_lock);
	for (slot = 0; slot < PROCESS_TIMER_MAX && count < 16U; slot++) {
		struct process_timer *timer = &process_timers[slot];
		uint64_t now, missed = 1;
		if (timer->owner == NULL || !timer->armed)
			continue;
		now = timer->realtime_absolute ? now_realtime : now_ticks;
		if (now < timer->expiry)
			continue;
		if (timer->interval != 0)
			missed += (now - timer->expiry) / timer->interval;
		if (timer->interval == 0 || missed >
		    (UINT64_MAX - timer->expiry) / timer->interval)
			timer->armed = 0;
		else
			timer->expiry += missed * timer->interval;
		timer->overrun = missed - 1U > INT_MAX ? INT_MAX : (int)missed - 1;
		if (timer->event.sigev_notify == SIGEV_NONE)
			continue;
		notifications[count].owner = timer->owner;
		process_ref(timer->owner);
		notifications[count].signo = timer->event.sigev_signo;
		memset(&notifications[count].info, 0,
		    sizeof(notifications[count].info));
		notifications[count].info.code = SI_TIMER;
		notifications[count].info.value = timer->event.sigev_value.__sival_pad;
		notifications[count].slot = slot;
		notifications[count].generation = timer->generation;
		notifications[count].overrun = timer->overrun;
		count++;
	}
	spin_unlock_irqrestore(&process_timer_lock, irq);
	for (slot = 0; slot < count; slot++) {
		(void)signal_send_process_info(notifications[slot].owner,
		    notifications[slot].signo, &notifications[slot].info);
		process_release(notifications[slot].owner);
	}
}

void
process_timer_cleanup(struct process *owner)
{
	unsigned slot, releases = 0;
	unsigned long irq;
	irq = spin_lock_irqsave(&process_timer_lock);
	for (slot = 0; slot < PROCESS_TIMER_MAX; slot++)
		if (process_timers[slot].owner == owner) {
			process_timers[slot].owner = NULL;
			process_timers[slot].armed = 0;
			releases++;
		}
	spin_unlock_irqrestore(&process_timer_lock, irq);
	while (releases-- != 0)
		process_release(owner);
}
