#ifndef ZEDBSD_HAL_AMD64_ACPI_H
#define ZEDBSD_HAL_AMD64_ACPI_H

#include <hal/types.h>
#include "../defs.h"

#define AMD64_IOAPIC_MAX 4U

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

struct amd64_acpi_info {
	uint32_t lapic_address;
	unsigned cpu_count;
	unsigned ioapic_count;
	struct amd64_acpi_cpu cpus[AMD64_SMP_MAX_CPUS];
	struct amd64_acpi_ioapic ioapics[AMD64_IOAPIC_MAX];
	struct amd64_acpi_iso isa[16];
};

int amd64_acpi_discover(struct amd64_acpi_info *result,
	hal_physaddr_t rsdp_address);

#endif
