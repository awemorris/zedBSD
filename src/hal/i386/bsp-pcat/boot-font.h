/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_HAL_I386_BSP_PCAT_BOOT_FONT_H
#define ZEDBSD_HAL_I386_BSP_PCAT_BOOT_FONT_H

#include <stdint.h>

#define BSP_PCAT_ASCII_GLYPHS 128U
#define BSP_PCAT_GLYPH_HEIGHT 16U

int bsp_pcat_get_boot_font(
	uint8_t font[BSP_PCAT_ASCII_GLYPHS][BSP_PCAT_GLYPH_HEIGHT]);

#endif
