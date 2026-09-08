/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * PC/AT VGA 8x16 ASCII font preservation.
 */

#ifndef ZEDBSD_DRIVERS_GRAPHICS_PCAT_FONT_H
#define ZEDBSD_DRIVERS_GRAPHICS_PCAT_FONT_H

#include <stdint.h>

void drv_pcat_font_init(void);
void drv_pcat_font_restore_ascii(void);
int drv_pcat_font_get_glyph(uint32_t, uint8_t[32], unsigned *, unsigned *);

#endif
