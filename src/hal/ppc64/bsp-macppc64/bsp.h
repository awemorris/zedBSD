/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The 64-bit PowerPC Mac's firmware reading and handoff, which its start
 * calls.
 */

#ifndef KERN_HAL_PPC64_MACPPC64_BSP_H
#define KERN_HAL_PPC64_MACPPC64_BSP_H

#include <hal/hal.h>
#include "../board.h"

void
macppc64_firmware_read(
	void);

void
macppc64_set_table(
	uint64_t base,
	uint32_t shift);

const char *
macppc64_model(
	void);

const struct kern_boot_handoff *
macppc64_kernel_handoff(
	void);

#endif
