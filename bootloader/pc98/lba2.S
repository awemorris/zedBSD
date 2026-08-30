/* SPDX-License-Identifier: Zlib */
/*
 * Generic PC-98 LBA 2 boot-partition selector.
 * Copyright (C) 2026 Awe Morris
 *
 * disk-ipl.S loads the first sector of this image at 1fc0:0000.  The stub
 * relocates itself, reads the native PC-98 partition table from LBA 1, finds
 * the partition whose 16-byte name is exactly "BOOT", and chain-loads the
 * first sector at that entry's IPL-start CHS.  The remaining LBA 3..15 area
 * is intentionally zero-filled so a user may replace the complete 14-sector
 * image with another fixed-disk menu implementation.
 */

	.code16
	.section .text
	.globl _start

	.set RELOC_SEG, 0x9000
	.set LOAD_SEG,  0x1fc0
	.set TABLE_SEG, 0x0800

_start:
	cli
	cld
	movw	%cs, %ax
	movw	%ax, %ds
	movw	%si, saved_si
	movw	%di, saved_di
	movw	$RELOC_SEG, %ax
	movw	%ax, %ss
	movw	$0xfffe, %sp
	movw	%ax, %es
	xorw	%si, %si
	xorw	%di, %di
	movw	$256, %cx
	rep movsw
	ljmp	$RELOC_SEG, $relocated

relocated:
	movw	%cs, %ax
	movw	%ax, %ds
	sti
#if ZEDBSD_PC98_IPL_DIAGNOSTIC
	/* Four bounded tones prove that LBA 0 transferred control to LBA 2. */
	movw	$0x4532, %dx	/* "2E": Stage 2 entry. */
	movw	$4, %cx
	call	diagnostic_checkpoint
#endif

	/* The BIOS work area records the fixed disk selected by the ITF. */
	pushw	%ds
	xorw	%ax, %ax
	movw	%ax, %ds
	movb	0x0584, %al
	popw	%ds
	andb	$0x7f, %al
	orb	$0x80, %al
	movb	%al, boot_id

	/* LBA 1 is physical CHS 0/0/1 and contains sixteen 32-byte entries. */
	movw	$TABLE_SEG, %ax
	movw	%ax, %es
	xorw	%bp, %bp
	xorw	%cx, %cx
	movw	$0x0001, %dx
	call	read_sector
	jc	boot_failure_table

	movw	$TABLE_SEG, %ax
	movw	%ax, %ds
	xorw	%si, %si
	movw	$16, %cx
find_partition:
	cmpb	$0, (%si)
	je	next_partition
	pushw	%cx
	pushw	%si
	addw	$16, %si
	xorw	%bx, %bx
	movw	$16, %cx
compare_name:
	lodsb
	cmpb	%cs:boot_name(%bx), %al
	jne	name_mismatch
	incw	%bx
	loop	compare_name
	popw	%si
	popw	%cx
	jmp	partition_found
name_mismatch:
	popw	%si
	popw	%cx
next_partition:
	addw	$32, %si
	loop	find_partition
	jmp	boot_failure_name

partition_found:
	/* Native entry bytes 4..7 are IPL-start sector, head, cylinder. */
	movw	6(%si), %cx
	movw	4(%si), %dx
	pushw	%cs
	popw	%ds
	movw	$LOAD_SEG, %ax
	movw	%ax, %es
	xorw	%bp, %bp
	call	read_sector
	jc	boot_failure_pbr

	/* Preserve the normal firmware entry contract for the partition IPL. */
	xorw	%ax, %ax
	movw	%ax, %ds
	movb	%cs:boot_id, %al
	movb	%al, 0x0584
	movw	%cs:saved_si, %si
	movw	%cs:saved_di, %di
	sti
	ljmp	$LOAD_SEG, $0

read_sector:
	pushw	%ds
	xorw	%ax, %ax
	movw	%ax, %ds
	movb	%cs:boot_id, %al
	movb	$0x06, %ah
	movw	$512, %bx
	movw	%cs:saved_si, %si
	movw	%cs:saved_di, %di
	int	$0x1b
	pushfw
	popw	%ax
	movw	%ax, %cs:read_flags
	popw	%ds
	pushw	%cs:read_flags
	popfw
	ret
boot_failure_table:
	movw	$0x5432, %dx	/* "2T": partition-table read failed. */
#if ZEDBSD_PC98_IPL_DIAGNOSTIC
	movw	$5, %cx
#endif
	jmp	boot_failure
boot_failure_name:
	movw	$0x4e32, %dx	/* "2N": BOOT partition name not found. */
#if ZEDBSD_PC98_IPL_DIAGNOSTIC
	movw	$6, %cx
#endif
	jmp	boot_failure
boot_failure_pbr:
	movw	$0x5032, %dx	/* "2P": partition boot-record read failed. */
#if ZEDBSD_PC98_IPL_DIAGNOSTIC
	movw	$7, %cx
#endif
boot_failure:
	movw	$0xa000, %ax
	movw	%ax, %es
	xorw	%bx, %bx
	xorw	%ax, %ax
	movb	%dl, %al
	movw	%ax, %es:(%bx)
	movb	%dh, %al
	movw	%ax, %es:2(%bx)
	movw	$0xa200, %ax
	movw	%ax, %es
	movw	$0x00e1, %es:(%bx)
	movw	$0x00e1, %es:2(%bx)
#if ZEDBSD_PC98_IPL_DIAGNOSTIC
	call	diagnostic_checkpoint
#else
	movb	%dl, %al
	outb	%al, $0xe9
	movb	%dh, %al
	outb	%al, $0xe9
	movb	$0x06, %al
	outb	%al, $0x37
#endif
1:
	hlt
	jmp	1b

#if ZEDBSD_PC98_IPL_DIAGNOSTIC
/* Keep the diagnostic stage self-contained; the two fixed IPL binaries do
 * not share runtime code or state.  See disk-ipl.S for the marker contract. */
diagnostic_checkpoint:
	pushw	%ax
	pushw	%bx
	pushw	%cx
	pushw	%dx
	movb	$'P', %al
	outb	%al, $0xe9
	movb	%dl, %al
	outb	%al, $0xe9
	movb	%dh, %al
	outb	%al, $0xe9
	movb	$'\n', %al
	outb	%al, $0xe9
	movb	%cl, %bl
1:
	movb	$0x06, %ah
	movw	$8, %cx
	movw	$1000, %dx
	int	$0x1c
	movw	$8, %cx
	call	diagnostic_wait_ticks
	movb	$0x06, %ah
	movw	$8, %cx
	movw	$1000, %dx
	int	$0x1c
	movb	$0x07, %al
	outb	%al, $0x37
	movw	$8, %cx
	call	diagnostic_wait_ticks
	decb	%bl
	jnz	1b
	movb	$0x06, %ah
	movw	$32, %cx
	movw	$1000, %dx
	int	$0x1c
	movb	$0x07, %al
	outb	%al, $0x37
	movw	$32, %cx
	call	diagnostic_wait_ticks
	popw	%dx
	popw	%cx
	popw	%bx
	popw	%ax
	ret

diagnostic_wait_ticks:
	sti
1:
	hlt
	loop	1b
	ret
#endif

saved_si: .word 0
saved_di: .word 0
read_flags: .word 0
boot_id: .byte 0
boot_name: .ascii "BOOT            "

	.org	0x01fe
	.word	0xaa55
	/* LBA 2 through 15 are distributed and replaced as one binary. */
	.org	(14 * 512)
