/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The ROM's services the second stage calls, as C functions.
 *
 * The ROM's register-based traps take their parameter block in a0 and
 * report a result code in d0; Gestalt takes its selector in d0 and
 * answers in a0.  A trap may change d0 to d2, a0 and a1; d2 is saved
 * here because C keeps it across calls.
 */

/*
 * A trap that takes a parameter block: int16_t name(void *block).
 *
 * Values referenced: Apple, Inside Macintosh: Files (1992) and
 * Inside Macintosh: Devices (1994) (the trap words of _Close, _Read,
 * _Control, _MountVol, _GetEOF and the HFS variant of _Open).
 */
.macro parameter_block_trap name, word
	.text
	.globl \name
	.type \name,@function
\name:
	move.l %d2,-(%sp)
	movea.l 8(%sp),%a0
	.short \word
	ext.l %d0
	move.l (%sp)+,%d2
	rts
	.size \name,.-\name
.endm

	parameter_block_trap mac68k_trap_close, 0xa001
	parameter_block_trap mac68k_trap_read, 0xa002
	parameter_block_trap mac68k_trap_control, 0xa004
	parameter_block_trap mac68k_trap_mount_volume, 0xa00f
	parameter_block_trap mac68k_trap_get_eof, 0xa011
	parameter_block_trap mac68k_trap_hfs_open, 0xa200

/*
 * void *mac68k_trap_new_pointer(uint32_t size, int system)
 *
 * Takes a nonrelocatable block from the current heap, or from the system
 * heap when system is nonzero; reports NULL when there is no room.
 *
 * Values referenced: Apple, Inside Macintosh: Memory (1992) (the trap
 * words of _NewPtr with its SYS bit and of _MaxApplZone, below).
 */
	.text
	.globl mac68k_trap_new_pointer
	.type mac68k_trap_new_pointer,@function
mac68k_trap_new_pointer:
	move.l %d2,-(%sp)
	move.l 8(%sp),%d0
	tst.l 12(%sp)
	bne.s 1f
	.short 0xa11e
	bra.s 2f
1:	.short 0xa51e
2:	tst.w %d0
	beq.s 3f
	suba.l %a0,%a0
3:	move.l %a0,%d0
	move.l (%sp)+,%d2
	rts
	.size mac68k_trap_new_pointer,.-mac68k_trap_new_pointer

/*
 * void mac68k_trap_max_application_zone(void)
 *
 * Grows the application heap to its limit.
 */
	.globl mac68k_trap_max_application_zone
	.type mac68k_trap_max_application_zone,@function
mac68k_trap_max_application_zone:
	move.l %d2,-(%sp)
	.short 0xa063
	move.l (%sp)+,%d2
	rts
	.size mac68k_trap_max_application_zone,.-mac68k_trap_max_application_zone

/*
 * int32_t mac68k_trap_gestalt(uint32_t selector, uint32_t *response)
 *
 * Asks Gestalt; reports its result code and stores the answer.
 *
 * Value referenced: Apple, Inside Macintosh: Operating System Utilities
 * (1994) (the trap word of _Gestalt).
 */
	.globl mac68k_trap_gestalt
	.type mac68k_trap_gestalt,@function
mac68k_trap_gestalt:
	move.l %d2,-(%sp)
	move.l 8(%sp),%d0
	.short 0xa1ad
	movea.l 12(%sp),%a1
	move.l %a0,(%a1)
	ext.l %d0
	move.l (%sp)+,%d2
	rts
	.size mac68k_trap_gestalt,.-mac68k_trap_gestalt
