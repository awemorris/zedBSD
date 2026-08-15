/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_TIME_H
#define ZEDBSD_TIME_H

#include <stdint.h>

#ifdef ZEDBSD_USER_ABI_LP64
typedef int64_t time_t;
#else
typedef int32_t time_t;
#endif
typedef int clockid_t;
#define CLOCK_MONOTONIC 1
#define CLOCK_REALTIME  2
#define UTIME_NOW  1073741823L
#define UTIME_OMIT 1073741822L
struct timespec { time_t tv_sec; long tv_nsec; };
time_t time(time_t *result);
int clock_gettime(clockid_t, struct timespec *);
int nanosleep(const struct timespec *, struct timespec *);

#endif
