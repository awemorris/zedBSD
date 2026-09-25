.code32
.text
.globl _start
.type _start, @function
_start:
	.cfi_startproc
	/*
	 * Nothing called this, so a debugger walking the stack stops here:
	 * the register it would return through holds no return address.
	 */
	.cfi_undefined %eip
	xorl %ebp, %ebp
	movl %esp, %esi
	call __x86.get_pc_thunk.bx
	addl $_GLOBAL_OFFSET_TABLE_, %ebx
	movl (%esi), %eax
	leal 4(%esi), %ecx
	leal 8(%esi,%eax,4), %edx
	pushl %edx
	pushl %ecx
	pushl %eax
	call __libc_init@PLT
	addl $12, %esp
	movl (%esi), %eax
	leal 4(%esi), %ecx
	leal 8(%esi,%eax,4), %edx
	pushl %edx
	pushl %ecx
	pushl %eax
	call main
	addl $12, %esp
	pushl %eax
	call exit@PLT
1:
	jmp 1b
	.cfi_endproc
.size _start, .-_start

.section .text.__x86.get_pc_thunk.bx,"axG",@progbits,__x86.get_pc_thunk.bx,comdat
.globl __x86.get_pc_thunk.bx
.hidden __x86.get_pc_thunk.bx
.type __x86.get_pc_thunk.bx, @function
__x86.get_pc_thunk.bx:
	movl (%esp), %ebx
	ret
.size __x86.get_pc_thunk.bx, .-__x86.get_pc_thunk.bx
