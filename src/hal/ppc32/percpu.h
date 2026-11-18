/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The state the processor keeps for itself.  The port is uniprocessor, so
 * there is one record.
 */

#ifndef KERN_HAL_M68K_PPC32_PERCPU_H
#define KERN_HAL_M68K_PPC32_PERCPU_H

#include <hal/hal.h>

/* The acknowledgement of the tick, which needs no controller. */
#define PPC32_ACK_LOCAL	((hal_irq_ack_t)0xffffffffU)

struct ppc32_task;

struct ppc32_percpu {
	/* The processor's number in the HAL, always 0. */
	hal_cpu_id_t logical_id;

	/* The task running on the processor. */
	struct ppc32_task *running_task;

	/*
	 * The task whose context is still being left: hal_task_context_switch()
	 * records it before the stack switch, and the arriving context marks
	 * it as no longer running.
	 */
	struct ppc32_task *switching_from;

	/* The acknowledgement of the interrupt being handled, or none. */
	hal_irq_ack_t active_ack;

	/* How many scheduling ticks the processor has taken. */
	unsigned long timer_ticks;
};

struct ppc32_percpu *
ppc32_percpu_current(
	void);

#endif
