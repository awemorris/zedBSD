/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The sun4u board's framebuffer: the firmware framebuffer the loader handed
 * over, or else the emulator's standard VGA in a linear mode, drawn as a
 * grid of text cells.
 */

#ifndef KERN_HAL_SPARCV9_BSP_SUN4U_FRAMEBUFFER_H
#define KERN_HAL_SPARCV9_BSP_SUN4U_FRAMEBUFFER_H

#include <hal/types.h>
#include "bootloader/sparcv9/handoff.h"

/* The text grid the screen holds. */
#define SUN4U_CONSOLE_COLUMNS	80U
#define SUN4U_CONSOLE_ROWS	25U

/*
 * The screen shown, each value a 64-bit word for the HAL's named objects:
 * the physical address of the first pixel, the bytes of the screen, its
 * size in pixels, the bytes of a row, the KERN_BOOT_FRAMEBUFFER_* format,
 * and the display's PCI vendor and device, zero when it is not on PCI.
 */
struct sun4u_framebuffer_names {
	uint64_t phys;
	uint64_t size;
	uint64_t width;
	uint64_t height;
	uint64_t pitch;
	uint64_t format;
	uint64_t vendor;
	uint64_t device;
};

/*
 * Shows the screen and clears it: the loader's firmware framebuffer, or
 * else the standard VGA the loader found, set to its linear mode.
 *
 * Returns HAL_OK once the screen shows, or HAL_ERR_UNSUPPORTED when there
 * is no display the HAL can draw.
 */
int
sun4u_framebuffer_init(
	const struct sun4u_boot_handoff *handoff);

/*
 * Reports the screen shown, or NULL when sun4u_framebuffer_init() showed
 * none.
 */
struct sun4u_framebuffer_names *
sun4u_framebuffer_shown(
	void);

/*
 * Draws one text cell: a glyph in the attribute's colours.
 */
void
sun4u_framebuffer_cell(
	unsigned row,
	unsigned column,
	int character,
	uint8_t attribute);

/*
 * Draws the cursor in one cell.
 */
void
sun4u_framebuffer_cursor(
	unsigned row,
	unsigned column);

/*
 * Reports whether the screen shows.
 */
int
sun4u_framebuffer_ready(
	void);

#endif
