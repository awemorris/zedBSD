/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Raspberry Pi 4's part of locore.S: assembler macros the boot path
 * expands.
 */

#ifndef KERN_HAL_ARM64_BSP_RPI4_LOCORE_BOARD_H
#define KERN_HAL_ARM64_BSP_RPI4_LOCORE_BOARD_H

/*
 * Puts the image where it is linked.  The Pi 4's firmware loads it at
 * kernel_address (0x80000) itself, so there is nothing to do.
 */
.macro arm64_board_place_image
.endm

/*
 * Gives x3 the level-1 block descriptor of the boot's device gigabyte, and
 * x4 its byte offset in the level-1 table: Device-nGnRE for
 * 0xc000_0000..0xffff_ffff, where the firmware's peripherals are.
 */
.macro arm64_board_boot_device_block
	mov x3, #0xc0000000
	add x3, x3, #0x405
	mov x4, #(3 * 8)
.endm

/*
 * Turns the MPIDR_EL1 value in a register into the core's number: the
 * Cortex-A72 numbers its cores in Aff0.  arm64_bsp_core_of() computes the
 * same in C.
 */
.macro arm64_board_core_index register
	and \register, \register, #0xff
.endm

/*
 * Maps RAM past the first GiB into the boot's tables, for a board whose
 * early boot touches it; the Raspberry Pi's framebuffer is in the first
 * GiB, so nothing more is mapped.
 */
.macro arm64_board_boot_extra_ram
.endm

#endif
