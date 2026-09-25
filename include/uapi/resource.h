/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The resource interface of zedBSD: the resource limits, scheduling
 * priorities and usage accounting a program exchanges with the kernel.
 * <sys/resource.h> of the C library includes this and adds the functions.
 */

#ifndef KERN_UAPI_RESOURCE_H
#define KERN_UAPI_RESOURCE_H

#include <stdint.h>
#include <uapi/hosted.h>

#define RLIMIT_NOFILE	0
#define RLIMIT_STACK	1
#define RLIMIT_AS	2
#define RLIMIT_CORE	3
#define RLIMIT_CPU	4
#define RLIMIT_DATA	5
#define RLIMIT_FSIZE	6
#define RLIMIT_NLIMITS	7
#define RLIM_INFINITY	UINT64_MAX

struct rlimit_record {
	uint64_t current;
	uint64_t maximum;
};

/*
 * The priorities, the limit record and the usage record.  A host fixture
 * takes these from the host C library when it needs them; the host's
 * <sys/resource.h> cannot be included here because it declares the resource
 * numbers above as enumerators.
 */
#if !KERN_UAPI_HOST_LIBC
#include <uapi/time.h>

#define PRIO_PROCESS 0
#define PRIO_PGRP 1
#define PRIO_USER 2
#define RUSAGE_SELF 0
#define RUSAGE_CHILDREN (-1)

typedef uint64_t rlim_t;

struct rlimit {
	rlim_t rlim_cur;
	rlim_t rlim_max;
};

struct rusage {
	struct timeval ru_utime, ru_stime;
	long ru_maxrss, ru_ixrss, ru_idrss, ru_isrss;
	long ru_minflt, ru_majflt, ru_nswap, ru_inblock, ru_oublock;
	long ru_msgsnd, ru_msgrcv, ru_nsignals, ru_nvcsw, ru_nivcsw;
};
#endif

#endif
