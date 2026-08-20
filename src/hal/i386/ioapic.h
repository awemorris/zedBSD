/* I/O APIC private interface. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_HAL_I386_IOAPIC_H
#define ZEDBSD_HAL_I386_IOAPIC_H
#include "apic-topology.h"
int i386_ioapic_init(const struct i386_apic_topology*,uint8);
void i386_ioapic_mask(int);
void i386_ioapic_unmask(int);
int i386_ioapic_route(int,uint8);
#endif
