/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_SYS_TIME_H
#define ZEDBSD_SYS_TIME_H

#include <zedbsd/features.h>
#include <time.h>

struct timeval {
	time_t tv_sec;
	long tv_usec;
};

struct itimerval { struct timeval it_interval, it_value; };
#define ITIMER_REAL 0
#define ITIMER_VIRTUAL 1
#define ITIMER_PROF 2

int utimes(const char *, const struct timeval [2]);
#if __ZEDBSD_LEGACY_VISIBLE
int gettimeofday(struct timeval *, void *);
int getitimer(int, struct itimerval *);
int setitimer(int, const struct itimerval *, struct itimerval *);
#endif

#endif
