/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The firmware's or the bootloader's framebuffer, which the early console
 * and the stall watch paint text cells on.  rpi_framebuffer_init() is the
 * board's: it finds the framebuffer and hands it to
 * rpi_framebuffer_adopt().  The kernel's tables give the painting its own
 * window of the framebuffer (rpi_framebuffer_move()), and the kernel's
 * console ends it for good (rpi_framebuffer_forget()).
 */

#ifndef KERN_HAL_ARM64_BSP_RPI_COMMON_FRAMEBUFFER_H
#define KERN_HAL_ARM64_BSP_RPI_COMMON_FRAMEBUFFER_H

#include <hal/types.h>

int rpi_framebuffer_init(uintptr_t mailbox_phys);
int rpi_framebuffer_adopt(uint64_t phys, uint64_t size, uint32_t pixel_width, uint32_t pixel_height, uint32_t row_pitch, uint32_t pixel_order, uint32_t opaque_bits);
int rpi_framebuffer_ready(void);
int rpi_framebuffer_extent(uint64_t *phys, uint64_t *size);
void rpi_framebuffer_move(volatile uint8_t *address, int cached);
void rpi_framebuffer_forget(void);
int rpi_framebuffer_given_up(uint64_t phys, uint64_t size);
void rpi_framebuffer_test_pattern(void);
void rpi_framebuffer_describe(void);
void rpi_framebuffer_cell(unsigned row, unsigned column, int character, uint8_t attribute);
void rpi_framebuffer_cursor(unsigned row, unsigned column, int visible);

#endif
