/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_DRIVERS_GRAPHICS_PCAT_FONT_H
#define ZEDBSD_DRIVERS_GRAPHICS_PCAT_FONT_H

#include <stdint.h>

void pcat_font_init(void);
void pcat_font_restore_ascii(void);
int pcat_font_get_glyph(uint32_t, uint8_t[32], unsigned *, unsigned *);

#endif
