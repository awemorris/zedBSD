/* Linux paging-off exit trampoline.  SPDX-License-Identifier: Zlib */
	.code32
	.section .text.low, "ax"
	.globl zedbsd_pc98_jump_linux
	.type zedbsd_pc98_jump_linux, @function
zedbsd_pc98_jump_linux:
	movl 4(%esp), %edx	/* physical Linux entry */
	movl 8(%esp), %esi	/* boot_params */
	cli
	movb $0xff, %al
	outb %al, $0x0a
	outb %al, $0x50
	/* Continue at the identity alias before removing the higher-half map. */
	movl $1f, %eax
	subl $0x80000000, %eax
	jmp *%eax
1:
	movl %cr0, %eax
	andl $0x7fffffff, %eax
	movl %eax, %cr0
	jmp 2f
2:
	xorl %ebp, %ebp
	xorl %edi, %edi
	xorl %ebx, %ebx
	jmp *%edx
	.size zedbsd_pc98_jump_linux, .-zedbsd_pc98_jump_linux
	.section .note.GNU-stack, "", @progbits
