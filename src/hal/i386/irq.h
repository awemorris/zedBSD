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
	unsigned in_flight;
	unsigned in_handler;
	struct hal_cpu_mask requested;
};

/* irq.c */
void irq_init(void);
void irq_handler(int irq_num);	/* called from int.c */

#endif
