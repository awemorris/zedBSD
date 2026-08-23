/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_SYS_RESOURCE_H
#define ZEDBSD_SYS_RESOURCE_H

#include <stdint.h>
#include <zedbsd/resource.h>
#include <sys/time.h>

typedef uint64_t rlim_t;
struct rlimit {
	rlim_t rlim_cur;
	rlim_t rlim_max;
};

#define PRIO_PROCESS 0
#define PRIO_PGRP 1
#define PRIO_USER 2
#define RUSAGE_SELF 0
#define RUSAGE_CHILDREN (-1)
struct rusage {
	struct timeval ru_utime, ru_stime;
	long ru_maxrss, ru_ixrss, ru_idrss, ru_isrss;
	long ru_minflt, ru_majflt, ru_nswap, ru_inblock, ru_oublock;
	long ru_msgsnd, ru_msgrcv, ru_nsignals, ru_nvcsw, ru_nivcsw;
};

int getrlimit(int, struct rlimit *);
int setrlimit(int, const struct rlimit *);
int getpriority(int, id_t);
int setpriority(int, id_t, int);
int getrusage(int, struct rusage *);

#endif
