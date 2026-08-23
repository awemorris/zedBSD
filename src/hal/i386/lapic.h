/* Local APIC private interface. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_HAL_I386_LAPIC_H
#define ZEDBSD_HAL_I386_LAPIC_H
#include <hal/hal.h>
int i386_lapic_init(uint32_t address);
void i386_lapic_init_cpu(void);
uint8_t i386_lapic_id(void);
void i386_lapic_eoi(void);
int i386_lapic_send_init(uint8_t);
int i386_lapic_send_startup(uint8_t,uint8_t);
int i386_lapic_send_fixed(uint8_t,uint8_t);
void i386_lapic_timer_prepare(void);
uint32_t i386_lapic_timer_elapsed(void);
void i386_lapic_timer_start(uint32_t);
void i386_lapic_timer_stop(void);
#endif
