/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The X68000 board support the MC68030 HAL calls into.
 */

#ifndef KERN_HAL_M68K_X68K_BSP_H
#define KERN_HAL_M68K_X68K_BSP_H

#include <kern/boot.h>
#include "bootloader/x68k/handoff.h"

/*
 * Keeps a copy of the loader's handoff and builds the kernel's from it.
 */
void
x68k_boot_init(
	const struct x68k_boot_handoff *handoff);

/*
 * Returns the HAL's copy of the loader's handoff.
 */
const struct x68k_boot_handoff *
x68k_boot_handoff(
	void);

/*
 * Returns the handoff the kernel's entry reads: the common record alone.
 */
const struct kern_boot_handoff *
x68k_kernel_handoff(
	void);

/*
 * Reports whether a loader's handoff is one the HAL can use.
 */
int
x68k_boot_handoff_valid(
	const struct x68k_boot_handoff *handoff);

/*
 * Sets the text screen's palette and clears the console.
 */
void
x68k_cons_init(
	void);

#endif
