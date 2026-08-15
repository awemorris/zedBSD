#ifndef ZEDBSD_HAL_AMD64_LAPIC_H
#define ZEDBSD_HAL_AMD64_LAPIC_H

#include <hal/types.h>

int amd64_lapic_init(uint32 physical_address);
void amd64_lapic_init_cpu(void);
uint32 amd64_lapic_id(void);
void amd64_lapic_eoi(void);
void amd64_lapic_timer_start(void);
void amd64_lapic_timer_stop(void);
int amd64_lapic_send_init(uint32 apic_id);
int amd64_lapic_send_startup(uint32 apic_id, uint8 vector);
int amd64_lapic_notify(uint32 apic_id);
int amd64_lapic_send_vector(uint32 apic_id, uint8 vector);
_Noreturn void amd64_lapic_panic_all(void);

#endif
