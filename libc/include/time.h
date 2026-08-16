/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_TIME_H
#define ZEDBSD_TIME_H

#include <stdint.h>

typedef int64_t time_t;
typedef int clockid_t;
#define CLOCK_MONOTONIC 1
#define CLOCK_REALTIME  2
#define TIMER_ABSTIME   1
#define UTIME_NOW  1073741823L
#define UTIME_OMIT 1073741822L
struct timespec { time_t tv_sec; long tv_nsec; };
time_t time(time_t *result);
int clock_gettime(clockid_t, struct timespec *);
int clock_getres(clockid_t, struct timespec *);
int clock_settime(clockid_t, const struct timespec *);
int nanosleep(const struct timespec *, struct timespec *);
int clock_nanosleep(clockid_t, int, const struct timespec *, struct timespec *);

#endif
