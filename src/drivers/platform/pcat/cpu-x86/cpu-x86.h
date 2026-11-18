/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The x86 processor driver: it tells the CPU performance framework where
 * each CPU sits and what kind of core it is, and, in a kernel built with
 * the performance control, starts and requests each CPU's
 * hardware-controlled performance and reads its energy, its hardware
 * feedback and its idle residency.
 */

#ifndef DRIVERS_PLATFORM_PCAT_CPU_X86_CPU_X86_H
#define DRIVERS_PLATFORM_PCAT_CPU_X86_CPU_X86_H

#include <kern/types.h>

/*
 * Describes the CPUs to the performance framework.
 *
 * Thread context, once, after the framework is up and every CPU takes
 * notifications; sleeps while each CPU reads its place.  Returns 0,
 * ENODEV for a processor the driver does not serve or when the boot
 * parameter cpuperf=off turns it off, ENOMEM, or the framework's refusal.
 */
int
drv_cpu_x86_init(void);

/*
 * Reads, on a CPU itself as it starts, its x2APIC ID and the kind of its
 * core, which drv_cpu_x86_init() later turns into the scheduler's table of
 * where each CPU sits.
 *
 * On the CPU, once, while it starts with interrupts masked; executes
 * CPUID only and keeps the answer in a table of its own.
 */
void
drv_cpu_x86_cpu_start(
	kern_cpu_id_t cpu);

#endif
