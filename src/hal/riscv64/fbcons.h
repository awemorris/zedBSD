/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The riscv64 early console's drawing on a board's framebuffer.
 */

#ifndef KERN_HAL_RISCV64_FBCONS_H
#define KERN_HAL_RISCV64_FBCONS_H

#include <hal/types.h>

/* The grid of text cells the console keeps and draws. */
#define RISCV64_CONSOLE_COLUMNS	80U
#define RISCV64_CONSOLE_ROWS	25U

/*
 * A screen the early console draws on, as a board describes it.
 *
 * address is where the kernel reaches the first pixel, pitch the bytes
 * from one row to the next, and format a KERN_BOOT_FRAMEBUFFER_* code.
 * flush, when not NULL, writes a drawn range of the kernel's addresses
 * through to the memory the display reads; a board whose display sees
 * the processor's writes passes NULL.  The console keeps its own copy.
 */
struct riscv64_fbcons_screen {
	uintptr_t address;
	uint32_t width;
	uint32_t height;
	uint32_t pitch;
	uint32_t format;
	void (*flush)(uintptr_t start, size_t length);
};

int
riscv64_fbcons_attach(
	const struct riscv64_fbcons_screen *screen);

int
riscv64_fbcons_ready(
	void);

void
riscv64_fbcons_move(
	uintptr_t address);

void
riscv64_fbcons_detach(
	void);

void
riscv64_fbcons_cell(
	unsigned row,
	unsigned column,
	int character,
	uint8_t attribute);

void
riscv64_fbcons_cursor(
	unsigned row,
	unsigned column);

void
riscv64_fbcons_flush_cell(
	unsigned row,
	unsigned column);

void
riscv64_fbcons_flush_grid(
	void);

#endif
