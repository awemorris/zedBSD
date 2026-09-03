/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The amd64 I/O APIC routing contract.
 */

#ifndef ZEDBSD_HAL_AMD64_IOAPIC_H
#define ZEDBSD_HAL_AMD64_IOAPIC_H

#include <hal/types.h>
#include "acpi.h"

int amd64_ioapic_init(const struct amd64_acpi_info *acpi, uint32_t bootstrap_apic_id);
void amd64_ioapic_mask(int irq);
void amd64_ioapic_unmask(int irq);
int amd64_ioapic_route(int irq, uint32_t apic_id);

#endif
