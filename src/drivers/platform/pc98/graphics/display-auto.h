/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * zedBSD graphics NEC PC-9821 Core-Graph / Cirrus GD5440 display backend.
 */

#ifndef ZEDBSD_DRIVERS_GRAPHICS_PC98_DISPLAY_AUTO_H
#define ZEDBSD_DRIVERS_GRAPHICS_PC98_DISPLAY_AUTO_H

#include "drivers/platform/pc98/graphics/display-cirrus.h"
#include "drivers/platform/pc98/graphics/display-glyph.h"
#include "drivers/platform/pc98/graphics/display-gdc.h"

struct pc98_auto {
	struct pc98_cirrus cirrus;
	struct pc98_gdc gdc;
	struct pc98_glyph glyph;
	struct pc98_display_backend cirrus_hal;
	struct pc98_display_backend gdc_hal;
	struct pc98_display_ops *active;
};

void drv_pc98_auto_default(struct pc98_auto *backend,
			   pc98_display_reset_fn display_reset,
			   pc98_display_reset_fn display_stop,
			   void *bios_context, pc98_in8_fn port_in8,
			   pc98_out8_fn port_out8, void *io_context,
			   volatile uint8_t *cirrus_framebuffer);
int drv_pc98_auto_make_hal(struct pc98_display_backend *hal,
			   struct pc98_auto *backend);

#endif
