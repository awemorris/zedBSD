/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The receiving side of an ARM PrimeCell UART (PL011) behind a GIC.
 *
 * The hardware layer has already set the line up and writes the
 * console's output through it; this driver leaves that alone and takes
 * what the line receives, by interrupt, for whoever started it.
 */

#ifndef KERN_DRIVERS_SERIAL_PL011_H
#define KERN_DRIVERS_SERIAL_PL011_H

#include <stdbool.h>
#include <stdint.h>

struct drv_fdt;

/* Receives one byte the line delivered; called in interrupt context. */
typedef void (*drv_pl011_receive_t)(uint8_t byte);

/*
 * One UART the device tree describes.
 *
 * The caller owns it and keeps it for the life of the system once input is
 * started, because the interrupt handler reads it.  drv_pl011_probe()
 * fills every field but receive and input_started, which
 * drv_pl011_start_input() sets once.
 */
struct drv_pl011 {
	uint64_t physical;
	uintptr_t base;
	int irq;
	drv_pl011_receive_t receive;
	bool input_started;
};

/*
 * Finds the first present PL011 the tree describes and maps its registers.
 *
 * Returns 0, ENOENT when the tree has no such UART, or another errno.
 */
int drv_pl011_probe(const struct drv_fdt *fdt, struct drv_pl011 *port);

/*
 * Starts taking received bytes by interrupt and handing them to receive.
 *
 * Bytes the line held before the start are dropped.  Starting twice is
 * refused with EBUSY.  Returns 0 or an errno.
 */
int drv_pl011_start_input(struct drv_pl011 *port, drv_pl011_receive_t receive);

#endif
