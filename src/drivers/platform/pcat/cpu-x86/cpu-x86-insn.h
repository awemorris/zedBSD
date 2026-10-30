/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The three processor instructions the x86 processor driver executes:
 * CPUID, RDMSR and WRMSR.
 *
 * Each acts on the logical processor it runs on.  Nothing here checks
 * that a register exists: a model-specific register the processor does
 * not have faults, and the kernel stops.  The driver decides from CPUID
 * and its allow list which registers it may touch, and touches no other.
 */

#ifndef DRIVERS_PLATFORM_PCAT_CPU_X86_CPU_X86_INSN_H
#define DRIVERS_PLATFORM_PCAT_CPU_X86_CPU_X86_INSN_H

#include <stdint.h>

#include "cpux86.h"

/* Runs CPUID for one leaf and sub-leaf on this logical processor. */
void
drv_cpu_x86_cpuid(
	uint32_t leaf,
	uint32_t subleaf,
	struct cpux86_cpuid_regs *regs);

#if CONFIG_CPU_PERF

/* Reads one model-specific register of this logical processor. */
uint64_t
drv_cpu_x86_rdmsr(
	uint32_t msr);

/* Writes one model-specific register of this logical processor. */
void
drv_cpu_x86_wrmsr(
	uint32_t msr,
	uint64_t value);

#endif

#endif
