/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The PC-98 text console layer owned by /dev/graphics.
 *
 * /dev/console reaches these through kern_text_*(); the display driver
 * calls drv_pc98_text_init() during its own bring-up, which publishes
 * the table. Nothing here is exposed to user space.
 */

#ifndef KERN_DRIVERS_GRAPHICS_PC98_TEXT_H
#define KERN_DRIVERS_GRAPHICS_PC98_TEXT_H

#include <stdint.h>

void drv_pc98_text_init(void);
int drv_pc98_text_ready(void);
void drv_pc98_text_get_size(unsigned *columns, unsigned *rows);
void drv_pc98_text_putc(int character);
void drv_pc98_text_write(unsigned row, unsigned column, uint8_t attribute,
			 const char *utf8);
void drv_pc98_text_clear(void);
int drv_pc98_text_set_cursor(unsigned row, unsigned column);
void drv_pc98_text_get_cursor(unsigned *row, unsigned *column, int *visible);
void drv_pc98_text_show_cursor(int visible);
void drv_pc98_text_update_cursor(void);
void drv_pc98_text_suspend(void);
void drv_pc98_text_resume(void);

#endif
