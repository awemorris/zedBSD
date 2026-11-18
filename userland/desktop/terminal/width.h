/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * How many cells a character takes in terminal's grid (width.c).
 *
 * It has no other part of the terminal in it, so that the host tests
 * compile width.c on its own.
 */

#ifndef TERMINAL_WIDTH_H
#define TERMINAL_WIDTH_H

#include <stdint.h>

/*
 * One run of code points, first and last included, that a width table
 * lists.
 */
struct terminal_width_range {
	uint32_t first;
	uint32_t last;
};

int terminal_width_wide(uint32_t codepoint, int ambiguous_wide);
int terminal_width_ambiguous(uint32_t codepoint);

#endif
