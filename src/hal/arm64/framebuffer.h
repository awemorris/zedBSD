/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The firmware's or the bootloader's framebuffer, which the early console
 * and the stall watch paint text cells on.  The board finds it
 * (arm64_bsp_framebuffer_init() in bsp.h) and hands it to
 * arm64_framebuffer_adopt().  The kernel's tables give the painting its
 * own window of the framebuffer (arm64_framebuffer_move()), and the
 * kernel's console ends it for good (arm64_framebuffer_forget()).
 */

#ifndef KERN_HAL_ARM64_FRAMEBUFFER_H
#define KERN_HAL_ARM64_FRAMEBUFFER_H

#include <hal/types.h>

int arm64_framebuffer_adopt(uint64_t phys, uint64_t size, uint32_t pixel_width, uint32_t pixel_height, uint32_t row_pitch, uint32_t pixel_order, uint32_t opaque_bits);
int arm64_framebuffer_ready(void);
int arm64_framebuffer_extent(uint64_t *phys, uint64_t *size);
void arm64_framebuffer_move(volatile uint8_t *address, int cached);
void arm64_framebuffer_forget(void);
int arm64_framebuffer_given_up(uint64_t phys, uint64_t size);
void arm64_framebuffer_test_pattern(void);
void arm64_framebuffer_describe(void);
void arm64_framebuffer_cell(unsigned row, unsigned column, int character, uint8_t attribute);
void arm64_framebuffer_cursor(unsigned row, unsigned column, int visible);

#endif
