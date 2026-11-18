/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The interrupts of RP1's peripherals.
 *
 * A peripheral's interrupt is one of RP1's MSI-X vectors.  RP1 writes the
 * vector's message to the BCM2712's MSI receiver (the MIP), which raises
 * one of its shared GIC lines.
 */

#ifndef KERN_DRIVERS_PLATFORM_RPI5_RP1_IRQ_H
#define KERN_DRIVERS_PLATFORM_RPI5_RP1_IRQ_H

#include <stdint.h>

/* The trigger codes of an RP1 interrupt specifier's second cell. */
#define DRV_RP1_IRQ_EDGE	1U
#define DRV_RP1_IRQ_LEVEL	4U

/*
 * A handler of an RP1 interrupt.  It runs in interrupt context with
 * interrupts off and must not sleep; the module ends the interrupt.
 */
typedef void (*drv_rp1_irq_handler_t)(void *argument);

/*
 * Finds the MSI receiver and RP1's MSI-X table and enables them.
 * drv_rp1_init() must have succeeded.  Calling it again does nothing.
 */
int drv_rp1_irq_init(void);

/*
 * Reads the vector and trigger of an rp1 child's interrupt (its index-th
 * specifier).
 */
int drv_rp1_irq_of(uint32_t node, unsigned index, unsigned *vector, unsigned *trigger);

/*
 * Routes one of RP1's vectors to a handler: a free MIP line is taken, the
 * vector's MSI-X entry is pointed at it, the vector is enabled in RP1
 * and the line is unmasked in the GIC.  Returns 0, ENOSPC when no MIP
 * line is free, or another errno.
 */
int drv_rp1_irq_establish(unsigned vector, unsigned trigger, drv_rp1_irq_handler_t handler, void *argument);

#endif
