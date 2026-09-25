.code64
.text
.globl _start
.type _start, @function
_start:
	.cfi_startproc
	/*
	 * Nothing called this, so a debugger walking the stack stops here:
	 * the register it would return through holds no return address.
	 */
	.cfi_undefined %rip
	xorq %rbp, %rbp
	movq %rsp, %r12
	movq (%r12), %rdi
	leaq 8(%r12), %rsi
	leaq 16(%r12,%rdi,8), %rdx
	call __libc_init
	movq (%r12), %rdi
	leaq 8(%r12), %rsi
	leaq 16(%r12,%rdi,8), %rdx
	call main
	movq %rax, %rdi
	call exit
1:
	pause
	jmp 1b
	.cfi_endproc
.size _start, .-_start
