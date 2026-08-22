/*
 * zedBSD adapters for the selected musl string scanner.
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#include "src/softfloat/musl-floatscan.h"

int
__uflow(FILE *stream)
{
	(void)stream;
	return EOF;
}
