/*
 * -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*-
 */

/*
 * zedBSD
 * Copyright (c) 2025, 2026, Awe Morris
 *
 * zedBSD graphics PC-98 display selection, imported from Boots.  Probing
 * prefers the Core-Graph / Cirrus board at 640x480x8 and falls back to the
 * always-present GDC at 640x400x4, so one HAL covers both machines.
 */

#ifndef ZEDBSD_DRIVERS_GRAPHICS_PC98_DISPLAY_AUTO_H
#define ZEDBSD_DRIVERS_GRAPHICS_PC98_DISPLAY_AUTO_H

#include "drivers/graphics/pc98/display-cirrus.h"
#include "drivers/graphics/pc98/display-glyph.h"
#include "drivers/graphics/pc98/display-gdc.h"

struct pc98_auto {
	struct pc98_cirrus cirrus;
	struct pc98_gdc gdc;
	struct pc98_glyph glyph;
	struct pc98_display_backend cirrus_hal;
	struct pc98_display_backend gdc_hal;
	struct pc98_display_ops *active;
};

void
pc98_auto_default(
	struct pc98_auto *backend,
	pc98_display_reset_fn display_reset,
	pc98_display_reset_fn display_stop,
	void *bios_context,
	pc98_in8_fn port_in8,
	pc98_out8_fn port_out8,
	void *io_context,
	volatile uint8_t *cirrus_framebuffer);
int
pc98_auto_make_hal(
	struct pc98_display_backend *hal,
	struct pc98_auto *backend);

#endif
