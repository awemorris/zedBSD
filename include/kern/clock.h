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
 * sched_ticks(), and every deadline is a tick count.  A tick here is a
 * unit of time, not a timer interrupt, whose rate changes as the CPUs
 * idle.  How long a tick is belongs to the clock alone: no other code may
 * assume a length or a rate, or multiply or divide by one of its own.  A
 * time is written in its own unit and turned into ticks, or back, by the
 * functions below.
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
clock_ticks_ceil(void);

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
 * The timer interrupt's rate on each CPU.  The idle loop asks for a
 * choice as it halts, giving the nearest deadline the CPU watches (or
 * UINT64_MAX); the scheduler brings the busy rate back as the CPU leaves
 * its idle thread, and accounts the idle time when that reports true.
 * Both run on the CPU itself with interrupts disabled.
 */
void
kern_clock_tick_idle(
	unsigned cpu,
	uint64_t next);

bool
kern_clock_tick_busy(
	unsigned cpu);

/*
 * The timers that only CPU 0's tick advances (the process timers and
 * ITIMER_REAL).  Its tick reports their nearest deadline with
 * kern_clock_timer_due(); arming or moving one elsewhere calls
 * kern_clock_timer_armed() with its deadline in ticks (0 when it is not
 * known) after the timer is published.
 */
void
kern_clock_timer_due(
	uint64_t deadline);

void
kern_clock_timer_armed(
	uint64_t deadline);

/*
 * Read the monotonic counter and its frequency.
 *
 * The epoch is unspecified; only differences between samples are
 * meaningful. Reports false when no counter is available.
 */
bool kern_rtc_read_counter(uint64_t *counter, uint64_t *frequency_hz);

/*
 * Sleeps for at least `min_us` microseconds off the monotonic counter,
 * yielding the CPU.  The wake comes at tick granularity, so `max_us` is
 * advisory and the sleep may last longer.
 */
void kern_usleep_range(unsigned min_us, unsigned max_us);

#endif
