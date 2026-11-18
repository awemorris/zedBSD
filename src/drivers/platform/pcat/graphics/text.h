/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The PC/AT text console layer owned by /dev/graphics.
 *
 * /dev/console calls these directly; they are not exposed to user space.
 */

#ifndef KERN_DRIVERS_GRAPHICS_PCAT_TEXT_H
#define KERN_DRIVERS_GRAPHICS_PCAT_TEXT_H

#include <stdint.h>

struct kern_text_range;
struct kern_text_snapshot;

/* Light grey on black, the default text attribute. */
#define DRV_PCAT_TEXT_ATTRIB_NORMAL	0x07U

void drv_pcat_text_init(void);
int drv_pcat_text_ready(void);
void drv_pcat_text_get_size(unsigned *columns, unsigned *rows);
void drv_pcat_text_putc(int character);
void drv_pcat_text_write(unsigned row, unsigned column, uint8_t attribute,
			 const char *utf8);
void drv_pcat_text_clear(void);
/*
 * Draws through another mapping of the framebuffer from now on (to for
 * from), the spinner too.
 */
void drv_pcat_text_retarget(volatile uint32_t *from, volatile uint32_t *to);
int drv_pcat_text_set_cursor(unsigned row, unsigned column);
void drv_pcat_text_get_cursor(unsigned *row, unsigned *column, int *visible);
void drv_pcat_text_show_cursor(int visible);
void drv_pcat_text_update_cursor(void);
void drv_pcat_text_suspend(void);
void drv_pcat_text_resume(void);
void drv_pcat_text_reveal(void);
void drv_pcat_text_reveal_fatal(void);
void drv_pcat_text_progress(int end);
/*
 * Ends the console's use of the display for good (the board's end op of
 * kern_text_end()).
 */
int drv_pcat_text_end(const struct kern_text_range *ranges, unsigned count);

int drv_pcat_text_snapshot(struct kern_text_snapshot *snapshot);

#endif
