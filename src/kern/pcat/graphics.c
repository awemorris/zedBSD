/* PC/AT Cirrus GD5446 and standard VGA BeUI display backends.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/platform.h"
#include "kern/pcat/font.h"

#include <hal/hal.h>
#include <string.h>

#define WIDTH 640U
#define HEIGHT 480U
#define CIRRUS_APERTURE ((volatile uint8_t *)0xf0000000U)
#define VGA_APERTURE ((volatile uint8_t *)0x800a0000U)
#define CIRRUS_STRIDE8 WIDTH
#define CIRRUS_STRIDE24 (WIDTH * 3U)
#define PCI_CONFIG_ADDRESS 0x0cf8U
#define PCI_CONFIG_DATA 0x0cfcU
#define PCI_CIRRUS_VENDOR 0x1013U
#define PCI_CIRRUS_LFB 0xf0000000U

enum display_backend { DISPLAY_NONE, DISPLAY_CIRRUS, DISPLAY_VGA };

static enum display_backend active_backend;
static uint8_t active_bpp;
static uint8_t cirrus_bus, cirrus_device, cirrus_function;
static int cirrus_present;
static int vga_color_cache = -1;

static const uint32_t vga_palette[16] = {
	0x000000U, 0x0000aaU, 0x00aa00U, 0x00aaaaU,
	0xaa0000U, 0xaa00aaU, 0xaa5500U, 0xaaaaaaU,
	0x555555U, 0x5555ffU, 0x55ff55U, 0x55ffffU,
	0xff5555U, 0xff55ffU, 0xffff55U, 0xffffffU
};

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

static uint32_t in32(uint16_t port)
{
	uint32_t value;
	__asm__ volatile ("inl %w1,%0" : "=a"(value) : "Nd"(port));
	return value;
}

static void out32(uint16_t port, uint32_t value)
{
	__asm__ volatile ("outl %0,%w1" : : "a"(value), "Nd"(port));
}

static void seq_write(uint8_t index, uint8_t value)
{
	out8(0x3c4U, index);
	out8(0x3c5U, value);
}

static uint8_t seq_read(uint8_t index)
{
	out8(0x3c4U, index);
	return in8(0x3c5U);
}

static void gfx_write(uint8_t index, uint8_t value)
{
	out8(0x3ceU, index);
	out8(0x3cfU, value);
}

static void crtc_write(uint8_t index, uint8_t value)
{
	uint16_t port = (in8(0x3ccU) & 1U) ? 0x3d4U : 0x3b4U;
	out8(port, index);
	out8((uint16_t)(port + 1U), value);
}

static uint8_t crtc_read(uint8_t index)
{
	uint16_t port = (in8(0x3ccU) & 1U) ? 0x3d4U : 0x3b4U;
	out8(port, index);
	return in8((uint16_t)(port + 1U));
}

static uint32_t pci_address(uint8_t bus, uint8_t device, uint8_t function,
			    uint8_t offset)
{
	return 0x80000000U | ((uint32_t)bus << 16) |
		((uint32_t)device << 11) | ((uint32_t)function << 8) |
		(offset & 0xfcU);
}

static uint32_t pci_read32(uint8_t bus, uint8_t device, uint8_t function,
			   uint8_t offset)
{
	out32(PCI_CONFIG_ADDRESS, pci_address(bus, device, function, offset));
	return in32(PCI_CONFIG_DATA);
}

static void pci_write32(uint8_t bus, uint8_t device, uint8_t function,
			uint8_t offset, uint32_t value)
{
	out32(PCI_CONFIG_ADDRESS, pci_address(bus, device, function, offset));
	out32(PCI_CONFIG_DATA, value);
}

static void find_cirrus(void)
{
	unsigned bus, device, function;

	cirrus_present = 0;
	for (bus = 0; bus < 256U; bus++)
		for (device = 0; device < 32U; device++)
			for (function = 0; function < 8U; function++) {
				uint32_t id = pci_read32((uint8_t)bus,
					(uint8_t)device, (uint8_t)function, 0);
				if ((id & 0xffffU) == 0xffffU) {
					if (function == 0)
						break;
					continue;
				}
				if ((id & 0xffffU) != PCI_CIRRUS_VENDOR ||
				    (pci_read32((uint8_t)bus, (uint8_t)device,
				    (uint8_t)function, 0x08U) >> 16) != 0x0300U)
					continue;
				cirrus_bus = (uint8_t)bus;
				cirrus_device = (uint8_t)device;
				cirrus_function = (uint8_t)function;
				cirrus_present = 1;
				hal_printf("graphics: PCI Cirrus %04x:%04x at %u:%u.%u\n",
				    (unsigned)(id & 0xffffU), (unsigned)(id >> 16),
				    bus, device, function);
				return;
			}
}

static void configure_cirrus_pci(void)
{
	uint32_t command;

	command = pci_read32(cirrus_bus, cirrus_device, cirrus_function, 0x04U);
	pci_write32(cirrus_bus, cirrus_device, cirrus_function, 0x04U,
		command & ~0x00000002U);
	pci_write32(cirrus_bus, cirrus_device, cirrus_function, 0x10U,
		PCI_CIRRUS_LFB);
	command |= 0x00000003U;
	pci_write32(cirrus_bus, cirrus_device, cirrus_function, 0x04U, command);
	hal_printf("graphics: Cirrus LFB BAR0=%08x\n", PCI_CIRRUS_LFB);
}

static void hidden_dac_write(uint8_t value)
{
	unsigned i;
	(void)in8(0x3c8U);
	for (i = 0; i < 4U; i++)
		(void)in8(0x3c6U);
	out8(0x3c6U, value);
}

static void load_rgb332_palette(void)
{
	unsigned i;
	out8(0x3c6U, 0xffU);
	out8(0x3c8U, 0);
	for (i = 0; i < 256U; i++) {
		unsigned red = (i >> 5) & 7U;
		unsigned green = (i >> 2) & 7U;
		unsigned blue = i & 3U;
		out8(0x3c9U, (uint8_t)(red * 63U / 7U));
		out8(0x3c9U, (uint8_t)(green * 63U / 7U));
		out8(0x3c9U, (uint8_t)(blue * 63U / 3U));
	}
}

static void cirrus_mode_640x480(unsigned bits_per_pixel)
{
	static const uint8_t seq_index[] = {
		0x00,0x01,0x02,0x03,0x04,0x07,0x08,0x0b,0x0c,0x0d,
		0x0e,0x0f,0x16,0x18,0x1b,0x1c,0x1d,0x1e,0x1f
	};
	static const uint8_t seq_value[] = {
		0x01,0x01,0x0f,0x00,0x0e,0x11,0x00,0x66,0x48,0x56,
		0x60,0x30,0x58,0x40,0x3b,0x23,0x3d,0x3b,0x20
	};
	static const uint8_t crtc[0x1c] = {
		0x5f,0x4f,0x50,0x84,0x54,0x80,0x0b,0x3e,
		0x00,0x40,0x00,0x00,0x00,0x00,0x00,0x00,
		0xe5,0x87,0xdf,0x50,0x00,0xe7,0x04,0xe3,
		0xff,0x00,0x90,0x22
	};
	static const uint8_t graphics[9] =
		{ 0,0,0,0,0,0x40,0x05,0x0f,0xff };
	static const uint8_t attribute[21] = {
		0,1,2,3,4,5,6,7,8,9,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,
		0x41,0,0x0f,0,0
	};
	unsigned i;

	gfx_write(0x33U, 0);
	gfx_write(0x31U, 0x04U);
	gfx_write(0x31U, 0);
	seq_write(0x06U, 0x12U);
	seq_write(0x12U, 0);
	for (i = 0; i < sizeof(seq_index); i++) {
		uint8_t value = seq_value[i];
		if (seq_index[i] == 0x07U && bits_per_pixel == 24U)
			value = 0x15U;
		seq_write(seq_index[i], value);
	}
	seq_write(0x0fU, (uint8_t)((seq_read(0x0fU) & 0xdfU) | 0x20U));
	out8(0x3c2U, 0xe3U);
	gfx_write(0x06U, 0x05U);
	seq_write(0x00U, 0x03U);
	crtc_write(0x11U, 0x20U);
	for (i = 0; i < sizeof(crtc); i++) {
		uint8_t value = crtc[i];
		if (i == 0x13U && bits_per_pixel == 24U)
			value = 0xf0U;
		crtc_write((uint8_t)i, value);
	}
	for (i = 0; i < sizeof(graphics); i++)
		gfx_write((uint8_t)i, graphics[i]);
	(void)in8(0x3daU);
	for (i = 0; i < sizeof(attribute); i++) {
		out8(0x3c0U, (uint8_t)i);
		out8(0x3c0U, attribute[i]);
	}
	(void)in8(0x3daU);
	out8(0x3c0U, 0x20U);
	hidden_dac_write(bits_per_pixel == 24U ? 0xc5U : 0x20U);
	out8(0x3c6U, 0xffU);
	gfx_write(0x09U, 0);
	gfx_write(0x0aU, 0);
	gfx_write(0x0bU, 0x21U);
	seq_write(0x17U, (uint8_t)(seq_read(0x17U) | 0x44U));
	seq_write(0x18U, (uint8_t)(seq_read(0x18U) & 0xbfU));
	gfx_write(0x31U, 0x04U);
	gfx_write(0x31U, 0);
	if (bits_per_pixel == 8U)
		load_rgb332_palette();
	seq_write(0x01U, 0x21U);
}

static int cirrus_enter(unsigned bits_per_pixel)
{
	unsigned bytes, i;
	uint8_t chip;

	configure_cirrus_pci();
	seq_write(0x06U, 0x12U);
	chip = crtc_read(0x27U);
	if (chip == 0 || chip == 0xffU)
		return 0;
	cirrus_mode_640x480(bits_per_pixel);
	bytes = (bits_per_pixel == 24U ? CIRRUS_STRIDE24 : CIRRUS_STRIDE8) *
		HEIGHT;
	for (i = 0; i < bytes; i++)
		CIRRUS_APERTURE[i] = 0;
	seq_write(0x01U, 0x01U);
	active_bpp = (uint8_t)bits_per_pixel;
	active_backend = DISPLAY_CIRRUS;
	return 1;
}

static void vga_write_registers(const uint8_t registers[61])
{
	const uint8_t *value = registers;
	unsigned i;

	out8(0x3c2U, *value++);
	for (i = 0; i < 5U; i++)
		seq_write((uint8_t)i, *value++);
	crtc_write(0x03U, (uint8_t)(crtc_read(0x03U) | 0x80U));
	crtc_write(0x11U, (uint8_t)(crtc_read(0x11U) & 0x7fU));
	for (i = 0; i < 25U; i++)
		crtc_write((uint8_t)i, *value++);
	for (i = 0; i < 9U; i++)
		gfx_write((uint8_t)i, *value++);
	for (i = 0; i < 21U; i++) {
		(void)in8(0x3daU);
		out8(0x3c0U, (uint8_t)i);
		out8(0x3c0U, *value++);
	}
	(void)in8(0x3daU);
	out8(0x3c0U, 0x20U);
}

static void vga_load_palette(void)
{
	unsigned i;
	out8(0x3c6U, 0xffU);
	out8(0x3c8U, 0);
	for (i = 0; i < 16U; i++) {
		out8(0x3c9U, (uint8_t)(((vga_palette[i] >> 16) & 0xffU) >> 2));
		out8(0x3c9U, (uint8_t)(((vga_palette[i] >> 8) & 0xffU) >> 2));
		out8(0x3c9U, (uint8_t)((vga_palette[i] & 0xffU) >> 2));
	}
}

static void vga_graphics_mode(void)
{
	static const uint8_t mode[61] = {
		0xe3, 0x03,0x01,0x0f,0x00,0x06,
		0x5f,0x4f,0x50,0x82,0x54,0x80,0x0b,0x3e,
		0x00,0x40,0x00,0x00,0x00,0x00,0x00,0x00,
		0xea,0x0c,0xdf,0x28,0x00,0xe7,0x04,0xe3,0xff,
		0x00,0x00,0x00,0x00,0x00,0x00,0x05,0x0f,0xff,
		0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
		0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,
		0x01,0x00,0x0f,0x00,0x00
	};
	vga_write_registers(mode);
	vga_load_palette();
	vga_color_cache = -1;
}

static void vga_text_mode(void)
{
	static const uint8_t mode[61] = {
		0x67, 0x03,0x00,0x03,0x00,0x02,
		0x5f,0x4f,0x50,0x82,0x55,0x81,0xbf,0x1f,
		0x00,0x4f,0x0d,0x0e,0x00,0x00,0x00,0x50,
		0x9c,0x0e,0x8f,0x28,0x1f,0x96,0xb9,0xa3,0xff,
		0x00,0x00,0x00,0x00,0x00,0x10,0x0e,0x00,0xff,
		0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
		0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,
		0x0c,0x00,0x0f,0x08,0x00
	};
	vga_write_registers(mode);
	pcat_font_restore_ascii();
}

static uint8_t rgb332(uint32_t color)
{
	return (uint8_t)(((color >> 16) & 0xe0U) |
		((color >> 11) & 0x1cU) | ((color >> 6) & 3U));
}

static uint8_t rgb_to_vga(uint32_t color)
{
	unsigned best = 0, best_distance = UINT32_MAX, i;
	int red = (int)((color >> 16) & 0xffU);
	int green = (int)((color >> 8) & 0xffU);
	int blue = (int)(color & 0xffU);
	for (i = 0; i < 16U; i++) {
		int dr = red - (int)((vga_palette[i] >> 16) & 0xffU);
		int dg = green - (int)((vga_palette[i] >> 8) & 0xffU);
		int db = blue - (int)(vga_palette[i] & 0xffU);
		unsigned distance = (unsigned)(dr * dr + dg * dg + db * db);
		if (distance < best_distance) {
			best = i;
			best_distance = distance;
		}
	}
	return (uint8_t)best;
}

static void vga_write_pixel(unsigned x, unsigned y, uint8_t color)
{
	unsigned offset = y * (WIDTH / 8U) + x / 8U;
	uint8_t mask = (uint8_t)(0x80U >> (x & 7U));
	volatile uint8_t latch;

	if (vga_color_cache != color) {
		seq_write(0x02U, 0x0fU);
		gfx_write(0x00U, color);
		gfx_write(0x01U, 0x0fU);
		gfx_write(0x05U, 0x00U);
		vga_color_cache = color;
	}
	gfx_write(0x08U, mask);
	latch = VGA_APERTURE[offset];
	(void)latch;
	VGA_APERTURE[offset] = 0xffU;
}

static void write_pixel(unsigned x, unsigned y, uint32_t color)
{
	if (active_backend == DISPLAY_VGA) {
		vga_write_pixel(x, y, rgb_to_vga(color));
	} else if (active_bpp == 8U) {
		CIRRUS_APERTURE[y * CIRRUS_STRIDE8 + x] = rgb332(color);
	} else {
		volatile uint8_t *pixel = CIRRUS_APERTURE +
			y * CIRRUS_STRIDE24 + x * 3U;
		pixel[0] = (uint8_t)color;
		pixel[1] = (uint8_t)(color >> 8);
		pixel[2] = (uint8_t)(color >> 16);
	}
}

static int pattern_bit(uint64_t pattern, unsigned x, unsigned y)
{
	uint8_t row = (uint8_t)(pattern >> ((y & 7U) * 8U));
	return (row & (uint8_t)(0x80U >> (x & 7U))) != 0;
}

int kern_platform_graphics_init(uint64_t (*milliseconds)(void *),
				int (*key_state)(void *, int),
				void (*drain)(void *))
{
	(void)milliseconds;
	(void)key_state;
	(void)drain;
	pcat_font_init();
	find_cirrus();
	if (!cirrus_present)
		hal_printf("graphics: PCI Cirrus absent; VGA fallback ready\n");
	return 1;
}

int kern_platform_graphics_enter(struct kern_graphics_mode *mode)
{
	unsigned requested;
	if (mode == NULL)
		return 0;
	requested = mode->preferred_bits_per_pixel == 24U ? 24U : 8U;
	hal_cons_show_cursor(0);
	hal_cons_set_mode(HAL_CONS_FIXED_MENU);
	if (cirrus_present && cirrus_enter(requested)) {
		mode->width = WIDTH;
		mode->height = HEIGHT;
		mode->bits_per_pixel = requested;
		mode->stride = requested == 24U ? CIRRUS_STRIDE24 : CIRRUS_STRIDE8;
		hal_printf("graphics: PC/AT Cirrus %ux%ux%u stride=%u\n",
		    WIDTH, HEIGHT, requested, mode->stride);
		return 1;
	}
	vga_graphics_mode();
	active_backend = DISPLAY_VGA;
	active_bpp = 4;
	mode->width = WIDTH;
	mode->height = HEIGHT;
	mode->bits_per_pixel = 4;
	mode->stride = WIDTH / 8U;
	(void)kern_platform_graphics_clear();
	hal_printf("graphics: PC/AT VGA fallback %ux%ux4 planar\n",
	    WIDTH, HEIGHT);
	return 1;
}

int kern_platform_graphics_clear(void)
{
	const struct kern_graphics_rect screen = { 0, 0, WIDTH, HEIGHT };
	if (active_backend == DISPLAY_NONE)
		return 0;
	return kern_platform_graphics_fill(&screen, 0);
}

void kern_platform_graphics_leave(void)
{
	if (active_backend == DISPLAY_NONE)
		return;
	if (active_backend == DISPLAY_CIRRUS)
	{
		seq_write(0x01U, 0x21U);
		seq_write(0x06U, 0x12U);
		seq_write(0x07U, 0x00U);
		hidden_dac_write(0x00U);
		gfx_write(0x0bU, 0x00U);
	}
	vga_text_mode();
	active_backend = DISPLAY_NONE;
	active_bpp = 0;
	hal_cons_reset();
	hal_cons_set_mode(HAL_CONS_TERMINAL);
	hal_cons_show_cursor(1);
	hal_printf("graphics: PC/AT text mode restored\n");
}

int kern_platform_graphics_fill(const struct kern_graphics_rect *rect,
				uint32_t color)
{
	unsigned x, y;
	if (rect == NULL || active_backend == DISPLAY_NONE)
		return 0;
	if (active_backend == DISPLAY_CIRRUS && active_bpp == 8U) {
		uint8_t pixel = rgb332(color);
		for (y = rect->y; y < rect->y + rect->height; y++) {
			volatile uint8_t *row = CIRRUS_APERTURE +
				y * CIRRUS_STRIDE8 + rect->x;
			for (x = 0; x < rect->width; x++)
				row[x] = pixel;
		}
		return 1;
	}
	if (active_backend == DISPLAY_CIRRUS && active_bpp == 24U) {
		for (y = rect->y; y < rect->y + rect->height; y++) {
			volatile uint8_t *row = CIRRUS_APERTURE +
				y * CIRRUS_STRIDE24 + rect->x * 3U;
			for (x = 0; x < rect->width; x++) {
				row[x * 3U] = (uint8_t)color;
				row[x * 3U + 1U] = (uint8_t)(color >> 8);
				row[x * 3U + 2U] = (uint8_t)(color >> 16);
			}
		}
		return 1;
	}
	for (y = rect->y; y < rect->y + rect->height; y++)
		for (x = rect->x; x < rect->x + rect->width; x++)
			write_pixel(x, y, color);
	return 1;
}

int kern_platform_graphics_line(unsigned x0, unsigned y0, unsigned x1,
				unsigned y1, uint32_t color)
{
	int x = (int)x0, y = (int)y0;
	int target_x = (int)x1, target_y = (int)y1;
	int dx = target_x >= x ? target_x - x : x - target_x;
	int sx = x < target_x ? 1 : -1;
	int dy = target_y >= y ? y - target_y : target_y - y;
	int sy = y < target_y ? 1 : -1;
	int error = dx + dy;
	if (active_backend == DISPLAY_NONE)
		return 0;
	for (;;) {
		int twice;
		write_pixel((unsigned)x, (unsigned)y, color);
		if (x == target_x && y == target_y)
			break;
		twice = error * 2;
		if (twice >= dy) { error += dy; x += sx; }
		if (twice <= dx) { error += dx; y += sy; }
	}
	return 1;
}

int kern_platform_graphics_pattern_fill(const struct kern_graphics_rect *rect,
					uint32_t color, uint64_t pattern)
{
	unsigned x, y;
	if (rect == NULL || active_backend == DISPLAY_NONE)
		return 0;
	for (y = 0; y < rect->height; y++)
		for (x = 0; x < rect->width; x++)
			if (pattern_bit(pattern, x, y))
				write_pixel(rect->x + x, rect->y + y, color);
	return 1;
}

int kern_platform_graphics_blit(unsigned destination_x,
				unsigned destination_y,
				const struct kern_graphics_image *image,
				uint64_t pattern, int patterned)
{
	unsigned x, y;
	if (image == NULL || active_backend == DISPLAY_NONE)
		return 0;
	for (y = 0; y < image->height; y++) {
		const uint8_t *row = image->pixels + (size_t)y * image->stride;
		for (x = 0; x < image->width; x++) {
			uint32_t rgb;
			if (patterned && !pattern_bit(pattern, x, y))
				continue;
			if (image->format == 1U) {
				unsigned index = row[x];
				rgb = index < image->palette_size ?
					image->palette[index] : 0;
			} else {
				const uint8_t *source = row + (size_t)x * 3U;
				rgb = ((uint32_t)source[0] << 16) |
					((uint32_t)source[1] << 8) | source[2];
			}
			write_pixel(destination_x + x, destination_y + y, rgb);
		}
	}
	return 1;
}

int kern_platform_graphics_flush(const struct kern_graphics_rect *rectangles,
				 size_t count)
{
	(void)rectangles;
	(void)count;
	return active_backend != DISPLAY_NONE;
}

int kern_platform_graphics_get_glyph(uint32_t codepoint, uint8_t bitmap[32],
				     unsigned *width, unsigned *height)
{
	return pcat_font_get_glyph(codepoint, bitmap, width, height);
}

void kern_platform_restore_text(void)
{
	if (active_backend != DISPLAY_NONE)
		kern_platform_graphics_leave();
	else
		hal_cons_reset();
}
