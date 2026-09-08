/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * zedBSD graphics NEC PC-9800 GDC.
 */

#ifndef ZEDBSD_DRIVERS_GRAPHICS_PC98_DISPLAY_GDC_H
#define ZEDBSD_DRIVERS_GRAPHICS_PC98_DISPLAY_GDC_H

#include "drivers/platform/pc98/graphics/display.h"

#define PC98_DISPLAY_GDC_PLANE_BYTES (640U * 400U / 8U)

typedef int (*pc98_display_reset_fn)(void *context);
typedef uint8_t (*pc98_in8_fn)(void *context, uint16_t port);
typedef void (*pc98_out8_fn)(void *context, uint16_t port, uint8_t value);

struct pc98_gdc {
	void *bios_context;
	pc98_display_reset_fn display_reset;
	pc98_display_reset_fn display_stop;
	void *io_context;
	uint8_t (*port_in8)(void *context, uint16_t port);
	void (*port_out8)(void *context, uint16_t port, uint8_t value);
	volatile uint8_t *planes[4];
};

void drv_pc98_gdc_default(struct pc98_gdc *backend,
			  pc98_display_reset_fn display_reset,
			  pc98_display_reset_fn display_stop,
			  void *bios_context, pc98_in8_fn port_in8,
			  pc98_out8_fn port_out8, void *io_context);
int drv_pc98_gdc_make_hal(struct pc98_display_backend *hal,
			  struct pc98_gdc *backend);
int drv_pc98_gdc_clear_graphics(struct pc98_gdc *backend);

#endif
