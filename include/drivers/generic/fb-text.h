/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The text console over the boot framebuffer (drivers/generic/boot-fb.h)
 * and a board's serial line, for a board without its own display driver.
 */

#ifndef KERN_DRIVERS_GENERIC_FB_TEXT_H
#define KERN_DRIVERS_GENERIC_FB_TEXT_H

/*
 * What a board adds to the console.  The table lives as long as the
 * kernel.
 *
 * serial_write sends one character to the board's serial line, a carriage
 * return before a newline, without sleeping and from any context; NULL
 * when the serial line is not the console.  reveal_fatal, when not NULL,
 * puts the boot screen back on the display for a fatal stop before the
 * console draws its text there; interrupts are disabled.
 */
struct drv_fb_text_board {
	void (*serial_write)(int character);
	void (*reveal_fatal)(void);
};

/*
 * Publishes the console: a grid of cells on the boot framebuffer when
 * drv_boot_fb_init() mapped it, the board's serial line, the text
 * operations, and kernel_putc when the console can show a character.
 * Call once, from the board's start-up, before /dev/console opens.
 */
void
drv_fb_text_init(
	const struct drv_fb_text_board *board);

#endif
