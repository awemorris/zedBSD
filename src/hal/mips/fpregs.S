/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The MIPS floating-point registers: their save and load, for the lazy
 * switch of fpu.c.
 *
 * Both run with Status.CU1 and Status.FR set by their caller, so that
 * the thirty-two registers are 64 bits wide (the FR=1 model every task
 * that uses the unit runs with).  The layout is struct hal_fpregs: the
 * registers by number, eight bytes each, then FCSR and FIR.
 */

#include "asm.h"

	.set noreorder
	.set noat
	.set hardfloat
	.set fp=64
	.set oddspreg

	.text
	.balign 4

	/*
	 * Stores the unit's registers, FCSR and FIR at a0.
	 */
	.global mips_fpu_save
	.type mips_fpu_save, %function
mips_fpu_save:
	sdc1 $f0, 0($4)
	sdc1 $f1, 8($4)
	sdc1 $f2, 16($4)
	sdc1 $f3, 24($4)
	sdc1 $f4, 32($4)
	sdc1 $f5, 40($4)
	sdc1 $f6, 48($4)
	sdc1 $f7, 56($4)
	sdc1 $f8, 64($4)
	sdc1 $f9, 72($4)
	sdc1 $f10, 80($4)
	sdc1 $f11, 88($4)
	sdc1 $f12, 96($4)
	sdc1 $f13, 104($4)
	sdc1 $f14, 112($4)
	sdc1 $f15, 120($4)
	sdc1 $f16, 128($4)
	sdc1 $f17, 136($4)
	sdc1 $f18, 144($4)
	sdc1 $f19, 152($4)
	sdc1 $f20, 160($4)
	sdc1 $f21, 168($4)
	sdc1 $f22, 176($4)
	sdc1 $f23, 184($4)
	sdc1 $f24, 192($4)
	sdc1 $f25, 200($4)
	sdc1 $f26, 208($4)
	sdc1 $f27, 216($4)
	sdc1 $f28, 224($4)
	sdc1 $f29, 232($4)
	sdc1 $f30, 240($4)
	sdc1 $f31, 248($4)

	/*
	 * Stores the control registers after the data registers.
	 *
	 * Values referenced: MIPS32 Architecture for Programmers Volume I
	 * (FCSR is control register 31, FIR control register 0).
	 */
	cfc1 $8, $31
	sw $8, MIPS_FPREGS_FCSR($4)
	cfc1 $8, $0
	jr $31
	sw $8, MIPS_FPREGS_FIR($4)
	.size mips_fpu_save, . - mips_fpu_save

	/*
	 * Loads the unit's registers and FCSR from a0.  FCSR's Cause field
	 * is cleared before it is written: a Cause bit set with its Enable
	 * bit would raise the floating-point exception at this very write.
	 */
	.global mips_fpu_restore
	.type mips_fpu_restore, %function
mips_fpu_restore:
	ldc1 $f0, 0($4)
	ldc1 $f1, 8($4)
	ldc1 $f2, 16($4)
	ldc1 $f3, 24($4)
	ldc1 $f4, 32($4)
	ldc1 $f5, 40($4)
	ldc1 $f6, 48($4)
	ldc1 $f7, 56($4)
	ldc1 $f8, 64($4)
	ldc1 $f9, 72($4)
	ldc1 $f10, 80($4)
	ldc1 $f11, 88($4)
	ldc1 $f12, 96($4)
	ldc1 $f13, 104($4)
	ldc1 $f14, 112($4)
	ldc1 $f15, 120($4)
	ldc1 $f16, 128($4)
	ldc1 $f17, 136($4)
	ldc1 $f18, 144($4)
	ldc1 $f19, 152($4)
	ldc1 $f20, 160($4)
	ldc1 $f21, 168($4)
	ldc1 $f22, 176($4)
	ldc1 $f23, 184($4)
	ldc1 $f24, 192($4)
	ldc1 $f25, 200($4)
	ldc1 $f26, 208($4)
	ldc1 $f27, 216($4)
	ldc1 $f28, 224($4)
	ldc1 $f29, 232($4)
	ldc1 $f30, 240($4)
	ldc1 $f31, 248($4)

	/*
	 * Loads FCSR without its Cause bits, so the load raises nothing.
	 *
	 * Value referenced: MIPS32 Architecture for Programmers Volume I
	 * (FCSR is control register 31).
	 */
	lw $8, MIPS_FPREGS_FCSR($4)
	li $9, ~MIPS_FCSR_CAUSE_MASK
	and $8, $8, $9
	ctc1 $8, $31
	jr $31
	nop
	.size mips_fpu_restore, . - mips_fpu_restore
