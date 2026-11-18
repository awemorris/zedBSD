/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The virt board's framebuffer: a RAM display the emulator shows.
 */

#ifndef KERN_HAL_RISCV32_BSP_VIRT_FRAMEBUFFER_H
#define KERN_HAL_RISCV32_BSP_VIRT_FRAMEBUFFER_H

#include <hal/types.h>

/* The grid of text cells the framebuffer holds exactly. */
#define VIRT_CONSOLE_COLUMNS	80U
#define VIRT_CONSOLE_ROWS	25U

int
virt_framebuffer_init(
	void);

int
virt_framebuffer_ready(
	void);

void
virt_framebuffer_cell(
	unsigned row,
	unsigned column,
	int character,
	uint8_t attribute);

void
virt_framebuffer_cursor(
	unsigned row,
	unsigned column);

void
virt_framebuffer_describe(
	void);

#endif
