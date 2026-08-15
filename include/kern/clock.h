/*
 * Clock
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_KERN_CLOCK_H
#define ZEDBSD_KERN_CLOCK_H

#include <stdint.h>
#include <time.h>

struct ucred;

#define KERN_CLOCK_HZ 100U
#define KERN_NSEC_PER_SEC 1000000000ULL

struct kern_timespec {
	int64_t tv_sec;
	int32_t tv_nsec;
};

uint64_t zedbsd_kernel_ticks(void);
uint64_t zedbsd_kernel_milliseconds(void *context);
void zedbsd_clock_realtime(time_t *seconds, long *nanoseconds);
int kern_timespec_validate(const struct timespec *);
int kern_timespec_normalize(struct kern_timespec *);
int kern_timespec_add(const struct kern_timespec *,
	const struct kern_timespec *, struct kern_timespec *);
int kern_timespec_sub(const struct kern_timespec *,
	const struct kern_timespec *, struct kern_timespec *);
int kern_timespec_compare(const struct kern_timespec *,
	const struct kern_timespec *);
int kern_duration_to_ticks_ceil(const struct timespec *, uint64_t *);
int kern_deadline_after(uint64_t, uint64_t, uint64_t *);
uint64_t kern_deadline_remaining(uint64_t, uint64_t);
int kern_clock_gettime(clockid_t, struct timespec *);
int kern_clock_getres(clockid_t, struct timespec *);
int kern_clock_settime(clockid_t, const struct timespec *,
	const struct ucred *);
int kern_clock_realtime_synchronized(void);
int kern_cpu_notify_probe(void);

#endif
