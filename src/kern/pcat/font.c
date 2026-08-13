/* PC/AT VGA 8x16 ASCII font preservation.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/pcat/font.h"
#include "hal/i386/bsp-pcat/boot-font.h"

#include <hal/hal.h>
#include <string.h>

#define VGA_FONT_MEMORY ((volatile uint8_t *)0x800a0000U)
#define ASCII_GLYPHS 128U
#define GLYPH_HEIGHT 16U
#define VGA_GLYPH_SLOT 32U

static uint8_t ascii_font[ASCII_GLYPHS][GLYPH_HEIGHT]
	__attribute__((section(".vfs_bss")));
static int font_valid __attribute__((section(".vfs_bss")));

static uint8_t in8(uint16_t port)
{
	uint8_t value;
	__asm__ volatile ("inb %w1,%0" : "=a"(value) : "Nd"(port));
	return value;
}

static void out8(uint16_t port, uint8_t value)
{
	__asm__ volatile ("outb %0,%w1" : : "a"(value), "Nd"(port));
}

static uint8_t indexed_read(uint16_t index_port, uint16_t data_port,
			    uint8_t index)
{
	out8(index_port, index);
	return in8(data_port);
}

static void indexed_write(uint16_t index_port, uint16_t data_port,
			  uint8_t index, uint8_t value)
{
	out8(index_port, index);
	out8(data_port, value);
}

static void plane2_access_begin(uint8_t saved[5], int writing)
{
	saved[0] = indexed_read(0x3c4U, 0x3c5U, 0x02U);
	saved[1] = indexed_read(0x3c4U, 0x3c5U, 0x04U);
	saved[2] = indexed_read(0x3ceU, 0x3cfU, 0x04U);
	saved[3] = indexed_read(0x3ceU, 0x3cfU, 0x05U);
	saved[4] = indexed_read(0x3ceU, 0x3cfU, 0x06U);
	indexed_write(0x3c4U, 0x3c5U, 0x00U, 0x01U);
	indexed_write(0x3c4U, 0x3c5U, 0x02U, writing ? 0x04U : 0x00U);
	indexed_write(0x3c4U, 0x3c5U, 0x04U, 0x07U);
	indexed_write(0x3c4U, 0x3c5U, 0x00U, 0x03U);
	indexed_write(0x3ceU, 0x3cfU, 0x04U, 0x02U);
	indexed_write(0x3ceU, 0x3cfU, 0x05U, 0x00U);
	indexed_write(0x3ceU, 0x3cfU, 0x06U, 0x04U);
}

static void plane2_access_end(const uint8_t saved[5])
{
	indexed_write(0x3c4U, 0x3c5U, 0x00U, 0x01U);
	indexed_write(0x3c4U, 0x3c5U, 0x02U, saved[0]);
	indexed_write(0x3c4U, 0x3c5U, 0x04U, saved[1]);
	indexed_write(0x3c4U, 0x3c5U, 0x00U, 0x03U);
	indexed_write(0x3ceU, 0x3cfU, 0x04U, saved[2]);
	indexed_write(0x3ceU, 0x3cfU, 0x05U, saved[3]);
	indexed_write(0x3ceU, 0x3cfU, 0x06U, saved[4]);
}

static void capture_plane2(void)
{
	uint8_t saved[5];
	unsigned glyph, row;

	plane2_access_begin(saved, 0);
	for (glyph = 0; glyph < ASCII_GLYPHS; glyph++)
		for (row = 0; row < GLYPH_HEIGHT; row++)
			ascii_font[glyph][row] =
				VGA_FONT_MEMORY[glyph * VGA_GLYPH_SLOT + row];
	plane2_access_end(saved);
	font_valid = 1;
}

void pcat_font_init(void)
{
	if (font_valid)
		return;
	if (bsp_pcat_get_boot_font(ascii_font)) {
		font_valid = 1;
		hal_printf("graphics: BIOS 8x16 ASCII font handoff accepted\n");
		return;
	}
	capture_plane2();
	hal_printf("graphics: VGA plane-2 ASCII font captured\n");
}

void pcat_font_restore_ascii(void)
{
	uint8_t saved[5];
	unsigned glyph, row;

	if (!font_valid)
		return;
	plane2_access_begin(saved, 1);
	for (glyph = 0; glyph < ASCII_GLYPHS; glyph++) {
		for (row = 0; row < GLYPH_HEIGHT; row++)
			VGA_FONT_MEMORY[glyph * VGA_GLYPH_SLOT + row] =
				ascii_font[glyph][row];
		for (; row < VGA_GLYPH_SLOT; row++)
			VGA_FONT_MEMORY[glyph * VGA_GLYPH_SLOT + row] = 0;
	}
	plane2_access_end(saved);
}

int pcat_font_get_glyph(uint32_t codepoint, uint8_t bitmap[32],
			unsigned *width, unsigned *height)
{
	if (bitmap == NULL || width == NULL || height == NULL)
		return 0;
	if (!font_valid)
		pcat_font_init();
	if (codepoint >= ASCII_GLYPHS)
		codepoint = '?';
	memset(bitmap, 0, 32);
	memcpy(bitmap, ascii_font[codepoint], GLYPH_HEIGHT);
	*width = 8;
	*height = GLYPH_HEIGHT;
	return 1;
}
