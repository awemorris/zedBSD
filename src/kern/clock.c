/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Kernel time.
 *
 * CPU 0's timer interrupt advances the monotonic tick counter that every
 * deadline in the kernel uses.  The realtime clock is that monotonic time
 * plus an offset seeded from the RTC and adjusted by clock_settime(); the
 * offset is published through a sequence lock so that readers never see a
 * torn value.  The remaining functions are the timespec arithmetic shared
 * by the timer and sleep paths.
 */

#include "kern/clock.h"
#include "kern/atomic.h"
#include "kern/cred.h"
#include "kern/process-timer.h"
#include "kern/sched.h"
#include "kern/usync.h"

#include <errno.h>
#include <hal/hal.h>
#include <limits.h>

#define ZEDBSD_REALTIME_EPOCH_2026 1767225600LL

static volatile uint64_t kernel_ticks;
static volatile uint32_t cpu_notify_count[HAL_CPU_MAX];
static struct kern_timespec realtime_offset = {
	ZEDBSD_REALTIME_EPOCH_2026, 0
};
static int realtime_synchronized;
static volatile unsigned realtime_sequence;
static atomic_uint_t realtime_writer;

static void realtime_write_begin(void);
static void realtime_write_end(void);

/*
 * Initializes the tick counter and seeds the realtime offset from the RTC.
 *
 * Without a readable RTC the realtime clock starts at the 2026 epoch and
 * stays unsynchronized until clock_settime() sets it.
 */
void
kern_clock_init(
	void)
{
	uint64_t seconds;

	/* Starts the clocks at the compiled-in epoch. */
	atomic_u64_store_release(&kernel_ticks, 0);
	atomic_raw_store_release(&realtime_sequence, 0);
	atomic_store_release(&realtime_writer, 0);
	realtime_offset.tv_sec = ZEDBSD_REALTIME_EPOCH_2026;
	realtime_offset.tv_nsec = 0;
	atomic_raw_store_release((volatile unsigned *)&realtime_synchronized, 0);
	process_timer_init();

	/* Adopts the RTC time when the platform can read it. */
	if (hal_rtc_read_epoch_time(&seconds)) {
		realtime_offset.tv_sec = (int64_t)seconds;
		atomic_raw_store_release((volatile unsigned *)&realtime_synchronized, 1);
	}
}

/*
 * Handles the periodic timer interrupt on one CPU.
 *
 * Only CPU 0 advances the tick counter and the process timers; every CPU
 * feeds the current tick to its scheduler.
 */
void
kernel_timer_handler(
	hal_cpu_id_t cpu,
	hal_irq_ack_t acknowledge)
{
	uint64_t now;

	hal_irq_send_eoi(acknowledge);

	/* Advances the shared tick from the boot CPU only. */
	if (cpu == 0)
		(void)atomic_u64_fetch_add_relaxed(&kernel_ticks, 1U);
	now = atomic_u64_load_acquire(&kernel_ticks);

	/* Expires process timers from the boot CPU only. */
	if (cpu == 0)
		process_timer_tick(now);

	sched_clock_cpu(cpu, now);
}

/*
 * Handles the cross-CPU notification interrupt on one CPU.
 */
void
kernel_cpu_notify_handler(
	hal_cpu_id_t cpu,
	hal_irq_ack_t acknowledge)
{
	hal_irq_send_eoi(acknowledge);

	/* Counts the notification for the delivery probe. */
	if (cpu < HAL_CPU_MAX)
		(void)atomic_raw_fetch_add_relaxed(&cpu_notify_count[cpu], 1U);

	sched_cpu_notify(cpu);
}

/*
 * Proves that every secondary CPU receives the notification interrupt.
 *
 * The probe sends one notification to every secondary CPU and waits, with
 * a bounded spin, for each of them to count it.
 */
int
kern_cpu_notify_probe(
	void)
{
	struct hal_cpu_mask targets;
	uint32_t before[HAL_CPU_MAX];
	hal_cpu_id_t cpu;
	unsigned timeout;

	/* Records each secondary CPU's count and targets it. */
	hal_cpu_mask_zero(&targets);
	for (cpu = 1; cpu < hal_cpu_count(); cpu++) {
		before[cpu] = atomic_raw_load_acquire(&cpu_notify_count[cpu]);
		hal_cpu_mask_set(&targets, cpu);
	}

	/* Passes trivially on a single CPU. */
	if (hal_cpu_count() <= 1)
		return HAL_OK;

	/* Sends one notification to every target. */
	if (hal_cpu_notify_mask(&targets) != HAL_OK)
		return HAL_ERR_IO;

	/* Waits until every target's count has advanced. */
	for (timeout = 0; timeout < 10000000U; timeout++) {
		for (cpu = 1; cpu < hal_cpu_count(); cpu++) {
			if (atomic_raw_load_acquire(&cpu_notify_count[cpu]) == before[cpu])
				break;
		}

		/* Reports success once no target is still pending. */
		if (cpu == hal_cpu_count())
			return HAL_OK;
		hal_compiler_barrier();
	}

	/* Reports a target that never counted the notification. */
	return HAL_ERR_TIMEOUT;
}

/*
 * Reads the monotonic tick counter.
 */
uint64_t
clock_ticks(
	void)
{
	uint64_t ticks;

	/* Reads the counter with acquire ordering. */
	ticks = atomic_u64_load_acquire(&kernel_ticks);

	/* Reports the current tick. */
	return ticks;
}

/*
 * Reads the monotonic clock in milliseconds for callback-style users.
 */
uint64_t
clock_milliseconds(
	void *context)
{
	uint64_t ticks;

	(void)context;

	/* Scales the tick counter to milliseconds. */
	ticks = clock_ticks();

	/* Reports the elapsed milliseconds. */
	return ticks * (1000U / KERN_CLOCK_HZ);
}

/*
 * Validates a user-supplied timespec.
 */
int
kern_timespec_validate(
	const struct timespec *value)
{
	/* Rejects a missing, negative, or denormalized value. */
	if (value == NULL ||
	    value->tv_sec < 0 ||
	    value->tv_nsec < 0 ||
	    (uint64_t)value->tv_nsec >= KERN_NSEC_PER_SEC)
		return EINVAL;

	/* Reports a valid timespec. */
	return 0;
}

/*
 * Normalizes a timespec so its nanoseconds lie in [0, 1s).
 */
int
kern_timespec_normalize(
	struct kern_timespec *value)
{
	int64_t carry;

	/* Rejects a missing value. */
	if (value == NULL)
		return EINVAL;

	/* Moves whole seconds out of the nanosecond field. */
	carry = value->tv_nsec / (int64_t)KERN_NSEC_PER_SEC;
	value->tv_nsec %= (int32_t)KERN_NSEC_PER_SEC;
	if (value->tv_nsec < 0) {
		value->tv_nsec += (int32_t)KERN_NSEC_PER_SEC;
		carry--;
	}

	/* Rejects a carry that would overflow the seconds. */
	if ((carry > 0 && value->tv_sec > INT64_MAX - carry) ||
	    (carry < 0 && value->tv_sec < INT64_MIN - carry))
		return EOVERFLOW;

	value->tv_sec += carry;

	/* Reports the normalized value. */
	return 0;
}

/*
 * Adds two timespecs with overflow detection.
 */
int
kern_timespec_add(
	const struct kern_timespec *a,
	const struct kern_timespec *b,
	struct kern_timespec *result)
{
	int error;

	/* Rejects a missing operand or result. */
	if (a == NULL || b == NULL || result == NULL)
		return EINVAL;

	/* Rejects a seconds sum that would overflow. */
	if ((b->tv_sec > 0 && a->tv_sec > INT64_MAX - b->tv_sec) ||
	    (b->tv_sec < 0 && a->tv_sec < INT64_MIN - b->tv_sec))
		return EOVERFLOW;

	/* Adds the fields and normalizes the nanosecond carry. */
	result->tv_sec = a->tv_sec + b->tv_sec;
	result->tv_nsec = a->tv_nsec + b->tv_nsec;
	error = kern_timespec_normalize(result);

	/* Reports why the normalization failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Subtracts one timespec from another with overflow detection.
 */
int
kern_timespec_sub(
	const struct kern_timespec *a,
	const struct kern_timespec *b,
	struct kern_timespec *result)
{
	struct kern_timespec negated;
	int error;

	/* Rejects a missing operand or result. */
	if (a == NULL || b == NULL || result == NULL)
		return EINVAL;

	/* Rejects the one subtrahend that cannot be negated. */
	if (b->tv_sec == INT64_MIN)
		return EOVERFLOW;

	/* Subtracts by adding the negated subtrahend. */
	negated.tv_sec = -b->tv_sec;
	negated.tv_nsec = -b->tv_nsec;
	error = kern_timespec_add(a, &negated, result);

	/* Reports why the addition failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Orders two normalized timespecs.
 */
int
kern_timespec_compare(
	const struct kern_timespec *a,
	const struct kern_timespec *b)
{
	/* Orders by whole seconds first. */
	if (a->tv_sec < b->tv_sec)
		return -1;
	if (a->tv_sec > b->tv_sec)
		return 1;

	/* Orders equal seconds by nanoseconds. */
	if (a->tv_nsec < b->tv_nsec)
		return -1;
	if (a->tv_nsec > b->tv_nsec)
		return 1;

	/* Reports equal times. */
	return 0;
}

/*
 * Converts a duration to ticks, rounding up.
 */
int
kern_duration_to_ticks_ceil(
	const struct timespec *duration,
	uint64_t *ticks)
{
	uint64_t seconds;
	uint64_t fraction;
	int error;

	/* Rejects an invalid duration or a missing result. */
	error = kern_timespec_validate(duration);
	if (error != 0)
		return error;
	if (ticks == NULL)
		return EINVAL;

	/* Rejects whole seconds that would overflow the tick count. */
	seconds = (uint64_t)duration->tv_sec;
	if (seconds > UINT64_MAX / KERN_CLOCK_HZ)
		return EOVERFLOW;

	/* Rounds the nanosecond fraction up to whole ticks. */
	fraction = ((uint64_t)duration->tv_nsec * KERN_CLOCK_HZ +
	    KERN_NSEC_PER_SEC - 1U) / KERN_NSEC_PER_SEC;

	/* Rejects a total that would overflow the tick count. */
	if (seconds * KERN_CLOCK_HZ > UINT64_MAX - fraction)
		return EOVERFLOW;

	*ticks = seconds * KERN_CLOCK_HZ + fraction;

	/* Reports the converted tick count. */
	return 0;
}

/*
 * Computes a deadline a fixed number of ticks after a moment.
 */
int
kern_deadline_after(
	uint64_t now,
	uint64_t delta,
	uint64_t *deadline)
{
	/* Rejects a missing result. */
	if (deadline == NULL)
		return EINVAL;

	/* Rejects a deadline beyond the tick range. */
	if (now > UINT64_MAX - delta)
		return EOVERFLOW;

	*deadline = now + delta;

	/* Reports the computed deadline. */
	return 0;
}

/*
 * Reports the ticks that remain until a deadline, or zero once it passed.
 */
uint64_t
kern_deadline_remaining(
	uint64_t now,
	uint64_t deadline)
{
	/* Reports an expired deadline as no remaining time. */
	if (now >= deadline)
		return 0;

	/* Reports the remaining ticks. */
	return deadline - now;
}

/*
 * Reads the monotonic or realtime clock.
 */
int
kern_clock_gettime(
	clockid_t clock,
	struct timespec *result)
{
	struct kern_timespec monotonic;
	struct kern_timespec value;
	struct kern_timespec offset;
	uint64_t ticks;
	unsigned before;
	unsigned after;
	int error;

	/* Rejects a missing result or an unsupported clock. */
	if (result == NULL)
		return EINVAL;
	if (clock != CLOCK_MONOTONIC && clock != CLOCK_REALTIME)
		return EINVAL;

	/* Converts the tick counter to monotonic time. */
	ticks = clock_ticks();
	monotonic.tv_sec = (int64_t)(ticks / KERN_CLOCK_HZ);
	monotonic.tv_nsec = (int32_t)((ticks % KERN_CLOCK_HZ) *
	    (KERN_NSEC_PER_SEC / KERN_CLOCK_HZ));
	value = monotonic;

	/* Adds the realtime offset read consistently under the sequence lock. */
	if (clock == CLOCK_REALTIME) {
		for (;;) {
			before = atomic_raw_load_acquire(&realtime_sequence);
			if (before & 1U)
				continue;
			offset = realtime_offset;
			after = atomic_raw_load_acquire(&realtime_sequence);
			if (before == after)
				break;
		}
		error = kern_timespec_add(&monotonic, &offset, &value);
		if (error != 0)
			return error;
	}

	/* Publishes the time in the public representation. */
	result->tv_sec = (time_t)value.tv_sec;
	result->tv_nsec = (long)value.tv_nsec;

	/* Reports the time. */
	return 0;
}

/*
 * Reports the resolution of the monotonic and realtime clocks.
 */
int
kern_clock_getres(
	clockid_t clock,
	struct timespec *result)
{
	/* Rejects an unsupported clock. */
	if (clock != CLOCK_MONOTONIC && clock != CLOCK_REALTIME)
		return EINVAL;

	/* Reports one tick when a result is wanted. */
	if (result != NULL) {
		result->tv_sec = 0;
		result->tv_nsec = (long)(KERN_NSEC_PER_SEC / KERN_CLOCK_HZ);
	}

	/* Reports a supported clock. */
	return 0;
}

/*
 * Sets the realtime clock.
 *
 * Only the superuser may set it.  The new offset is published under the
 * sequence lock and marks the clock synchronized.
 */
int
kern_clock_settime(
	clockid_t clock,
	const struct timespec *requested,
	const struct ucred *cred)
{
	struct timespec now;
	struct kern_timespec target;
	struct kern_timespec monotonic;
	int error;

	/* Rejects any clock but realtime. */
	if (clock != CLOCK_REALTIME)
		return EINVAL;

	/* Rejects an unprivileged caller. */
	if (!cred_is_superuser(cred))
		return EPERM;

	/* Rejects an invalid time. */
	error = kern_timespec_validate(requested);
	if (error != 0)
		return error;

	/* Reads the monotonic time the offset is relative to. */
	error = kern_clock_gettime(CLOCK_MONOTONIC, &now);
	if (error != 0)
		return error;

	/* Converts both times to the kernel representation. */
	target.tv_sec = requested->tv_sec;
	target.tv_nsec = (int32_t)requested->tv_nsec;
	monotonic.tv_sec = now.tv_sec;
	monotonic.tv_nsec = (int32_t)now.tv_nsec;

	/* Publishes the new offset under the sequence lock. */
	realtime_write_begin();
	error = kern_timespec_sub(&target, &monotonic, &realtime_offset);
	if (error == 0)
		atomic_raw_store_release((volatile unsigned *)&realtime_synchronized, 1);
	realtime_write_end();

	/* Wakes absolute-time waiters after a successful change. */
	if (error == 0)
		usync_realtime_changed();

	/* Reports why the change failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Tests whether the realtime clock was set from the RTC or by a caller.
 */
int
kern_clock_realtime_synchronized(
	void)
{
	unsigned synchronized;

	/* Reads the synchronization flag. */
	synchronized = atomic_raw_load_acquire((volatile unsigned *)&realtime_synchronized);

	/* Reports whether the clock is synchronized. */
	return synchronized != 0;
}

/*
 * Reads the realtime clock into separate second and nanosecond fields.
 */
void
clock_realtime(
	time_t *seconds,
	long *nanoseconds)
{
	struct timespec now;

	/* Reads the realtime clock, which cannot fail for a valid result. */
	(void)kern_clock_gettime(CLOCK_REALTIME, &now);

	/* Publishes the fields the caller asked for. */
	if (seconds != NULL)
		*seconds = now.tv_sec;
	if (nanoseconds != NULL)
		*nanoseconds = now.tv_nsec;
}

/* Enters the realtime offset write side of the sequence lock. */
static void
realtime_write_begin(
	void)
{
	/* Waits for exclusive writer ownership. */
	while (!atomic_try_acquire_zero(&realtime_writer))
		hal_compiler_barrier();

	/* Marks the offset as being written. */
	(void)atomic_raw_fetch_add_relaxed(&realtime_sequence, 1U);
}

/* Leaves the realtime offset write side of the sequence lock. */
static void
realtime_write_end(
	void)
{
	/* Marks the offset as stable again and releases the writer. */
	(void)atomic_raw_fetch_add_relaxed(&realtime_sequence, 1U);
	atomic_store_release(&realtime_writer, 0);
}
