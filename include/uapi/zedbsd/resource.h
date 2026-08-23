/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_UAPI_RESOURCE_H
#define ZEDBSD_UAPI_RESOURCE_H

#include <stdint.h>

#define RLIMIT_NOFILE 0
#define RLIMIT_STACK  1
#define RLIMIT_AS     2
#define RLIMIT_CORE   3
#define RLIMIT_CPU    4
#define RLIMIT_DATA   5
#define RLIMIT_FSIZE  6
#define RLIMIT_NLIMITS 7
#define RLIM_INFINITY UINT64_MAX

struct rlimit_record {
	uint64_t current;
	uint64_t maximum;
};

#endif
