/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The board's framebuffer: the display the firmware set up, drawn on
 * directly.
 */

#ifndef KERN_HAL_PPC64_PSERIES_FRAMEBUFFER_H
#define KERN_HAL_PPC64_PSERIES_FRAMEBUFFER_H

#include <hal/types.h>

/* The grid of text cells the framebuffer holds exactly. */
#define VIRT_CONSOLE_COLUMNS	80U
#define VIRT_CONSOLE_ROWS	25U

int
virt_framebuffer_init(
	uintptr_t address,
	uint32_t width,
	uint32_t height,
	uint32_t linebytes,
	uint32_t depth);

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
virt_framebuffer_flush(
	void);

void
virt_framebuffer_describe(
	void);

#endif
