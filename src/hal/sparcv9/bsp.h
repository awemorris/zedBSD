/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The sun4u board support the SPARC V9 HAL calls into.
 */

#ifndef KERN_HAL_SPARCV9_BSP_H
#define KERN_HAL_SPARCV9_BSP_H

#include <kern/boot.h>
#include "bootloader/sparcv9/handoff.h"

/*
 * Keeps a copy of the loader's handoff and builds the kernel's from it.
 */
void
sun4u_boot_init(
	const struct sun4u_boot_handoff *handoff);

/*
 * Returns the HAL's copy of the loader's handoff.
 */
const struct sun4u_boot_handoff *
sun4u_boot_handoff(
	void);

/*
 * Returns the handoff the kernel's entry reads: the common record alone.
 */
const struct kern_boot_handoff *
sun4u_kernel_handoff(
	void);

#endif
