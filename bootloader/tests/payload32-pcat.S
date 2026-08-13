/* PC/AT ELF32 native-loader payload. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
.code32
.section .text
.global _start
_start:
	cmpl $0x2badb002, %eax
	jne fail
	cmpl $0, payload_bss
	jne fail
	movl $pass, %esi
	jmp write
fail:
	movl $failure, %esi
write:
	lodsb
	testb %al, %al
	jz halt
	outb %al, $0xe9
	jmp write
halt:
	cli
1:	hlt
	jmp 1b
.section .rodata
pass: .asciz "P32 PASS\n"
failure: .asciz "P32 FAIL\n"
.section .bss
.balign 4
payload_bss: .long 0

