/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The second stage's text on the screen the ROM set up.
 */

#ifndef KERN_BOOT_MAC68K_SCREEN_H
#define KERN_BOOT_MAC68K_SCREEN_H

#include <stdint.h>

/* The glyphs the font holds (the printable ASCII characters) and their size. */
#define MAC68K_FONT_FIRST	0x20U
#define MAC68K_FONT_GLYPHS	95U
#define MAC68K_FONT_WIDTH	8U
#define MAC68K_FONT_HEIGHT	16U

extern const uint8_t mac68k_loader_font[MAC68K_FONT_GLYPHS * MAC68K_FONT_HEIGHT];

void mac68k_screen_init(uint8_t *base, uint32_t row_bytes, uint32_t width, uint32_t height, uint32_t depth, int rgb332);
void mac68k_screen_clear(void);
void mac68k_screen_puts(const char *text);
void mac68k_screen_put_hex(uint32_t value);
void mac68k_screen_put_decimal(uint32_t value);

#endif
