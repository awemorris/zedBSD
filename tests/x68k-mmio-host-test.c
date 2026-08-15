/* X68000 byte-lane and direct-map address tests. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include <stdio.h>
#include "src/hal/m68k/bsp-x68k/mmio.h"

#define CHECK(x) do { if (!(x)) { \
	fprintf(stderr, "FAIL:%d: %s\n", __LINE__, #x); return 1; \
} } while (0)

int
main(void)
{
	CHECK(X68K_MFP_ADDRESS(0) == 0x80e88001U);
	CHECK(X68K_MFP_ADDRESS(23) == 0x80e8802fU);
	CHECK(X68K_SPC_ADDRESS(0) == 0x80e96021U);
	CHECK(X68K_SPC_ADDRESS(14) == 0x80e9603dU);
	CHECK(X68K_DEVICE_ADDRESS(X68K_TVRAM_PHYSICAL) == 0x80e00000U);
	puts("X68k MMIO host tests passed");
	return 0;
}
