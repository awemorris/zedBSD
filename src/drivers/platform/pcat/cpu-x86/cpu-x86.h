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

#endif
