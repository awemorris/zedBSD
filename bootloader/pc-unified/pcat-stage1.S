/* PC/AT Stage 1.5: load the PC/AT ZBL2 slot through INT 13h EDD. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
.intel_syntax noprefix
.code16
.section .text
.global _start

#include "bootloader/pc-unified/layout.inc"
#include "bootloader/include/stage2-header.inc"

_start:
	cli
	cld
	push cs
	pop ds
	xor ax, ax
	mov ss, ax
	mov sp, 0x7c00
	sti
	mov byte ptr [boot_drive], dl
	mov al, 'a'
	call putc

	mov bx, 0x55aa
	mov ah, 0x41
	mov dl, byte ptr [boot_drive]
	int 0x13
	jc error_edd
	cmp bx, 0xaa55
	jne error_edd
	test cx, 1
	jz error_edd

	mov word ptr [dap_segment], ZBU_STAGE2_SEG
	mov dword ptr [dap_lba], ZBU_PCAT_STAGE2_LBA
	call read_one

	push ds
	mov ax, ZBU_STAGE2_SEG
	mov ds, ax
	cmp dword ptr [0], ZBL2_MAGIC
	jne error_header_ds
	cmp word ptr [4], ZBL2_VERSION
	jne error_header_ds
	cmp word ptr [6], ZBL2_HEADER_SIZE
	jne error_header_ds
	cmp word ptr [ZBL2_MACHINE], ZBL2_MACHINE_PCAT
	jne error_header_ds
	mov cx, word ptr [ZBL2_SECTOR_COUNT]
	test cx, cx
	jz error_header_ds
	cmp cx, ZBU_STAGE2_MAX_SECT
	ja error_header_ds
	mov eax, dword ptr [ZBL2_IMAGE_SIZE]
	test eax, eax
	jz error_header_ds
	movzx edx, cx
	shl edx, 9
	cmp eax, edx
	ja error_header_ds
	mov bx, word ptr [ZBL2_ENTRY_OFFSET]
	cmp dword ptr [ZBL2_ENTRY_OFFSET], eax
	jae error_header_ds
	pop ds

	dec cx
	jz loaded
	mov ax, ZBU_STAGE2_SEG + 0x20
	mov dword ptr [dap_lba], ZBU_PCAT_STAGE2_LBA + 1
load_loop:
	mov word ptr [dap_segment], ax
	call read_one
	add ax, 0x20
	inc dword ptr [dap_lba]
	loop load_loop
loaded:
	push ds
	mov ax, ZBU_STAGE2_SEG
	mov ds, ax
	movzx ecx, word ptr [ZBL2_SECTOR_COUNT]
	shl ecx, 7
	xor esi, esi
	xor eax, eax
checksum_loop:
	add eax, dword ptr [esi]
	add esi, 4
	loop checksum_loop
	test eax, eax
	jnz error_checksum_ds
	mov bx, word ptr [ZBL2_ENTRY_OFFSET]
	pop ds
	mov dl, byte ptr [boot_drive]
	mov al, '2'
	call putc
	push ZBU_STAGE2_SEG
	push bx
	retf

error_header_ds:
	pop ds
	mov al, 'H'
	jmp fatal
error_checksum_ds:
	pop ds
	mov al, 'C'
	jmp fatal
error_edd:
	mov al, 'E'
	jmp fatal
error_disk:
	mov al, 'D'
fatal:
	call putc
	cli
1:	hlt
	jmp 1b

read_one:
	push ax
	push bx
	push cx
	push dx
	push si
	mov byte ptr [retry], 3
1:	mov si, offset dap
	mov ah, 0x42
	mov dl, byte ptr [boot_drive]
	int 0x13
	jnc 2f
	xor ah, ah
	int 0x13
	dec byte ptr [retry]
	jnz 1b
	pop si
	pop dx
	pop cx
	pop bx
	pop ax
	jmp error_disk
2:	pop si
	pop dx
	pop cx
	pop bx
	pop ax
	ret

putc:
	push ax
	push dx
	mov dx, 0x00e9
	out dx, al
	pop dx
	pop ax
	ret

.balign 4
dap:
	.byte 0x10, 0
	.word 1
	.word 0
dap_segment: .word 0
dap_lba: .quad 0
boot_drive: .byte 0
retry: .byte 0

.org 0x0200

