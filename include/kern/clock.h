/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Clock
 */

#ifndef ZEDBSD_KERN_CLOCK_H
#define ZEDBSD_KERN_CLOCK_H

#include <stdint.h>
#include <time.h>

struct ucred;

#define KERN_CLOCK_HZ	100U
#define KERN_NSEC_PER_SEC	1000000000ULL

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

#endif
