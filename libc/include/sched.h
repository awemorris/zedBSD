/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_SCHED_H
#define ZEDBSD_SCHED_H

#include <sys/types.h>

#define SCHED_OTHER 0
#define SCHED_FIFO  1
#define SCHED_RR    2

struct sched_param {
	int sched_priority;
};

int sched_yield(void);

#endif
