/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_SYS_RESOURCE_H
#define ZEDBSD_SYS_RESOURCE_H

#include <stdint.h>
#include <zedbsd/resource.h>

typedef uint64_t rlim_t;
struct rlimit {
	rlim_t rlim_cur;
	rlim_t rlim_max;
};

int getrlimit(int, struct rlimit *);
int setrlimit(int, const struct rlimit *);

#endif
