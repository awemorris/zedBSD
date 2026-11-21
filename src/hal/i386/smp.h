/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The private i386 SMP startup and notification contract.
 */

#ifndef HAL_I386_SMP_H
#define HAL_I386_SMP_H

#include "apic-topology.h"

/*
 * Nonzero once the boot CPU starts another CPU; never cleared.  A
 * uniprocessor build (HAL_CPU_UP) has no other CPU and no APIC code, and
 * the value is a constant zero.
 */
#if defined(HAL_CPU_UP)
#define i386_smp_started 0U
#else
extern volatile unsigned i386_smp_started;
#endif

/*
 * The current CPU's logical ID: the boot CPU's, 0, without asking the
 * APIC while it runs alone.
 */
static inline hal_cpu_id_t
i386_cpu_index(
	void)
{
	/* Only the boot CPU runs until another one starts. */
	if (!i386_smp_started)
		return 0;

	/* Asks the APIC which CPU runs this. */
	return hal_cpu_current();
}

void i386_smp_configure(const struct i386_apic_topology *);
void i386_smp_ap_entry(uint32_t);
void *i386_smp_bootstrap_stack(hal_cpu_id_t);
int i386_smp_apic_id(hal_cpu_id_t, uint8_t *);
int i386_smp_send_tlb(hal_cpu_id_t);

#endif
