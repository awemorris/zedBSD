/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * What the MC68040 code asks of the board it runs on.
 *
 * The processor's parts (the exceptions, the pages, the address spaces,
 * the tasks) are the same on every board; the RAM, the interrupt sources
 * behind each level and the early console are the board's.  Each board
 * support package defines these functions once.
 */

#ifndef KERN_HAL_M68K_M68040_BOARD_H
#define KERN_HAL_M68K_M68040_BOARD_H

#include <hal/types.h>

/* The most separate RAM ranges a board reports. */
#define M68040_MEMORY_BANKS_MAX		8U

/*
 * One physically contiguous range of RAM.  A board with holes in its
 * physical memory reports one bank for each contiguous piece.
 */
struct m68040_memory_bank {
	uint32_t base;
	uint32_t size;
};

unsigned m68040_board_memory(const struct m68040_memory_bank **banks);
bool m68040_board_reserved(unsigned index, uint32_t *base, uint32_t *size);
void m68040_board_interrupt(unsigned level);
void m68040_board_putc(int character);

#endif
