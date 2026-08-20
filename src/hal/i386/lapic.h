/* Local APIC private interface. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_HAL_I386_LAPIC_H
#define ZEDBSD_HAL_I386_LAPIC_H
#include <hal/hal.h>
int i386_lapic_init(uint32 address);
void i386_lapic_init_cpu(void);
uint8 i386_lapic_id(void);
void i386_lapic_eoi(void);
int i386_lapic_send_init(uint8);
int i386_lapic_send_startup(uint8,uint8);
int i386_lapic_send_fixed(uint8,uint8);
void i386_lapic_timer_prepare(void);
uint32 i386_lapic_timer_elapsed(void);
void i386_lapic_timer_start(uint32);
void i386_lapic_timer_stop(void);
#endif
