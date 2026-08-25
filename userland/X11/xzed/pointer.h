/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef XZED_POINTER_H
#define XZED_POINTER_H

#include <stdint.h>

static inline void
xzed_pointer_move(int *x, int *y, int32_t dx, int32_t dy, unsigned width,
		  unsigned height)
{
	int64_t next_x = (int64_t)*x + dx;
	int64_t next_y = (int64_t)*y + dy;

	if (next_x < 0)
		next_x = 0;
	if (next_y < 0)
		next_y = 0;
	if (width != 0 && next_x >= (int64_t)width)
		next_x = (int64_t)width - 1;
	if (height != 0 && next_y >= (int64_t)height)
		next_y = (int64_t)height - 1;
	*x = (int)next_x;
	*y = (int)next_y;
}

#endif
