/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_TIME_H
#define ZEDBSD_TIME_H

#include <stdint.h>

/* Keep the existing ELF32 user ABI identical in an LP64 kernel build. */
typedef int32_t time_t;
typedef int clockid_t;
#define CLOCK_MONOTONIC 1
struct timespec { time_t tv_sec; int32_t tv_nsec; };
time_t time(time_t *result);
int clock_gettime(clockid_t, struct timespec *);
int nanosleep(const struct timespec *, struct timespec *);

#endif
