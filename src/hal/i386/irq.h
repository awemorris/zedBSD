/*
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 *
 * IRQ numbers and the ISR-task registry.
 */

#ifndef HAL_I386_IRQ_H
#define HAL_I386_IRQ_H

#include <hal/hal.h>

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
	unsigned in_flight[HAL_CPU_MAX];
	unsigned in_handler[HAL_CPU_MAX];
	struct hal_cpu_mask requested;
};

void
irq_init(void);

/* called from int.c */
void
irq_handler(int irq_num);

int
i386_interrupt_select(void);

int
i386_interrupt_uses_apic(void);

int
i386_interrupt_validate(
	int irq);

void
i386_interrupt_mask(
	int irq);

void
i386_interrupt_unmask(
	int irq);

void
i386_interrupt_eoi(
	int irq);

int
i386_interrupt_route(
	int irq,
	hal_cpu_id_t cpu);

int
i386_interrupt_calibration_tick(void);

uint32_t
i386_interrupt_timer_ticks(void);

#endif
