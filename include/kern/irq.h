/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Interrupt registration and control for drivers.
 *
 * Drivers reach the interrupt controller through the kernel, never through
 * the HAL. The acknowledgement token is opaque: a handler receives one and
 * gives it back to kern_irq_send_eoi() before it returns.
 */

#ifndef KERN_IRQ_H
#define KERN_IRQ_H

#include <stdbool.h>
#include <stdint.h>

/* One pending acknowledgement, owned by the handler it was passed to. */
typedef uintptr_t kern_irq_ack_t;

#define KERN_IRQ_ACK_NONE ((kern_irq_ack_t)0)

/*
 * An interrupt handler.
 *
 * It runs in interrupt context, so it must not sleep, and it must call
 * kern_irq_send_eoi() with its acknowledgement before returning.
 */
typedef void (*kern_irq_handler_t)(int irq, kern_irq_ack_t acknowledge,
				   void *argument);

/*
 * Install or remove a handler for one numbered interrupt.
 *
 * Removal requires the same handler and argument that were registered.
 * Both report 0 on success.
 */
int kern_irq_register(int irq, kern_irq_handler_t handler, void *argument);
int kern_irq_unregister(int irq, kern_irq_handler_t handler, void *argument);

/*
 * Allocate one message-signalled interrupt for a device.
 *
 * source is a canonical bus identity such as "PCI 0000:00:1f.2". The output
 * values are published only after the handler is fully installed.
 */
int kern_irq_register_msi(const char *source, kern_irq_handler_t handler,
			  void *argument, int *mapped_irq,
			  uint64_t *mapped_address, uint32_t *mapped_event);
int kern_irq_unregister_msi(int mapped_irq);

/* Stop and resume delivery of one numbered interrupt. */
void kern_irq_mask(int irq);
void kern_irq_unmask(int irq);

/* Retire one acknowledgement. A handler must do this before it returns. */
void kern_irq_send_eoi(kern_irq_ack_t acknowledge);

/*
 * Disable and restore interrupt delivery on the current processor.
 *
 * kern_irq_disable() reports whether delivery was enabled beforehand, so
 * the caller can restore exactly that state.
 */
bool kern_irq_disable(void);
void kern_irq_enable(void);

#endif
