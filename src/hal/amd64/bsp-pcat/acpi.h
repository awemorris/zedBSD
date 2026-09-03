/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The amd64 ACPI topology and PCI ECAM discovery contract.
 */

#ifndef ZEDBSD_HAL_AMD64_ACPI_H
#define ZEDBSD_HAL_AMD64_ACPI_H

#include <hal/types.h>
#include "../defs.h"

#define AMD64_IOAPIC_MAX 4U
#define AMD64_ECAM_MAX   8U

struct amd64_acpi_cpu {
	uint32_t apic_id;
};

struct amd64_acpi_ioapic {
	uint32_t id;
	uint32_t address;
	uint32_t gsi_base;
};

struct amd64_acpi_iso {
	uint32_t gsi;
	uint16_t flags;
	uint8_t source;
	uint8_t present;
};

struct amd64_acpi_ecam {
	paddr_t address;
	uint16_t segment;
	uint8_t start_bus;
	uint8_t end_bus;
};

struct amd64_acpi_info {
	uint32_t lapic_address;
	unsigned cpu_count;
	unsigned ioapic_count;
	unsigned ecam_count;
	struct amd64_acpi_cpu cpus[AMD64_SMP_MAX_CPUS];
	struct amd64_acpi_ioapic ioapics[AMD64_IOAPIC_MAX];
	struct amd64_acpi_iso isa[16];
	struct amd64_acpi_ecam ecam[AMD64_ECAM_MAX];
};

int
amd64_acpi_discover(
	struct amd64_acpi_info *result,
	hal_physaddr_t rsdp_address);

int
amd64_acpi_parse_mcfg(
	const void *table,
	size_t available,
	struct amd64_acpi_ecam *regions,
	unsigned *region_count);

int
amd64_acpi_ecam_address(
	uint16_t segment,
	uint8_t bus,
	uint8_t device,
	uint8_t function,
	paddr_t *result);

int
amd64_acpi_ecam_pointer(
	uint16_t segment,
	uint8_t bus,
	uint8_t device,
	uint8_t function,
	volatile uint8_t **result);

#endif
