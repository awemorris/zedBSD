/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Boots PC-98 CGROM glyph backend
 */

#ifndef PC98_DISPLAY_PC98_GLYPH_H
#define PC98_DISPLAY_PC98_GLYPH_H

#include "drivers/pc98-display.h"
#include "drivers/pc98-display-gdc.h"

struct pc98_glyph {
	void *io_context;
	uint8_t (
		*port_in8)(
		void *context,
		uint16_t port);
	void (
		*port_out8)(
		void *context,
		uint16_t port,
		uint8_t value);
	volatile uint8_t *cg_window;
	struct pc98_display_ops *display;
	struct {
		uint16_t jis;
		uint8_t valid;
		uint8_t font[32];
	} cache[64];
	unsigned cache_next;
};

void
pc98_glyph_default(
	struct pc98_glyph *backend,
	struct pc98_display_ops *display,
	pc98_in8_fn port_in8,
	pc98_out8_fn port_out8,
	void *io_context);
int
pc98_glyph_make_hal(
	struct pc98_glyph_ops *hal,
	struct pc98_glyph *backend);
uint16_t
pc98_unicode_to_jis(
	uint32_t codepoint);

#endif
