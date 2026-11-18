/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The armv7 HAL's checks of itself at boot.
 */

#ifndef KERN_HAL_ARMV7_SELFTEST_H
#define KERN_HAL_ARMV7_SELFTEST_H

void
armv7_aeabi_selftest(
	void);

void
armv7_atomic_selftest(
	void);

void
armv7_paging_selftest(
	void);

int
armv7_vfp_selftest(
	void);

#endif
