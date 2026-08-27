/* PC/AT ELF64 long-mode native-loader payload. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "bootloader/include/amd64-handoff.h"
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
	cmpl $ZBL6_HANDOFF_MAGIC, ZBL6_HANDOFF_MAGIC_OFFSET(%rdi)
	jne fail
	cmpw $ZBL6_HANDOFF_VERSION, ZBL6_HANDOFF_VERSION_OFFSET(%rdi)
	je legacy_handoff
	cmpw $ZBL6_HANDOFF_V5_VERSION, ZBL6_HANDOFF_VERSION_OFFSET(%rdi)
	jne fail
	cmpw $ZBL6_HANDOFF_V5_BIOS_SIZE, ZBL6_HANDOFF_SIZE_OFFSET(%rdi)
	jne fail
	testw $ZBL6_HANDOFF_FLAG_BOOT_PARAMETERS, ZBL6_HANDOFF_FLAGS_OFFSET(%rdi)
	jz fail
	cmpl $ZEDBSD_BOOT_PARAMETER_RECORD_MAGIC, \
	    ZBL6_HANDOFF_V5_BIOS_PARAMETERS_OFFSET(%rdi)
	jne fail
	jmp common_handoff
legacy_handoff:
	cmpw $ZBL6_HANDOFF_SIZE, ZBL6_HANDOFF_SIZE_OFFSET(%rdi)
	je common_handoff
	cmpw $ZBL6_HANDOFF_FB_SIZE, ZBL6_HANDOFF_SIZE_OFFSET(%rdi)
	jne fail
common_handoff:
	cmpb $0x80, ZBL6_HANDOFF_BOOT_DRIVE_OFFSET(%rdi)
	jne fail
	cmpb $1, ZBL6_HANDOFF_PARTITION_OFFSET(%rdi)
	jne fail
	cmpl $640, ZBL6_HANDOFF_MEM_LOWER_OFFSET(%rdi)
	jne fail
	cmpl $0, ZBL6_HANDOFF_MEM_UPPER_OFFSET(%rdi)
	je fail
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
