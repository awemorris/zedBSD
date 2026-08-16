/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_KERN_RESOURCE_LIMIT_H
#define ZEDBSD_KERN_RESOURCE_LIMIT_H

#include <stdint.h>
#include <zedbsd/resource.h>

struct process;
struct vmspace;

struct process_limits {
	struct zedbsd_rlimit values[RLIMIT_NLIMITS];
};

void resource_limits_default(struct process_limits *);
int resource_limit_get(struct process *, int, struct zedbsd_rlimit *);
int resource_limit_set(struct process *, int,
	const struct zedbsd_rlimit *);
int resource_limit_apply_vm(struct process *, struct vmspace *);
uint64_t resource_limit_current(struct process *, int);

#endif
