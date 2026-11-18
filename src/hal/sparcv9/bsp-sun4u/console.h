/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The sun4u board's text console.
 */

#ifndef KERN_HAL_SPARCV9_BSP_SUN4U_CONSOLE_H
#define KERN_HAL_SPARCV9_BSP_SUN4U_CONSOLE_H

/*
 * Draws the whole grid once the framebuffer is shown, so the lines printed
 * before it appear.
 */
void
sun4u_console_attach(
	void);

#endif
