/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The state the two halves of the x86 processor driver share: the
 * description of the machine and of each CPU, and the calls from the
 * part every amd64 kernel has (topology) into the part only a kernel
 * with the performance control has (the model-specific registers).
 */

#ifndef DRIVERS_PLATFORM_PCAT_CPU_X86_CPU_X86_PRIVATE_H
#define DRIVERS_PLATFORM_PCAT_CPU_X86_CPU_X86_PRIVATE_H

#include <stdbool.h>
#include <stdint.h>

#include <kern/cpuperf.h>
#include <kern/types.h>

#include "cpux86.h"
#include "cpux86-cpuid.h"
#include "cpux86-hwp.h"

/* A package index that names no package. */
#define CPU_X86_PACKAGE_NONE	0xffffffffU

/*
 * What the driver found about the whole machine, once, on the CPU that
 * ran its initialization.
 *
 * Filled before the provider is registered and read-only after that, so
 * the CPUs read it in their notifications without a lock.  Every CPU of
 * an x86 machine reports the same identity and the same leaf 01H
 * features; the per-CPU leaves are kept in struct cpu_x86_cpu.
 */
struct cpu_x86_machine {
	struct cpux86_identity identity;
	struct cpux86_features features;
	struct cpux86_extended extended;
	uint32_t hybrid;		/* leaf 07H EDX[15], 0 or 1 */
	bool msr;			/* the model-specific registers may be touched at all */
	bool hwp;			/* HWP is the way the CPUs are controlled */
	bool hwp_enabled_at_boot;	/* the firmware or a previous system enabled HWP */
	bool counters;			/* IA32_APERF and IA32_MPERF exist */
	bool feedback;			/* the hardware feedback interface is used */
	uint32_t model_registers;	/* CPUX86_MODEL_* the allow list names, 0 off the list */
	uint32_t base_mhz;		/* the maximum non-turbo frequency, 0 unknown */
	uint64_t tsc_hz;		/* the rate of the TSC-rate counters, 0 unknown */
};

/*
 * What the driver keeps of one CPU.
 *
 * The CPU's own notifications write it, on that CPU: the topology read
 * when the provider registers, the start of its control, its samples and
 * measurements.  The threads that call describe and feedback_read read
 * the fields the topology read filled, after the framework has waited for
 * that read to finish.
 */
struct cpu_x86_cpu {
	bool read;			/* the topology read ran on this CPU */
	uint32_t x2apic_id;
	uint32_t power_features;	/* CPUX86_POWER_* of this CPU's leaf 06H */
	uint32_t feedback_row;		/* this CPU's row of the feedback table */
	struct cpux86_mwait mwait;
	bool mwait_known;
	uint32_t package_index;		/* the driver's package slot, CPU_X86_PACKAGE_NONE */
	bool hwp_enabled;		/* HWP_ENABLE was seen set on this CPU */
	bool capabilities_known;
	struct cpux86_hwp_capabilities capabilities;
	bool sample_marked;
	uint64_t sample_aperf;
	uint64_t sample_mperf;
	bool measure_marked;
	uint64_t measure_aperf;
	uint64_t measure_mperf;
};

/*
 * The machine, filled by drv_cpu_x86_init() before the provider exists.
 */
extern struct cpu_x86_machine drv_cpu_x86_machine;

/*
 * One entry per kernel CPU, indexed by the CPU number, allocated by
 * drv_cpu_x86_init() for the kernel's life; NULL before that.
 */
extern struct cpu_x86_cpu *drv_cpu_x86_cpus;

/* The number of entries of drv_cpu_x86_cpus. */
extern uint32_t drv_cpu_x86_cpu_count;

#if CONFIG_CPU_PERF

/*
 * Decides which model-specific registers the driver may touch.
 *
 * Thread context, on the CPU that runs the initialization, before the
 * provider is registered.  Fills the gates of drv_cpu_x86_machine; reads
 * only registers that CPUID and the allow list name.
 */
void
drv_cpu_x86_control_probe(
	bool hwp_off);

/*
 * Reads what a CPU's control has before its first request.
 *
 * In the CPU's topology notification, interrupts masked.
 */
void
drv_cpu_x86_control_cpu(
	struct cpu_x86_cpu *state);

/*
 * Fills the operations of the provider that touch the registers.
 */
void
drv_cpu_x86_control_ops(
	struct kern_cpuperf_provider_ops *ops);

/*
 * Prepares what needs the CPUs' places: the feedback tables of the
 * packages and the energy meters.
 *
 * Thread context, after the provider is registered.
 */
void
drv_cpu_x86_control_attach(void);

#endif

#endif
