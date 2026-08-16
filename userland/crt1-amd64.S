.code64
.text
.globl _start
.type _start, @function
_start:
	xorq %rbp, %rbp
	movq %rsp, %r12
	movq (%r12), %rdi
	leaq 8(%r12), %rsi
	leaq 16(%r12,%rdi,8), %rdx
	call zedbsd_user_libc_init
	movq (%r12), %rdi
	leaq 8(%r12), %rsi
	leaq 16(%r12,%rdi,8), %rdx
	call main
	movq %rax, %rdi
	call exit
1:
	pause
	jmp 1b
.size _start, .-_start
