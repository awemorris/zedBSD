/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The amd64 symmetric-multiprocessing contract.
 */

#ifndef ZEDBSD_HAL_AMD64_SMP_H
#define ZEDBSD_HAL_AMD64_SMP_H

#include <hal/types.h>
#include "bsp-pcat/acpi.h"

void amd64_smp_init(const struct amd64_acpi_info *acpi);
void amd64_ap_entry(uint64_t logical_cpu) __attribute__((noreturn));
uint32_t amd64_smp_apic_id(hal_cpu_id_t cpu);
int amd64_smp_panic_available(void);

#endif
