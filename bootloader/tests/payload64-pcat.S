/* PC/AT ELF64 long-mode native-loader payload. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
.code64
.section .text
.global _start
_start:
	movq %cr0, %rax
	btq $31, %rax
	jnc fail
	movq %cr4, %rax
	testq $0x20, %rax
	jz fail
	movl $0xc0000080, %ecx
	rdmsr
	testl $0x400, %eax
	jz fail
	cmpq $0, payload_bss(%rip)
	jne fail
	testq %rdi, %rdi
	jz fail
	leaq pass(%rip), %rsi
	jmp write
fail:
	leaq failure(%rip), %rsi
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
pass: .asciz "P64 PASS\n"
failure: .asciz "P64 FAIL\n"
.section .bss
.balign 8
payload_bss: .quad 0
