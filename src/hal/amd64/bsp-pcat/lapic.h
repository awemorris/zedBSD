#ifndef ZEDBSD_HAL_AMD64_LAPIC_H
#define ZEDBSD_HAL_AMD64_LAPIC_H

#include <hal/types.h>

struct amd64_acpi_info;

int amd64_lapic_init(const struct amd64_acpi_info *acpi);
int amd64_lapic_init_secondary(uint32_t expected_apic_id,
	unsigned *failure_reason);
uint32_t amd64_lapic_id(void);
void amd64_lapic_eoi(void);
int amd64_lapic_timer_start(void);
void amd64_lapic_timer_stop(void);
int amd64_lapic_send_init(uint32_t apic_id);
int amd64_lapic_send_startup(uint32_t apic_id, uint8_t vector);
int amd64_lapic_notify(uint32_t apic_id);
int amd64_lapic_send_vector(uint32_t apic_id, uint8_t vector);
_Noreturn void amd64_lapic_panic_all(void);

#endif
