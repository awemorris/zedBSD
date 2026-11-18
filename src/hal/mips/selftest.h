/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The MIPS self-test of the address spaces, the TLB and the lazy
 * floating-point switch.
 */

#ifndef KERN_HAL_MIPS_SELFTEST_H
#define KERN_HAL_MIPS_SELFTEST_H

void
mips_paging_selftest(
	void);

#endif
