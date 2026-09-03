/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The private i386 SMP startup and notification contract.
 */

#ifndef ZEDBSD_HAL_I386_SMP_H
#define ZEDBSD_HAL_I386_SMP_H

#include "apic-topology.h"

void i386_smp_configure(const struct i386_apic_topology *);
void i386_smp_ap_entry(uint32_t);
void *i386_smp_bootstrap_stack(hal_cpu_id_t);
int i386_smp_apic_id(hal_cpu_id_t, uint8_t *);
int i386_smp_send_tlb(hal_cpu_id_t);

#endif
