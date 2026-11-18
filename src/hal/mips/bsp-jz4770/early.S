/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The RG350's marks of the boot before the display shows: the power
 * LED blinks the number of the stage, and the backlight is turned on.
 * An exception taken before the trap handling exists turns the
 * backlight off and blinks one long blink and then the exception's code,
 * over and over.
 *
 * locore.S calls mips_board_early_mark() before the bss and the stack
 * exist, so both functions are leaves that use neither: they write the
 * GPIO registers through KSEG1 and wait by counting loops.  They change
 * only their argument, the temporaries $8 to $13 and ra's use for the
 * return.
 */

#include "../asm.h"
#include "jz4770.h"

	.set noreorder

/*
 * Waits the given units of the marks' timing.  Uses $11 and $12.
 */
.macro MARK_WAIT units
	li $11, \units
.Lmark_unit\@:
	li $12, RG350_MARK_UNIT_LOOPS
.Lmark_loop\@:
	bnez $12, .Lmark_loop\@
	addiu $12, $12, -1
	addiu $11, $11, -1
	bnez $11, .Lmark_unit\@
	nop
.endm

/*
 * Makes the LED and the backlight's enable plain outputs, the LED off
 * (its pin high) and the backlight as the PAT0 offset given says.
 * Expects $8 = the GPIO block, $9 = the LED's bit, $10 = the
 * backlight's bit.
 */
.macro MARK_PINS backlight
	sw $9, (RG350_LED_PORT_OFFSET + JZ4770_GPIO_PAT0_SET)($8)
	sw $9, (RG350_LED_PORT_OFFSET + JZ4770_GPIO_INT_CLEAR)($8)
	sw $9, (RG350_LED_PORT_OFFSET + JZ4770_GPIO_MASK_SET)($8)
	sw $9, (RG350_LED_PORT_OFFSET + JZ4770_GPIO_PAT1_CLEAR)($8)
	sw $10, (RG350_BACKLIGHT_PORT_OFFSET + \backlight)($8)
	sw $10, (RG350_BACKLIGHT_PORT_OFFSET + JZ4770_GPIO_INT_CLEAR)($8)
	sw $10, (RG350_BACKLIGHT_PORT_OFFSET + JZ4770_GPIO_MASK_SET)($8)
	sw $10, (RG350_BACKLIGHT_PORT_OFFSET + JZ4770_GPIO_PAT1_CLEAR)($8)
	sync
.endm

/*
 * Blinks the LED once: lit for the units given, then dark for
 * RG350_MARK_OFF_UNITS.  Expects $8 and $9 as MARK_PINS.
 */
.macro MARK_BLINK units
	sw $9, (RG350_LED_PORT_OFFSET + JZ4770_GPIO_PAT0_CLEAR)($8)
	sync
	MARK_WAIT \units
	sw $9, (RG350_LED_PORT_OFFSET + JZ4770_GPIO_PAT0_SET)($8)
	sync
	MARK_WAIT RG350_MARK_OFF_UNITS
.endm

	.text
	.balign 4

/*
 * Shows a stage of the boot: a0 is its number, 1 for the kernel's first
 * instruction.  The backlight is turned on, a pause marks the start of
 * the series, and the LED blinks a0 times.
 */
	.global mips_board_early_mark
	.type mips_board_early_mark, %function
mips_board_early_mark:
	/* The pins as outputs, the backlight on. */
	li $8, JZ4770_GPIO_BASE
	li $9, RG350_LED_BIT
	li $10, RG350_BACKLIGHT_BIT
	MARK_PINS JZ4770_GPIO_PAT0_SET

	/* The pause that starts a series. */
	MARK_WAIT RG350_MARK_LEAD_UNITS

	/* One blink for each stage. */
	move $13, $4
1:
	beqz $13, 2f
	nop
	MARK_BLINK RG350_MARK_ON_UNITS
	b 1b
	addiu $13, $13, -1
2:
	jr $31
	nop
	.size mips_board_early_mark, . - mips_board_early_mark

/*
 * Shows an exception taken before the trap handling exists, and never
 * returns: a0 is the exception's code from Cause.  The backlight goes
 * off, and the LED blinks once long and then a0 times short, with a
 * pause between the series, until the board is reset.
 */
	.global mips_board_early_fault
	.type mips_board_early_fault, %function
mips_board_early_fault:
	/* The pins as outputs, the backlight off. */
	li $8, JZ4770_GPIO_BASE
	li $9, RG350_LED_BIT
	li $10, RG350_BACKLIGHT_BIT
	MARK_PINS JZ4770_GPIO_PAT0_CLEAR

	/* The series, over and over: the long blink, the code's short blinks, the pause. */
1:
	MARK_BLINK RG350_FAULT_LONG_UNITS
	move $13, $4
2:
	beqz $13, 3f
	nop
	MARK_BLINK RG350_MARK_ON_UNITS
	b 2b
	addiu $13, $13, -1
3:
	MARK_WAIT RG350_MARK_LEAD_UNITS
	b 1b
	nop
	.size mips_board_early_fault, . - mips_board_early_fault
