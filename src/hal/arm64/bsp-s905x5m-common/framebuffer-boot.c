/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Where an S905X5M board's console framebuffer comes from: the bootloader
 * brought up the HDMI output and left OSD1 scanning out a 32-bit buffer,
 * which its boot script describes to the system as a simple-framebuffer
 * node of the device tree.
 */

#include <hal/hal.h>
#include "../bsp.h"
#include "../framebuffer.h"

/*
 * Takes the bootloader's framebuffer for the console.
 *
 * Returns 0 when the framebuffer is ready, and nonzero when the tree
 * names none the console can paint.
 */
int
arm64_bsp_framebuffer_init(
	void)
{
	const struct arm64_fdt_info *info;
	uint32_t opaque_bits;
	int error;

	/* Without a framebuffer in the tree the console stays on the UART. */
	info = arm64_boot_info();
	if (info->framebuffer_size == 0)
		return -1;

	/* A plane that blends by the top byte shows only pixels whose alpha is full. */
	opaque_bits = 0;
	if (info->framebuffer_alpha)
		opaque_bits = 0xff000000U;

	/* Takes the buffer, which checks that the console fits in it. */
	error = arm64_framebuffer_adopt(info->framebuffer_base,
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
