/* i386 APIC topology shared by the ACPI and MPS discovery frontends. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_HAL_I386_APIC_TOPOLOGY_H
#define ZEDBSD_HAL_I386_APIC_TOPOLOGY_H

#include <hal/hal.h>

#define I386_APIC_MAX_CPUS 8U
#define I386_IOAPIC_MAX 4U
#define I386_APIC_ROUTE_MAX 32U

struct i386_apic_cpu {
	uint8_t apic_id;
	uint8_t bootstrap;
};

struct i386_ioapic_desc {
	uint8_t apic_id;
	uint32_t address;
	uint32_t gsi_base;
};

struct i386_apic_route {
	uint8_t source_irq;
	uint8_t ioapic_id;
	uint8_t ioapic_pin;
	uint8_t polarity_low;
	uint8_t level_triggered;
};

struct i386_apic_topology {
	uint32_t lapic_address;
	unsigned cpu_count;
	unsigned ioapic_count;
	unsigned route_count;
	uint8_t imcr_present;
	struct i386_apic_cpu cpus[I386_APIC_MAX_CPUS];
	struct i386_ioapic_desc ioapics[I386_IOAPIC_MAX];
	struct i386_apic_route routes[I386_APIC_ROUTE_MAX];
};

int i386_mps_discover(struct i386_apic_topology *);
int i386_acpi_discover(struct i386_apic_topology *);

#endif
