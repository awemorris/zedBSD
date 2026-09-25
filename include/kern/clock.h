/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Clock
 */

#ifndef KERN_KERN_CLOCK_H
#define KERN_KERN_CLOCK_H

#include <stdbool.h>
#include <stdint.h>
#include <hal/arch.h>
#include <uapi/time.h>

struct ucred;

/*
 * The kernel's tick, in hertz.
 *
 * It is the machine's timer rate and nothing else: each architecture chooses
 * it in its <hal/arch/ARCH.h> (HAL_TIMER_FREQUENCY), and the kernel only reads it.
 * No code may assume a particular value.  A time given in milliseconds is
 * turned into ticks with kern_ms_to_ticks(), and back with kern_ticks_to_ms();
 * nothing multiplies or divides by a number of its own.
 */
#define KERN_CLOCK_HZ	HAL_TIMER_FREQUENCY
#define KERN_NSEC_PER_SEC	1000000000ULL

/*
 * Converts a time in milliseconds to ticks, rounding up.
 *
 * Rounding up is what makes a wait safe at any rate: at 100 Hz a 2 ms
 * recovery interval becomes one tick, 10 ms, and never zero, which would
 * not wait at all.  Zero stays zero.  A time too long to count saturates.
 */
static inline uint64_t
kern_ms_to_ticks(
	uint64_t milliseconds)
{
	/* Handles a time too long to count in ticks. */
	if (milliseconds > (UINT64_MAX - 999U) / KERN_CLOCK_HZ)
		return UINT64_MAX;

	/* Returns the computed result. */
	return (milliseconds * KERN_CLOCK_HZ + 999U) / 1000U;
}

/*
 * The same conversion for a constant, so a driver can name its interval in
 * milliseconds where an expression must be constant.  The argument must be
 * small enough not to overflow; kern_ms_to_ticks() is for run-time values.
 */
#define KERN_MS_TO_TICKS(milliseconds)                                        \
	(((uint64_t)(milliseconds) * KERN_CLOCK_HZ + 999U) / 1000U)

/*
 * Converts a number of ticks to milliseconds, rounding down.
 *
 * Rounding down reports no more time than has passed.  A count too large
 * to express saturates.
 */
static inline uint64_t
kern_ticks_to_ms(
	uint64_t ticks)
{
	uint64_t seconds;

	/* Handles a count too large to express in milliseconds. */
	seconds = ticks / KERN_CLOCK_HZ;
	if (seconds > UINT64_MAX / 1000U - 1U)
		return UINT64_MAX;

	/* Returns the computed result. */
	return seconds * 1000U + (ticks % KERN_CLOCK_HZ) * 1000U / KERN_CLOCK_HZ;
}

/*
 * Converts a number of ticks to a count at another fixed rate, rounding down.
 *
 * This is how the kernel hands a CPU time to a user ABI whose unit is fixed
 * (times(2), the process list), whatever the tick is on this machine.  Whole
 * seconds are taken apart from the rest so the product cannot overflow.
 */
static inline uint64_t
kern_ticks_to_rate(
	uint64_t ticks,
	uint64_t rate)
{
	/* Returns the computed result. */
	return ticks / KERN_CLOCK_HZ * rate +
	    ticks % KERN_CLOCK_HZ * rate / KERN_CLOCK_HZ;
}

struct kern_timespec {
	int64_t tv_sec;
	int32_t tv_nsec;
};

void
kern_clock_init(void);

uint64_t
clock_ticks(void);

uint64_t
clock_milliseconds(
	void *context);

void
clock_realtime(
	time_t *seconds,
	long *nanoseconds);

int
kern_timespec_validate(
	const struct timespec *value);

int
kern_timespec_normalize(
	struct kern_timespec *value);

int
kern_timespec_add(
	const struct kern_timespec *a,
	const struct kern_timespec *b,
	struct kern_timespec *result);

int
kern_timespec_sub(
	const struct kern_timespec *a,
	const struct kern_timespec *b,
	struct kern_timespec *result);

int
kern_timespec_compare(
	const struct kern_timespec *a,
	const struct kern_timespec *b);

int
kern_duration_to_ticks_ceil(
	const struct timespec *duration,
	uint64_t *ticks);

int
kern_deadline_after(
	uint64_t now,
	uint64_t delta,
	uint64_t *deadline);

uint64_t
kern_deadline_remaining(
	uint64_t now,
	uint64_t deadline);

int
kern_clock_gettime(
	clockid_t clock,
	struct timespec *result);

int
kern_clock_getres(
	clockid_t clock,
	struct timespec *result);

int
kern_clock_settime(
	clockid_t clock,
	const struct timespec *requested,
	const struct ucred *cred);

int
kern_clock_realtime_synchronized(void);

int
kern_cpu_notify_probe(void);

/*
 * Read the monotonic counter and its frequency.
 *
 * The epoch is unspecified; only differences between samples are
 * meaningful. Reports false when no counter is available.
 */
bool kern_rtc_read_counter(uint64_t *counter, uint64_t *frequency_hz);

/*
 * Short high-resolution wait for at least `min_us` microseconds (bounded busy
 * delay off the monotonic counter).  A true yielding hrtimer-backed sleep-range
 * is a pending HAL item; this serves the microsecond-scale waits (usleep_range).
 */
void kern_usleep_range(unsigned min_us, unsigned max_us);

#endif
