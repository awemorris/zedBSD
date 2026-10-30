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

#include "kern/random.h"
#include "kern/clock.h"
#include "kern/cpuperf.h"
#include "kern/atomic.h"
#include "kern/cred.h"
#include "kern/process-timer.h"
#include "kern/sched.h"
#include "kern/sched_private.h"
#include "kern/usync.h"
#include "kern/waitq.h"
#include "kern/lock.h"

#include <uapi/errno.h>
#include <hal/hal.h>
#include <limits.h>

#define KERN_REALTIME_EPOCH_2026 1767225600LL

static volatile uint64_t kernel_ticks;

/*
 * The ticks in a second.  It is the rate the HAL runs the periodic timer
 * at, and only this file reads it: everything else asks the functions
 * below to convert.  It is at most one tick per nanosecond.
 */
static const uint32_t tick_rate = HAL_TIMER_FREQUENCY;

/*
 * The counter's value when CPU 0 last entered suspend-to-idle, and the
 * counts left over from the ticks it brought forward.  Only
 * CPU 0's idle loop touches them, around hal_cpu_idle_suspend(); begun is
 * 1 between the two halves.
 */
static uint64_t suspend_counter;
static uint64_t suspend_remainder;
static unsigned suspend_begun;
static volatile uint32_t cpu_notify_count[HAL_CPU_MAX];
static struct kern_timespec realtime_offset = {
	KERN_REALTIME_EPOCH_2026, 0
};
static int realtime_synchronized;
static volatile unsigned realtime_sequence;
static atomic_uint_t realtime_writer;

static void realtime_write_begin(void);
static void realtime_write_end(void);
static uint64_t scale_floor(uint64_t value, uint64_t multiplier, uint64_t divisor);
static uint64_t scale_ceil(uint64_t value, uint64_t multiplier, uint64_t divisor);

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
	realtime_offset.tv_sec = KERN_REALTIME_EPOCH_2026;
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

	/* Expires process timers from the boot CPU only. */
	now = atomic_u64_load_acquire(&kernel_ticks);
	if (cpu == 0)
		process_timer_tick(now);

	sched_clock_cpu(cpu, now);

	/* The tick's timing against the counter feeds the random pool. */
	kern_random_tick(cpu);
}

/*
 * Notes the counter as CPU 0 enters suspend-to-idle, whose tick stops
 * until it returns.  Only CPU 0 calls it, from its idle loop.
 */
void
kern_clock_idle_suspend_begin(
	void)
{
	uint64_t counter;
	uint64_t frequency;
	bool counted;

	/* The counter now; without one no time can be brought forward. */
	suspend_begun = 0U;
	counted = hal_rtc_read_counter(&counter, &frequency);
	if (!counted)
		return;

	/* Kept for the end. */
	suspend_counter = counter;
	suspend_begun = 1U;
}

/*
 * Brings kernel_ticks forward by the time CPU 0 spent in suspend-to-idle,
 * from the counter that kept running, carrying the counts short of a tick
 * to the next time.  Only CPU 0 calls it, from its idle loop.
 */
void
kern_clock_idle_suspend_end(
	void)
{
	uint64_t counter;
	uint64_t frequency;
	uint64_t elapsed;
	uint64_t per_tick;
	uint64_t ticks;
	bool counted;

	/* Nothing began. */
	if (!suspend_begun)
		return;
	suspend_begun = 0U;

	/* The counter now. */
	counted = hal_rtc_read_counter(&counter, &frequency);
	if (!counted)
		return;

	/* The counts of one tick; a counter slower than the tick cannot measure it. */
	per_tick = frequency / tick_rate;
	if (per_tick == 0U)
		return;

	/* The whole ticks the CPU slept, the rest kept for next time. */
	elapsed = counter - suspend_counter + suspend_remainder;
	ticks = elapsed / per_tick;
	suspend_remainder = elapsed % per_tick;

	/* The shared tick count forward; the deadlines compare with <= and take the jump. */
	if (ticks != 0U)
		(void)atomic_u64_fetch_add_relaxed(&kernel_ticks, ticks);
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

	/* Applies the CPU performance work posted for this CPU, if any. */
	kern_cpuperf_notify(cpu);

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
	uint64_t milliseconds;

	(void)context;

	/* Scales the tick counter to milliseconds. */
	ticks = clock_ticks();
	milliseconds = kern_ticks_to_ms(ticks);

	/* Reports the elapsed milliseconds. */
	return milliseconds;
}

/*
 * Converts a time in milliseconds to ticks, rounding up.
 *
 * At 100 Hz a 2 ms recovery interval becomes one tick, 10 ms, and never
 * zero, which would not wait at all.  Zero stays zero.
 */
uint64_t
kern_ms_to_ticks(
	uint64_t milliseconds)
{
	uint64_t ticks;

	/* Rounds the time up to the ticks that cover it. */
	ticks = scale_ceil(milliseconds, tick_rate, 1000U);

	/* Reports the ticks that cover the time. */
	return ticks;
}

