/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A memory-mapped 16550-compatible UART.
 *
 * The firmware has already set the line up (its speed and framing) and
 * writes the console's output through it; this driver leaves that alone,
 * takes what the line receives, by interrupt, for whoever started it, and
 * sends bytes by polling for a kernel that writes the console itself.
 */

#ifndef KERN_DRIVERS_SERIAL_NS16550_H
#define KERN_DRIVERS_SERIAL_NS16550_H

#include <stdbool.h>
#include <stdint.h>

struct drv_fdt;

/* Receives one byte the line delivered; called in interrupt context. */
typedef void (*drv_ns16550_receive_t)(uint8_t byte);

/*
 * One UART the device tree describes.
 *
 * The caller owns it and keeps it for the life of the system once input is
 * started or bytes are sent, because the interrupt handler and the senders
 * read it.  drv_ns16550_probe() and drv_ns16550_probe_console() fill every
 * field but receive and input_started, which drv_ns16550_start_input()
 * sets once; irq is 0 for a console UART the tree gives no interrupt.
 * output_lock is nonzero while one processor sends a byte.
 */
struct drv_ns16550 {
	uint64_t physical;
	uintptr_t base;
	unsigned reg_shift;
	unsigned io_width;
	int irq;
	bool busy_detect;
	drv_ns16550_receive_t receive;
	bool input_started;
	unsigned output_lock;
};

/*
 * Finds the first present UART the tree describes and maps its registers.
 *
 * Returns 0, ENOENT when the tree has no such UART, or another errno.
 */
int drv_ns16550_probe(const struct drv_fdt *fdt, struct drv_ns16550 *port);

/*
 * Finds the UART the tree's /chosen/stdout-path names (a path, or an alias
 * of /aliases, with any options after a colon) and maps its registers.
 *
 * Returns 0, ENOENT when the tree names no console or names one that is
 * not present or not a UART the driver knows, or another errno.
 */
int drv_ns16550_probe_console(const struct drv_fdt *fdt, struct drv_ns16550 *port);

/*
 * Starts taking received bytes by interrupt and handing them to receive.
 *
 * Bytes the line held before the start are dropped.  Starting twice is
 * refused with EBUSY.  Returns 0 or an errno.
 */
int drv_ns16550_start_input(struct drv_ns16550 *port, drv_ns16550_receive_t receive);

/*
 * Sends one byte, waiting a bounded time for the transmitter.
 *
 * Never sleeps; callable with interrupts disabled, from an interrupt and
 * on a fatal stop.  A byte the transmitter does not take in time is
 * dropped.
 */
void drv_ns16550_putc_polled(struct drv_ns16550 *port, uint8_t byte);

#endif
