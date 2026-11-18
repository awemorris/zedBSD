/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The virt board's framebuffer: a RAM display the emulator shows.
 */

#ifndef KERN_HAL_RISCV64_BSP_VIRT_FRAMEBUFFER_H
#define KERN_HAL_RISCV64_BSP_VIRT_FRAMEBUFFER_H

#include <hal/types.h>

int
virt_framebuffer_init(
	void);

void
virt_framebuffer_describe(
	void);

#endif
