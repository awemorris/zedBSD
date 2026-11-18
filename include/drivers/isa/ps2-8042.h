/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The i8042 PS/2 keyboard and mouse controller, shared by the boards that
 * have one on an ISA-style I/O space (PC/AT, sun4u's ebus).
 */

#ifndef KERN_DRIVERS_ISA_PS2_8042_H
#define KERN_DRIVERS_ISA_PS2_8042_H

#include <stdint.h>

/*
 * Where one controller is and how it reports.
 *
 * data_port and status_port are the controller's two I/O ports (the
 * status register is read and the command register written at the same
 * port).  keyboard_irq and mouse_irq are its interrupt lines; when either
 * is negative the controller is polled by a kernel thread instead.  The
 * names and paths are those of the two input devices and must stay valid
 * for the life of the system.
 */
struct drv_isa_ps2_8042_config {
	uint16_t data_port;
	uint16_t status_port;
	int keyboard_irq;
	int mouse_irq;
	const char *keyboard_name;
	const char *keyboard_path;
	const char *mouse_name;
	const char *mouse_path;
};

/*
 * Registers one controller's keyboard and mouse as input devices.
 *
 * Called once at boot.  Returns 0, or ENODEV when a polled controller
 * does not answer, or the error of an interrupt or device registration.
 */
int
drv_isa_ps2_8042_init(
	const struct drv_isa_ps2_8042_config *config);

#endif
