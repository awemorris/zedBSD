/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The virt board's processor setup before the MMU is on: none.
 *
 * The processor enters non-secure, where the auxiliary control register
 * may not be written, and the emulator's caches are coherent by
 * construction, so there is nothing to turn on (bsp.h).
 */

	.syntax unified
	.arm

	.text
	.balign 4
	.global bsp_cpu_early_setup
	.type bsp_cpu_early_setup, %function
bsp_cpu_early_setup:
	bx lr
	.size bsp_cpu_early_setup, . - bsp_cpu_early_setup
