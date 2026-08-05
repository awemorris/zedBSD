/*
 * Boots
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Boots 1.44 MiB floppy Stage 0.
 *
 * The boot sector doubles as the FAT12 BPB: make-fdd-image.sh merges the
 * code below around the BPB that mformat writes, so bytes 11..61 belong
 * to the filesystem.  The code loads Stage 1 raw from the reserved
 * sectors 3..16 at 1000:0000 and enters it with the version-1 bootstrap
 * handoff describing the FAT12 boot volume at LBA 0.
 */
	.code16
	.section .text
	.globl _start

	.set HANDOFF,          0x0700
	.set HANDOFF_MAGIC,    0x4839384c
	.set HANDOFF_VERSION,  1
	.set HANDOFF_SIZE,     24
	.set HO_FS_FAT12,      2

_start:
	jmp	start
	nop
	.ascii	"IPL1    "          /* OEM name doubles as the IPL tag. */
	/* BPB area (bytes 11..61), populated when the image is built. */
	.org	0x3e
start:
	cld
	pushw	%ds
	xorw	%ax, %ax
	movw	%ax, %ds
	movw	%si, HANDOFF+20
	movw	%di, HANDOFF+22
	movl	$HANDOFF_MAGIC, HANDOFF
	movw	$HANDOFF_VERSION, HANDOFF+4
	movw	$HANDOFF_SIZE, HANDOFF+6
	movl	$0, HANDOFF+8       /* the volume starts at LBA 0 */
	movb	$2, HANDOFF+13      /* heads */
	movb	$18, HANDOFF+14     /* sectors per track */
	movb	$HO_FS_FAT12, HANDOFF+15
	movw	$512, HANDOFF+16
	movw	$1, HANDOFF+18      /* geometry is valid */
	movb	0x0584, %al
	andb	$0x0f, %al
	orb	$0x30, %al
	movb	%al, HANDOFF+12     /* boot BIOS id, kept in AL for the read */
	popw	%ds
	movw	$0x1000, %bx
	movw	%bx, %es
	xorw	%bp, %bp
	movw	$(14 * 512), %bx
	movw	$0x0200, %cx        /* N=512, cylinder 0 */
	movw	$0x0003, %dx        /* head 0, sector 3 */
	movb	$0x56, %ah
	int	$0x1b
	jc	failure
	ljmp	$0x1000, $0
failure:
	hlt
	jmp	failure
	.org 510
	.word 0xaa55
