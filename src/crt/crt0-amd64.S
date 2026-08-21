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
	call __libc_init
	movq (%r12), %rdi
	leaq 8(%r12), %rsi
	leaq 16(%r12,%rdi,8), %rdx
	call main
	movq %rax, %rdi
	call _exit
1:
	pause
	jmp 1b
.size _start, .-_start

.globl __syscall6
.type __syscall6, @function
__syscall6:
	pushq %rbx
	pushq %rbp
	movq %rdi, %rax
	movq %rsi, %rbx
	movq %rdx, %r10
	movq %rcx, %r11
	movq %r10, %rcx
	movq %r11, %rdx
	movq %r8, %rsi
	movq %r9, %rdi
	movq 24(%rsp), %rbp
	int $0xc2
	popq %rbp
	popq %rbx
	ret
.size __syscall6, .-__syscall6

.globl __signal_restorer
.type __signal_restorer, @function
__signal_restorer:
	movq (%rsp), %rbx
	movq 8(%rsp), %rcx
	movq $80, %rax
	int $0xc2
	ud2
.size __signal_restorer, .-__signal_restorer
