/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_SYS_TIME_H
#define ZEDBSD_SYS_TIME_H

#include <time.h>

struct timeval {
	time_t tv_sec;
	long tv_usec;
};

int gettimeofday(struct timeval *, void *);

#endif
