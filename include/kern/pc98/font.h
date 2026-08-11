/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_KERN_PC98_FONT_H
#define ZEDBSD_KERN_PC98_FONT_H
#include <stdint.h>
int pc98_font_get_glyph(uint32_t, uint8_t[32], unsigned *, unsigned *);
#endif
