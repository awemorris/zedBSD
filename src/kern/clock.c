/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/clock.h"
#include "kern/cred.h"

#include <errno.h>
#include <hal/hal.h>
#include <limits.h>

#define ZEDBSD_REALTIME_EPOCH_2026 1767225600LL

static volatile uint64_t kernel_ticks;
static struct kern_timespec realtime_offset = {
	ZEDBSD_REALTIME_EPOCH_2026, 0
};
static int realtime_synchronized;

void
kernel_timer_handler(void)
{
	kernel_ticks++;
}

uint64_t
zedbsd_kernel_ticks(void)
{
	uint64_t value;
	bool enabled = hal_irq_disable();

	value = kernel_ticks;
	if (enabled)
		hal_irq_enable();
	return value;
}

uint64_t
zedbsd_kernel_milliseconds(void *context)
{
	(void)context;
	return zedbsd_kernel_ticks() * (1000U / KERN_CLOCK_HZ);
}

int
kern_timespec_validate(const struct timespec *value)
{
	return value == NULL || value->tv_sec < 0 || value->tv_nsec < 0 ||
	    (uint64_t)value->tv_nsec >= KERN_NSEC_PER_SEC ? EINVAL : 0;
}

int
kern_timespec_normalize(struct kern_timespec *value)
{
	int64_t carry;

	if (value == NULL)
		return EINVAL;
	carry = value->tv_nsec / (int64_t)KERN_NSEC_PER_SEC;
	value->tv_nsec %= (int32_t)KERN_NSEC_PER_SEC;
	if (value->tv_nsec < 0) {
		value->tv_nsec += (int32_t)KERN_NSEC_PER_SEC;
		carry--;
	}
	if ((carry > 0 && value->tv_sec > INT64_MAX - carry) ||
	    (carry < 0 && value->tv_sec < INT64_MIN - carry))
		return EOVERFLOW;
	value->tv_sec += carry;
	return 0;
}

int
kern_timespec_add(const struct kern_timespec *a,
	const struct kern_timespec *b, struct kern_timespec *result)
{
	if (a == NULL || b == NULL || result == NULL)
		return EINVAL;
	if ((b->tv_sec > 0 && a->tv_sec > INT64_MAX - b->tv_sec) ||
	    (b->tv_sec < 0 && a->tv_sec < INT64_MIN - b->tv_sec))
		return EOVERFLOW;
	result->tv_sec = a->tv_sec + b->tv_sec;
	result->tv_nsec = a->tv_nsec + b->tv_nsec;
	return kern_timespec_normalize(result);
}

int
kern_timespec_sub(const struct kern_timespec *a,
	const struct kern_timespec *b, struct kern_timespec *result)
{
	struct kern_timespec negated;

	if (a == NULL || b == NULL || result == NULL)
		return EINVAL;
	if (b->tv_sec == INT64_MIN)
		return EOVERFLOW;
	negated.tv_sec = -b->tv_sec;
	negated.tv_nsec = -b->tv_nsec;
	return kern_timespec_add(a, &negated, result);
}

int
kern_timespec_compare(const struct kern_timespec *a,
	const struct kern_timespec *b)
{
	if (a->tv_sec != b->tv_sec)
		return a->tv_sec < b->tv_sec ? -1 : 1;
	return a->tv_nsec == b->tv_nsec ? 0 : a->tv_nsec < b->tv_nsec ? -1 : 1;
}

int
kern_duration_to_ticks_ceil(const struct timespec *duration, uint64_t *ticks)
{
	uint64_t seconds, fraction;
	int error = kern_timespec_validate(duration);

	if (error != 0 || ticks == NULL)
		return error != 0 ? error : EINVAL;
	seconds = (uint64_t)duration->tv_sec;
	if (seconds > UINT64_MAX / KERN_CLOCK_HZ)
		return EOVERFLOW;
	fraction = ((uint64_t)duration->tv_nsec * KERN_CLOCK_HZ +
	    KERN_NSEC_PER_SEC - 1U) / KERN_NSEC_PER_SEC;
	if (seconds * KERN_CLOCK_HZ > UINT64_MAX - fraction)
		return EOVERFLOW;
	*ticks = seconds * KERN_CLOCK_HZ + fraction;
	return 0;
}

int
kern_deadline_after(uint64_t now, uint64_t delta, uint64_t *deadline)
{
	if (deadline == NULL)
		return EINVAL;
	if (now > UINT64_MAX - delta)
		return EOVERFLOW;
	*deadline = now + delta;
	return 0;
}

uint64_t
kern_deadline_remaining(uint64_t now, uint64_t deadline)
{
	return now >= deadline ? 0 : deadline - now;
}

int
kern_clock_gettime(clockid_t clock, struct timespec *result)
{
	uint64_t ticks;
	struct kern_timespec monotonic, value;
	int error;

	if (result == NULL)
		return EINVAL;
	if (clock != CLOCK_MONOTONIC && clock != CLOCK_REALTIME)
		return EINVAL;
	ticks = zedbsd_kernel_ticks();
	monotonic.tv_sec = (int64_t)(ticks / KERN_CLOCK_HZ);
	monotonic.tv_nsec = (int32_t)((ticks % KERN_CLOCK_HZ) *
	    (KERN_NSEC_PER_SEC / KERN_CLOCK_HZ));
	value = monotonic;
	if (clock == CLOCK_REALTIME) {
		error = kern_timespec_add(&monotonic, &realtime_offset, &value);
		if (error != 0)
			return error;
	}
	result->tv_sec = (time_t)value.tv_sec;
	result->tv_nsec = (long)value.tv_nsec;
	return 0;
}

int
kern_clock_getres(clockid_t clock, struct timespec *result)
{
	if (clock != CLOCK_MONOTONIC && clock != CLOCK_REALTIME)
		return EINVAL;
	if (result != NULL) {
		result->tv_sec = 0;
		result->tv_nsec = (long)(KERN_NSEC_PER_SEC / KERN_CLOCK_HZ);
	}
	return 0;
}

int
kern_clock_settime(clockid_t clock, const struct timespec *requested,
	const struct ucred *cred)
{
	struct timespec now;
	struct kern_timespec target, monotonic;
	int error;

	if (clock != CLOCK_REALTIME)
		return EINVAL;
	if (!cred_is_superuser(cred))
		return EPERM;
	error = kern_timespec_validate(requested);
	if (error != 0)
		return error;
	error = kern_clock_gettime(CLOCK_MONOTONIC, &now);
	if (error != 0)
		return error;
	target.tv_sec = requested->tv_sec;
	target.tv_nsec = (int32_t)requested->tv_nsec;
	monotonic.tv_sec = now.tv_sec;
	monotonic.tv_nsec = (int32_t)now.tv_nsec;
	error = kern_timespec_sub(&target, &monotonic, &realtime_offset);
	if (error == 0)
		realtime_synchronized = 1;
	return error;
}

int
kern_clock_realtime_synchronized(void)
{
	return realtime_synchronized;
}

void
zedbsd_clock_realtime(time_t *seconds, long *nanoseconds)
{
	struct timespec now;

	(void)kern_clock_gettime(CLOCK_REALTIME, &now);
	if (seconds != NULL)
		*seconds = now.tv_sec;
	if (nanoseconds != NULL)
		*nanoseconds = now.tv_nsec;
}
