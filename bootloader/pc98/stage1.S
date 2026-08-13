/* PC-98 MBR Stage 1: SENSE and load ZBL2 from raw LBA 1. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
.code16
.section .text
.global _start

#include "bootloader/include/disk-layout.inc"
#include "bootloader/include/stage2-header.inc"

.set BOOTINFO, 0x0700
.set BOOTINFO_MAGIC, 0x3150395a /* Z9P1 */
.set PC98_STAGE2_LBA, 2

_start:
	jmp start
	nop
	nop
	.ascii "IPL1"
start:
	cli
	cld
	movb $'1', %al
	outb %al, $0xe9
	pushw %cs
	popw %ds
	movw %si, saved_si
	movw %di, saved_di
	xorw %ax, %ax
	movw %ax, %ss
	movw $0x7000, %sp
	sti

	pushw %ds
	xorw %ax, %ax
	movw %ax, %ds
	movb 0x0584, %al
	popw %ds
	andb $0x7f, %al
	orb $0x80, %al
	movb %al, boot_drive
	call bios_sense
	jc error_sense

	movw $ZBL_STAGE2_SEG, target_segment
	movl $PC98_STAGE2_LBA, read_lba
	call read_one
	jc error_disk

	pushw %ds
	movw $ZBL_STAGE2_SEG, %ax
	movw %ax, %ds
	cmpl $ZBL2_MAGIC, 0
	jne error_header_ds
	cmpw $ZBL2_MACHINE_PC98, ZBL2_MACHINE
	jne error_header_ds
	movw ZBL2_SECTOR_COUNT, %cx
	testw %cx, %cx
	jz error_header_ds
	cmpw $ZBL_STAGE2_MAX_SECT, %cx
	ja error_header_ds
	movw ZBL2_ENTRY_OFFSET, %bx
	popw %ds

	decw %cx
	jz loaded
	movw $(ZBL_STAGE2_SEG + 0x20), %ax
	movl $(PC98_STAGE2_LBA + 1), read_lba
load_loop:
	movw %ax, target_segment
	call read_one
	jc error_disk
	addw $0x20, %ax
	incl read_lba
	loop load_loop
loaded:
	call write_bootinfo
	movb $'2', %al
	outb %al, $0xe9
	movb boot_drive, %al
	pushw $ZBL_STAGE2_SEG
	pushw %bx
	lret

error_header_ds:
	popw %ds
	movb $'H', %al
	jmp fatal
error_sense:
	movb $'S', %al
	jmp fatal
error_disk:
	movb $'D', %al
fatal:
	outb %al, $0xe9
	cli
1:	hlt
	jmp 1b

write_bootinfo:
	pushw %es
	xorw %ax, %ax
	movw %ax, %es
	movl $BOOTINFO_MAGIC, %es:BOOTINFO
	movb boot_drive, %al
	movb %al, %es:BOOTINFO+4
	movb sense_heads, %al
	movb %al, %es:BOOTINFO+5
	movb sense_sectors, %al
	movb %al, %es:BOOTINFO+6
	movw sense_cylinders, %ax
	movw %ax, %es:BOOTINFO+8
	movw saved_si, %ax
	movw %ax, %es:BOOTINFO+10
	movw saved_di, %ax
	movw %ax, %es:BOOTINFO+12
	popw %es
	ret

bios_sense:
	pushw %ds
	xorw %bx, %bx
	movw %bx, %ds
	movb %cs:boot_drive, %al
	movb $0x84, %ah
	movw $0x0100, %bx
	movw %cs:saved_si, %si
	movw %cs:saved_di, %di
	int $0x1b
	movw %cx, %cs:sense_cylinders
	movb %dh, %cs:sense_heads
	movb %dl, %cs:sense_sectors
	popw %ds
	cmpb $0, %cs:sense_heads
	je 1f
	cmpb $0, %cs:sense_sectors
	je 1f
	clc
	ret
1:	stc
	ret

/* One 512-byte fixed-disk read: read_lba -> target_segment:0000. */
read_one:
	pushal
	pushw %ds
	pushw %es
	pushw %bp
	movl %cs:read_lba, %eax
	xorl %edx, %edx
	movzbl %cs:sense_heads, %edi
	movzbl %cs:sense_sectors, %ecx
	imull %ecx, %edi
	testl %edi, %edi
	jz 2f
	divl %edi
	cmpl $0xffff, %eax
	ja 2f
	movw %ax, %cx
	movl %edx, %eax
	xorl %edx, %edx
	movzbl %cs:sense_sectors, %edi
	divl %edi
	movb %al, %dh
	movw %cs:target_segment, %ax
	movw %ax, %es
	xorw %bp, %bp
	xorw %ax, %ax
	movw %ax, %ds
	movb %cs:boot_drive, %al
	movb $0x06, %ah
	movw $512, %bx
	movw %cs:saved_si, %si
	movw %cs:saved_di, %di
	int $0x1b
	pushfw
	popw %ax
	movw %ax, %cs:read_flags
	jmp 3f
2:	movw $1, %cs:read_flags
3:	popw %bp
	popw %es
	popw %ds
	popal
	pushw %cs:read_flags
	popfw
	ret

saved_si:        .word 0
saved_di:        .word 0
sense_cylinders: .word 0
read_flags:      .word 0
target_segment:  .word 0
read_lba:        .long 0
boot_drive:      .byte 0
sense_heads:     .byte 0
sense_sectors:   .byte 0

.org 0x01be
	.space 62
	/* PC-98 firmware IPL sector count metadata; the MBR signature follows. */
	.word 9
	.word 0xaa55
