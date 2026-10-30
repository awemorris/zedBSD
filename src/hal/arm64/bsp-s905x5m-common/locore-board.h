/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The ODROID-C5's part of locore.S: assembler macros the boot path
 * expands.
 */

#ifndef KERN_HAL_ARM64_BSP_S905X5M_LOCORE_BOARD_H
#define KERN_HAL_ARM64_BSP_S905X5M_LOCORE_BOARD_H

/*
 * Puts the image where it is linked, then goes on at arm64_boot_placed.
 *
 * The kernel runs only at the physical address it is linked for.  A
 * bootloader that put it elsewhere (above it) has it copied there first.
 * The copy runs with the MMU off, where data accesses reach memory
 * directly.  This stub, with its literals, is copied first and the rest is
 * copied from the stub's new place, so no instruction is overwritten while
 * it runs; a forward copy to a lower address never overwrites a byte it
 * has still to read.  An image below its place cannot be moved up safely
 * and parks.
 */
.macro arm64_board_place_image
	adr x1, _start
	ldr x2, =_start
	movz x3, #0xffff, lsl #48
	sub x2, x2, x3
	cmp x1, x2
	b.eq arm64_boot_placed
	b.lo arm64_secondary_park

	/* Copies the image's first bytes: the header, this stub and its literals. */
	mov x4, x1
	mov x5, x2
	mov x6, #(arm64_boot_stub_end - _start)
1:	ldp x7, x8, [x4], #16
	stp x7, x8, [x5], #16
	subs x6, x6, #16
	b.hi 1b
	dsb sy
	ic iallu
	dsb sy
	isb
	add x9, x2, #(arm64_boot_copy_rest - _start)
	br x9

arm64_boot_copy_rest:
	/*
	 * Copies the whole loaded image, the stub's bytes again unchanged.  Its
	 * length is the header's image size, the raw file the bootloader read.
	 */
	ldr x6, [x1, #16]
	mov x4, x1
	mov x5, x2
2:	ldp x7, x8, [x4], #16
	stp x7, x8, [x5], #16
	subs x6, x6, #16
	b.hi 2b
	dsb sy
	ic iallu
	dsb sy
	isb
	add x9, x2, #(arm64_boot_placed - _start)
	br x9
	.ltorg
	.balign 16
arm64_boot_stub_end:
.endm

/*
 * Gives x3 the level-1 block descriptor of the boot's device gigabyte, and
 * x4 its byte offset in the level-1 table: Device-nGnRE for
 * 0xc000_0000..0xffff_ffff, where the S905X5M's peripherals are (the
 * debug UART at 0xfe07_a000, the GIC at 0xfff0_1000, the VPU at
 * 0xff90_0000).
 */
.macro arm64_board_boot_device_block
	mov x3, #0xc0000000
	add x3, x3, #0x405
	mov x4, #(3 * 8)
.endm

/*
 * Turns the MPIDR_EL1 value in a register into the core's number: the
 * Cortex-A55 has MT set and numbers its cores in Aff1 (bits 8-15), with
 * Aff0 always 0.  arm64_bsp_core_of() computes the same in C.
 */
.macro arm64_board_core_index register
	ubfx \register, \register, #8, #8
.endm

/*
 * Maps the second GiB of RAM (0x4000_0000..0x7fff_ffff) Normal WB into
 * the boot's tables: the bootloader's framebuffer, which the HAL clears
 * and draws on before the kernel's own tables are built, is at
 * 0x7f80_0000.
 */
.macro arm64_board_boot_extra_ram
	mov x3, #0x40000000
	add x3, x3, #0x701
	adrp x1, arm64_l1_low
	add x1, x1, :lo12:arm64_l1_low
	str x3, [x1, #8]
	adrp x1, arm64_l1_high
	add x1, x1, :lo12:arm64_l1_high
	str x3, [x1, #8]
.endm

#endif
