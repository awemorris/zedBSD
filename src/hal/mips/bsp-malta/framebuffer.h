/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The malta board's framebuffer: the emulator's standard VGA on PCI.
 */

#ifndef KERN_HAL_MIPS_BSP_MALTA_FRAMEBUFFER_H
#define KERN_HAL_MIPS_BSP_MALTA_FRAMEBUFFER_H

#include <hal/types.h>

/* The grid of text cells the framebuffer holds exactly. */
#define MALTA_CONSOLE_COLUMNS	80U
#define MALTA_CONSOLE_ROWS	30U

int
malta_framebuffer_init(
	void);

int
malta_framebuffer_ready(
	void);

void
malta_framebuffer_cell(
	unsigned row,
	unsigned column,
	int character,
	uint8_t attribute);

void
malta_framebuffer_cursor(
	unsigned row,
	unsigned column);

void
malta_framebuffer_describe(
	void);

#endif
