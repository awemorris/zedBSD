/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The armv7 floating-point unit's registers, read and written for the
 * lazy switch in fpu.c.
 *
 * The kernel is built without the unit, so its instructions are allowed
 * here only, by the assembler's .fpu directive: VFPv3 with thirty-two
 * doubleword registers, which the Cortex-A9 of the ZYBO and the Cortex-A15
 * of the emulator's virt board both have.  The upper sixteen registers
 * are moved only when the caller says the unit has them (MVFR0), so the
 * same code serves a unit with sixteen.
 *
 * A save or a load on a processor whose unit is off for the caller is
 * an undefined instruction, as the lazy switch expects: the trap turns the
 * unit on and the same instruction runs again.
 *
 * Values referenced: ARM DDI 0406C, VMRS and VMSR with FPSID, FPSCR,
 * MVFR0 and FPEXC; VSTM and VLDM of doubleword registers.
 */

#include "defs.h"

	.syntax unified
	.arm
	.fpu vfpv3
	.text

	/* Reads FPEXC, the unit's enable and exception state. */
	.balign 4
	.global armv7_fpu_read_fpexc
	.type armv7_fpu_read_fpexc, %function
armv7_fpu_read_fpexc:
	vmrs r0, fpexc
	bx lr
	.size armv7_fpu_read_fpexc, . - armv7_fpu_read_fpexc

	/* Writes FPEXC from r0. */
	.balign 4
	.global armv7_fpu_write_fpexc
	.type armv7_fpu_write_fpexc, %function
armv7_fpu_write_fpexc:
	vmsr fpexc, r0
	bx lr
	.size armv7_fpu_write_fpexc, . - armv7_fpu_write_fpexc

	/* Reads FPSID, which names the unit. */
	.balign 4
	.global armv7_fpu_read_fpsid
	.type armv7_fpu_read_fpsid, %function
armv7_fpu_read_fpsid:
	vmrs r0, fpsid
	bx lr
	.size armv7_fpu_read_fpsid, . - armv7_fpu_read_fpsid

	/* Reads MVFR0, which says how many registers the unit has. */
	.balign 4
	.global armv7_fpu_read_mvfr0
	.type armv7_fpu_read_mvfr0, %function
armv7_fpu_read_mvfr0:
	vmrs r0, mvfr0
	bx lr
	.size armv7_fpu_read_mvfr0, . - armv7_fpu_read_mvfr0

	/*
	 * Stores the unit's registers at r0 (struct hal_fpregs): the status
	 * and control register first, then d0 to d15, and d16 to d31 when r1
	 * is nonzero.
	 */
	.balign 4
	.global armv7_fpu_save
	.type armv7_fpu_save, %function
armv7_fpu_save:
	vmrs r2, fpscr
	str r2, [r0, #ARMV7_FPREGS_FPSCR]
	vstmia r0!, {d0-d15}
	cmp r1, #0
	beq 1f
	vstmia r0, {d16-d31}
1:	bx lr
	.size armv7_fpu_save, . - armv7_fpu_save

	/*
	 * Loads the unit's registers from r0 (struct hal_fpregs): d0 to d15,
	 * d16 to d31 when r1 is nonzero, then the status and control
	 * register.
	 */
	.balign 4
	.global armv7_fpu_load
	.type armv7_fpu_load, %function
armv7_fpu_load:
	mov r3, r0
	vldmia r3!, {d0-d15}
	cmp r1, #0
	beq 1f
	vldmia r3, {d16-d31}
1:	ldr r2, [r0, #ARMV7_FPREGS_FPSCR]
	vmsr fpscr, r2
	bx lr
	.size armv7_fpu_load, . - armv7_fpu_load
