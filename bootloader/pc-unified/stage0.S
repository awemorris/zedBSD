/* Polyglot PC/AT MBR and PC-98 IPL dispatcher. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
.intel_syntax noprefix
.code16
.section .text
.global _start

#include "bootloader/pc-unified/layout.inc"

_start:
	jmp short locate
	nop
	nop
	.ascii "IPL1"
locate:
	cli
	cld
	xor ax, ax
	mov ss, ax
	mov sp, 0x7c00
	call 1f
1:	pop bx
	sub bx, 1b - _start
	mov byte ptr cs:[bx + saved_dl - _start], dl
	mov word ptr cs:[bx + saved_si - _start], si
	mov word ptr cs:[bx + saved_di - _start], di
	push cs
	pop ds
	xor ax, ax
	mov es, ax
	mov si, bx
	mov di, ZBU_RELOC_ADDR
	mov cx, 256
	rep movsw
	.byte 0xea
	.word ZBU_RELOC_ADDR + relocated - _start
	.word 0

relocated:
	xor ax, ax
	mov ds, ax
	mov es, ax
	mov ss, ax
	mov sp, 0x7c00
	sti
	mov al, 'U'
	call putc

	/* A PC-98 IDE boot ID and its presence bitmap must agree. */
	mov al, byte ptr [0x0584]
	mov ah, al
	and ah, 0xf0
	cmp ah, 0x80
	jne pcat
	mov cl, al
	and cl, 0x0f
	cmp cl, 7
	ja pcat
	mov bl, byte ptr [0x055d]
	shr bl, cl
	test bl, 1
	jz pcat

pc98:
	mov al, 'N'
	call putc
	mov ax, ZBU_PC98_STAGE1_SEG
	mov es, ax
	xor bp, bp
	xor ax, ax
	mov ds, ax
	mov al, byte ptr [0x0584]
	and al, 0x7f
	or al, 0x80
	mov ah, 0x06
	mov bx, 512
	xor cx, cx
	xor dh, dh
	mov dl, ZBU_PC98_STAGE1_LBA
	mov si, word ptr [ZBU_RELOC_ADDR + saved_si - _start]
	mov di, word ptr [ZBU_RELOC_ADDR + saved_di - _start]
	int 0x1b
	jc disk_error
	push ZBU_PC98_STAGE1_SEG
	push 0
	retf

pcat:
	mov al, 'A'
	call putc
	mov dl, byte ptr [ZBU_RELOC_ADDR + saved_dl - _start]
	mov bx, 0x55aa
	mov ah, 0x41
	int 0x13
	jc edd_error
	cmp bx, 0xaa55
	jne edd_error
	test cx, 1
	jz edd_error
	mov si, ZBU_RELOC_ADDR + dap - _start
	mov ah, 0x42
	mov dl, byte ptr [ZBU_RELOC_ADDR + saved_dl - _start]
	int 0x13
	jc disk_error
	mov dl, byte ptr [ZBU_RELOC_ADDR + saved_dl - _start]
	push ZBU_PCAT_STAGE1_SEG
	push 0
	retf

edd_error:
	mov al, 'E'
	jmp fatal
disk_error:
	mov al, 'D'
fatal:
	call putc
	cli
2:	hlt
	jmp 2b

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
	.word ZBU_PCAT_STAGE1_SEG
	.quad ZBU_PCAT_STAGE1_LBA
saved_dl: .byte 0
saved_si: .word 0
saved_di: .word 0

/* The last two bytes of entry four are reserved for PC-98 IPL metadata. */
.org 0x01be
	.space 62
	.word 9
	.word 0xaa55