/*
 * Converts a time in microseconds to ticks, rounding up.
 */
uint64_t
kern_us_to_ticks(
	uint64_t microseconds)
{
	uint64_t ticks;

	/* Rounds the time up to the ticks that cover it. */
	ticks = scale_ceil(microseconds, tick_rate, 1000000U);

	/* Reports the ticks that cover the time. */
	return ticks;
}

/*
 * Converts a time in nanoseconds to ticks, rounding up.
 */
uint64_t
kern_ns_to_ticks(
	uint64_t nanoseconds)
{
	uint64_t ticks;

	/* Rounds the time up to the ticks that cover it. */
	ticks = scale_ceil(nanoseconds, tick_rate, KERN_NSEC_PER_SEC);

	/* Reports the ticks that cover the time. */
	return ticks;
}

/*
 * Converts a number of ticks to milliseconds, rounding down.
 */
uint64_t
kern_ticks_to_ms(
	uint64_t ticks)
{
	uint64_t milliseconds;

	/* Rounds the ticks down to whole milliseconds. */
	milliseconds = scale_floor(ticks, 1000U, tick_rate);

	/* Reports no more time than the ticks have counted. */
	return milliseconds;
}

/*
 * Converts a number of ticks to microseconds, rounding down.
 */
uint64_t
kern_ticks_to_us(
	uint64_t ticks)
{
	uint64_t microseconds;

	/* Rounds the ticks down to whole microseconds. */
	microseconds = scale_floor(ticks, 1000000U, tick_rate);

	/* Reports no more time than the ticks have counted. */
	return microseconds;
}

/*
 * Converts a number of ticks to nanoseconds, rounding down.
 */
uint64_t
kern_ticks_to_ns(
	uint64_t ticks)
{
	uint64_t nanoseconds;

	/* Rounds the ticks down to whole nanoseconds. */
	nanoseconds = scale_floor(ticks, KERN_NSEC_PER_SEC, tick_rate);

	/* Reports no more time than the ticks have counted. */
	return nanoseconds;
}

/*
 * Converts a number of ticks to a count at another fixed rate, rounding down.
 *
 * This is how the kernel hands a CPU time to a user ABI whose unit is fixed
 * (times(2), the process list), whatever the tick is on this machine.  The
 * rate is at most one count per nanosecond.
 */
uint64_t
kern_ticks_to_rate(
	uint64_t ticks,
	uint64_t rate)
{
	uint64_t count;

	/* Rounds the ticks down to whole counts at the other rate. */
	count = scale_floor(ticks, rate, tick_rate);

	/* Reports the whole counts the ticks have reached. */
	return count;
}

/*
 * Converts a number of ticks to a timespec, rounding down.
 */
void
kern_ticks_to_timespec(
	uint64_t ticks,
	struct timespec *result)
{
	uint64_t fraction;

	/* Whole seconds apart from the rest, so the product cannot overflow. */
	fraction = ticks % tick_rate;
	result->tv_sec = (time_t)(ticks / tick_rate);
	result->tv_nsec = (long)(fraction * KERN_NSEC_PER_SEC / tick_rate);
}

/*
 * Reports the time between two ticks, in nanoseconds, rounded up.
 *
 * It is the finest step clock_ticks() takes, and the longest a deadline
 * can wait past its time before it is noticed.  Like the rate, it is
 * asked for each time and never kept as a constant.
 */
uint64_t
kern_clock_resolution_ns(
	void)
{
	/* Reports one tick, rounded up so it is never shorter than it is. */
	return (KERN_NSEC_PER_SEC + tick_rate - 1U) / tick_rate;
}

/*
 * Reports the ticks in a second.
 *
 * Only a report whose ABI states its unit as a rate (hw.cputimes), or
 * arithmetic that must stay in whole ticks, asks for it; a time is
 * converted with the functions above instead.  The rate is fixed at boot
 * today, but a caller asks each time it needs it and never keeps it as a
 * constant.
 */
uint32_t
kern_clock_tick_rate(
	void)
{
	/* Reports the rate the tick counter advances at. */
	return tick_rate;
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

	/* Reports why the normalization failed. */
	error = kern_timespec_normalize(result);
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

	/* Reports why the addition failed. */
	error = kern_timespec_add(a, &negated, result);
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
	if (seconds > UINT64_MAX / tick_rate)
		return EOVERFLOW;

	/* Rounds the nanosecond fraction up to whole ticks. */
	fraction = ((uint64_t)duration->tv_nsec * tick_rate +
	    KERN_NSEC_PER_SEC - 1U) / KERN_NSEC_PER_SEC;

	/* Rejects a total that would overflow the tick count. */
	if (seconds * tick_rate > UINT64_MAX - fraction)
		return EOVERFLOW;

	*ticks = seconds * tick_rate + fraction;

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
	struct timespec uptime;
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
	kern_ticks_to_timespec(ticks, &uptime);
	monotonic.tv_sec = (int64_t)uptime.tv_sec;
	monotonic.tv_nsec = (int32_t)uptime.tv_nsec;
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
		result->tv_nsec = (long)kern_clock_resolution_ns();
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

/*
 * Sleeps for at least min_us microseconds, yielding the CPU.
 *
 * The earliest return is computed once from the monotonic counter and a
 * wake never extends it.  Until it is reached the thread sleeps on a
 * private wait queue for the time that is left, and re-reads the counter
 * when it wakes: a deadline counts from the last tick, so a wake may come
 * up to a tick early.  The wake is at tick granularity
 * (kern_clock_resolution_ns()) and may be later than max_us, which is
 * advisory; callers re-check their condition after the sleep.  The wait
 * queue's token is removed on return, so no timer state outlives the call.
 */
void
kern_usleep_range(
	unsigned min_us,
	unsigned max_us)
{
	struct wait_queue queue;
	struct spinlock lock;
	unsigned long enabled;
	uint64_t base;
	uint64_t frequency;
	uint64_t now;
	uint64_t now_frequency;
	uint64_t earliest;
	uint64_t left_us;
	uint64_t sequence;
	uint64_t deadline;
	bool counted;

	/* The upper bound is advisory: the tick decides when the thread wakes. */
	(void)max_us;

	/* Reads the counter the earliest return is measured on; without one there is nothing to wait on. */
	base = 0U;
	frequency = 0U;
	counted = kern_rtc_read_counter(&base, &frequency);
	if (!counted)
		return;
	if (frequency == 0U)
		return;

	/* The counter value at which min_us has passed. */
	earliest = base + ((uint64_t)min_us * frequency) / 1000000U;

	/* Prepares the private wait queue the thread sleeps on. */
	waitq_init(&queue, "usleep_range");
	spin_init(&lock, LOCK_RANK_USYNC, "usleep_range");

	/* Sleeps for what is left until the counter passes the earliest return. */
	for (;;) {
		/* A counter that fails or changes its rate ends the wait rather than spinning on it. */
		now = 0U;
		now_frequency = 0U;
		counted = kern_rtc_read_counter(&now, &now_frequency);
		if (!counted)
			return;
		if (now_frequency != frequency)
			return;

		/* Returns once min_us has passed. */
		if ((int64_t)(now - earliest) >= 0)
			return;

		/* The time left, rounded up to a whole microsecond. */
		left_us = scale_ceil(earliest - now, 1000000U, frequency);

		/*
		 * Sleeps for the time left.  The lock is taken with interrupts
		 * disabled, as the locked sleep requires: the spinlock does not
		 * hold off preemption, and a thread preempted between the lock
		 * and the sleep may resume on another CPU, whose release traps.
		 */
		enabled = spin_lock_irqsave(&lock);

		sequence = waitq_sequence(&queue);
		deadline = sched_ticks() + kern_us_to_ticks(left_us);
		(void)waitq_sleep(&queue, &lock, sequence, deadline, 0U);

		spin_unlock_irqrestore(&lock, enabled);
	}
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

/*
 * Scales a count by multiplier / divisor, rounding down and saturating.
 *
 * Whole divisors are taken apart from the rest so that the product of the
 * rest cannot overflow; both factors are at most a billion.
 */
static uint64_t
scale_floor(
	uint64_t value,
	uint64_t multiplier,
	uint64_t divisor)
{
	uint64_t whole;
	uint64_t part;

	/* A count of whole divisors too large to scale saturates. */
	whole = value / divisor;
	if (multiplier != 0U && whole > UINT64_MAX / multiplier)
		return UINT64_MAX;

	/* The rest, scaled on its own; the sum saturates as well. */
	part = value % divisor * multiplier / divisor;
	if (whole * multiplier > UINT64_MAX - part)
		return UINT64_MAX;

	/* Succeeded: the scaled count. */
	return whole * multiplier + part;
}

/*
 * Scales a count by multiplier / divisor, rounding up and saturating.
 */
static uint64_t
scale_ceil(
	uint64_t value,
	uint64_t multiplier,
	uint64_t divisor)
{
	uint64_t whole;
	uint64_t part;

	/* A count of whole divisors too large to scale saturates. */
	whole = value / divisor;
	if (multiplier != 0U && whole > UINT64_MAX / multiplier)
		return UINT64_MAX;

	/* The rest, scaled on its own and rounded up; the sum saturates as well. */
	part = (value % divisor * multiplier + divisor - 1U) / divisor;
	if (whole * multiplier > UINT64_MAX - part)
		return UINT64_MAX;

	/* Succeeded: the scaled count. */
	return whole * multiplier + part;
}
