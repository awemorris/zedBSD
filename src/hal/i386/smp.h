/* i386 SMP private interface. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_HAL_I386_SMP_H
#define ZEDBSD_HAL_I386_SMP_H
#include "apic-topology.h"
void i386_smp_configure(const struct i386_apic_topology *);
void i386_smp_ap_entry(uint32);
void *i386_smp_bootstrap_stack(hal_cpu_id_t);
int i386_smp_apic_id(hal_cpu_id_t, uint8 *);
#endif
