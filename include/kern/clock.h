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
#include <uapi/time.h>

struct ucred;

#define KERN_NSEC_PER_SEC	1000000000ULL

/*
 * The kernel's time is counted in ticks, from clock_ticks() and
 * sched_ticks(), and every deadline is a tick count.  How long a tick is
 * belongs to the clock alone: no other code may assume a length or a rate,
 * or multiply or divide by one of its own.  A time is written in its own
 * unit and turned into ticks, or back, by the functions below.
 *
 * A wait rounds up, so that it never ends early and a nonzero time is never
 * zero ticks; a report rounds down, so that it never claims more time than
 * has passed.  A value too large to express saturates at UINT64_MAX.
 */
uint64_t
kern_ms_to_ticks(
	uint64_t milliseconds);

uint64_t
kern_us_to_ticks(
	uint64_t microseconds);

uint64_t
kern_ns_to_ticks(
	uint64_t nanoseconds);

uint64_t
kern_ticks_to_ms(
	uint64_t ticks);

uint64_t
kern_ticks_to_us(
	uint64_t ticks);

uint64_t
kern_ticks_to_ns(
	uint64_t ticks);

uint64_t
kern_ticks_to_rate(
	uint64_t ticks,
	uint64_t rate);

void
kern_ticks_to_timespec(
	uint64_t ticks,
	struct timespec *result);

uint64_t
kern_clock_resolution_ns(void);

uint32_t
kern_clock_tick_rate(void);

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

void
kern_clock_idle_suspend_begin(void);

void
kern_clock_idle_suspend_end(void);

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
