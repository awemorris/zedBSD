#ifndef ZEDBSD_HAL_AMD64_IOAPIC_H
#define ZEDBSD_HAL_AMD64_IOAPIC_H

#include <hal/types.h>
#include "acpi.h"

int amd64_ioapic_init(const struct amd64_acpi_info *acpi,
	uint32 bootstrap_apic_id);
void amd64_ioapic_mask(int irq);
void amd64_ioapic_unmask(int irq);
int amd64_ioapic_route(int irq, uint32 apic_id);

#endif
