/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The PC/AT's i8042 at its fixed ports and interrupt lines.
 */

#ifndef KERN_DRIVERS_HID_PS2_8042_H
#define KERN_DRIVERS_HID_PS2_8042_H

#include "drivers/isa/ps2-8042.h"

/*
 * The PC/AT controller's ports and interrupt lines.
 *
 * Values referenced: IBM PC/AT Technical Reference (the 8042 at I/O ports
 * 0x60 and 0x64, the keyboard on IRQ 1 and the auxiliary device on IRQ 12).
 */
#define PCAT_I8042_DATA_PORT	0x60U
#define PCAT_I8042_STATUS_PORT	0x64U
#define PCAT_I8042_KEYBOARD_IRQ	1
#define PCAT_I8042_MOUSE_IRQ	12

/*
 * Registers the PC/AT keyboard and mouse.
 */
static __inline int
drv_pcat_ps2_8042_init(
	void)
{
	static const struct drv_isa_ps2_8042_config config = {
		.data_port = PCAT_I8042_DATA_PORT,
		.status_port = PCAT_I8042_STATUS_PORT,
		.keyboard_irq = PCAT_I8042_KEYBOARD_IRQ,
		.mouse_irq = PCAT_I8042_MOUSE_IRQ,
		.keyboard_name = "PC/AT PS/2 keyboard",
		.keyboard_path = "pcat/i8042/kbd0",
		.mouse_name = "PC/AT PS/2 mouse",
		.mouse_path = "pcat/i8042/aux0",
	};
	int error;

	/* Registers the controller at its fixed place. */
	error = drv_isa_ps2_8042_init(&config);
	if (error != 0)
		return error;

	/* Succeeded: the keyboard and the mouse are published. */
	return 0;
}

#endif
