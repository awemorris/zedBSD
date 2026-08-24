/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_KERN_RESOURCE_LIMIT_H
#define ZEDBSD_KERN_RESOURCE_LIMIT_H

#include <stdint.h>
#include <zedbsd/resource.h>

struct process;
struct vmspace;

struct process_limits {
	struct rlimit_record values[RLIMIT_NLIMITS];
};

void
resource_limits_default(
	struct process_limits *limits);

int
resource_limit_get(
	struct process *process,
	int resource,
	struct rlimit_record *result);

int
resource_limit_set(
	struct process *process,
	int resource,
	const struct rlimit_record *requested);

int
resource_limit_apply_vm(
	struct process *process,
	struct vmspace *vm);

uint64_t
resource_limit_current(
	struct process *process,
	int resource);

void
resource_limit_cpu_tick(
	struct process *process,
	uint64_t total_ticks);

#endif
