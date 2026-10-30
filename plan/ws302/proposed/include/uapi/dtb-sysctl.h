/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The read-only report of the desktop thread balancer, kern.dtb.
 *
 * The proposal adds the name to <uapi/sysctl.h> next to the other KERN_
 * names; it is kept apart here only to show its layout.  Every field has a
 * fixed width and the record has no pointers, so 32-bit and 64-bit
 * programs read the same bytes.
 */

#ifndef UAPI_DTB_SYSCTL_H
#define UAPI_DTB_SYSCTL_H

#include <stdint.h>

/* The second-level name under CTL_KERN. */
#define KERN_DTB			11

#define KERN_DTB_VERSION		1U

/* The most CPUs the record describes; a machine with more lists the first ones. */
#define KERN_DTB_CPU_MAX		64U

/* What the record says beyond its numbers, as bits of flags. */
#define KERN_DTB_BUILT			0x1U	/* the kernel has the balancer */
#define KERN_DTB_RUNNING		0x2U	/* its thread runs (not dtb=off) */
#define KERN_DTB_HYBRID			0x4U	/* the CPUs are of two kinds */

/* The person at the machine, as the balancer sees it. */
#define KERN_DTB_STATE_AWAY		0U
#define KERN_DTB_STATE_QUIET		1U
#define KERN_DTB_STATE_ACTIVE		2U

/* The rules, which index rule_threads. */
#define KERN_DTB_RULE_NICE_FAST		0U
#define KERN_DTB_RULE_NICE_EFFICIENT	1U
#define KERN_DTB_RULE_BUSY		2U
#define KERN_DTB_RULE_PRESENTER		3U
#define KERN_DTB_RULE_GPU		4U
#define KERN_DTB_RULE_TERMINAL		5U
#define KERN_DTB_RULE_OTHER		6U
#define KERN_DTB_RULE_COUNT		8U	/* room for one more */

/* The kinds of CPU, as cpu_core reports them. */
#define KERN_DTB_CORE_ANY		0U
#define KERN_DTB_CORE_FAST		1U
#define KERN_DTB_CORE_EFFICIENT		2U

/*
 * One CPU as the balancer described it to the scheduler.
 */
struct dtb_cpu_info {
	uint8_t core;			/* KERN_DTB_CORE_* */
	uint8_t usable;
	uint16_t rank;			/* 0..1000 */
	uint16_t sibling_rank;
	uint16_t core_group;
};

/*
 * The balancer's report.  A reader checks version and struct_size; a
 * later version only adds fields at the end.
 */
struct dtb_info {
	uint32_t version;		/* KERN_DTB_VERSION */
	uint32_t struct_size;		/* sizeof(struct dtb_info) */
	uint32_t flags;			/* KERN_DTB_* */
	uint32_t state;			/* KERN_DTB_STATE_* */
	uint64_t passes;
	uint64_t pass_ns_max;
	uint64_t pass_ns_total;
	uint32_t rule_threads[KERN_DTB_RULE_COUNT];	/* threads each rule chose at the last pass */
	uint64_t hints;
	uint64_t moves;
	uint64_t refused;
	uint64_t handoffs;
	uint64_t handoff_retries;
	uint64_t handoff_failures;
	uint64_t starved;
	uint64_t kernel_held;
	uint64_t reserved[8];
	uint32_t cpu_count;		/* the CPUs described below */
	uint32_t reserved_cpu;
	struct dtb_cpu_info cpus[KERN_DTB_CPU_MAX];
};

#endif
