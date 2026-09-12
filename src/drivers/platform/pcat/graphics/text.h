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
int drv_pcat_text_set_cursor(unsigned row, unsigned column);
void drv_pcat_text_get_cursor(unsigned *row, unsigned *column, int *visible);
void drv_pcat_text_show_cursor(int visible);
void drv_pcat_text_update_cursor(void);
void drv_pcat_text_suspend(void);
void drv_pcat_text_resume(void);

int drv_pcat_text_snapshot(struct kern_text_snapshot *snapshot);

#endif
