/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Where the ODROID-C5's console framebuffer comes from: the bootloader
 * brought up the HDMI output and left OSD1 scanning out a 32-bit buffer,
 * which its boot script describes to the system as a simple-framebuffer
 * node of the device tree.
 */

#include <hal/hal.h>
#include "../bsp.h"
#include "../bsp-rpi-common/framebuffer.h"

/*
 * Takes the bootloader's framebuffer for the console.
 *
 * The board has no firmware mailbox, so the address the common boot
 * passes is not used.  Returns 0 when the framebuffer is ready, and
 * nonzero when the tree names none the console can paint.
 */
int
rpi_framebuffer_init(
	uintptr_t mailbox_phys)
{
	const struct rpi_fdt_info *info;
	uint32_t opaque_bits;
	int error;

	/* The board has no mailbox. */
	(void)mailbox_phys;

	/* Without a framebuffer in the tree the console stays on the UART. */
	info = rpi_boot_info();
	if (info->framebuffer_size == 0)
		return -1;

	/* A plane that blends by the top byte shows only pixels whose alpha is full. */
	opaque_bits = 0;
	if (info->framebuffer_alpha)
		opaque_bits = 0xff000000U;

	/* Takes the buffer, which checks that the console fits in it. */
	error = rpi_framebuffer_adopt(info->framebuffer_base,
				      info->framebuffer_size,
				      info->framebuffer_width,
				      info->framebuffer_height,
				      info->framebuffer_stride,
				      info->framebuffer_red_low,
				      opaque_bits);
	if (error != 0)
		return -1;

	/* Succeeded: the framebuffer is ready for the console. */
	return 0;
}
