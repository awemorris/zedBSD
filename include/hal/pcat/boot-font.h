/*
 * PC/AT BIOS font handoff shared by the i386 and amd64 BSPs.
 */

#ifndef HAL_PCAT_BOOT_FONT_H
#define HAL_PCAT_BOOT_FONT_H

#include <stdint.h>

#define BSP_PCAT_ASCII_GLYPHS 128U
#define BSP_PCAT_GLYPH_HEIGHT 16U

int bsp_pcat_get_boot_font(
	uint8_t font[BSP_PCAT_ASCII_GLYPHS][BSP_PCAT_GLYPH_HEIGHT]);

#endif
