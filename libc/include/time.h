/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef BOOTS_TIME_H
#define BOOTS_TIME_H

typedef long time_t;
typedef int clockid_t;
#define CLOCK_MONOTONIC 1
struct timespec { time_t tv_sec; long tv_nsec; };
time_t time(time_t *result);
int clock_gettime(clockid_t, struct timespec *);
int nanosleep(const struct timespec *, struct timespec *);

#endif
