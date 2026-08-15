#ifndef ZEDBSD_HAL_AMD64_ACPI_H
#define ZEDBSD_HAL_AMD64_ACPI_H

#include <hal/types.h>
#include "../defs.h"

#define AMD64_IOAPIC_MAX 4U

struct amd64_acpi_cpu {
	uint32 apic_id;
};

struct amd64_acpi_ioapic {
	uint32 id;
	uint32 address;
	uint32 gsi_base;
};

struct amd64_acpi_iso {
	uint32 gsi;
	uint16 flags;
	uint8 source;
	uint8 present;
};

struct amd64_acpi_info {
	uint32 lapic_address;
	unsigned cpu_count;
	unsigned ioapic_count;
	struct amd64_acpi_cpu cpus[AMD64_SMP_MAX_CPUS];
	struct amd64_acpi_ioapic ioapics[AMD64_IOAPIC_MAX];
	struct amd64_acpi_iso isa[16];
};

int amd64_acpi_discover(struct amd64_acpi_info *result,
	hal_physaddr_t rsdp_address);

#endif
