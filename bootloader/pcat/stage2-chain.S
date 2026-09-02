/* PC/AT ZBL2 Stage 2: select the active partition and chain its PBR. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
.intel_syntax noprefix
.code16
.section .text
.global stage2_entry

#include "bootloader/include/stage2-header.inc"

stage2_header:
	.long ZBL2_MAGIC
	.word ZBL2_VERSION
	.word ZBL2_HEADER_SIZE
	.long stage2_end
	.word 0
	.word ZBL2_MACHINE_PCAT
	.long stage2_entry
	.long 0
	.long 0
	.long 0

stage2_entry:
	cli
	cld
	mov ax, cs
	mov ds, ax
	xor ax, ax
	mov ss, ax
	mov sp, 0x7c00
	sti
	mov byte ptr [boot_drive], dl

	/* Reload the MBR into a scratch buffer and select its active entry. */
	mov word ptr [dap_offset], 0
	mov word ptr [dap_segment], 0x0800
	mov dword ptr [dap_lba], 0
	call read_one
	mov ax, 0x0800
	mov es, ax
	cmp word ptr es:[0x1fe], 0xaa55
	jne fatal
	mov si, 0x01be
	mov cx, 4
find_active:
	cmp byte ptr es:[si], 0x80
	je active_found
	add si, 16
	loop find_active
	jmp fatal
active_found:
	mov eax, dword ptr es:[si + 8]
	test eax, eax
	jz fatal
	mov dword ptr [dap_lba], eax
	mov word ptr [dap_offset], 0x7c00
	mov word ptr [dap_segment], 0
	call read_one
	mov al, '3'
	call putc
	mov dl, byte ptr [boot_drive]
	/* 16:16 immediate far jump (EA offset16 segment16). */
	.byte 0xea
	.word 0x7c00
	.word 0x0000

read_one:
	mov si, offset dap
	mov ah, 0x42
	mov dl, byte ptr [boot_drive]
	int 0x13
	jc fatal
	ret

fatal:
	mov al, 'B'
	call putc
	cli
1:	hlt
	jmp 1b

putc:
	mov dx, 0x00e9
	out dx, al
	ret

.balign 4
dap:
	.byte 0x10, 0
	.word 1
dap_offset: .word 0
dap_segment: .word 0
dap_lba: .quad 0
boot_drive: .byte 0
stage2_end:
