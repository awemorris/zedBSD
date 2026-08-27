/* PC-98 ELF32 native MBR-loader payload. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "include/boot/pc98-handoff.h"
.code32
.section .text
.global _start
_start:
	cmpl $0, payload_bss
	jne fail
	cmpl $0x48323842, 0(%ebx)
	jne fail
	cmpw $2, 4(%ebx)
	jne legacy_v3
	cmpw $ZEDBSD_PC98_PARAMETER_HANDOFF_SIZE, 6(%ebx)
	jne fail
	cmpw $0, 10(%ebx)
	jne fail
	cmpl $ZEDBSD_BOOT_PARAMETER_RECORD_MAGIC, ZEDBSD_PC98_PARAMETER_RECORD_OFFSET(%ebx)
	jne fail
	jmp origin_ready
legacy_v3:
	cmpw $3, 4(%ebx)
	jne fail
	cmpw $ZEDBSD_PC98_HANDOFF_COMMON_SIZE, 6(%ebx)
	jne fail
	cmpb $1, 10(%ebx)
	jne fail
	cmpb $1, 11(%ebx)
	jne fail
origin_ready:
	cmpl $2048, 20(%ebx)
	jne fail
	movl 12(%ebx), %edi
	testl %edi, %edi
	jz fail
	cmpb $2, 0(%edi)
	jne fail
	movb 3(%edi), %al
	andb $7, %al
	cmpb $7, %al
	jne fail
	cmpw $512, 4(%edi)
	jne fail
	cmpb $17, 9(%edi)
	jne fail
	movl 16(%ebx), %edx
	testl %edx, %edx
	jz fail
	movw $1, bios_request
	movb 2(%edi), %al
	movb %al, bios_request+4
	movb 8(%edi), %al
	movb %al, bios_request+5
	movb 9(%edi), %al
	movb %al, bios_request+6
	movl $2048, bios_request+8
	movl $bios_buffer, bios_request+12
	pushl $bios_request
	call *%edx
	addl $4, %esp
	testl %eax, %eax
	jne fail
	cmpw $512, bios_buffer+11
	jne fail
	cmpb $4, 8(%edi)
	je pass_h4
	cmpb $8, 8(%edi)
	jne fail
	movl $pass_h8, %esi
	jmp write
pass_h4:
	movl $pass_h4_message, %esi
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
pass_h8: .asciz "N32 H8 PASS\n"
pass_h4_message: .asciz "N32 H4 PASS\n"
failure: .asciz "N32 FAIL\n"
.section .bss
.balign 4
payload_bss: .long 0
.balign 4
bios_request: .space 16
bios_buffer: .space 512
