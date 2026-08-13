/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_KERN_PLATFORM_H
#define ZEDBSD_KERN_PLATFORM_H

#include <stddef.h>
#include <stdint.h>
#include "kern/boot.h"

struct disk;
struct zedbsd_filesystem;

struct kern_graphics_mode {
	unsigned preferred_bits_per_pixel;
	unsigned width, height, bits_per_pixel, stride;
};
struct kern_graphics_rect { unsigned x, y, width, height; };
struct kern_graphics_image {
	unsigned format, width, height;
	size_t stride;
	const uint8_t *pixels;
	const uint32_t *palette;
	unsigned palette_size;
};

#define KERN_PLATFORM_MAX_DEVICES 12U

size_t kern_platform_init(const struct zedbsd_handoff *handoff,
			  struct zedbsd_device *devices, size_t capacity);
void kern_platform_refresh_devices(const struct zedbsd_device *devices,
				   size_t count);
struct disk *kern_platform_block_device(
	const struct zedbsd_device *device);
int kern_platform_boot_linux(struct zedbsd_filesystem *filesystem,
			     const char *path, const char *arguments,
			     const struct zedbsd_device *devices, unsigned count,
			     int boot_device);
int kern_platform_graphics_init(uint64_t (*milliseconds)(void *),
				int (*key_state)(void *, int),
				void (*drain)(void *));
int kern_platform_graphics_enter(struct kern_graphics_mode *);
int kern_platform_graphics_clear(void);
void kern_platform_graphics_leave(void);
int kern_platform_graphics_fill(const struct kern_graphics_rect *, uint32_t);
int kern_platform_graphics_line(unsigned, unsigned, unsigned, unsigned,
				uint32_t);
int kern_platform_graphics_pattern_fill(const struct kern_graphics_rect *,
					uint32_t, uint64_t);
int kern_platform_graphics_blit(unsigned, unsigned,
				const struct kern_graphics_image *, uint64_t, int);
int kern_platform_graphics_flush(const struct kern_graphics_rect *, size_t);
int kern_platform_graphics_get_glyph(uint32_t, uint8_t[32], unsigned *,
				     unsigned *);
void kern_platform_restore_text(void);
void kern_platform_debug_write(const char *text);
void kern_platform_halt(void) __attribute__((noreturn));
void kern_platform_reboot(void) __attribute__((noreturn));

#endif
