/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/pc98/font.h"

#include <hal/hal.h>
#include <string.h>

extern const uint16_t hal_pc98_jisx0208_to_ucs[7896];

struct cached_glyph {
	uint16_t jis;
	uint8_t valid;
	uint8_t font[32];
};

static struct cached_glyph cache[64] __attribute__((section(".vfs_bss")));
static unsigned cache_next __attribute__((section(".vfs_bss")));

static uint8_t port_in8(uint16_t port)
{
	uint8_t value;
	__asm__ volatile ("inb %w1,%0" : "=a"(value) : "Nd"(port));
	return value;
}

static void port_out8(uint16_t port, uint8_t value)
{
	__asm__ volatile ("outb %0,%w1" : : "a"(value), "Nd"(port));
}

static uint16_t unicode_to_jis(uint32_t codepoint)
{
	size_t index;
	if (codepoint < 0x80U)
		return (uint16_t)(0x2000U | codepoint);
	if (codepoint >= 0xff61U && codepoint <= 0xff9fU)
		return (uint16_t)(0x20a1U + codepoint - 0xff61U);
	if (codepoint > 0xffffU)
		return 0;
	for (index = 0; index < 7896U; index++) {
		if (hal_pc98_jisx0208_to_ucs[index] == (uint16_t)codepoint) {
			unsigned row = (unsigned)(index / 94U) + 0x21U;
			unsigned cell = (unsigned)(index % 94U) + 0x21U;
			return (uint16_t)((row << 8) | cell);
		}
	}
	return 0;
}

static void wait_vsync(void)
{
	while (port_in8(0x60) & 0x20U)
		;
	while (!(port_in8(0x60) & 0x20U))
		;
}

static int read_font(uint16_t jis, uint8_t font[32])
{
	volatile uint8_t *window = (volatile uint8_t *)0x800a4000U;
	uint8_t row = (uint8_t)(jis >> 8);
	uint8_t cell = (uint8_t)jis;
	int special = (row >= 0x29U && row <= 0x2fU) ||
		(row >= 0x76U && row <= 0x7fU);
	unsigned index;

	if (!special) {
		for (index = 0; index < 64U; index++) {
			if (cache[index].valid && cache[index].jis == jis) {
				memcpy(font, cache[index].font, 32);
				return 1;
			}
		}
	}
	memset(font, 0, 32);
	wait_vsync();
	port_out8(0x68, 0x0b);
	if (row == 0x20U) {
		port_out8(0xa1, 0x00);
		port_out8(0xa3, cell);
		port_out8(0xa5, 0x00);
		for (index = 0; index < 16U; index++)
			font[index] = window[index * 2U + 1U];
	} else if (!special) {
		port_out8(0xa1, cell);
		port_out8(0xa3, (uint8_t)(row - 0x20U));
		port_out8(0xa5, 0x00);
		for (index = 0; index < 32U; index++)
			font[index] = window[index];
	} else {
		port_out8(0xa1, cell);
		port_out8(0xa3, (uint8_t)(row - 0x20U));
		port_out8(0xa5, 0x20);
		for (index = 0; index < 16U; index++)
			font[index * 2U] = window[index * 2U + 1U];
		port_out8(0xa5, 0x00);
		for (index = 0; index < 16U; index++)
			font[index * 2U + 1U] = window[index * 2U + 1U];
	}
	port_out8(0x68, 0x0a);
	if (!special) {
		index = cache_next++ % 64U;
		cache[index].jis = jis;
		cache[index].valid = 1;
		memcpy(cache[index].font, font, 32);
	}
	return 1;
}

int
pc98_font_get_glyph(uint32_t codepoint, uint8_t font[32], unsigned *width,
		    unsigned *height)
{
	uint16_t jis;
	if (font == NULL || width == NULL || height == NULL ||
	    codepoint > 0x10ffffU || (codepoint >= 0xd800U && codepoint <= 0xdfffU))
		return 0;
	jis = unicode_to_jis(codepoint);
	if (jis == 0)
		jis = unicode_to_jis('?');
	*width = (jis >> 8) == 0x20U ? 8U : 16U;
	*height = 16U;
	return read_font(jis, font);
}
