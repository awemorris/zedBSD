/* Runtime-selected i386 interrupt-controller backend. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_HAL_I386_INTERRUPT_CONTROLLER_H
#define ZEDBSD_HAL_I386_INTERRUPT_CONTROLLER_H
#include <hal/hal.h>
int i386_interrupt_select(void);
int i386_interrupt_uses_apic(void);
int i386_interrupt_validate(int);
void i386_interrupt_mask(int);
void i386_interrupt_unmask(int);
void i386_interrupt_eoi(int);
int i386_interrupt_route(int,hal_cpu_id_t);
int i386_interrupt_calibration_tick(void);
uint32_t i386_interrupt_timer_ticks(void);
#endif
