/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The pseries board's firmware reading and handoff, which its start calls.
 */

#ifndef KERN_HAL_PPC64_PSERIES_BSP_H
#define KERN_HAL_PPC64_PSERIES_BSP_H

#include <hal/hal.h>
#include "../board.h"

void
pseries_firmware_read(
	void);

const struct kern_boot_handoff *
pseries_kernel_handoff(
	void);

#endif
