/* PC-98 Stage 1.5: fixed H=8/S=17 loader for the PC-98 ZBL2 slot. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
.code16
.section .text
.global _start

#include "bootloader/unified/layout.inc"
#include "bootloader/include/stage2-header.inc"

.set BOOTINFO, 0x0700
.set BOOTINFO_MAGIC, 0x3150395a

_start:
	cli
	cld
	pushw %cs
	popw %ds
	movw %si, saved_si
	movw %di, saved_di
	xorw %ax, %ax
	movw %ax, %ss
	movw $0x7000, %sp
	sti
	movb $'n', %al
	outb %al, $0xe9

	pushw %ds
	xorw %ax, %ax
	movw %ax, %ds
	movb 0x0584, %al
	popw %ds
	andb $0x7f, %al
	orb $0x80, %al
	movb %al, boot_drive
	call bios_sense_cylinders

	movw $ZBU_STAGE2_SEG, target_segment
	movl $ZBU_PC98_STAGE2_LBA, read_lba
	call read_one
	jc error_disk

	pushw %ds
	movw $ZBU_STAGE2_SEG, %ax
	movw %ax, %ds
	cmpl $ZBL2_MAGIC, 0
	jne error_header_ds
	cmpw $ZBL2_VERSION, 4
	jne error_header_ds
	cmpw $ZBL2_HEADER_SIZE, 6
	jne error_header_ds
	cmpw $ZBL2_MACHINE_PC98, ZBL2_MACHINE
	jne error_header_ds
	movw ZBL2_SECTOR_COUNT, %cx
	testw %cx, %cx
	jz error_header_ds
	cmpw $ZBU_STAGE2_MAX_SECT, %cx
	ja error_header_ds
	movl ZBL2_IMAGE_SIZE, %eax
	testl %eax, %eax
	jz error_header_ds
	movzwl %cx, %edx
	shll $9, %edx
	cmpl %edx, %eax
	ja error_header_ds
	movw ZBL2_ENTRY_OFFSET, %bx
	cmpl %eax, ZBL2_ENTRY_OFFSET
	jae error_header_ds
	popw %ds

	decw %cx
	jz loaded
	movw $(ZBU_STAGE2_SEG + 0x20), %ax
	movl $(ZBU_PC98_STAGE2_LBA + 1), read_lba
load_loop:
	movw %ax, target_segment
	call read_one
	jc error_disk
	addw $0x20, %ax
	incl read_lba
	loop load_loop
loaded:
	pushw %ds
	movw $ZBU_STAGE2_SEG, %ax
	movw %ax, %ds
	movzwl ZBL2_SECTOR_COUNT, %ecx
	shll $7, %ecx
	xorl %esi, %esi
	xorl %eax, %eax
checksum_loop:
	addl (%esi), %eax
	addl $4, %esi
	loop checksum_loop
	testl %eax, %eax
	jnz error_checksum_ds
	movw ZBL2_ENTRY_OFFSET, %bx
	popw %ds
	call write_bootinfo
	movb $'2', %al
	outb %al, $0xe9
	movb boot_drive, %al
	pushw $ZBU_STAGE2_SEG
	pushw %bx
	lret

error_header_ds:
	popw %ds
	movb $'H', %al
	jmp fatal
error_checksum_ds:
	popw %ds
	movb $'C', %al
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
	movb $ZBU_PC98_HEADS, %es:BOOTINFO+5
	movb $ZBU_PC98_SECTORS, %es:BOOTINFO+6
	movw sense_cylinders, %ax
	movw %ax, %es:BOOTINFO+8
	movw saved_si, %ax
	movw %ax, %es:BOOTINFO+10
	movw saved_di, %ax
	movw %ax, %es:BOOTINFO+12
	popw %es
	ret

bios_sense_cylinders:
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
	popw %ds
	ret

read_one:
	pushal
	pushw %ds
	pushw %es
	pushw %bp
	movl %cs:read_lba, %eax
	xorl %edx, %edx
	movl $(ZBU_PC98_HEADS * ZBU_PC98_SECTORS), %edi
	divl %edi
	cmpl $0xffff, %eax
	ja read_bad
	movw %ax, %cx
	movl %edx, %eax
	xorl %edx, %edx
	movl $ZBU_PC98_SECTORS, %edi
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
	jmp read_done
read_bad:
	movw $1, %cs:read_flags
read_done:
	popw %bp
	popw %es
	popw %ds
	popal
	pushw %cs:read_flags
	popfw
	ret

saved_si: .word 0
saved_di: .word 0
sense_cylinders: .word 0
read_flags: .word 0
target_segment: .word 0
read_lba: .long 0
boot_drive: .byte 0

.org 0x0200

