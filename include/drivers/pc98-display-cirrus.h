/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (c) 1996-2024, Keiichi Tabata
 * Copyright (c) 2025, 2026, Awe Morris
 *
 * zedBSD graphics NEC PC-9821 Core-Graph / Cirrus GD5440 display backend, imported
 * from Boots.  The register sequence is adapted from StratoHAL
 * 98disp_cirrus.c at commit 76e909577bdf4629f11e473539b446a948fef830 and
 * is deliberately limited to the Core-Graph path at 640x480x8/24.
 * Port I/O and the linear framebuffer are injected by the embedder so
 * the driver stays compiler and host neutral.
 */

#ifndef PC98_DISPLAY_PC98_CIRRUS_H
#define PC98_DISPLAY_PC98_CIRRUS_H

#include "drivers/pc98-display.h"
#include "drivers/pc98-display-gdc.h"

#define PC98_DISPLAY_CIRRUS_WIDTH 640U
#define PC98_DISPLAY_CIRRUS_HEIGHT 480U
#define PC98_DISPLAY_CIRRUS_STRIDE_8 PC98_DISPLAY_CIRRUS_WIDTH
#define PC98_DISPLAY_CIRRUS_STRIDE_24 (PC98_DISPLAY_CIRRUS_WIDTH * 3U)
#define PC98_DISPLAY_CIRRUS_VISIBLE_BYTES \
	(PC98_DISPLAY_CIRRUS_STRIDE_24 * PC98_DISPLAY_CIRRUS_HEIGHT)

struct pc98_cirrus {
	void *io_context;
	uint8_t (*port_in8)(void *context, uint16_t port);
	void (*port_out8)(void *context, uint16_t port, uint8_t value);
	volatile uint8_t *framebuffer;
	uint8_t saved_sleep;
	uint8_t saved_window;
	uint8_t saved_linear;
	uint8_t saved_relay;
	uint8_t bits_per_pixel;
	uint8_t active;
};

/*
 * framebuffer is the host's view of the board's linear aperture.  A
 * target that maps physical memory one-to-one passes the aperture
 * address itself; a hosted target passes whatever its memory manager
 * returned for that physical range.
 */
void pc98_cirrus_default(
	struct pc98_cirrus *backend,
	pc98_in8_fn port_in8, pc98_out8_fn port_out8,
	void *io_context, volatile uint8_t *framebuffer);
int pc98_cirrus_make_hal(
	struct pc98_display_backend *hal,
	struct pc98_cirrus *backend);

#endif
