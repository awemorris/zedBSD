/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The receiving side of a Cadence UART, the Zynq-7000's.
 *
 * The hardware layer has already set the line up and writes the
 * console's output through it; this driver leaves that alone and takes
 * what the line receives, by interrupt, for whoever started it.
 */

#ifndef KERN_DRIVERS_SERIAL_CDNS_UART_H
#define KERN_DRIVERS_SERIAL_CDNS_UART_H

#include <stdbool.h>
#include <stdint.h>

/* Receives one byte the line delivered; called in interrupt context. */
typedef void (*drv_cdns_uart_receive_t)(uint8_t byte);

/*
 * One UART at an address and interrupt its board knows.
 *
 * The caller owns it and keeps it for the life of the system once input is
 * started, because the interrupt handler reads it.  drv_cdns_uart_attach()
 * fills every field but receive and input_started, which
 * drv_cdns_uart_start_input() sets once.
 */
struct drv_cdns_uart {
	uint64_t physical;
	uintptr_t base;
	int irq;
	drv_cdns_uart_receive_t receive;
	bool input_started;
};

/*
 * Maps the registers of the UART at physical, which raises the GIC
 * interrupt irq, with its interrupts off.
 *
 * Returns 0 or an errno.
 */
int drv_cdns_uart_attach(struct drv_cdns_uart *port, uint64_t physical, int irq);

/*
 * Starts taking received bytes by interrupt and handing them to receive.
 *
 * Bytes the line held before the start are dropped.  Starting twice is
 * refused with EBUSY.  Returns 0 or an errno.
 */
int drv_cdns_uart_start_input(struct drv_cdns_uart *port, drv_cdns_uart_receive_t receive);

#endif
