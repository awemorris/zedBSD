/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The i386 IRQ-controller and service contract.
 */

#ifndef HAL_I386_IRQ_H
#define HAL_I386_IRQ_H

#include <hal/hal.h>

#include "apic-topology.h"

#define IRQ_MAX		(15)

/* Both boards put the interval timer on IRQ 0 and the keyboard on 1. */
#define IRQ_TIMER	(0)
#define IRQ_KEYBOARD	(1)

struct irq_service_info {
	int mode;
	hal_irq_handler_t handler;
	void *argument;
	hal_task_t waiter;
	hal_irq_ack_t acknowledge;
	unsigned pending;
	/*
	 * Per processor, for as many as the i386 HAL brings up: the topology
	 * discovery stops at I386_APIC_MAX_CPUS, so hal_cpu_current() is
	 * always below it.
	 */
	unsigned in_flight[I386_APIC_MAX_CPUS];
	unsigned in_handler[I386_APIC_MAX_CPUS];
	struct hal_cpu_mask requested;
};

void irq_init(void);

/* called from int.c */
void irq_handler(int irq_num);

int i386_interrupt_select(void);

int i386_interrupt_uses_apic(void);

int i386_interrupt_validate(int irq);

void i386_interrupt_spurious(int irq);

void i386_interrupt_mask(int irq);

void i386_interrupt_unmask(int irq);

void i386_interrupt_eoi(int irq);

int i386_interrupt_route(int irq, hal_cpu_id_t cpu);

int i386_interrupt_calibration_tick(void);

uint32_t i386_interrupt_timer_ticks(void);

#endif
